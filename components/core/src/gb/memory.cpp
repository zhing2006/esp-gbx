#include "gbx/gb/memory.h"

#include <algorithm>
#include <cstring>
#include <new>

#include "gbx/platform.h"
#include "offload.h"

namespace gbx::gb {
namespace {
// Pan Docs, Audio Registers. Ones are unreadable bits, not stored state.
constexpr uint8_t sound_masks[]{
    0x80,0x3f,0x00,0xff,0xbf,0xff,0x3f,0x00,
    0xff,0xbf,0x7f,0xff,0x9f,0xff,0xbf,0xff,
    0xff,0x00,0x00,0xbf,0x00,0x00,0x70};
// Every register except SB, DIV, TIMA and STAT reads as io[address] | mask.
// Unmapped registers read 0xff.
constexpr auto kIoReadMasks = [] {
    std::array<uint8_t, 128> masks{};
    masks.fill(0xff);
    masks[0] = 0xc0; masks[2] = 0x7e; masks[6] = 0; masks[7] = 0xf8; masks[0x0f] = 0xe0;
    for (unsigned a = 0x10; a <= 0x26; ++a) masks[a] = sound_masks[a - 0x10];
    for (unsigned a = 0x30; a <= 0x4b; ++a) masks[a] = 0;
    return masks;
}();
// A CGB's registers, both modes (Pan Docs, CGB Registers; mooneye
// misc/bits/unused_hwio-C, which runs in compatibility mode): VBK, BGPI,
// OBPI and OPRI read their unused bits as 1, and in compatibility mode
// keep the values the boot ROM left. FF72, FF73 and FF75 bits 4-6 work in
// both modes.
constexpr auto kCompatMasks = [] {
    auto masks = kIoReadMasks;
    masks[0x4f] = 0xfe; masks[0x68] = masks[0x6a] = 0x40; masks[0x6c] = 0xfe;
    masks[0x72] = masks[0x73] = 0; masks[0x75] = 0x8f;
    return masks;
}();
// CGB mode adds SC's clock speed bit, KEY1, RP, SVBK and FF74. HDMA1-4 are
// write-only.
constexpr auto kCgbMasks = [] {
    auto masks = kCompatMasks;
    masks[2] = 0x7c; masks[0x4d] = 0x7e; masks[0x56] = 0x3e; masks[0x70] = 0xf8; masks[0x74] = 0;
    return masks;
}();
// How read_io() reads each register. A table, so that the common plain
// read takes one branch: sc and cgb are the registers whose value or mask
// differs on a CGB, which read_cgb() reads there.
enum class IoRead : uint8_t { plain, serial, sc, div, tima, sound, stat, ly, cgb };
constexpr auto kIoReads = [] {
    std::array<IoRead, 128> kinds{};
    kinds[1] = IoRead::serial; kinds[2] = IoRead::sc; kinds[4] = IoRead::div; kinds[5] = IoRead::tima;
    kinds[0x26] = IoRead::sound;
    for (unsigned a = 0x30; a < 0x40; ++a) kinds[a] = IoRead::sound;
    kinds[0x41] = IoRead::stat; kinds[0x44] = IoRead::ly;
    for (unsigned a = 0; a < 128; ++a)
        if (a != 2 && (kCompatMasks[a] != kIoReadMasks[a] || kCgbMasks[a] != kIoReadMasks[a])) kinds[a] = IoRead::cgb;
    for (unsigned a : {0x55, 0x69, 0x6b, 0x76, 0x77}) kinds[a] = IoRead::cgb;
    return kinds;
}();
static_assert([] {
    for (unsigned a = 0; a < 128; ++a)
        if (kIoReads[a] == IoRead::cgb && a < 0x4c) return false;
    return true;
}(), "A register before FF4C differs on a CGB; give it its own kind");
uint16_t canonical(uint16_t address) {
    return address >= 0xe000 && address < 0xfe00 ? address - 0x2000 : address;
}
}

Memory::Memory() : io_masks_(kIoReadMasks.data()) {}
Memory::~Memory() = default;
void Memory::ReleaseBanks::operator()(Banks* banks) const {
    banks->~Banks();
    platform::free_external(banks);
}
bool Memory::initialize() {
    if (!offload_) offload_.reset(new (std::nothrow) Offload);
    return offload_ && offload_->start() && ppu.initialize(*offload_) && apu.initialize(*offload_);
}
void Memory::set_worker_polling(bool polling) {
    if (offload_) offload_->set_polling(polling);
}
bool Memory::load(std::span<const uint8_t> rom, Mode mode) {
    // The CGB banks go to PSRAM, like cartridge RAM: taken from internal RAM
    // before the JIT starts, their 32 KiB could leave its code pool no room.
    if (mode != Mode::dmg && !banks_) {
        void* block = platform::allocate_external(sizeof(Banks));
        if (!block) return false;
        banks_.reset(new (block) Banks);
    }
    if (!cartridge.load(rom)) return false;
    mode_ = mode;
    clock.reset();  // A new cartridge's clock starts from zero at time zero.
    reset();
    return true;
}
void Memory::reset() {
    // The cartridge clock has its own battery: it keeps its state.
    const auto rtc = cartridge.rtc().state(clock.now);
    clock.reset();
    cartridge.reset(rtc, clock.now);
    wram.fill(0); vram.fill(0); oam.fill(0); io.fill(0); high.fill(0); unseen_interrupts = 0;
    if (banks_) { banks_->wram.fill(0); banks_->vram.fill(0); }
    io_masks_ = mode_ == Mode::dmg ? kIoReadMasks.data() : mode_ == Mode::cgb ? kCgbMasks.data() : kCompatMasks.data();
    wram_d000_ = wram.data() + 4096; vram_cpu_ = vram.data();
    wram_bank_ = mode_ == Mode::dmg ? 0 : 1; vram_bank_ = 0;
    hdma_source_ = hdma_destination_ = 0; hdma_length_ = 0x7f; hdma_active_ = false;
    cpu_waiting = false; wake = kNever; serial_shift_ = 10; mcycle_ = 8;
    // DMG boot ROM hand-off: expand the cartridge header's 4x4 logo tiles
    // to 8x8 tiles in bitplane zero, then leave the logo and registered mark
    // in the BG map. Source: neviksti's hardware boot ROM disassembly,
    // https://gbdev.gg8.se/wiki/articles/Gameboy_Bootstrap_ROM (0021..0053).
    // What the CGB boot ROM leaves in VRAM is not documented, so a CGB
    // starts with it clear.
    const auto rom = cartridge.rom();
    if (mode_ == Mode::dmg && rom.size() >= 0x134) {
        unsigned destination = 0x10;
        for (unsigned source = 0x104; source < 0x134; ++source) {
            for (int shift : {4, 0}) {
                const unsigned nibble = (rom[source] >> shift) & 15;
                uint8_t row = 0;
                for (unsigned bit = 0; bit < 4; ++bit)
                    if (nibble & (1u << bit)) row |= 3u << (bit * 2);
                vram[destination] = vram[destination + 2] = row;
                destination += 4;
            }
        }
        constexpr uint8_t registered_mark[]{0x3c, 0x42, 0xb9, 0xa5, 0xb9, 0xa5, 0x42, 0x3c};
        for (unsigned row = 0; row < 8; ++row) vram[0x190 + row * 2] = registered_mark[row];
        for (unsigned tile = 0; tile < 12; ++tile) {
            vram[0x1904 + tile] = static_cast<uint8_t>(tile + 1);
            vram[0x1924 + tile] = static_cast<uint8_t>(tile + 13);
        }
        vram[0x1910] = 25;
    }
    // Uninitialized RAM/OBJ palettes are deterministic for reproducible runs;
    // zero is NOT claimed as a guaranteed hardware power-up value (Pan Docs).
    ++mapping_generation;
    dma_restart_ = kNever; dma_running_ = false; dma_source_ = 0; dma_index_ = 0; dma_value_ = 0xff;
    frames = 0; instruction_pc = 0x100;
    ++interrupt_generation;
    stopped_ = false; buttons_ = 0;
    timer_edge_ = timer_reload_ = serial_edge_ = kNever; timer_reload_end_ = 0;
    serial_bits_ = serial_byte_ = 0; serial_size_ = 0; serial_truncated_ = false;
    io[0] = 0xcf; io[2] = 0x7e; io[7] = 0xf8; io[0x0f] = 0xe1;
    io[0x10] = 0x80; io[0x11] = 0xbf; io[0x12] = 0xf3;
    io[0x14] = 0xbf; io[0x16] = 0x3f; io[0x19] = 0xbf;
    io[0x1a] = 0x7f; io[0x1c] = 0x9f; io[0x1e] = 0xbf;
    io[0x23] = 0xbf; io[0x24] = 0x77; io[0x25] = 0xf3; io[0x26] = 0xf1;  // CH1 still on.
    io[0x40] = 0x91; io[0x41] = 0x85; io[0x46] = 0xff; io[0x47] = 0xfc;
    // boot_div-dmgABCmgb reads at M-cycle indices 13,77,140,204,268,333.
    // ABCC+3+4*m yields AC,AD,AD,AE,AF,B1 at the T4 sampling edge.
    // boot_sclk_align additionally fixes serial completion at M-cycle 1037.
    div_initial_ = 0xabcc; div_origin_ = 0;
    if (color()) reset_cgb();
    ppu.reset(*this);
    apu.reset(*this);
    io[0x26] &= 0x80;  // The APU keeps the channel bits.
    map();
}
// CPU CGB E after its boot ROM, from Pan Docs, Power Up Sequence: Console
// state after boot ROM hand-off, and mooneye misc/boot_hwio-C, which reads
// the registers in compatibility mode. Where the two differ, the test, run
// on hardware, wins: P1 reads FF and SC 7E there. Nothing documents the
// timing state, which depends on how long the boot ROM ran: the DIV counter
// comes from what misc/boot_div-cgbABCDE reads, and holds for cartridges
// like the mooneye ones, whose licensee is not Nintendo, in compatibility
// mode. CGB mode and the PPU's phase have no source and keep those of
// compatibility mode and of the DMG.
void Memory::reset_cgb() {
    io[0] = 0xff;     // Neither button group selected.
    io[0x46] = 0;     // DMA
    // The boot ROM writes the palettes with auto-increment, which leaves
    // BGPI and OBPI past the bytes written: all eight BG palettes in CGB
    // mode, where it also writes the first byte of OBJ palette 0 (Pan Docs,
    // LCD Color Palettes); palette 0 and OBJ palettes 0 and 1 in
    // compatibility mode, where boot_hwio-C reads C8 and D0.
    if (mode_ == Mode::cgb) { io[0x68] = 0x80; io[0x6a] = 0x81; }
    else { io[0x68] = 0x88; io[0x6a] = 0x90; io[0x6c] = 1; }
    // boot_div-cgbABCDE has 21 NOPs more than the DMG's boot_div-dmgABCmgb
    // before its first read, so it reads at M-cycle indices 34,98,161,225,
    // 289 and 354, and finds 27,28,28,29,2A,2C: with the phase of the DMG's
    // ABCC (see reset()), the counter starts at 2678.
    div_initial_ = 0x2678;
}
void Memory::tag_code(unsigned windows) {
    const auto set = [&](unsigned slot, unsigned count) {
        const uint32_t tag = uint32_t{code_bank(static_cast<uint16_t>(slot << 12))} << 16;
        for (unsigned k = 0; k < count; ++k) code_tags[slot + k] = tag;
    };
    if (windows & kRom0) set(0, 4);
    if (windows & kRom1) set(4, 4);
    if (windows & kCartRam) set(10, 2);
    if (windows & kWramBank) { set(13, 1); set(15, 1); }
}
void Memory::map(unsigned first, unsigned last) {
    if (first == 0 && last == 512) tag_code(kRom0 | kRom1 | kCartRam | kWramBank);
    if (trace_ && !trace_direct_) {  // Mode-change boundary, not an access.
        std::fill(pages.begin() + first, pages.begin() + last, MemoryPage{});
        return;
    }
    // Some games switch ROM banks many times a frame, so each bank's window
    // is looked up once and its pages are written in one pass. A running OAM
    // DMA blocks all of ROM or none.
    const unsigned rom_end = std::min(last, 0x8000u >> 7);
    for (unsigned p = first; p < rom_end;) {
        const unsigned slot = p >> 7, end = std::min(rom_end, (slot + 1) << 7);
        const auto* base = dma_running_ && dma_conflict(0) ? nullptr : cartridge.rom_window(slot);
        if (!base) {
            for (; p < end; ++p) pages[p] = {};
            continue;
        }
        // ROM is never written directly: only the read pointers change,
        // eight a step, written out, as the compiler keeps the loop rolled.
        base += (p & 127) * 128;
        for (; p + 8 <= end; p += 8, base += 8 * 128) {
            auto* page = &pages[p];
            page[0].read = base; page[1].read = base + 128; page[2].read = base + 256; page[3].read = base + 384;
            page[4].read = base + 512; page[5].read = base + 640; page[6].read = base + 768; page[7].read = base + 896;
        }
        for (; p < end; ++p, base += 128) pages[p].read = base;
    }
    const unsigned rest = std::max(first, rom_end);
    if (rest < last) std::fill(pages.begin() + rest, pages.begin() + last, MemoryPage{});
    for (unsigned p = rest; p < last; ++p) {
        const auto address = static_cast<uint16_t>(p * 128);
        uint8_t* writable = nullptr;
        if (dma_running_ && dma_conflict(address)) continue;
        if (address >= 0xa000 && address < 0xc000 && cartridge.ram_window())
            writable = cartridge.ram_window() + ((address - 0xa000) & (cartridge.ram_window_size() - 1));
        else if (address >= 0xc000 && address < 0xfe00) {
            const auto a = canonical(address);
            writable = a < 0xd000 ? wram.data() + (a - 0xc000) : wram_d000_ + (a - 0xd000);
        } else if (address == 0xff80) writable = high.data();
        if (writable) {
            pages[p].read = writable;
            if (!(address >= 0xa000 && address < 0xc000 && cartridge.nibble_ram())) pages[p].write = writable;
        }
    }
}
const uint8_t* Memory::backing(uint16_t address) const {
    if (address >= 0x8000 && address < 0xa000) return vram_cpu_ + (address - 0x8000);
    if (address >= 0xa000 && address < 0xc000) {
        const auto* window = cartridge.ram_window();
        return window ? window + ((address - 0xa000) & (cartridge.ram_window_size() - 1)) : nullptr;
    }
    if (address >= 0xc000 && address < 0xfe00) {
        const auto a = canonical(address);
        return a < 0xd000 ? wram.data() + (a - 0xc000) : wram_d000_ + (a - 0xd000);
    }
    return address >= 0xff80 ? high.data() + (address & 127) : nullptr;
}
void Memory::trace(TraceBuffer* sink, bool keep_direct) {
    trace_ = sink; trace_direct_ = keep_direct;
    read_slow_ = sink && !keep_direct ? traced_read : normal_read;
    write_slow_ = sink && !keep_direct ? traced_write : normal_write;
    map();
}
uint16_t Memory::divider() const {
    // Only the low 16 bits count, so the shift may take the low word.
    return static_cast<uint16_t>(div_initial_ +
                                 (stopped_ ? 0 : static_cast<uint32_t>(clock.now - div_origin_) >> tcycle_shift()));
}
unsigned Memory::timer_shift() const {
    constexpr unsigned shifts[]{10, 4, 6, 8};
    return shifts[io[7] & 3];
}
Tick Memory::next_div_edge(unsigned period) const {
    const unsigned shift = tcycle_shift();
    return clock.now + (Tick{period - (divider() & (period - 1))} << shift) -
           ((clock.now - div_origin_) & ((1u << shift) - 1));
}
uint8_t Memory::timer_value() const {
    const auto ticks = timer_edge_ <= clock.now ? ((clock.now - timer_edge_) >> (timer_shift() + tcycle_shift())) + 1 : 0;
    return static_cast<uint8_t>(io[5] + ticks);
}
void Memory::sync_timer() {
    if (timer_edge_ > clock.now) return;
    const auto shift = timer_shift() + tcycle_shift();
    const auto ticks = ((clock.now - timer_edge_) >> shift) + 1;
    const unsigned value = io[5] + static_cast<unsigned>(ticks);
    timer_edge_ += ticks << shift;
    io[5] = static_cast<uint8_t>(value);
    if (value >= 256) timer_reload_ = clock.now + mcycle_;
}
void Memory::tick_timer() {
    if (++io[5] == 0) {
        // rapid_toggle requires the IRQ before the following DEC BC. The
        // early reload must still protect TIMA and feed TMA writes through
        // during the following M-cycle (Pan Docs, Timer obscure behaviour).
        // This timestamp is the triggering write's access edge: the next
        // access is an M-cycle later, and only the one after is outside the
        // window.
        io[5] = io[6]; timer_reload_ = kNever;
        timer_reload_end_ = clock.now + 2 * mcycle_;
        request_interrupt(4);
    }
}
void Memory::schedule_timer() {
    if (timer_reload_ != kNever) clock.schedule(1, timer_reload_, timer_event, this);
    else if (timer_edge_ != kNever)
        clock.schedule(1, timer_edge_ + (Tick{(255u - io[5]) * timer_period()} << tcycle_shift()), timer_event, this);
    else clock.cancel(1);
}
void Memory::timer_event(void* context) {
    auto& m = *static_cast<Memory*>(context);
    m.sync_timer();
    if (m.timer_reload_ <= m.clock.now) {
        m.io[5] = m.io[6];
        m.timer_reload_ = kNever;
        m.timer_reload_end_ = m.clock.now + m.mcycle_;
        m.request_interrupt(4);
    }
    m.schedule_timer();
}
unsigned Memory::elapsed_serial_bits() const {
    return serial_edge_ <= clock.now ?
        static_cast<unsigned>(std::min<Tick>(((clock.now - serial_edge_) >> serial_shift_) + 1, serial_bits_)) : 0;
}
uint8_t Memory::serial_value() const {
    const auto bits = elapsed_serial_bits();
    return static_cast<uint8_t>((io[1] << bits) | ((1u << bits) - 1));
}
void Memory::sync_serial() {
    const auto bits = elapsed_serial_bits();
    if (!bits) return;
    serial_byte_ = static_cast<uint8_t>((serial_byte_ << bits) | (io[1] >> (8 - bits)));
    io[1] = serial_value();
    serial_bits_ = static_cast<uint8_t>(serial_bits_ - bits);
    serial_edge_ += Tick{bits} << serial_shift_;
    if (!serial_bits_) {
        serial_edge_ = kNever;
        io[2] &= ~0x80;
        request_interrupt(8);
        if (serial_size_ < serial_output_.size()) serial_output_[serial_size_++] = serial_byte_;
        else serial_truncated_ = true;
    }
}
void Memory::schedule_serial() {
    if (serial_edge_ != kNever)
        clock.schedule(2, serial_edge_ + (Tick{serial_bits_ - 1u} << serial_shift_), serial_event, this);
    else clock.cancel(2);
}
void Memory::serial_event(void* context) {
    auto& m = *static_cast<Memory*>(context);
    m.sync_serial(); m.schedule_serial();
}
void Memory::reset_divider() {
    sync_timer(); sync_serial();
    reset_divider_at(divider());
}
// DIV restarts now. Bits of `old`, the counter before, that were high fall
// and clock the devices DIV drives.
void Memory::reset_divider_at(uint16_t old) {
    if ((io[7] & 4) && (old & (timer_period() >> 1))) tick_timer();
    // DIV-APU follows DIV's bit 4, or bit 5 at double speed, so that it
    // keeps its rate (Pan Docs, DIV-APU).
    apu.restart_divider(old & (double_speed() ? 0x2000 : 0x1000), double_speed());
    // The internal serial clock is the shared divider's bit 8, or bit 3 with
    // the CGB's fast clock, not a timer restarted by SC. Resetting a high
    // clock creates its falling edge.
    const unsigned serial_period = 1u << (serial_shift_ - tcycle_shift());
    div_initial_ = 0; div_origin_ = clock.now;
    timer_edge_ = io[7] & 4 ? next_div_edge(timer_period()) : kNever;
    schedule_timer();
    if (serial_bits_ && (io[2] & 1)) {
        if (old & (serial_period >> 1)) { serial_edge_ = clock.now; sync_serial(); }
        if (serial_bits_) serial_edge_ = next_div_edge(1u << (serial_shift_ - tcycle_shift()));
        schedule_serial();
    }
}
// Pan Docs, KEY1 and the STOP chart, say what the switch does, not when.
// AGE's tests, verified on CPU CGB E, fix the timing, here in ticks, which
// are cycles of the CGB's 8 MiHz master clock:
// - spsw-tima-cgbE: TIMA ticks for DIV's reset as if DIV's bits from 4 up
//   stopped 4 T-cycles after STOP's fetch and the reset came 3 T-cycles
//   later, at the old speed. Bit 3, the 262144 Hz timer's, runs to the reset.
// - spsw-div-cgbBCE: DIV counts through the pause from the reset. Back at
//   normal speed, it changes its rate an M-cycle after the reset.
// - lcd-align-ly-cgbE: the CPU runs again 0x20000 + 15 ticks after STOP's
//   fetch at double speed, 0x40000 + 7 at normal speed, so that every switch
//   moves the CPU's M-cycles against the PPU's dots.
// - spsw-ch2-lc-delay-cgbBCE: DIV-APU, see Apu::align().
void Memory::switch_speed(bool pause) {
    const Tick start = clock.now;
    const unsigned tcycle = mcycle_ >> 2;
    const bool to_double = !double_speed();
    clock.delay(4 * tcycle);
    sync_timer(); sync_serial();
    const auto frozen = divider();
    // The falling bits that stop: the timer's, unless it is bit 3,
    // DIV-APU's and the normal serial clock's.
    if ((io[7] & 7) != 5) { timer_edge_ = kNever; schedule_timer(); }
    apu.stop_divider();
    if (serial_bits_ && (io[2] & 1) && serial_shift_ - tcycle_shift() > 4) { serial_edge_ = kNever; schedule_serial(); }
    clock.delay(3 * tcycle);
    sync_timer(); sync_serial();
    reset_divider_at(static_cast<uint16_t>((frozen & ~15u) | (divider() & 15u)));
    if (!to_double) { clock.delay(mcycle_); sync_timer(); sync_serial(); }
    div_initial_ = divider(); div_origin_ = clock.now;
    const unsigned before = tcycle_shift();
    mcycle_ ^= 8 ^ 4;
    serial_shift_ = static_cast<uint8_t>(serial_shift_ - before + tcycle_shift());
    io[0x4d] = double_speed() ? 0x80 : 0;
    timer_edge_ = io[7] & 4 ? next_div_edge(timer_period()) : kNever;
    schedule_timer();
    if (serial_bits_ && (io[2] & 1)) {
        serial_edge_ = next_div_edge(1u << (serial_shift_ - tcycle_shift()));
        schedule_serial();
    }
    // DIV-APU follows bit 5 at double speed, see reset_divider_at().
    apu.retime_divider(next_div_edge(double_speed() ? 0x4000 : 0x2000), double_speed());
    if (pause) wake = start + (to_double ? 0x20000 + 15 : 0x40000 + 7);
}
void Memory::stop_clock() { reset_divider(); stopped_ = true; }
void Memory::update_joypad() {
    uint8_t low = 15;
    if (!(io[0] & 0x10)) low &= ~(buttons_ & 15);
    if (!(io[0] & 0x20)) low &= ~(buttons_ >> 4);
    if (io[0] & ~low & 15) request_interrupt(16);
    io[0] = (io[0] & 0xf0) | low;
}
void Memory::set_buttons(uint8_t buttons) { buttons_ = buttons; update_joypad(); }
int Memory::io_read_mask(uint8_t a) const {
    a &= 127;
    if (a == 1 || a == 4 || a == 5 || a == 0x26 || (a >= 0x30 && a < 0x40) || a == 0x41) return -1;
    if (color() && (a == 0x76 || a == 0x77 || (mode_ == Mode::cgb && (a == 0x55 || a == 0x69 || a == 0x6b))))
        return -1;
    return io_masks_[a];
}
uint8_t Memory::read_io(uint8_t a) {
    switch (kIoReads[a]) {
    case IoRead::plain: break;
    case IoRead::serial: return serial_value();
    case IoRead::sc: return io[a] | io_masks_[a];  // CGB mode reads SC's clock speed bit.
    case IoRead::div: return divider() >> 8;
    case IoRead::tima: return timer_value();
    case IoRead::sound: return apu.read(a);
    case IoRead::stat: return ppu.stat();
    case IoRead::ly: if (color()) return ppu.ly_cgb(); break;
    case IoRead::cgb: if (color()) return read_cgb(a); break;
    }
    return io[a] | kIoReadMasks[a];
}
uint8_t Memory::read_cgb(uint8_t a) {
    if (a == 0x76 || a == 0x77) return apu.read(a);
    if (mode_ == Mode::cgb) {
        // HDMA5: bit 7 clear while an HBlank transfer runs, with the blocks
        // left minus one; FF once a transfer ended (Pan Docs, HDMA5).
        if (a == 0x55) return static_cast<uint8_t>((hdma_active_ ? 0 : 0x80) | hdma_length_);
        if (a == 0x69 || a == 0x6b) return ppu.read_palette(a == 0x6b);
    }
    return io[a] | io_masks_[a];
}
void Memory::write_cgb(uint8_t a, uint8_t value) {
    if (a < 0x4c) return;
    const bool cgb = mode_ == Mode::cgb;
    switch (a) {
    case 0x72: case 0x73: io[a] = value; return;
    case 0x75: io[a] = value & 0x70; return;
    default: break;
    }
    if (!cgb) return;
    switch (a) {
    // KEY1: only the switch can be armed; STOP switches the speed, see
    // switch_speed().
    case 0x4d: io[a] = static_cast<uint8_t>((io[a] & 0x80) | (value & 1)); break;
    case 0x4f: select_vram(value & 1); break;
    // Source and VRAM destination, without their four low bits; the
    // destination keeps bits 12-4 (Pan Docs, HDMA1-4).
    case 0x51: hdma_source_ = static_cast<uint16_t>((hdma_source_ & 0x00f0) | value << 8); break;
    case 0x52: hdma_source_ = static_cast<uint16_t>((hdma_source_ & 0xff00) | (value & 0xf0)); break;
    case 0x53: hdma_destination_ = static_cast<uint16_t>((hdma_destination_ & 0x00f0) | (value & 0x1f) << 8); break;
    case 0x54: hdma_destination_ = static_cast<uint16_t>((hdma_destination_ & 0x1f00) | (value & 0xf0)); break;
    case 0x55: start_vram_dma(value); break;
    case 0x56: io[a] = value & 0xc1; break;  // RP without a partner: nothing is received.
    case 0x68: case 0x6a: io[a] = value & 0xbf; break;
    case 0x69: case 0x6b: ppu.write_palette(a == 0x6b, value); break;
    // OPRI reads back. The priority mode itself is the boot ROM's: Pan Docs
    // reports no effect of later writes.
    case 0x6c: io[a] = value & 1; break;
    case 0x70: select_wram(value & 7); break;
    case 0x74: io[a] = value; break;
    default: break;
    }
}
// SVBK: bank 0 selects bank 1 too (Pan Docs, SVBK).
void Memory::select_wram(uint8_t bank) {
    io[0x70] = bank;
    const uint8_t selected = bank ? bank : 1;
    if (selected == wram_bank_) return;
    wram_bank_ = selected;
    wram_d000_ = selected == 1 ? wram.data() + 4096 : banks_->wram.data() + (selected - 2) * 4096;
    if (dma_running_ || trace_) {
        map(0xd000 >> 7, 0xe000 >> 7); map(0xf000 >> 7, 0xfe00 >> 7);
    } else {
        // Games switch it many times a frame. Without OAM DMA or tracing,
        // map() would make every page of the bank and its echo direct.
        for (unsigned k = 0; k < 32; ++k) pages[(0xd000 >> 7) + k] = {wram_d000_ + k * 128, wram_d000_ + k * 128};
        for (unsigned k = 0; k < 28; ++k) pages[(0xf000 >> 7) + k] = {wram_d000_ + k * 128, wram_d000_ + k * 128};
    }
    tag_code(kWramBank);
    remapped |= kWramBank;
    ++mapping_generation;
}
// VRAM pages are never direct, so no mapping changes. Code in VRAM is fetched
// through peek() and checked at each fetch, see Memory::code_bank().
void Memory::select_vram(uint8_t bank) {
    io[0x4f] = bank;
    vram_bank_ = bank;
    vram_cpu_ = bank ? banks_->vram.data() : vram.data();
}
// HDMA5 (Pan Docs, LCD VRAM DMA Transfers). Every write loads the length
// bits: SameSuite dma/hdma_lcd_off and hdma_mode0 stop an HBlank transfer by
// writing 00 and then read 80, not the blocks that were left.
void Memory::start_vram_dma(uint8_t value) {
    hdma_length_ = value & 0x7f;
    if (hdma_active_ && !(value & 0x80)) {
        hdma_active_ = false;
        ppu.hdma_changed();
        return;
    }
    if (value & 0x80) {
        hdma_active_ = true;
        // With the LCD off, or in mode 0, no HBlank is about to start, and
        // the first block moves at once (SameSuite dma/hdma_lcd_off,
        // hdma_mode0).
        if (ppu.in_hblank()) hdma_block(false);
        ppu.hdma_changed();
        return;
    }
    // General-purpose DMA moves two bytes an M-cycle while the CPU waits
    // (Pan Docs, Transfer Timings). The devices run on between them, so that
    // the PPU sees each byte when it arrives.
    for (unsigned n = 0; n <= (value & 0x7fu); ++n)
        if (!copy_block(true)) break;
    hdma_length_ = 0x7f;
}
void Memory::hdma_block(bool event) {
    // HALT and STOP hold the transfer until the CPU runs again (Pan Docs,
    // HBlank DMA).
    if (!hdma_active_ || cpu_waiting) return;
    if (!copy_block(false) || hdma_length_-- == 0) { hdma_active_ = false; hdma_length_ = 0x7f; ppu.hdma_changed(); }
    if (event) clock.stall(64);
    else clock.delay(64);
}
// The source reads ROM, cartridge RAM and WRAM as the CPU would; Pan Docs
// says VRAM gives garbage and leaves the area from FE00 untested, which
// read FF here. A destination that runs past 9FFF ends the transfer early.
// HDMA1 to HDMA4 drop the low four bits, so a block lies within one page of
// the source and one of VRAM: a direct page is read in one copy, and an
// HBlank block, which moves at one time, is written in one.
bool Memory::copy_block(bool paced) {
    uint8_t bytes[16];
    const auto source = hdma_source_;
    const bool readable = source < 0x8000 || (source >= 0xa000 && source < 0xfe00);
    if (const uint8_t* page = readable ? pages[source >> 7].read : nullptr) {
        std::memcpy(bytes, page + (source & 127), sizeof bytes);
    } else {
        for (unsigned i = 0; i < 16; ++i) bytes[i] = readable ? peek(static_cast<uint16_t>(source + i)) : 0xff;
    }
    hdma_source_ = static_cast<uint16_t>(source + 16);
    if (paced) {
        for (unsigned i = 0; i < 16; ++i) {
            ppu.write_vram(hdma_destination_ + i, bytes[i], vram_bank_);
            if (i & 1) clock.delay(8);
        }
    } else {
        ppu.write_vram_block(hdma_destination_, bytes, vram_bank_);
    }
    hdma_destination_ = (hdma_destination_ + 16) & 0x1fff;
    return hdma_destination_ != 0;
}
uint8_t Memory::peek(uint16_t address) {
    address = canonical(address);
    if (address < 0x8000) return cartridge.read(address);
    if (address < 0xa000) return vram_cpu_[address - 0x8000];
    if (address < 0xc000) return cartridge.read(address);
    if (address < 0xd000) return wram[address - 0xc000];
    if (address < 0xe000) return wram_d000_[address - 0xd000];
    if (address < 0xfea0) return oam[address - 0xfe00];
    // FEA0..FEFF without OAM blocking: 00 on the DMG, the high nibble of the
    // address's low byte twice on CPU CGB E (Pan Docs, FEA0-FEFF range).
    if (address < 0xff00) return color() ? static_cast<uint8_t>(((address >> 4) & 15) * 0x11) : 0;
    if (address < 0xff80) return read_io(address & 127);
    return high[address & 127];
}
void Memory::write_io(uint8_t a, uint8_t value) {
    if (a >= 0x10 && a < 0x40) {
        if (a != 0x15 && a != 0x1f && (a < 0x27 || a >= 0x30)) apu.write(a, value);
        return;
    }
    if (a >= 0x40 && a <= 0x4b) {
        if (a == 0x46) { start_dma(value); return; }
        ppu.write(a, value);
        return;
    }
    switch (a) {
    case 0: io[a] = (value & 0x30) | (io[a] & 15) | 0xc0; update_joypad(); break;
    case 1: sync_serial(); io[a] = value; break;
    case 2: {
        sync_serial();
        // CGB mode adds the fast clock, bit 1 (Pan Docs, SC).
        const bool fast = mode_ == Mode::cgb && (value & 2);
        io[a] = value & (mode_ == Mode::cgb ? 0x83 : 0x81);
        serial_shift_ = static_cast<uint8_t>((fast ? 4 : 9) + tcycle_shift());
        serial_bits_ = value & 0x80 ? 8 : 0; serial_byte_ = 0;
        serial_edge_ = (value & 0x81) == 0x81 ? next_div_edge(1u << (serial_shift_ - tcycle_shift())) : kNever;
        schedule_serial();
        break;
    }
    case 4: reset_divider(); break;
    case 5:
        sync_timer();
        if (clock.now >= timer_reload_end_) { io[a] = value; timer_reload_ = kNever; }
        schedule_timer();
        break;
    case 6:
        io[a] = value;
        if (clock.now < timer_reload_end_) { sync_timer(); io[5] = value; schedule_timer(); }
        break;
    case 7: {
        sync_timer();
        const auto div = divider();
        const bool was_high = (io[a] & 4) && (div & (timer_period() >> 1));
        io[a] = value & 7;
        const bool is_high = (io[a] & 4) && (div & (timer_period() >> 1));
        if (was_high && !is_high) tick_timer();
        timer_edge_ = io[a] & 4 ? next_div_edge(timer_period()) : kNever;
        schedule_timer();
        break;
    }
    case 0x0f: io[a] = value & 31; ppu.write_if(); ++interrupt_generation; break;
    // A CGB's registers follow the DMG's, see write_cgb().
    default: if (color()) write_cgb(a, value); break;
    }
}
void Memory::poke(uint16_t address, uint8_t value) {
    if (address == 0xffff) ++interrupt_generation;
    address = canonical(address);
    if (address < 0x8000) {
        const auto old_ram = cartridge.ram_bank();
        const auto old0 = cartridge.rom_bank[0], old1 = cartridge.rom_bank[1];
        if (cartridge.control(address, value, clock.now)) {
            unsigned windows = 0;
            if (old_ram != cartridge.ram_bank()) { map(0xa000 >> 7, 0xc000 >> 7); windows |= kCartRam; }
            if (old0 != cartridge.rom_bank[0]) { map(0, 0x4000 >> 7); windows |= kRom0; }
            if (old1 != cartridge.rom_bank[1]) { map(0x4000 >> 7, 0x8000 >> 7); windows |= kRom1; }
            tag_code(windows);
            remapped |= windows;
            ++mapping_generation;
        }
        return;
    }
    if (address >= 0xa000 && address < 0xc000) {
        cartridge.write_ram(address, value, clock.now);
        return;
    }
    uint8_t* target = nullptr;
    if (address >= 0x8000 && address < 0xa000) { ppu.write_vram(address - 0x8000, value, vram_bank_); return; }
    else if (address >= 0xc000 && address < 0xd000) target = &wram[address - 0xc000];
    else if (address >= 0xd000 && address < 0xe000) target = &wram_d000_[address - 0xd000];
    else if (address >= 0xfe00 && address < 0xfea0) { write_oam(address - 0xfe00, value); return; }
    else if (address >= 0xff80) target = &high[address & 127];
    else if (address >= 0xff00) {
        write_io(address & 127, value);
    }
    if (target) *target = value;
}
void Memory::write_oam(unsigned offset, uint8_t value) {
    ppu.write_oam(offset, value);
}
bool Memory::dma_conflict(uint16_t address) const {
    if (address >= 0xfe00) return address < 0xff00;
    const bool video = address >= 0x8000 && address < 0xa000;
    const bool source_video = dma_source_ >= 0x8000 && dma_source_ < 0xa000;
    return video == source_video;
}
uint8_t Memory::cpu_read(uint16_t address) {
    if (dma_running_ && dma_conflict(address)) return address >= 0xfe00 ? 0xff : dma_value_;
    if (address >= 0x8000 && address < 0xa000 && ppu.vram_blocked()) return 0xff;
    if (address >= 0xfe00 && address < 0xff00 && ppu.oam_blocked()) return 0xff;
    return peek(address);
}
void Memory::cpu_write(uint16_t address, uint8_t value) {
    if (address >= 0x8000 && address < 0xa000 && ppu.vram_blocked(true)) return;
    if (address >= 0xfe00 && address < 0xff00 && ppu.oam_blocked(true)) return;
    if (!dma_running_ || !dma_conflict(address)) poke(address, value);
}
void Memory::start_dma(uint8_t value) {
    io[0x46] = value;
    // mooneye oam_dma_start: M1 after the write is still accessible, M2
    // belongs to DMA. A restart leaves the old transfer running through M1.
    dma_restart_ = clock.now + mcycle_ + (mcycle_ >> 2);
    if (!dma_running_) clock.schedule(3, dma_restart_, dma_event, this);
}
void Memory::dma_event(void* context) {
    auto& m = *static_cast<Memory*>(context);
    if (m.dma_restart_ <= m.clock.now) {
        m.dma_source_ = static_cast<uint16_t>(m.io[0x46]) << 8;
        m.dma_index_ = 0; m.dma_restart_ = kNever; m.dma_running_ = true;
        m.map();
    }
    if (m.dma_index_ == 160) {
        m.dma_running_ = false; m.map();
        if (m.dma_restart_ != kNever) m.clock.schedule(3, m.dma_restart_, dma_event, &m);
        else m.clock.cancel(3);
        return;
    }
    auto address = static_cast<uint16_t>(m.dma_source_ + m.dma_index_);
    // GB Complete Technical Reference 8.1 and mooneye sources-GS: DMA
    // decodes FE/FF as external WRAM, rather than OAM/IO/HRAM.
    if (address >= 0xe000) address -= 0x2000;
    m.dma_value_ = m.peek(address);
    m.write_oam(m.dma_index_++, m.dma_value_);
    m.clock.schedule(3, m.clock.now + m.mcycle_, dma_event, &m);
}
// Keep event/IO handling out of each inlined interpreter memory access.
// Page permissions are fetched again after the event at the sampling edge.
uint8_t Memory::read_timed(uint16_t address) {
    clock.advance(clock.now + access_edge());
    const auto* direct = pages[address >> 7].read;
    const uint8_t value = direct ? direct[address & 127] : read_slow_(this, address);
    clock.advance(clock.now + (mcycle_ - access_edge()));
    return value;
}
void Memory::write_timed(uint16_t address, uint8_t value) {
    clock.advance(clock.now + access_edge());
    auto* direct = pages[address >> 7].write;
    if (direct && address != 0xffff) direct[address & 127] = value;
    else write_slow_(this, address, value);
    clock.advance(clock.now + (mcycle_ - access_edge()));
}
void Memory::write_from_edge(uint16_t address, uint8_t value) {
    auto* direct = pages[address >> 7].write;
    if (direct && address != 0xffff) direct[address & 127] = value;
    else write_slow_(this, address, value);
    clock.advance(clock.now + (mcycle_ - access_edge()));
}
uint8_t Memory::probed_read(uint16_t address) {
    clock.advance(clock.now + access_edge());
    const auto* direct = pages[address >> 7].read;
    const uint8_t value = direct ? direct[address & 127] : cpu_read(address);
    record_access(clock.now, address, value, 0);
    clock.advance(clock.now + (mcycle_ - access_edge()));
    return value;
}
void Memory::probed_write(uint16_t address, uint8_t value) {
    clock.advance(clock.now + access_edge());
    record_access(clock.now, address, value, 1);
    auto* direct = pages[address >> 7].write;
    if (direct && address != 0xffff) direct[address & 127] = value;
    else cpu_write(address, value);
    clock.advance(clock.now + (mcycle_ - access_edge()));
}
uint8_t Memory::normal_read(Memory* m, uint16_t a) { return m->cpu_read(a); }
void Memory::normal_write(Memory* m, uint16_t a, uint8_t v) { m->cpu_write(a, v); }
uint8_t Memory::traced_read(Memory* m, uint16_t a) {
    const auto v = m->cpu_read(a);
    m->trace_->push({m->clock.now, m->instruction_pc, a, v, 0});
    return v;
}
void Memory::traced_write(Memory* m, uint16_t a, uint8_t v) {
    m->trace_->push({m->clock.now, m->instruction_pc, a, v, 1});
    m->cpu_write(a, v);
}
}  // namespace gbx::gb
