#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "gbx/common/scheduler.h"
#include "gbx/gba/arm7.h"
#include "gbx/gba/backup.h"
#include "gbx/gba/ppu.h"
#include "gbx/gba/rom_cache.h"
#include "gbx/gba/rtc.h"

namespace gbx::gb {
class Offload;
}

namespace gbx::gba {

class Arm7Jit;
class Mixer;
namespace sound {
struct Batch;
enum class Event : uint8_t;
}  // namespace sound

inline constexpr size_t kBiosBytes = 16384;

// Device slots: the PPU's line edges, the timers' next overflow that
// matters, DMA starts, the serial transfer's end and the interrupt line
// reaching the CPU.
using Scheduler = BasicScheduler<5>;
}  // namespace gbx::gba
namespace gbx {
extern template class BasicScheduler<5>;
}  // namespace gbx
namespace gbx::gba {
enum : unsigned { kSlotPpu, kSlotTimer, kSlotDma, kSlotSerial, kSlotIrq };

// IE and IF bits (GBATEK "GBA Interrupt Control").
namespace irq {
inline constexpr uint16_t vblank = 1, hblank = 2, vcount = 4, timer0 = 8, serial = 0x80, dma0 = 0x100,
                          keypad = 0x1000, gamepak = 0x2000;
}

// Keys as KEYINPUT numbers them; a set bit is a pressed key here (GBATEK
// "GBA Keypad Input").
namespace key {
inline constexpr uint16_t a = 1, b = 2, select = 4, start = 8, right = 16, left = 32, up = 64, down = 128,
                          r = 256, l = 512;
}

// The machine's memory map, I/O registers and devices, owned by the CPU's
// task (GBATEK "GBA Memory Map"). The interpreter's bus (src/gba/mapped_bus.h)
// inlines the RAM paths and calls the slow paths here, which the JIT's
// helpers share.
//
// Time. Devices are brought up to date when accessed or when their events
// fall due, never cycle by cycle. The CPU counts its cycles down in `left`
// toward `deadline`, the earlier of the next event and the run's target.
// Every bus access and internal cycle first checks `left`: with none left it
// calls catch_up() or idle_slow(), which dispatch the events due by now()
// and run the DMA they start, so that the access sees their effects. Both
// backends check at the same places, so that events take effect at the
// same cycle. Inside device code clock.now is the current time.
//
// DMA. A channel that starts runs to the end of its transfer inside the
// check that found it due (GBATEK "GBA DMA Transfers"), with the events
// that fall due meanwhile; a bus access then waits for its end. Found at an
// internal cycle, it overlaps the CPU's later internal cycles, and only the
// CPU's next bus access waits, see idle_slow().
class Memory {
public:
    Memory();
    ~Memory();
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;

