#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include "gbx/common/scheduler.h"
#include "gbx/common/trace.h"
#include "gbx/gb/apu.h"
#include "gbx/gb/cartridge.h"
#include "gbx/gb/mode.h"
#include "gbx/gb/ppu.h"

namespace gbx::gb {

struct MemoryPage { const uint8_t* read = nullptr; uint8_t* write = nullptr; };
class Offload;

// CPU-owned mutable machine state. Rendering must consume copied tasks, never
// these arrays. 128-byte pages keep HRAM separate from IO without an additional
// branch on every read. IE writes remain handled even when HRAM is direct,
// so interrupt changes cannot bypass the CPU's instruction boundary.
//
// A CGB has 32 KiB of WRAM in eight 4 KiB banks and 16 KiB of VRAM in two
// (Pan Docs, CGB Registers: SVBK, VBK). wram holds banks 0 and 1 and vram
// bank 0, as on the DMG; the other banks come from a block of their own,
// allocated at the first load in a CGB mode, so that the DMG's state and
// layout stay as they are. The mode is fixed at loading, so the CGB's
// checks are only on paths that already branch: IO registers, bank
// switches, VRAM and events.
class Memory {
public:
    Memory();
    ~Memory();
    Scheduler clock;
    // VRAM is never direct: its permissions change at PPU events several
    // times per line, and batched JIT runs that defer those events must reach
    // it through careful accesses, see sm83_jit_runtime.cpp.
    std::array<MemoryPage, 512> pages{};
    // Verified JIT blocks compare their code an aligned word at a time.
    alignas(4) std::array<uint8_t, 8192> wram{}, vram{};
    std::array<uint8_t, 160> oam{};
    alignas(4) std::array<uint8_t, 128> io{}, high{};  // high[127] is IE, not HRAM.
    // Mapping changes stop translated blocks whose code they move, see
    // remapped. Directly writable RAM stays writable after code execution;
    // the JIT checks opcodes at each fetch.
    uint32_t mapping_generation = 0;
    uint32_t interrupt_generation = 0;
    uint16_t instruction_pc = 0;
    // Windows whose bank can switch, as bits: ROM 0000-3FFF and 4000-7FFF,
    // cartridge RAM, and in CGB mode VRAM and the WRAM at D000-DFFF with its
    // echo. See code_bank().
    enum : uint8_t { kRom0 = 1, kRom1 = 2, kCartRam = 4, kVram = 8, kWramBank = 16 };
    // The windows whose bank changed since the JIT last cleared this. Bank
    // switches set it next to mapping_generation; VRAM, whose code the JIT
    // checks at each fetch, never. In padding, so the layout stays.
    uint8_t remapped = 0;
    uint64_t frames = 0;
    Cartridge cartridge;
    Ppu ppu;
    Apu apu;