    // --- Fast-path state, first so that its offsets stay small ------------
    // Cycles before `deadline`; an access starting with none left takes the
    // slow path first. now() = deadline - left.
    int32_t left = 0;
    // Set while the CPU must leave its run at the next instruction
    // boundary: the interrupt line, HALT, the run's target. `left` then
    // stays at most 0, see rearm().
    bool stop = false;
    // The interrupt line as the CPU saw it when its last bus cycle began
    // (IME and IE & IF through its synchronizer, see gba_irq.cpp); taken
    // at an instruction boundary while CPSR's I is clear.
    bool irq_line = false;
    // The boundary takes the interrupt though CPSR's I is set, see
    // MappedBus::irq_masked_late().
    bool irq_unmasked = false;
    // HALTCNT was written: the CPU waits for IE & IF.
    bool halted = false;
    // The prefetch buffer is enabled in WAITCNT.
    bool prefetch_enabled = false;
    // Cycles of an access by region (address bits 24-27), width (byte,
    // half, word), N and S: 1 plus the wait states, changed by WAITCNT and
    // the internal memory control (GBATEK "GBA System Control", "ARM CPU
    // Instruction Cycle Times").
    std::array<std::array<uint8_t, 3>, 16> cycles_n{}, cycles_s{};
    Tick deadline = 0;
    Tick target = kNever;
    Arm7State* cpu = nullptr;
    uint8_t* ewram = nullptr;   // 256 KiB, external memory.
    uint8_t* vram = nullptr;    // 96 KiB, external memory.
    const uint8_t* bios = nullptr;
    RomCache* rom = nullptr;
    const uint8_t* const* rom_table = nullptr;  // rom->table(), for the JIT's reads of the ROM.
    // The data of the ROM page holding `address`, read in when not
    // resident: rom->map() a load closer.
    const uint8_t* rom_page(uint32_t address) const {
        const unsigned page = (address & (RomCache::kMaxBytes - 1)) >> RomCache::kPageBits;
        const uint8_t* data = rom_table[page];
        return data ? data : rom->map(page);
    }
    // JIT code by 64-byte line of IWRAM and EWRAM: a CPU or DMA write to a
    // marked line calls code_written(), see store_code().
    static constexpr unsigned kCodeLineBits = 6;
    uint8_t* ewram_code = nullptr;  // 4096 lines.
    void (*code_written)(void* context, uint32_t address) = nullptr;
    void* code_context = nullptr;
    // The most recent opcode fetched from the BIOS, which reads of it from
    // outside see (GBATEK "GBA Unpredictable Things").
    uint32_t bios_latch = 0;
    // The first byte of OBJ tiles in VRAM: byte writes from there on are
    // ignored, below it they fill the halfword (GBATEK "GBA Unpredictable
    // Things"). 10000h in the tile modes, 14000h in the bitmap modes.
    uint32_t vram_obj_start = 0x10000;
    // VRAM, palette RAM and OAM pages written since the renderer last took
    // them, see Ppu.
    std::array<uint32_t, Ppu::kDirtyWords> dirty{};
    // The clock's low word from which a write to the display registers
    // 08h-57h is not timed in the open line job, see Ppu::write_register():
    // the line's HBlank, or the last job's end. The JIT's write stub stores
    // those itself from there on, the reference points excepted.
    uint32_t display_quiet = 0;
    // The I/O reads below 210h that the JIT's read stub makes itself, by
    // halfword: the bits that read back, or 0 for one that goes to C++.
    // IF keeps a copy in I/O for them, and VCOUNT the line from
    // vcount_from, the clock's low word at its start, see
    // Ppu::mirror_vcount().
    static constexpr uint32_t kIoReads = 0x210;
    std::array<uint16_t, kIoReads / 2> io_reads{};
    uint32_t vcount_from = 0;
    std::array<uint8_t, (32768 >> kCodeLineBits)> iwram_code{};

    Scheduler clock;
    alignas(4) std::array<uint8_t, 32768> iwram{};
    alignas(4) std::array<uint8_t, 1024> palette{}, oam{};
    Ppu ppu;
    Backup backup;

    // --- Loading and the system's interface --------------------------------
    // Allocates the external memories and starts the renderer's worker.
    bool initialize();
    // Borrows the BIOS and the ROM, which outlive the memory. False without
    // the memory for the cartridge's GPIO.
    bool load(std::span<const uint8_t, kBiosBytes> bios_image, RomCache& cartridge);
    // Power-on contents and the state the BIOS leaves when it starts the
    // cartridge, see reset() in gba_memory.cpp; with `power_on` the state
    // before the BIOS's start-up instead.
    void reset(bool power_on = false);
    // Before a run to `target`: the events due now, and a stop for what
    // waits at this boundary.
    void begin_run(Tick until);
    void set_buttons(uint16_t pressed);
    // Whether the worker on the other core polls for the next line or
    // sound batch before it sleeps, see gb::Offload::set_polling().
    void set_worker_polling(bool polling);

    // --- Sound (gba_sound.cpp) ---------------------------------------------
    // The PSG's state the CPU can observe stays on this core: the channels'
    // on flags, length timers and the sweep, brought up to an access's time
    // by the access, and the FIFOs. Each write that changes the sound and
    // each sample a FIFO takes goes into a batch with its time, which the
    // mixer on the other core (src/gba/mixer.h) turns into PCM; nothing
    // comes back. Hands the sound up to now to the mixer: a run calls it at
    // its end.
    void flush_sound();
    // Interleaved left and right 16-bit frames at `rate` per second; 0, the
    // default, makes none. The rate and speed stay through reset.
    void set_audio_rate(uint32_t rate);
    // Adjusts the frames per emulated second by parts per million.
    void set_audio_speed(int32_t ppm);
    // One other task at a time may read the frames and ask for the dropped.
    size_t read_audio(std::span<int16_t> frames);
    size_t audio_queued() const;
    // The CPU's core only: also the frames of batches not mixed yet.
    size_t audio_buffered() const;
    // Waits until the mixer has processed every batch handed over.
    void finish_audio();
    uint64_t audio_dropped() const;
    static constexpr int32_t kMaxAudioSpeed = 20000;

    // --- Cartridge GPIO and RTC (gba_rtc.cpp) --------------------------------
    // Whether the cartridge has them: the games cartridge_has_rtc() lists.
    bool has_rtc() const { return gpio_page_ != nullptr; }
    // Its clock keeps counting through reset(); load() starts it as a new
    // battery leaves it.
    Rtc rtc;

    Tick now() const { return static_cast<Tick>(static_cast<int64_t>(deadline) - left); }
    // Brings the clock to now(), dispatching the events due by then and
    // running the DMA they start, then sets a new deadline. catch_up() is a
    // bus access's, which waits for the DMA; idle_slow() an internal
    // cycle's, which does not.
    void catch_up();
    void idle_slow();
    // catch_up() at an instruction boundary: a DMA that falls due is left
    // to the next access, which waits for it, so that a DMA that comes due
    // when another ends finds that access already waiting (alyosha
    // DMA/DMA_pause_timing_end_3).
    void catch_up_boundary();
    // catch_up() for an opcode fetch, which replaces what a DMA left on
    // the bus.
    void catch_up_fetch() {
        catch_up();
        if (dma_bus_) { dma_bus_ = false; rearm(); }
    }
    // A new deadline from clock.next and target; keeps left <= 0 while a
    // stop is requested.
    void rearm();
#ifdef GBX_PROFILER
    bool irq_watched() const { return irq_watch_; }
#endif
    // Ends the CPU's run at the next instruction boundary.
    void request_stop() {
        stop = true;
        if (left > 0) {
            deadline -= static_cast<Tick>(left);
            left = 0;
        }
    }

    // --- Slow paths of the CPU's bus, shared with the JIT ------------------
    // Accesses by width (0 byte, 1 half, 2 word) at now(), their cycles
    // counted inside. The address is the CPU's, unaligned; reads return the
    // aligned value of the width. `r15` is the CPU's R15 at the access, for
    // the open bus and the BIOS's protection.
    uint32_t read_slow(uint32_t address, unsigned width, bool sequential, uint32_t r15);
    void write_slow(uint32_t address, unsigned width, bool sequential, uint32_t value);
    uint32_t fetch_slow(uint32_t address, unsigned width, bool sequential);
    // An opcode fetch from the ROM with the prefetch buffer: its cycles.
    // Inline: a fetch of the buffer's head while it runs, and the CPU's own
    // S fetch of the head before it starts, away from 128 KiB boundaries;
    // the rest in rom_fetch_slow().
    [[gnu::always_inline]] unsigned rom_fetch(uint32_t address, unsigned width, uint32_t request) {
        auto& p = prefetch_;
        const uint32_t time = static_cast<uint32_t>(deadline) - static_cast<uint32_t>(left);
        // Not in the last 16 bytes before a 128 KiB boundary nor at one.
        if (p.head == address && !p.paused && ((address + 0x10) & 0x1ffff) > 0x10) {
            const uint32_t halves = width == 2 ? 2 : 1;
            const auto since = static_cast<int32_t>(time - p.start);
            if (p.running || since > 0) {
                if (static_cast<int32_t>(time - p.base) < 8 * static_cast<int32_t>(p.cycles)) {
                    const uint32_t arrival = p.base + halves * p.cycles;
                    const auto wait = static_cast<int32_t>(arrival - time);
                    p.running = true;
                    p.stalled = wait > 0 && halves == 2;
                    p.base = p.start = arrival;
                    p.head = address + 2 * halves;
                    return wait > 0 ? static_cast<unsigned>(wait) : 1;
                }
            } else if ((request & kSequential) && gamepak_next_ == address) {
                const unsigned region = address >> 24, cost = cycles_s[region][width];
                p.head = gamepak_next_ = address + 2 * halves;
                p.cycles = cycles_s[region][1];
                p.stalled = false;
                p.base = p.start = time + cost;
                return cost;
            }
        }
        return rom_fetch_slow(address, width, request);
    }
    unsigned rom_fetch_slow(uint32_t address, unsigned width, uint32_t request);
    // Reads without a bus cycle or side effect, for the JIT's compiler and
    // diagnosis: what a fetch would see.
    uint32_t peek(uint32_t address, unsigned width);
    // A write to a line of marked JIT code in IWRAM or EWRAM, at `offset`
    // in `base`: code_written() when it changes what is there, so that a
    // copy of the same code keeps its blocks.
    void store_code(uint8_t* base, uint32_t offset, unsigned width, uint32_t value, uint32_t address);
    // The BIOS latch once the CPU fetched at `address` in the BIOS.
    void latch_bios(uint32_t address) { bios_latch = word_at(bios, address & 0x3ffc); }