    // Starts the worker on the other core that renders and mixes.
    bool initialize();
    // Whether the worker polls for the next item before it sleeps, see
    // Offload::set_polling().
    void set_worker_polling(bool polling);
    bool load(std::span<const uint8_t> rom, Mode mode = Mode::dmg);
    void reset();
    Mode mode() const { return mode_; }
    // CGB hardware, in either of its modes.
    bool color() const { return mode_ != Mode::dmg; }
    // VRAM bank 1 of a CGB; nullptr in GB mode. A CGB mode has the banks.
    uint8_t* vram1() { return color() ? banks_->vram.data() : nullptr; }
    const uint8_t* vram1() const { return color() ? banks_->vram.data() : nullptr; }
    // The PPU's CGB state; nullptr in GB mode.
    PpuColor* ppu_color() { return color() ? &banks_->ppu : nullptr; }
    // WRAM banks 2..7 of a CGB, after the two in wram; empty in GB mode.
    std::span<const uint8_t> wram_banks() const {
        return color() ? std::span<const uint8_t>(banks_->wram) : std::span<const uint8_t>{};
    }
    void trace(TraceBuffer* sink, bool keep_direct = false);
    // Called only by selected trace implementations/generated probes.
    void record_access(Tick time, uint16_t address, uint8_t data, uint8_t operation) {
        trace_->push({time, instruction_pc, address, data, operation});
    }
    // Brings the PPU up to the current time, see Ppu::sync().
    uint8_t peek(uint16_t address);
    // Storage of a RAM address regardless of access permissions, so that
    // echo RAM and mirrored cartridge RAM resolve to the same byte.
    const uint8_t* backing(uint16_t address) const;
    void poke(uint16_t address, uint8_t value);
    // The bank of the code at `address`, for the JIT's block keys: ROM and
    // cartridge RAM banks, and on a CGB the VRAM bank and the WRAM bank at
    // D000-DFFF and its echo. Banks that never switch count as 0.
    [[gnu::always_inline]] uint16_t code_bank(uint16_t address) const {
        if (address < 0x8000) return cartridge.rom_bank[address >> 14];
        if (address >= 0xa000 && address < 0xc000) return cartridge.ram_bank();
        return mode_ == Mode::cgb ? cgb_code_bank(address) : 0;
    }
    uint16_t divider() const;
    // Requests the CPU can take that IF does not show yet, see Ppu's
    // kHblankSettle.
    uint8_t unseen_interrupts = 0;
    uint8_t pending_interrupts() const { return (io[0x0f] | unseen_interrupts) & high[127] & 31; }
    // Earliest event that may request one of the interrupts in `mask`. Only
    // events request VBlank, STAT, timer and serial interrupts, and a STAT
    // request needs an enabled source, see Ppu::update_stat(); with LY=LYC
    // the only one, it waits for LY. The joypad's come between runs, from
    // set_buttons().
    Tick next_interrupt(uint8_t mask) const {
        Tick next = kNever;
        if (mask & 1) next = ppu.next_vblank();
        if ((mask & 2) && (io[0x41] & 0x78))
            next = std::min(next, (io[0x41] & 0x78) == 0x40 ? ppu.next_lyc() : clock.deadline(0));
        if (mask & 4) next = std::min(next, clock.deadline(1));
        if (mask & 8) next = std::min(next, clock.deadline(2));
        return next;
    }
    void request_interrupt(uint8_t mask) { io[0x0f] |= mask; ++interrupt_generation; }
    void request_unseen(uint8_t mask) { unseen_interrupts |= mask; ++interrupt_generation; }
    // IF shows the unseen requests in mask.
    void show_interrupts(uint8_t mask) { io[0x0f] |= unseen_interrupts & mask; unseen_interrupts &= ~mask; }
    // Bits 0..3: Right, Left, Up, Down; bits 4..7: A, B, Select, Start.
    void set_buttons(uint8_t buttons);
    bool button_selected() const { return (io[0] & 15) != 15; }
    // CGB mode: KEY1 asks STOP to switch the speed (Pan Docs, KEY1).
    bool speed_switch_armed() const { return mode_ == Mode::cgb && (io[0x4d] & 1); }
    // Ticks per CPU M-cycle: 8, or 4 at the CGB's double speed, which the
    // CPU, the timer and DIV, the serial port and OAM DMA run at (Pan Docs,
    // KEY1). The PPU, VRAM DMA and the APU keep their timing in ticks.
    unsigned mcycle() const { return mcycle_; }
    bool double_speed() const { return mcycle_ == 4; }
    // STOP's speed switch, at the end of its fetch. With `pause`, the CPU
    // then waits in HALT until wake, see sm83_stop().
    void switch_speed(bool pause);
    // While OAM DMA runs, page permissions follow its per-byte events.
    bool dma_active() const { return dma_running_; }
    bool dma_busy() const { return dma_running_ || dma_restart_ != kNever; }
    // Earliest pending event that is not a PPU, timer or serial event. Only
    // those change nothing but IO, OAM and VRAM permissions. While an HBlank
    // DMA runs, the PPU's events also copy memory and hold the CPU, see
    // hdma_block(), so they count too.
    Tick deferral_limit() const {
        Tick limit = kNever;
        for (unsigned slot = 0; slot < Scheduler::kSlots; ++slot) {
            const auto callback = clock.callback(slot);
            if (callback && (!Ppu::owns(callback) || hdma_active_) && callback != &timer_event &&
                callback != &serial_event)
                limit = std::min(limit, clock.deadline(slot));
        }
        return limit;
    }
    // CGB VRAM DMA (Pan Docs, LCD VRAM DMA Transfers). An HBlank transfer is
    // under way; the PPU then times its mode 0 by an event, which moves one
    // block of 16 bytes, see Ppu::arm().
    bool hdma_active() const { return hdma_active_; }
    // One block of an HBlank transfer, at the start of mode 0, from the PPU's
    // event, or when the transfer starts without a line to wait for. The CPU
    // stops meanwhile.
    void hdma_block(bool event);
    void stop_clock();
    void resume_clock() { stopped_ = false; }
    std::span<const uint8_t> serial() const { return {serial_output_.data(), serial_size_}; }
    bool serial_truncated() const { return serial_truncated_; }
    std::span<const uint8_t> rom() const { return cartridge.rom(); }
    TraceBuffer* trace_sink() const { return trace_; }