    // --- Interrupts -------------------------------------------------------
    uint16_t ie() const { return io16(0x200); }
    uint16_t if_() const { return irq_flags_; }
    bool ime() const { return io16(0x208) & 1; }
    void request_irq(uint16_t bits);
    // Whether IE & IF wakes the CPU from HALT.
    bool wake_pending() const { return (ie() & irq_flags_ & 0x3fff) != 0; }
    // HALT: runs the devices until IE & IF wakes the CPU or the target
    // comes.
    void run_halt();


    // --- I/O ----------------------------------------------------------------
    uint16_t io16(uint32_t offset) const {
        uint16_t value;
        __builtin_memcpy(&value, io_.data() + offset, 2);
        return value;
    }

    // --- Diagnosis and tests --------------------------------------------------
    // --- Native code (Arm7Jit) ---------------------------------------------
    // Nonzero while native code runs, which does not keep cpu->pipeline:
    // the address of the opcode in pipeline[1] | 1, from which open_bus()
    // reads both opcodes again.
    uint32_t lazy_pipeline = 0;
    // Counts the changes of the wait states and the prefetch's enable,
    // which native code embeds. Each one also stops the CPU's run at the
    // next instruction boundary, so that no block compiled for the old
    // values runs on.
    uint32_t waits_generation = 0;
    uint32_t memory_control() const { return memory_control_; }

    // --- Wait loops (arm7_spin.h) ------------------------------------------
    // Moves whenever the deadline is set again, a read or fetch leaves the
    // fast paths of RAM, or the interpreter writes R15 other than by B or a
    // conditional branch, so that it sees whether anything but a wait loop's
    // own passes happened since its last pass.
    uint32_t spin_epoch = 0;
    // At a backward branch the interpreter took from `branch` to `head`:
    // skips the passes of a wait loop that would end with cycles left, see
    // arm7_mapped.cpp. Here only what every branch does: a new head, branch
    // or epoch starts over. The verdict holds while the loop stays the same
    // when it is no, which only costs skips if the code changes, or when
    // the code is in the BIOS or the ROM, where it cannot.
    [[gnu::always_inline]] void spin_pass(uint32_t head, uint32_t branch, bool thumb) {
        if (spin_.head != head || spin_.branch != branch || spin_.epoch != spin_epoch || spin_.thumb != thumb)
            [[unlikely]] {
            const unsigned region = head >> 24;
            if (spin_.head != head || spin_.branch != branch || spin_.thumb != thumb ||
                (spin_.verdict != 2 && region != 0 && (region < 8 || region > 13)))
                spin_.verdict = 0;
            spin_.head = head;
            spin_.branch = branch;
            spin_.thumb = thumb;
            spin_.epoch = spin_epoch;
            spin_.kept = false;
            return;
        }
        if (spin_.verdict != 2) spin_loop(head, thumb);
    }
    void spin_loop(uint32_t head, bool thumb);
    // Lends the prefetch buffer to native code whose next code fetch is
    // `next` in the ROM, at the rate of the current wait states, away from
    // 128 KiB boundaries (Prefetch: base = start): when it holds the
    // halfword at `next` and is running toward it, has yet to start or is
    // stopped full; or when it is stopped and empty, and the gamepak's
    // next sequential address is not `next`, so that the fetch there is N
    // with no wait. Returns the lent state, kLent* below, or 0: `offset`
    // is base - deadline. Memory then holds a stopped buffer, so that
    // native code stops it by keeping it. It must come back, through
    // return_prefetch() with the native buffer's head, before any other
    // code here runs.
    static constexpr uint32_t kLent = 1, kLentRunning = 2, kLentStalled = 4;
    // Stopped and empty: the next fetch is N. Drained: native code emptied
    // it, and the gamepak's next access is N too.
    static constexpr uint32_t kLentEmpty = 8, kLentDrained = 16;
    // Halfwords held while stopped full, from this bit.
    static constexpr unsigned kLentPausedShift = 8;
    uint32_t lend_prefetch(uint32_t next, int32_t& offset);
    void return_prefetch(uint32_t head, int32_t offset, uint32_t state);

    // Frames completed, counted at the start of VBlank.
    uint64_t frames = 0;
    std::span<const uint8_t> ewram_bytes() const { return {ewram, 256 * 1024}; }
    std::span<const uint8_t> vram_bytes() const { return {vram, 96 * 1024}; }
    // The I/O registers as reads see them at now(), for digests.
    void io_snapshot(std::span<uint8_t, 0x400> out);
    // Cycles the CPU waited in HALT.
    uint64_t halt_cycles = 0;
    // What is under way at an instruction boundary, for the save points of
    // the tests (tests/device/gba_rom_test.cpp): a repeating HBlank, video
    // or sound FIFO DMA that has moved its source from the register's, a
    // FIFO holding samples (brought up to now first, as a read of the sound
    // registers does), the prefetch buffer holding or fetching halfwords, a
    // Flash or EEPROM command, an RTC transfer.
    bool repeat_dma_moved() const {
        for (unsigned c = 0; c < 4; ++c) {
            const uint16_t control = io16(0xb0 + 12 * c + 10);
            const uint32_t source = io16(0xb0 + 12 * c) | uint32_t{io16(0xb0 + 12 * c + 2)} << 16;
            if ((control & 0x8200) == 0x8200 && (control >> 12 & 3) >= 2 && dma_[c].source != (source & 0x0ffffffe))
                return true;
        }
        return false;
    }
    bool fifo_queued() {
        catch_up_sound(clock.now);
        return fifo_[0].size || fifo_[1].size;
    }
    bool prefetch_held() const { return prefetch_.head != 0; }
    bool backup_busy() const { return backup.busy(clock.now); }
    bool rtc_busy() const { return rtc.busy(); }

    static uint32_t word_at(const uint8_t* base, uint32_t offset) {
        uint32_t value;
        __builtin_memcpy(&value, base + offset, 4);
        return value;
    }
    static uint16_t half_at(const uint8_t* base, uint32_t offset) {
        uint16_t value;
        __builtin_memcpy(&value, base + offset, 2);
        return value;
    }
    // A value of a width at an offset, aligned to the width.
    static uint32_t load(const uint8_t* base, uint32_t offset, unsigned width) {
        return width == 2 ? word_at(base, offset & ~3u) : width == 1 ? half_at(base, offset & ~1u) : base[offset];
    }
    static void store(uint8_t* base, uint32_t offset, unsigned width, uint32_t value) {
        if (width == 2) {
            __builtin_memcpy(base + (offset & ~3u), &value, 4);
        } else if (width == 1) {
            const auto half = static_cast<uint16_t>(value);
            __builtin_memcpy(base + (offset & ~1u), &half, 2);
        } else {
            base[offset] = static_cast<uint8_t>(value);
        }
    }

private:
    friend class Ppu;
    friend struct SnapshotAccess;
    // Its gateway reads the prefetch buffer's state and the gamepak's
    // address counter, see generate_gateway().
    friend class Arm7Jit;
    // The interpreter's bus writes palette RAM, VRAM and OAM, and reads the
    // ROM with the prefetch buffer stopped, itself.
    friend struct MappedBus;

    // --- Regions --------------------------------------------------------------
    // Reads and writes at clock.now without cycles, by the CPU (r15 its
    // R15) or DMA (r15 = kDmaAccess).
    static constexpr uint32_t kDmaAccess = 1;
    uint32_t read_region(uint32_t address, unsigned width, uint32_t r15);
    void write_region(uint32_t address, unsigned width, uint32_t value);
    void write_video(uint32_t address, unsigned width, uint32_t value);
    // VRAM's offset for an address: 96 KiB repeated every 128 KiB, the last
    // 32 KiB a mirror of the 32 KiB before. In the bitmap modes the first
    // half of that mirror is not mapped: 18000h, which reads 0 and ignores
    // writes (nba-hw-test ppu/vram-mirror).
    uint32_t vram_offset(uint32_t address) const {
        const uint32_t offset = address & 0x1ffff;
        if (offset < 0x18000) return offset;
        return offset < 0x1c000 && vram_obj_start != 0x10000 ? 0x18000 : offset - 0x8000;
    }
    // The CPU's reads where nothing drives the bus, of the width and at the
    // alignment of `address`: the opcodes it fetched last (GBATEK "GBA
    // Unpredictable Things"), or what a DMA moved since.
    uint32_t open_bus(uint32_t address, unsigned width, uint32_t r15);
    // Cycles of a gamepak access outside the prefetch buffer: N, or S when
    // sequential to the gamepak's last access and not at a 128 KiB
    // boundary (GBATEK "GBA System Control", nba-hw-test bus/128kb-boundary).
    unsigned gamepak_cycles(uint32_t address, unsigned width, bool sequential) {
        const unsigned region = address >> 24;
        const bool s = sequential && address == gamepak_next_ && (address & 0x1ffff);
        gamepak_next_ = (address & ~((1u << width) - 1)) + (1u << width);
        return (s ? cycles_s : cycles_n)[region][width];
    }
    uint32_t rom_value(uint32_t address, unsigned width);
    void update_waits();