    // Bus access occurs at the T4 edge (three elapsed T-cycles). This edge,
    // rather than instruction completion, timestamps IO and trace records.
    // The interpreter passes the M-cycle's length as a constant, see
    // sm83.cpp; the other callers take the current speed's.
    uint8_t read(uint16_t address, unsigned mcycle) {
        // A direct page has no access side effects. With no intervening event,
        // only the final clock value is observable. Traced interpreter pages
        // deliberately have no direct pointer.
        if (const auto* direct = pages[address >> 7].read) {
            const Tick end = clock.now + mcycle;
            if (end < clock.next) {
                const uint8_t value = direct[address & 127];
                clock.now = end;
                return value;
            }
        }
        return read_timed(address);
    }
    void write(uint16_t address, uint8_t value, unsigned mcycle) {
        if (auto* direct = pages[address >> 7].write) {
            const Tick end = clock.now + mcycle;
            if (address != 0xffff && end < clock.next) {
                direct[address & 127] = value;
                clock.now = end;
                return;
            }
        }
        write_timed(address, value);
    }
    void idle(unsigned mcycle) { clock.advance(clock.now + mcycle); }
    // The rest of a write's M-cycle from its access edge, which the caller
    // reached with idle(): interrupt dispatch reads IF there, before its
    // low-byte push, see System::boundary().
    void write_from_edge(uint16_t address, uint8_t value);
    unsigned access_ticks() const { return access_edge(); }
    uint8_t read(uint16_t address) { return read(address, mcycle_); }
    void write(uint16_t address, uint8_t value) { write(address, value, mcycle_); }
    void idle() { idle(mcycle_); }
    // Registers read as io[address] | mask and changed only by writes and
    // events; -1 for SB, DIV, TIMA, NR52 and wave RAM, whose values depend
    // on the time, and STAT, whose mode and LY=LYC bits the PPU keeps, and on
    // a CGB for PCM12 and PCM34, and in CGB mode HDMA5 and the palette data
    // registers, which the PPU and the DMA keep.
    int io_read_mask(uint8_t address) const;
    // read() of 0xff00 | address, whose page is never direct, without the
    // page and region checks.
    uint8_t read_io_timed(uint8_t address) {
        if (read_slow_ != normal_read) return read_timed(0xff00 | address);
        clock.advance(clock.now + access_edge());
        const auto value = read_io(address);
        clock.advance(clock.now + (mcycle_ - access_edge()));
        return value;
    }
    // read() and write() for a traced JIT run, whose pages stay direct. Each
    // records its access at the access's own time, after the events before
    // it and before its effects, as traced_read() and traced_write() do.
    uint8_t probed_read(uint16_t address);
    void probed_write(uint16_t address, uint8_t value);

private:
    friend class Apu;
    friend struct SnapshotAccess;
    TraceBuffer* trace_ = nullptr;
    bool trace_direct_ = false;
    uint8_t (*read_slow_)(Memory*, uint16_t) = normal_read;
    void (*write_slow_)(Memory*, uint16_t, uint8_t) = normal_write;
    Tick div_origin_ = 0;
    uint16_t div_initial_ = 0;
    bool stopped_ = false;
    uint8_t buttons_ = 0;
    Tick timer_edge_ = kNever, timer_reload_ = kNever, timer_reload_end_ = 0;
    Tick serial_edge_ = kNever;
    uint8_t serial_bits_ = 0, serial_byte_ = 0;
    std::array<uint8_t, 1024> serial_output_{};
    size_t serial_size_ = 0;
    bool serial_truncated_ = false;
    // Ticks to the access edge of an M-cycle, three T-cycles in.
    unsigned access_edge() const { return mcycle_ - (mcycle_ >> 2); }
    // Ticks per T-cycle of the CPU's clock, as a shift: DIV counts these.
    unsigned tcycle_shift() const { return mcycle_ >> 3; }
    unsigned timer_shift() const;
    unsigned timer_period() const { return 1u << timer_shift(); }
    Tick next_div_edge(unsigned period) const;
    uint8_t timer_value() const;
    void sync_timer();
    void tick_timer();
    void schedule_timer();
    void reset_divider();
    unsigned elapsed_serial_bits() const;
    uint8_t serial_value() const;
    void sync_serial();
    void schedule_serial();
    void update_joypad();
    static void timer_event(void* context);
    static void serial_event(void* context);
    Tick dma_restart_ = kNever;
    bool dma_running_ = false;
    uint16_t dma_source_ = 0;
    unsigned dma_index_ = 0;
    uint8_t dma_value_ = 0xff;
    static void dma_event(void* context);
    void start_dma(uint8_t value);
    bool dma_conflict(uint16_t address) const;
    uint8_t cpu_read(uint16_t address);
    void cpu_write(uint16_t address, uint8_t value);
    void write_oam(unsigned offset, uint8_t value);
    void map(unsigned first = 0, unsigned last = 512);
    // Sets code_tags for the windows in `windows`, kRom0 and so on.
    void tag_code(unsigned windows);
    uint8_t read_timed(uint16_t address);
    void write_timed(uint16_t address, uint8_t value);
    uint8_t read_io(uint8_t address);
    void write_io(uint8_t address, uint8_t value);
    // CGB registers FF4C-FF7F, in either mode.
    uint8_t read_cgb(uint8_t address);
    void write_cgb(uint8_t address, uint8_t value);
    void reset_cgb();
    uint16_t cgb_code_bank(uint16_t address) const {
        if (address < 0xa000) return vram_bank_;
        return (address >= 0xd000 && address < 0xe000) || (address >= 0xf000 && address < 0xfe00) ? wram_bank_ : 0;
    }
    void select_wram(uint8_t bank);
    void select_vram(uint8_t bank);
    void start_vram_dma(uint8_t value);
    // Copies one block of 16 bytes; false when the destination ran past VRAM.
    // `paced` moves two bytes an M-cycle, the devices running on between.
    bool copy_block(bool paced);
    static uint8_t normal_read(Memory* memory, uint16_t address);
    static void normal_write(Memory* memory, uint16_t address, uint8_t value);
    static uint8_t traced_read(Memory* memory, uint16_t address);
    static void traced_write(Memory* memory, uint16_t address, uint8_t value);
    // The CGB's state follows the DMG's, which keeps its place.
    Mode mode_ = Mode::dmg;
    // The read masks of the mode, see io_read_mask().
    const uint8_t* io_masks_ = nullptr;
    struct Banks {
        // Verified JIT blocks in D000-DFFF compare their code a word at a time.
        alignas(4) std::array<uint8_t, 6 * 4096> wram;
        alignas(4) std::array<uint8_t, 8192> vram;
        PpuColor ppu;
    };
    struct ReleaseBanks { void operator()(Banks* banks) const; };
    std::unique_ptr<Banks, ReleaseBanks> banks_;
    // What the CPU sees at D000-DFFF and 8000-9FFF, and the bank numbers;
    // GB mode keeps WRAM bank 1 and VRAM bank 0, numbered 0.
    uint8_t* wram_d000_ = wram.data() + 4096;
    uint8_t* vram_cpu_ = vram.data();
    uint8_t wram_bank_ = 0, vram_bank_ = 0;
    // VRAM DMA: the next source address and VRAM offset, and HDMA5's length
    // bits, blocks left minus one.
    uint16_t hdma_source_ = 0, hdma_destination_ = 0;
    uint8_t hdma_length_ = 0x7f;
    bool hdma_active_ = false;
    // Serial bit time: 512 T-cycles, or 16 with the CGB's fast clock (Pan
    // Docs, Serial Data Transfer: SC bit 1), as a shift of ticks.
    uint8_t serial_shift_ = 10;
    uint8_t mcycle_ = 8;  // See mcycle().
    void reset_divider_at(uint16_t old);
public:
    // The JIT's dispatch tags by 4 KiB of address space: the bank of the
    // window there << 16 (code_bank()), or 0 where no bank switches. VRAM
    // counts as 0 too: VBK changes no mapping, and the JIT checks code there
    // at each fetch. Kept with the mapping, see tag_code().
    std::array<uint32_t, 16> code_tags{};
    // The CPU is in HALT or STOP, which also holds an HBlank DMA (Pan Docs,
    // HBlank DMA). Set by sm83_halt() and sm83_stop(), cleared by the
    // system when the CPU runs again.
    bool cpu_waiting = false;
    // The end of the HALT that follows a speed switch, unless an interrupt
    // ends it first; kNever otherwise. See switch_speed().
    Tick wake = kNever;
private:
    // Last, so that the worker stops before the PPU and APU go away.
    std::unique_ptr<Offload> offload_;
};
}  // namespace gbx::gb