    // --- Prefetch buffer (GBATEK "GBA GamePak Prefetch"), see rom_fetch() -----
    // Its times are the low 32 bits of Tick: they are only compared with
    // now() and with each other, over spans that rearm() keeps short.
    struct Prefetch {
        uint32_t head = 0;     // Address of the halfword the CPU takes next; 0: empty and stopped.
        uint32_t base = 0;     // Halfword i from head arrives at base + (i + 1) * cycles.
        uint32_t start = 0;    // When the fetch of the head began.
        uint8_t paused = 0;    // Halfwords held while stopped full or at a boundary.
        uint8_t cycles = 0;    // 1 + S of the head's region.
        bool running = false;  // It has fetched since the CPU's last own access.
        bool stalled = false;  // The CPU's last fetch waited for it.
    } prefetch_;
    // The address after the gamepak's last access, see gamepak_cycles().
    uint32_t gamepak_next_ = 0;
    // The interpreter's last pass through a backward branch, see
    // spin_loop(): where to and from, the epoch and cycles left then, the
    // cycles of the pass before, whether a wait loop starts there and ends
    // at that branch (0 not known, 1 yes, 2 no) for Thumb or ARM code, and
    // the CPU's state then.
    struct Spin {
        uint32_t head = 0, branch = 0, epoch = 0;
        int32_t left = 0, cycles = 0;
        uint8_t verdict = 0;
        bool thumb = false, kept = false;
        std::array<uint32_t, 25> state{};
    } spin_;
    // A gamepak access by the CPU's data or the DMA stops the prefetch.
    void stop_prefetch();
    unsigned prefetch_wait(Tick time) const;
    void age_prefetch();

    // --- I/O state -------------------------------------------------------------
    alignas(4) std::array<uint8_t, 0x400> io_{};
    uint16_t irq_flags_ = 0;
    void mirror_if() { __builtin_memcpy(io_.data() + 0x202, &irq_flags_, 2); }
    // Flags set in the cycle of irq_recent_time_: reads see them from the
    // next cycle on, writes acknowledge them already (alyosha irq/IF).
    uint16_t irq_recent_ = 0;
    Tick irq_recent_time_ = kNever;
    uint16_t buttons_ = 0x3ff;  // KEYINPUT: 0 = pressed.
    // 4000800h. Its bits that disable or mirror the work RAM are kept for
    // reads only: GBATEK describes them, and no program of the test
    // suites or a game depends on them.
    uint32_t memory_control_ = 0x0d000020;
    uint32_t io_read(uint32_t address, unsigned width, uint32_t r15);
    void io_write(uint32_t address, unsigned width, uint32_t value);
    uint16_t io_read16(uint32_t offset);
    void init_io_reads();
    void io_write16(uint32_t offset, uint16_t value, uint16_t mask);
    void write_memory_control(uint32_t value);
    void check_keypad();

    // --- Interrupt line, see gba_irq.cpp -----------------------------------------
    // IE & IF, IME: their changes, kept long enough for the line's delay.
    struct IrqHistory {
        static constexpr unsigned kSize = 8;
        std::array<Tick, kSize> time{};
        uint8_t values = 0;  // Bit i: the value from time[i] on.
        uint8_t count = 0;
        bool before = false;  // The value before time[0].
        void reset(bool value) { count = 0, before = value; }
        void push(Tick when, bool value);
        bool at(Tick when) const;
        bool last() const { return count ? (values >> (count - 1)) & 1 : before; }
        Tick changed() const { return count ? time[count - 1] : 0; }
    };
    IrqHistory irq_request_, irq_master_;  // IE & IF, IME.
    Tick irq_request_rise_ = 0;            // When IE & IF last went up.
    // While IE & IF or IME change, the accesses from the line's time
    // irq_next_ on take the slow path to clock the CPU's stage, see
    // gba_irq.cpp.
    bool irq_watch_ = false;
    Tick irq_next_ = 0;
    Tick halt_entry_ = kNever;             // When the CPU halted, or kNever.
    bool halt_in_access_ = false;          // A DMA wrote HALTCNT.
    Tick halt_tail_ = 0;                   // Its cycles after the wake.
    // Waits in HALT from clock.now; false if the target came first.
    bool wait_halt(bool until_target);
    // The line's own time: the system's, less the cycles the CPU waited
    // for a DMA, during which it stands still (from irq_stall_from_).
    Tick irq_stalled_ = 0;
    Tick irq_stall_from_ = kNever;
    Tick irq_time(Tick time) const { return std::min(time, irq_stall_from_) - irq_stalled_; }
    void update_irq();
    void irq_edge(Tick time);
    void reset_irq();
    // Whether the DMA reads an I/O address, or keeps its latch.
    bool io_dma_readable(uint32_t address) const;

    // --- Timers (GBATEK "GBA Timers"), see gba_timer.cpp -----------------------
    struct Timer {
        uint16_t reload = 0, control = 0;
        uint16_t counter = 0;    // At `since`, or held while stopped.
        uint16_t held = 0;       // What reads see before `since`.
        Tick since = kNever;     // Counting resumed from `counter`; kNever when stopped.
        Tick end = kNever;       // Counting stops here (a stop's last cycle).
        Tick first = kNever;     // The next overflow from `since`, or kNever.
        Tick period = 0;         // Cycles between overflows after it.
        Tick handled = 0;        // Overflows up to here were handled.
    };
    std::array<Timer, 4> timers_{};
    // Interrupt requests of overflows, raised the cycle after them.
    uint16_t timer_irq_ = 0;
    Tick timer_irq_time_ = kNever;
    // Phase of the prescaler, which counts from power-on: the timers tick
    // when now() + phase crosses a multiple of their prescaler.
    Tick prescaler_phase_ = 0;
    uint16_t timer_counter(unsigned index, Tick time) const;
    void plan_timers(unsigned from);
    void write_timer(unsigned index, uint16_t reload, uint16_t control, bool control_written);
    void schedule_timers();
    Tick fifo_due(unsigned index) const;
    bool fifo_fed(unsigned which) const;
    // No timer that feeds a FIFO overflows before this since catch_up_fifos()
    // last counted; 0 when timers or the FIFOs' timers changed since.
    Tick fifo_quiet_until_ = 0;
    static void timer_event(void* context);
    void handle_overflows(Tick time);
    void raise_timer_irq(uint16_t requests, Tick time);
    Tick next_overflow(unsigned index, Tick after) const;
    uint64_t overflows(unsigned index, Tick from, Tick to) const;

    // --- DMA (GBATEK "GBA DMA Transfers"), see gba_dma.cpp ----------------------
    struct Dma {
        uint32_t source = 0, destination = 0;  // Internal addresses.
        uint32_t count = 0;                    // Units left.
        uint16_t control = 0;
        Tick start = kNever;                   // A start after its delay.
        uint32_t latch = 0;                    // The channel's data latch.
        bool gamepak_sequential = false;       // The next gamepak access is S.
        bool eeprom = false;                   // To or from the EEPROM.
        bool paused = false;                   // A channel above took over before this unit's write.
    };
    std::array<Dma, 4> dma_{};
    uint32_t dma_latch_ = 0;     // What the last DMA left on the bus.
    uint8_t dma_pending_ = 0;    // Channels whose transfer is due.
    bool dma_running_ = false;
    // A DMA found due at an internal cycle starts here; the CPU's next bus
    // access runs it, see idle_slow().
    Tick dma_deferred_ = kNever;
    // A DMA moved a value since the CPU's last opcode fetch: its reads of
    // the open bus see it.
    bool dma_bus_ = false;
    void write_dma(unsigned channel, uint16_t value, uint16_t old);
    // A channel is enabled with the start timing (3: DMA3's video capture).
    bool dma_waiting(unsigned timing) const;
    // Video capture starts when VCOUNT becomes 2 with DMA3 armed for it,
    // repeats each line and ends at line 162 (GBATEK "GBA DMA Transfers");
    // armed later, it waits for the next frame's line 2 (nba-hw-test
    // ppu/status-irq-dma).
    bool capture_ = false;
    void stop_capture();
    void trigger_dma(unsigned timing, unsigned channels = 15);
    void schedule_dma();
    static void dma_event(void* context);
    void run_dma(Tick access);
    void transfer(unsigned channel);

    // --- Serial (GBATEK "SIO Normal Mode"): no partner, internal clock ----
    void write_serial_control(uint16_t value);
    static void serial_event(void* context);

    // --- Sound registers and the Direct Sound FIFOs, see gba_sound.cpp -------
    struct Fifo {
        std::array<uint8_t, 32> data{};
        uint8_t read = 0, size = 0;
        int8_t sample = 0;  // The sample it plays, the last one it took.
    };
    std::array<Fifo, 2> fifo_{};
    void write_fifo(unsigned which, unsigned bytes, uint32_t value);
    // `count` overflows of a timer that feeds a FIFO, the first at `first`
    // and the others `period` apart.
    void fifo_overflow(unsigned timer, uint64_t count, Tick first, Tick period);
    // The FIFOs a timer feeds, bit 0 for A and bit 1 for B.
    unsigned fifos_of(unsigned timer) const;
    // Those of `fifos` that hold a sample.
    unsigned fifos_holding(unsigned fifos) const;
    // One overflow at `when` of a timer that feeds `fifos`, which hold a
    // sample each: those that still hold one after it.
    unsigned fifo_samples(unsigned fifos, Tick when);
    // The DMA requests of `fifos` after their timer's overflows.
    void fifo_requests(unsigned fifos);
    void fifo_resume();
    void write_sound(uint32_t offset, uint16_t value, uint16_t mask);
    // The PSG's sequencer (512 Hz): its next step and the step's number, the
    // channels on and as the mixer last learned of them, the length timers
    // and channel 1's sweep (GBATEK "GBA Sound Channel 1" to 4, which keep
    // the CGB's units).
    struct Psg {
        Tick edge = kNever;
        uint8_t step = 0, on = 0, reported = 0;
        std::array<uint16_t, 4> length{};
        uint16_t shadow = 0;
        uint8_t sweep_timer = 8;
        bool sweep_enabled = false, negated = false;
    } psg_;
    // The wave RAM bank channel 3 plays; I/O 90h-9Fh hold the other, which
    // the CPU reaches (GBATEK "GBA Sound Channel 3").
    std::array<uint8_t, 16> wave_played_{};
    std::unique_ptr<Mixer> mixer_;
    sound::Batch* batch_ = nullptr;  // Open batch, filled in place.
    Tick sound_end_ = 0;             // End of the last batch handed over.
    uint32_t audio_rate_ = 0;        // Output settings, sent again on reset.
    int32_t audio_speed_ = 0;
    void reset_sound();
    bool sound_on() const { return io_[0x84] & 0x80; }
    // Brings the sequencer and the FIFOs to `to`, in time order, so that
    // the batch takes the steps and samples as they happened.
    void catch_up_sound(Tick to);
    void catch_up_fifos(Tick to);
    void write_psg(unsigned offset, uint8_t value);
    void sequence(Tick when);
    void clock_sweep(Tick when);
    unsigned sweep_target();
    void turn_off(unsigned channel);
    void report(Tick when);
    void trigger(unsigned channel);
    uint16_t sound_status();
    void append(Tick when, sound::Event event, uint8_t address = 0, uint8_t value = 0, uint8_t extra = 0);
    void append_setting(sound::Event event, uint32_t value);
    void open_batch();
    void submit_batch(Tick end);

    // --- Cartridge GPIO, see gba_rtc.cpp ----------------------------------------
    struct Gpio {
        uint8_t data = 0, direction = 0, control = 0;
    } gpio_;
    // Page 0 of the ROM with the registers over 80000C4h-80000C9h, read
    // through the cache while they are readable; nullptr without the port.
    uint8_t* gpio_page_ = nullptr;
    bool load_gpio();
    void write_gpio(uint32_t address, unsigned width, uint32_t value);
    void write_gpio16(uint32_t offset, uint16_t value);
    void update_gpio();

    std::unique_ptr<gb::Offload> offload_;
};

}  // namespace gbx::gba
