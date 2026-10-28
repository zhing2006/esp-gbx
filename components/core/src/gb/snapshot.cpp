#include "gbx/gb/snapshot.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <new>
#include <type_traits>

#include "gbx/gb/system.h"
#include "gbx/platform.h"
#include "../common/encoding.h"
#include "mixer.h"
#include "offload.h"
#include "renderer.h"

// An encoded state, little endian:
//
//   0   8  magic "GBXSTATE"
//   8   2  format version, 5
//  10   2  system and mode: 1, a DMG-C in GB mode; 2, a CPU CGB E in CGB
//          mode; 3, a CPU CGB E in DMG compatibility mode
//  12  24  the ROM's identity, see rom_identity()
//  36   4  cartridge RAM bytes, r
//  40   4  payload bytes, n
//  44   n  payload: the fields of SnapshotState in the order fields() visits
//          them, then the r bytes of cartridge RAM
//  44+n 4  CRC-32 (the zlib polynomial) of everything before it
//
// In the payload, integers take their size, Tick 8 bytes, and bool one byte,
// 0 or 1; enums are one byte. Arrays of bytes are stored as they are, but
// three are packed: the two frames of shades four pixels a byte (the
// leftmost in bits 1:0), the VRAM pages of the row in flight only those its
// dirty mask names, and its changes only as many as it holds (dot,
// address, value: 5 bytes each). The LCDC and WX writes of mode 3 are
// there only as many as the PPU holds (dot, address, value: 4 bytes
// each). The row's OAM is there only when the row carries one. A CGB's
// state follows the DMG's fields, which it shares; its color frames and
// colors are 16-bit integers, and the row's palette RAM is there only when
// the row carries it.
//
// Any change to what a field means, or to the fields and their order, needs
// a new version: states of other versions are refused, never migrated. The
// state is not compressed: at about 44 to 180 KiB it takes the card a
// fraction of a second, and the copy and checks are simpler without it. A
// CGB's state takes about 120 KiB more, most of it the two color frames.
namespace gbx::gb {
namespace {
constexpr char kMagic[8] = {'G', 'B', 'X', 'S', 'T', 'A', 'T', 'E'};
constexpr uint16_t kVersion = 5;
constexpr unsigned kTimingWrites = 94;  // The capacity of Ppu::timing_.
// The header's system field by Mode, and back.
constexpr uint16_t system_of(Mode mode) { return mode == Mode::dmg ? 1 : mode == Mode::cgb ? 2 : 3; }
constexpr bool mode_of(uint16_t system, Mode& mode) {
    if (system < 1 || system > 3) return false;
    mode = system == 1 ? Mode::dmg : system == 2 ? Mode::cgb : Mode::compat;
    return true;
}
constexpr size_t kHeader = 44, kTrailer = 4;
// The largest cartridge RAM is 128 KiB (Pan Docs, The Cartridge Header, 0149).
constexpr size_t kMaxRam = 128 * 1024;

using encoding::get16;
using encoding::get32;
using encoding::put16;
using encoding::put32;
}  // namespace

// Everything a running DMG holds, as plain copies. Device classes keep their
// own layout for their hot paths; SnapshotAccess copies between the two.
struct SnapshotState {
    Sm83State cpu;
    Tick now;
    std::array<Tick, Scheduler::kSlots> events;  // Deadlines by slot, kNever for none.
    uint64_t frames;
    uint16_t instruction_pc;
    std::array<uint8_t, 8192> wram, vram;
    std::array<uint8_t, 160> oam;
    std::array<uint8_t, 128> io, high;
    Tick div_origin;
    uint16_t div_initial;
    bool stopped;
    uint8_t buttons;
    Tick timer_edge, timer_reload, timer_reload_end, serial_edge;
    uint8_t serial_bits, serial_byte;
    std::array<uint8_t, 1024> serial_output;
    uint16_t serial_size;
    bool serial_truncated;
    Tick dma_restart;
    bool dma_running;
    uint16_t dma_source;
    uint8_t dma_index, dma_value;
    struct {
        uint16_t bank1;
        uint8_t bank2, mode, ram_select;
        bool ram_enabled;
        RtcState rtc;  // At `now`.
        uint8_t last_latch;
    } cartridge;
    struct {
        uint16_t line;
        uint8_t mode;
        bool coincidence, stat_line, first_line, blank, window_y;
        uint16_t compared;
        bool flag, hblank_irq, oam_irq, mode1_irq;
        Tick lock_at, line_start, transfer_start;
        uint16_t transfer_dots;
        uint8_t phase;
        Tick phase_at, armed;
        uint64_t dirty;
        bool oam_dirty;
        // The objects the line in transfer selected, and whether the frame
        // under way is drawn.
        std::array<uint8_t, 10> objects;
        uint8_t object_count;
        bool skipping;
    } ppu;
    // Mode 3 as it started and the LCDC and WX writes in it, see
    // Ppu::time_transfer(); the window's start and carry; the requests that
    // settle; the last OBJ height change.
    struct {
        uint8_t edge, transfer_lcdc, transfer_wx, fine, timing_count;
        std::array<uint16_t, kTimingWrites> timing_dot;
        std::array<uint8_t, kTimingWrites> timing_address, timing_value;
        uint16_t window_from;
        bool window_carry, transfer_carry;
        uint8_t carry_count;
        Tick hblank_at, mode1_at, size_at;
        bool hblank_requested, mode1_requested, mode1_pending;
        uint8_t unseen_interrupts;  // Memory::unseen_interrupts
    } timing;
    // The row of the line in transfer, which the PPU fills until the line
    // ends and the renderer has not seen.
    struct {
        bool present;
        uint8_t y, count, fine;
        bool window_y, blank, oam_changed;
        uint16_t window_from;
        bool window_carry;
        uint8_t window_count;
        std::array<uint8_t, 12> registers;
        std::array<uint8_t, 10> objects;
        std::array<uint8_t, 160> oam;
        uint64_t dirty;
        std::array<uint8_t, 8192> vram;
        uint16_t changes_size;
        std::array<Change, 192> changes;
    } row;
    // The renderer's copies, the window line count and both frames: the
    // front one the last completed, the back one drawn up to the last row.
    struct {
        std::array<uint8_t, 8192> vram;
        std::array<uint8_t, 160> oam;
        uint8_t window_line;
        uint64_t frame_id;
        std::array<uint8_t, kFramePixels> front, back;
    } renderer;
    struct {
        Tick edge;
        uint8_t step, on, reported;
        std::array<uint16_t, 4> length;
        uint16_t shadow;
        uint8_t sweep_timer;
        bool sweep_enabled, negated;
        WaveTimer wave;
    } apu;
    struct Square { Tick next; uint32_t spacing; uint8_t step; bool silent; };
    struct Envelope { uint8_t volume, timer; bool running; };
    struct {
        std::array<uint8_t, 0x30> regs;
        uint8_t on;
        std::array<Square, 2> squares;
        WaveTimer wave;
        uint8_t sample;
        Tick noise_next;
        uint32_t noise_spacing;
        uint16_t lfsr;
        std::array<Envelope, 4> envelopes;
        std::array<uint8_t, 4> digital;
        std::array<int32_t, 4> analog, left, right;
        int32_t level_left, level_right;
        Tick now, base_time, chunk;
        uint64_t base, step;
        uint32_t rate;
        int32_t ppm;
        uint32_t first;
        std::array<std::array<int32_t, 2>, 1024> changes;
        int32_t sum_left, sum_right, charge_left, charge_right, retain;
        bool settled;
        uint32_t frame;
    } mixer;
    // A CGB's, in either mode: not encoded in GB mode.
    Mode mode;
    struct {
        std::array<uint8_t, 6 * 4096> wram;  // banks 2..7
        std::array<uint8_t, 8192> vram;      // bank 1
        std::array<uint8_t, 128> palettes;   // palette RAM, see PpuColor
        uint64_t dirty1;
        bool palettes_dirty;
        uint16_t hdma_source, hdma_destination;
        uint8_t hdma_length;
        bool hdma_active;
        uint8_t mcycle, serial_shift;
        Tick wake;
        bool cpu_waiting;
        // The renderer's copy of VRAM bank 1, its colors and both frames.
        std::array<uint8_t, 8192> renderer_vram;
        std::array<uint16_t, 64> colors;
        std::array<uint16_t, kFramePixels> front, back;
        // The row in flight: its bank 1 pages and palette RAM.
        uint64_t row_dirty1;
        std::array<uint8_t, 8192> row_vram;
        bool row_palettes_changed;
        std::array<uint8_t, 128> row_palettes;
    } cgb;
};

namespace {
// Visits every field of a state once, in the encoding's order, so that the
// writer and the reader cannot disagree. `f.present(flag)` decides whether
// a field that is only sometimes there follows: the reader has just read
// the flag, and measuring the largest encoding takes every one as there.
template<class S, class F> void fields(S& s, F& f) {
    auto& c = s.cpu;
    f(c.a); f(c.f); f(c.b); f(c.c); f(c.d); f(c.e); f(c.h); f(c.l); f(c.sp); f(c.pc);
    f(c.ime); f(c.ei_pending); f(c.halt_bug); f(c.mode);
    f(s.now);
    for (auto& event : s.events) f(event);
    f(s.frames); f(s.instruction_pc);
    f.bytes(s.wram); f.bytes(s.vram); f.bytes(s.oam); f.bytes(s.io); f.bytes(s.high);
    f(s.div_origin); f(s.div_initial); f(s.stopped); f(s.buttons);
    f(s.timer_edge); f(s.timer_reload); f(s.timer_reload_end); f(s.serial_edge);
    f(s.serial_bits); f(s.serial_byte);
    f(s.serial_size); f(s.serial_truncated);
    f.bytes(std::span(s.serial_output).first(f.limit(s.serial_size, s.serial_output.size())));
    f(s.dma_restart); f(s.dma_running); f(s.dma_source); f(s.dma_index); f(s.dma_value);

    auto& cart = s.cartridge;
    f(cart.bank1); f(cart.bank2); f(cart.mode); f(cart.ram_select); f(cart.ram_enabled);
    auto& rtc = cart.rtc;
    f(rtc.seconds); f(rtc.minutes); f(rtc.hours); f(rtc.days); f(rtc.halt); f(rtc.carry); f(rtc.subsecond);
    f.bytes(rtc.latched);
    f(cart.last_latch);

    auto& p = s.ppu;
    f(p.line); f(p.mode); f(p.coincidence); f(p.stat_line); f(p.first_line); f(p.blank); f(p.window_y);
    f(p.compared); f(p.flag); f(p.hblank_irq); f(p.oam_irq); f(p.mode1_irq); f(p.lock_at); f(p.line_start);
    f(p.transfer_start); f(p.transfer_dots); f(p.phase);
    f(p.phase_at); f(p.armed); f(p.dirty); f(p.oam_dirty);
    f.bytes(p.objects); f(p.object_count); f(p.skipping);
    auto& t = s.timing;
    f(t.edge); f(t.transfer_lcdc); f(t.transfer_wx); f(t.fine); f(t.timing_count);
    for (size_t i = 0, n = f.limit(t.timing_count, t.timing_dot.size()); i < n; ++i) {
        f(t.timing_dot[i]); f(t.timing_address[i]); f(t.timing_value[i]);
    }
    f(t.window_from); f(t.window_carry); f(t.transfer_carry); f(t.carry_count);
    f(t.hblank_at); f(t.mode1_at); f(t.size_at); f(t.hblank_requested); f(t.mode1_requested); f(t.mode1_pending);
    f(t.unseen_interrupts);

    auto& r = s.row;
    f(r.present);
    if (f.present(r.present)) {
        f(r.y); f(r.count); f(r.fine); f(r.window_y); f(r.blank);
        f(r.window_from); f(r.window_carry); f(r.window_count);
        f.bytes(r.registers); f.bytes(r.objects);
        f(r.oam_changed);
        if (f.present(r.oam_changed)) f.bytes(r.oam);
        f(r.dirty);
        f.pages(r.vram, r.dirty);
        f(r.changes_size);
        for (size_t i = 0, n = f.limit(r.changes_size, r.changes.size()); i < n; ++i) {
            f(r.changes[i].dot); f(r.changes[i].address); f(r.changes[i].value);
        }
    }

    auto& d = s.renderer;
    f.bytes(d.vram); f.bytes(d.oam); f(d.window_line); f(d.frame_id);
    f.shades(d.front); f.shades(d.back);

    auto& a = s.apu;
    f(a.edge); f(a.step); f(a.on); f(a.reported);
    for (auto& length : a.length) f(length);
    f(a.shadow); f(a.sweep_timer); f(a.sweep_enabled); f(a.negated);
    f(a.wave.next); f(a.wave.last); f(a.wave.spacing); f(a.wave.position);

    auto& m = s.mixer;
    f.bytes(m.regs); f(m.on);
    for (auto& square : m.squares) { f(square.next); f(square.spacing); f(square.step); f(square.silent); }
    f(m.wave.next); f(m.wave.last); f(m.wave.spacing); f(m.wave.position); f(m.sample);
    f(m.noise_next); f(m.noise_spacing); f(m.lfsr);
    for (auto& envelope : m.envelopes) { f(envelope.volume); f(envelope.timer); f(envelope.running); }
    f.bytes(m.digital);
    for (unsigned i = 0; i < 4; ++i) { f(m.analog[i]); f(m.left[i]); f(m.right[i]); }
    f(m.level_left); f(m.level_right);
    f(m.now); f(m.base_time); f(m.chunk); f(m.base); f(m.step); f(m.rate); f(m.ppm); f(m.first);
    for (auto& change : m.changes) { f(change[0]); f(change[1]); }
    f(m.sum_left); f(m.sum_right); f(m.charge_left); f(m.charge_right); f(m.retain);
    f(m.settled); f(m.frame);

    if (!f.color(s.mode != Mode::dmg)) return;
    auto& g = s.cgb;
    f.bytes(g.wram); f.bytes(g.vram); f.bytes(g.palettes); f(g.dirty1); f(g.palettes_dirty);
    f(g.hdma_source); f(g.hdma_destination); f(g.hdma_length); f(g.hdma_active);
    f(g.mcycle); f(g.serial_shift); f(g.wake); f(g.cpu_waiting);
    f.bytes(g.renderer_vram); f.words(g.colors); f.words(g.front); f.words(g.back);
    if (f.present(r.present)) {
        f(g.row_dirty1);
        f.pages(g.row_vram, g.row_dirty1);
        f(g.row_palettes_changed);
        if (f.present(g.row_palettes_changed)) f.bytes(g.row_palettes);
    }
}

// Writes the payload: the encoding's integers, with the frames of shades
// packed four pixels a byte and the row's VRAM in its dirty pages.
class Writer : public encoding::Output {
public:
    using Output::Output;
    void pages(const std::array<uint8_t, 8192>& vram, uint64_t dirty) {
        for (; dirty; dirty &= dirty - 1) bytes(std::span(vram).subspan(std::countr_zero(dirty) * 128, 128));
    }
    void shades(const std::array<uint8_t, kFramePixels>& pixels) {
        for (unsigned i = 0; i < kFramePixels; i += 4)
            put(static_cast<uint8_t>((pixels[i] & 3) | (pixels[i + 1] & 3) << 2 | (pixels[i + 2] & 3) << 4 |
                                     (pixels[i + 3] & 3) << 6));
    }
    bool color(bool flag) const { return flag; }
};

// Counts a payload: with `largest` the largest one in a mode, with every
// optional field there, every page dirty and the row full of changes, which
// reads no field; otherwise the one of the state it visits, which has that
// mode.
class Measure {
public:
    Measure(Mode mode, bool largest) : color_(mode != Mode::dmg), largest_(largest) {}
    template<class T> void operator()(const T&) { size_ += encoding::width<T>(); }
    void bytes(std::span<const uint8_t> data) { size_ += data.size(); }
    void pages(const std::array<uint8_t, 8192>& vram, uint64_t dirty) {
        size_ += largest_ ? vram.size() : size_t{128} * std::popcount(dirty);
    }
    void shades(const std::array<uint8_t, kFramePixels>&) { size_ += kFramePixels / 4; }
    void words(std::span<const uint16_t> data) { size_ += data.size() * 2; }
    bool present(bool flag) const { return largest_ || flag; }
    bool color(bool) const { return color_; }
    size_t limit(size_t count, size_t capacity) const { return largest_ ? capacity : count; }
    size_t size() const { return size_; }
private:
    bool color_, largest_;
    size_t size_ = 0;
};

class Reader : public encoding::Input {
public:
    using Input::Input;
    void pages(std::array<uint8_t, 8192>& vram, uint64_t dirty) {
        for (; dirty; dirty &= dirty - 1) bytes(std::span(vram).subspan(std::countr_zero(dirty) * 128, 128));
    }
    void shades(std::array<uint8_t, kFramePixels>& pixels) {
        for (unsigned i = 0; i < kFramePixels; i += 4) {
            uint8_t packed;
            if (!take(&packed, 1)) return;
            for (unsigned k = 0; k < 4; ++k) pixels[i + k] = (packed >> (k * 2)) & 3;
        }
    }
    bool color(bool flag) const { return flag; }
};

// The header of an encoding.
struct Header {
    RomIdentity identity{};
    size_t ram_size = 0, payload = 0;
    Mode mode = Mode::dmg;
};
// Reads the header of an encoding of `size` bytes. It is filled in unless
// the encoding is unknown, of another version or of another system.
Snapshot::Status read_header(Reader& reader, size_t size, Header& out) {
    using Status = Snapshot::Status;
    uint8_t h[kHeader];
    if (size < kHeader || !reader.take(h, kHeader) || std::memcmp(h, kMagic, sizeof kMagic) != 0)
        return Status::unknown;
    if (get16(h + 8) != kVersion) return Status::version;
    if (!mode_of(static_cast<uint16_t>(get16(h + 10)), out.mode)) return Status::system;
    std::copy_n(h + 12, out.identity.size(), out.identity.begin());
    out.ram_size = get32(h + 36);
    out.payload = get32(h + 40);
    if (out.ram_size > kMaxRam || out.payload < out.ram_size || size != kHeader + out.payload + kTrailer)
        return Status::damaged;
    return Status::ok;
}

// Values that index arrays or pick code paths. A state that passes its
// checksum was written by this version, so these hold unless it was
// written wrongly; they are checked so that such a state is refused rather
// than run.
bool plausible(const SnapshotState& s) {
    const auto& r = s.row;
    // Ppu::Phase: settle, the last.
    bool ok = (s.cpu.f & 15) == 0 && static_cast<unsigned>(s.cpu.mode) <= 3 && s.dma_index <= 160 &&
        s.ppu.line < 154 && s.ppu.mode <= 3 && s.ppu.phase <= 10 && s.io[0x44] < 154 && s.ppu.compared <= 0x100 &&
        s.ppu.transfer_dots < 512 && s.ppu.object_count <= 10 && s.timing.timing_count <= kTimingWrites &&
        s.timing.edge < 4 && s.apu.step < 8;
    for (unsigned i = 0; i < s.ppu.object_count; ++i) ok &= s.ppu.objects[i] < 40;
    if (r.present) {
        ok &= r.y < 144 && r.count <= 10 && r.fine < 8;
        for (unsigned i = 0; i < r.count; ++i) ok &= r.objects[i] < 40;
    }
    if (s.mode != Mode::dmg) {
        const auto& g = s.cgb;
        ok &= (g.mcycle == 8 || g.mcycle == 4) && g.hdma_length < 0x80 && g.hdma_destination < 0x2000 &&
              !((g.hdma_source | g.hdma_destination) & 15) && g.serial_shift >= 3 && g.serial_shift <= 10;
    }
    return ok;
}
}  // namespace

RomIdentity rom_identity(std::span<const uint8_t> rom) {
    RomIdentity id{};
    if (rom.size() < 0x150) return id;
    std::copy_n(rom.begin() + 0x134, 16, id.begin());
    std::copy_n(rom.begin() + 0x147, 3, id.begin() + 16);
    std::copy_n(rom.begin() + 0x14d, 3, id.begin() + 19);
    return id;
}

// Copies between the devices and a state. Only the owner calls it, and the
// worker on the other core is idle whenever it touches the renderer or the
// mixer, see System::capture() and System::restore().
struct SnapshotAccess {
    static void capture(System& system, SnapshotState& s, uint8_t* ram);
    static void restore(System& system, const SnapshotState& s, const uint8_t* ram);
    static void capture_cgb(System& system, SnapshotState& s);
    static void restore_cgb(System& system, const SnapshotState& s);
};

void SnapshotAccess::capture(System& system, SnapshotState& s, uint8_t* ram) {
    auto& m = system.memory;
    s.mode = m.mode();
    s.cpu = system.cpu;
    s.now = m.clock.now;
    for (unsigned slot = 0; slot < Scheduler::kSlots; ++slot) s.events[slot] = m.clock.deadline(slot);
    assert(!m.clock.callback(0) || Ppu::owns(m.clock.callback(0)));
    s.frames = m.frames; s.instruction_pc = m.instruction_pc;
    s.wram = m.wram; s.vram = m.vram; s.oam = m.oam; s.io = m.io; s.high = m.high;
    s.div_origin = m.div_origin_; s.div_initial = m.div_initial_; s.stopped = m.stopped_; s.buttons = m.buttons_;
    s.timer_edge = m.timer_edge_; s.timer_reload = m.timer_reload_; s.timer_reload_end = m.timer_reload_end_;
    s.serial_edge = m.serial_edge_; s.serial_bits = m.serial_bits_; s.serial_byte = m.serial_byte_;
    s.serial_size = static_cast<uint16_t>(m.serial_size_); s.serial_truncated = m.serial_truncated_;
    std::copy_n(m.serial_output_.begin(), m.serial_size_, s.serial_output.begin());
    s.dma_restart = m.dma_restart_; s.dma_running = m.dma_running_; s.dma_source = m.dma_source_;
    s.dma_index = static_cast<uint8_t>(m.dma_index_); s.dma_value = m.dma_value_;

    auto& cart = m.cartridge;
    s.cartridge = {cart.bank1_, cart.bank2_, cart.mode_, cart.ram_select_, cart.ram_enabled_,
                   cart.rtc_.state(m.clock.now), cart.rtc_.last_latch_};
    if (cart.ram_size_) std::memcpy(ram, cart.ram_.get(), cart.ram_size_);

    const auto& p = m.ppu;
    s.ppu = {p.line_, p.mode_, p.coincidence_, p.stat_line_, p.first_line_, p.blank_, p.window_y_,
             p.compared_, p.flag_, p.hblank_irq_, p.oam_irq_, p.mode1_irq_, p.lock_at_, p.line_start_,
             p.transfer_start_, static_cast<uint16_t>(p.transfer_dots_),
             static_cast<uint8_t>(p.phase_), p.phase_at_, p.armed_, p.dirty_, p.oam_dirty_,
             p.objects_, p.object_count_, p.skipping_};
    auto& q = s.timing;
    q.edge = p.edge_; q.transfer_lcdc = p.transfer_lcdc_; q.transfer_wx = p.transfer_wx_; q.fine = p.fine_;
    q.timing_count = p.timing_count_;
    static_assert(std::tuple_size_v<decltype(p.timing_)> == kTimingWrites);
    for (unsigned i = 0; i < p.timing_.size(); ++i) {
        const auto& write = p.timing_[i];
        q.timing_dot[i] = write.dot; q.timing_address[i] = write.address; q.timing_value[i] = write.value;
    }
    q.window_from = p.window_from_; q.window_carry = p.window_carry_; q.transfer_carry = p.transfer_carry_;
    q.carry_count = p.carry_count_;
    q.hblank_at = p.hblank_at_; q.mode1_at = p.mode1_at_; q.size_at = p.size_at_;
    q.hblank_requested = p.hblank_requested_; q.mode1_requested = p.mode1_requested_;
    q.mode1_pending = p.mode1_pending_; q.unseen_interrupts = m.unseen_interrupts;
    auto& r = s.row;
    r.present = p.job_ != nullptr;
    if (r.present) {
        const auto& row = *static_cast<const Row*>(p.job_);
        r.y = row.y; r.count = row.count; r.fine = static_cast<uint8_t>(row.fine);
        r.window_y = row.window_y; r.blank = row.blank; r.oam_changed = row.oam_changed;
        r.window_from = row.window_from; r.window_carry = row.window_carry; r.window_count = row.window_count;
        r.registers = row.registers; r.objects = row.objects;
        if (row.oam_changed) r.oam = row.oam;
        r.dirty = row.dirty;
        for (auto dirty = row.dirty; dirty; dirty &= dirty - 1) {
            const auto offset = std::countr_zero(dirty) * 128;
            std::memcpy(r.vram.data() + offset, row.vram.data() + offset, 128);
        }
        r.changes_size = static_cast<uint16_t>(row.changes_size);
        std::copy_n(row.changes.begin(), row.changes_size, r.changes.begin());
    }
    if (const auto* renderer = p.renderer_.get()) {
        auto& d = s.renderer;
        std::copy_n(renderer->vram_.begin(), d.vram.size(), d.vram.begin()); d.oam = renderer->oam_;
        d.window_line = renderer->window_line_; d.frame_id = renderer->id_;
        // A CGB's frames share their memory with GB mode's, see Renderer.
        if (renderer->mode_ == Mode::dmg) {
            d.front = renderer->shades_[renderer->front_]; d.back = renderer->shades_[renderer->front_ ^ 1];
        } else {
            d.front.fill(0); d.back.fill(0);
        }
    }

    const auto& apu = m.apu;
    s.apu = {apu.edge_, apu.step_, apu.on_, apu.reported_, apu.length_, apu.shadow_, apu.sweep_timer_,
             apu.sweep_enabled_, apu.negated_, apu.wave_};
    if (const auto* mixer = apu.mixer_.get()) {
        auto& x = s.mixer;
        x.regs = mixer->regs_; x.on = mixer->on_;
        for (unsigned i = 0; i < 2; ++i) {
            const auto& square = mixer->squares_[i];
            x.squares[i] = {square.next, square.spacing, square.step, square.silent};
        }
        x.wave = mixer->wave_; x.sample = mixer->sample_;
        x.noise_next = mixer->noise_.next; x.noise_spacing = mixer->noise_.spacing; x.lfsr = mixer->noise_.lfsr;
        for (unsigned i = 0; i < 4; ++i) {
            const auto& envelope = mixer->envelopes_[i];
            x.envelopes[i] = {envelope.volume, envelope.timer, envelope.running};
        }
        x.digital = mixer->digital_; x.analog = mixer->analog_; x.left = mixer->left_; x.right = mixer->right_;
        x.level_left = mixer->level_left_; x.level_right = mixer->level_right_;
        x.now = mixer->now_; x.base_time = mixer->base_time_; x.chunk = mixer->chunk_;
        x.base = mixer->base_; x.step = mixer->step_; x.rate = mixer->rate_; x.ppm = mixer->ppm_;
        x.first = mixer->first_;
        static_assert(Mixer::kPending == std::tuple_size_v<decltype(x.changes)>);
        x.changes = mixer->changes_;
        x.sum_left = mixer->sum_left_; x.sum_right = mixer->sum_right_;
        x.charge_left = mixer->charge_left_; x.charge_right = mixer->charge_right_;
        x.retain = mixer->retain_; x.settled = mixer->settled_; x.frame = mixer->frame_;
    }
    if (m.color()) capture_cgb(system, s);
}

void SnapshotAccess::capture_cgb(System& system, SnapshotState& s) {
    auto& m = system.memory;
    const auto& p = m.ppu;
    auto& g = s.cgb;
    g.wram = m.banks_->wram; g.vram = m.banks_->vram;
    g.palettes = m.banks_->ppu.palettes; g.dirty1 = m.banks_->ppu.dirty1;
    g.palettes_dirty = m.banks_->ppu.palettes_dirty;
    g.hdma_source = m.hdma_source_; g.hdma_destination = m.hdma_destination_;
    g.hdma_length = m.hdma_length_; g.hdma_active = m.hdma_active_;
    g.mcycle = m.mcycle_; g.serial_shift = m.serial_shift_; g.wake = m.wake; g.cpu_waiting = m.cpu_waiting;
    if (const auto* renderer = p.renderer_.get()) {
        std::copy_n(renderer->vram_.begin() + 8192, 8192, g.renderer_vram.begin());
        g.colors = renderer->cgb_colors_;
        std::copy_n(renderer->shown_, kFramePixels, g.front.begin());
        std::copy_n(renderer->back_frame(), kFramePixels, g.back.begin());
    }
    if (p.job_) {
        const auto& row = *static_cast<const Row*>(p.job_);
        g.row_dirty1 = row.dirty1;
        for (auto dirty = row.dirty1; dirty; dirty &= dirty - 1) {
            const auto offset = std::countr_zero(dirty) * 128;
            std::memcpy(g.row_vram.data() + offset, row.vram.data() + 0x2000 + offset, 128);
        }
        g.row_palettes_changed = row.palettes_changed;
        if (row.palettes_changed) g.row_palettes = row.palettes;
    }
}

void SnapshotAccess::restore(System& system, const SnapshotState& s, const uint8_t* ram) {
    auto& m = system.memory;
    auto& p = m.ppu;
    auto& apu = m.apu;
    // Hand the worker what is pending and wait for it, so that it is idle
    // while its state is replaced. A row in flight is reused for the
    // state's, or handed over when the state has none.
    if (p.job_ && !s.row.present) { p.renderer_->submit(); p.job_ = nullptr; }
    p.finish();
    apu.flush();
    apu.finish();

    system.cpu = s.cpu;
    m.clock.restore(s.now, s.events, {&Ppu::event, &Memory::timer_event, &Memory::serial_event, &Memory::dma_event},
                    {&p, &m, &m, &m});
    m.frames = s.frames; m.instruction_pc = s.instruction_pc;
    m.wram = s.wram; m.vram = s.vram; m.oam = s.oam; m.io = s.io; m.high = s.high;
    m.div_origin_ = s.div_origin; m.div_initial_ = s.div_initial; m.stopped_ = s.stopped; m.buttons_ = s.buttons;
    m.timer_edge_ = s.timer_edge; m.timer_reload_ = s.timer_reload; m.timer_reload_end_ = s.timer_reload_end;
    m.serial_edge_ = s.serial_edge; m.serial_bits_ = s.serial_bits; m.serial_byte_ = s.serial_byte;
    m.serial_size_ = s.serial_size; m.serial_truncated_ = s.serial_truncated;
    std::copy_n(s.serial_output.begin(), s.serial_size, m.serial_output_.begin());
    m.dma_restart_ = s.dma_restart; m.dma_running_ = s.dma_running; m.dma_source_ = s.dma_source;
    m.dma_index_ = s.dma_index; m.dma_value_ = s.dma_value;

    auto& cart = m.cartridge;
    cart.bank1_ = s.cartridge.bank1; cart.bank2_ = s.cartridge.bank2; cart.mode_ = s.cartridge.mode;
    cart.ram_select_ = s.cartridge.ram_select; cart.ram_enabled_ = s.cartridge.ram_enabled;
    cart.resolve();
    if (cart.ram_size_) std::memcpy(cart.ram_.get(), ram, cart.ram_size_);
    cart.rtc_.set(s.cartridge.rtc, s.now);
    cart.rtc_.last_latch_ = s.cartridge.last_latch;
    // The whole RAM and the clock may differ from the save file now.
    cart.ram_written_ = cart.ram_size_ ? static_cast<uint32_t>((uint64_t{1} << ((cart.ram_size_ + 8191) / 8192)) - 1) : 0;
    cart.rtc_written_ = cart.has_rtc_;

    const auto& q = s.ppu;
    p.line_ = q.line; p.mode_ = q.mode; p.coincidence_ = q.coincidence; p.stat_line_ = q.stat_line;
    p.first_line_ = q.first_line; p.blank_ = q.blank; p.window_y_ = q.window_y;
    p.compared_ = q.compared; p.flag_ = q.flag; p.hblank_irq_ = q.hblank_irq; p.oam_irq_ = q.oam_irq;
    p.mode1_irq_ = q.mode1_irq; p.lock_at_ = q.lock_at; p.line_start_ = q.line_start;
    p.transfer_start_ = q.transfer_start;
    p.transfer_dots_ = q.transfer_dots; p.phase_ = static_cast<Ppu::Phase>(q.phase);
    p.phase_at_ = q.phase_at; p.armed_ = q.armed; p.dirty_ = q.dirty; p.oam_dirty_ = q.oam_dirty;
    p.objects_ = q.objects; p.object_count_ = q.object_count; p.skipping_ = q.skipping;
    const auto& t = s.timing;
    p.edge_ = t.edge; p.transfer_lcdc_ = t.transfer_lcdc; p.transfer_wx_ = t.transfer_wx; p.fine_ = t.fine;
    p.timing_count_ = t.timing_count;
    for (unsigned i = 0; i < p.timing_.size(); ++i)
        p.timing_[i] = {t.timing_dot[i], t.timing_address[i], t.timing_value[i]};
    p.window_from_ = t.window_from; p.window_carry_ = t.window_carry; p.transfer_carry_ = t.transfer_carry;
    p.carry_count_ = t.carry_count;
    p.hblank_at_ = t.hblank_at; p.mode1_at_ = t.mode1_at; p.size_at_ = t.size_at;
    p.hblank_requested_ = t.hblank_requested; p.mode1_requested_ = t.mode1_requested;
    p.mode1_pending_ = t.mode1_pending; m.unseen_interrupts = t.unseen_interrupts;
    p.objects_height_ = 0;  // Rebuilds line_objects_ from the OAM.
    if (auto* renderer = p.renderer_.get()) {
        const auto& d = s.renderer;
        std::copy(d.vram.begin(), d.vram.end(), renderer->vram_.begin()); renderer->oam_ = d.oam;
        renderer->window_line_ = d.window_line; renderer->id_ = d.frame_id;
        if (s.mode == Mode::dmg) {
            renderer->shades_[renderer->front_] = d.front; renderer->shades_[renderer->front_ ^ 1] = d.back;
        }
        ++renderer->frames_;       // pixels() converts the front frame again.
        renderer->palette_ = -1;   // The BGP lookup is built again.
        renderer->mode_ = s.mode;
        if (s.row.present) {
            const auto& r = s.row;
            auto& row = p.job_ ? *static_cast<Row*>(p.job_) : renderer->acquire();
            p.job_ = &row;
            row.kind = Row::Kind::line; row.dirty1 = 0; row.palettes_changed = false;
            row.y = r.y; row.count = r.count; row.fine = r.fine;
            row.window_y = r.window_y; row.blank = r.blank; row.oam_changed = r.oam_changed;
            row.window_from = r.window_from; row.window_carry = r.window_carry; row.window_count = r.window_count;
            row.double_speed = false;  // See restore_cgb().
            row.registers = r.registers; row.objects = r.objects;
            if (r.oam_changed) row.oam = r.oam;
            row.dirty = r.dirty;
            for (auto dirty = r.dirty; dirty; dirty &= dirty - 1) {
                const auto offset = std::countr_zero(dirty) * 128;
                std::memcpy(row.vram.data() + offset, r.vram.data() + offset, 128);
            }
            row.changes_size = r.changes_size;
            std::copy_n(r.changes.begin(), r.changes_size, row.changes.begin());
        }
    }

    const auto& a = s.apu;
    apu.edge_ = a.edge; apu.step_ = a.step; apu.on_ = a.on; apu.reported_ = a.reported;
    apu.length_ = a.length; apu.shadow_ = a.shadow; apu.sweep_timer_ = a.sweep_timer;
    apu.sweep_enabled_ = a.sweep_enabled; apu.negated_ = a.negated; apu.wave_ = a.wave;
    // The flush above closed the open batch. The next one starts at the
    // restored time, and the next run hands it over at once.
    apu.last_end_ = s.now; apu.flush_at_ = s.now;
    if (auto* mixer = apu.mixer_.get()) {
        const auto& x = s.mixer;
        mixer->regs_ = x.regs; mixer->on_ = x.on;
        for (unsigned i = 0; i < 2; ++i)
            mixer->squares_[i] = {x.squares[i].next, x.squares[i].spacing, x.squares[i].step, x.squares[i].silent};
        mixer->wave_ = x.wave; mixer->sample_ = x.sample;
        mixer->noise_ = {x.noise_next, x.noise_spacing, x.lfsr};
        for (unsigned i = 0; i < 4; ++i)
            mixer->envelopes_[i] = {x.envelopes[i].volume, x.envelopes[i].timer, x.envelopes[i].running};
        mixer->digital_ = x.digital; mixer->analog_ = x.analog; mixer->left_ = x.left; mixer->right_ = x.right;
        mixer->level_left_ = x.level_left; mixer->level_right_ = x.level_right;
        mixer->now_ = x.now; mixer->base_time_ = x.base_time; mixer->chunk_ = x.chunk;
        mixer->base_ = x.base; mixer->step_ = x.step; mixer->rate_ = x.rate; mixer->ppm_ = x.ppm;
        mixer->first_ = x.first; mixer->changes_ = x.changes;
        mixer->sum_left_ = x.sum_left; mixer->sum_right_ = x.sum_right;
        mixer->charge_left_ = x.charge_left; mixer->charge_right_ = x.charge_right;
        mixer->retain_ = x.retain; mixer->settled_ = x.settled; mixer->frame_ = x.frame;
        // As after a reset, the reader skips the frames not read yet.
        mixer->reset_at_.store(mixer->written_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        mixer->dropped_.store(0, std::memory_order_relaxed);
        // The producer's count of committed frames goes on from the
        // restored position, see Mixer::submit().
        mixer->scheduled_time_ = x.now;
        mixer->scheduled_position_ = x.base + (x.now - x.base_time) * x.step;
        mixer->scheduled_step_ = x.step; mixer->scheduled_rate_ = x.rate; mixer->scheduled_ppm_ = x.ppm;
        // The output rate and speed are the caller's, not the state's.
        if (x.rate != apu.rate_) apu.append_setting(sound::Event::rate, apu.rate_);
        if (x.ppm != apu.speed_) apu.append_setting(sound::Event::speed, static_cast<uint32_t>(apu.speed_));
    }

    if (m.color()) restore_cgb(system, s);
    ++m.mapping_generation;
    ++m.interrupt_generation;
    m.map();
}

// After restore()'s DMG part, which put the row in flight in place.
void SnapshotAccess::restore_cgb(System& system, const SnapshotState& s) {
    auto& m = system.memory;
    auto& p = m.ppu;
    const auto& g = s.cgb;
    m.banks_->wram = g.wram; m.banks_->vram = g.vram;
    m.banks_->ppu.palettes = g.palettes; m.banks_->ppu.dirty1 = g.dirty1;
    m.banks_->ppu.palettes_dirty = g.palettes_dirty;
    // The banks SVBK and VBK select, which restore() put back in io.
    m.wram_bank_ = 0;
    m.select_wram(s.io[0x70]);
    m.select_vram(s.io[0x4f]);
    m.hdma_source_ = g.hdma_source; m.hdma_destination_ = g.hdma_destination;
    m.hdma_length_ = g.hdma_length; m.hdma_active_ = g.hdma_active;
    p.hdma_ = g.hdma_active;
    m.mcycle_ = g.mcycle; m.serial_shift_ = g.serial_shift; m.wake = g.wake; m.cpu_waiting = g.cpu_waiting;
    if (auto* renderer = p.renderer_.get()) {
        std::copy(g.renderer_vram.begin(), g.renderer_vram.end(), renderer->vram_.begin() + 8192);
        renderer->cgb_colors_ = g.colors;
        // The BG colors two pixels at a time, as render_color() builds them.
        for (unsigned palette = 0; palette < 8; ++palette) {
            const auto* c = g.colors.data() + palette * 4;
            for (unsigned i = 0; i < 16; ++i) renderer->cgb_pairs_[palette * 16 + i] = c[i & 3] | uint32_t{c[i >> 2]} << 16;
        }
        // A frame under way goes on in the back one.
        renderer->color_frames_[0] = g.front;
        renderer->color_frames_[1] = g.back;
        renderer->shown_ = renderer->color_frames_[0].data();
        renderer->back_ = renderer->color_frames_[1].data();
        if (s.row.present) {
            auto& row = *static_cast<Row*>(p.job_);
            // A row does not span a speed switch, so the speed restored is
            // the row's.
            row.double_speed = m.double_speed();
            row.dirty1 = g.row_dirty1;
            for (auto dirty = g.row_dirty1; dirty; dirty &= dirty - 1) {
                const auto offset = std::countr_zero(dirty) * 128;
                std::memcpy(row.vram.data() + 0x2000 + offset, g.row_vram.data() + offset, 128);
            }
            row.palettes_changed = g.row_palettes_changed;
            if (g.row_palettes_changed) row.palettes = g.row_palettes;
        }
    }
}

unsigned Ppu::pending_changes() const { return job_ ? static_cast<const Row*>(job_)->changes_size : 0; }

bool System::capture(Snapshot& out) {
    if (!out.allocated() || out.ram_size_ != memory.cartridge.ram().size()) return false;
    if (jit_ && jit_->pending()) return false;
    // The sound up to now goes to the mixer; then the worker finishes it and
    // the rows, and stays idle while its state is copied.
    memory.apu.flush();
    memory.apu.finish();
    memory.ppu.finish();
    SnapshotAccess::capture(*this, *out.state_, out.ram_);
    out.identity_ = rom_identity(memory.rom());
    out.mode_ = memory.mode();
    out.valid_ = true;
    return true;
}

bool System::restore(const Snapshot& in, bool other_rom) {
    if (!in.valid() || in.ram_size_ != memory.cartridge.ram().size() || in.mode_ != memory.mode()) return false;
    if (!other_rom && in.identity_ != rom_identity(memory.rom())) return false;
    SnapshotAccess::restore(*this, *in.state_, in.ram_);
    if (jit_) jit_->forget_ram_code();
    return true;
}

Snapshot::Snapshot() = default;
Snapshot::~Snapshot() { release(); }
void Snapshot::release() {
    platform::free_external(block_);
    block_ = nullptr; state_ = nullptr; ram_ = nullptr; ram_size_ = 0; valid_ = false;
}
bool Snapshot::allocate(size_t ram_size, Mode mode) {
    mode_ = mode;
    if (state_ && ram_size == ram_size_) return true;
    release();
    if (ram_size > kMaxRam) return false;
    // External memory is only word aligned; the state holds 64-bit fields.
    constexpr size_t align = alignof(SnapshotState);
    block_ = platform::allocate_external(sizeof(SnapshotState) + align + ram_size);
    if (!block_) return false;
    const auto base = (reinterpret_cast<uintptr_t>(block_) + align - 1) & ~uintptr_t{align - 1};
    state_ = new (reinterpret_cast<void*>(base)) SnapshotState{};
    ram_ = reinterpret_cast<uint8_t*>(state_ + 1);
    ram_size_ = ram_size;
    return true;
}
std::span<const uint8_t> Snapshot::frame() const {
    return state_ && valid_ && mode_ == Mode::dmg ? std::span<const uint8_t>(state_->renderer.front)
                                                  : std::span<const uint8_t>{};
}
std::span<const uint16_t> Snapshot::color_frame() const {
    return state_ && valid_ && mode_ != Mode::dmg ? std::span<const uint16_t>(state_->cgb.front)
                                                  : std::span<const uint16_t>{};
}
size_t Snapshot::max_encoded(Mode mode) const {
    if (!state_) return 0;
    Measure measure(mode, true);
    fields(*state_, measure);
    return kHeader + measure.size() + ram_size_ + kTrailer;
}
size_t Snapshot::encoded_size() const {
    if (!valid_) return 0;
    Measure measure(state_->mode, false);
    fields(*state_, measure);
    return kHeader + measure.size() + ram_size_ + kTrailer;
}
size_t Snapshot::encode(std::span<uint8_t> out) const {
    if (!valid_ || out.size() < max_encoded()) return 0;
    return encode(out, nullptr, nullptr);
}
size_t Snapshot::encode(std::span<uint8_t> buffer, PieceSink sink, void* context) const {
    const size_t size = encoded_size();
    if (!size) return 0;
    const size_t payload = size - kHeader - kTrailer;
    uint8_t h[kHeader];
    std::memcpy(h, kMagic, sizeof kMagic);
    put16(h + 8, kVersion);
    put16(h + 10, system_of(mode_));
    std::copy(identity_.begin(), identity_.end(), h + 12);
    put32(h + 36, static_cast<uint32_t>(ram_size_));
    put32(h + 40, static_cast<uint32_t>(payload));
    Writer writer(buffer, sink, context);
    writer.bytes(h);
    fields(*state_, writer);
    writer.bytes({ram_, ram_size_});
    return writer.written() == kHeader + payload && writer.finish() ? size : 0;
}
Snapshot::Status Snapshot::inspect(std::span<const uint8_t> bytes, RomIdentity* identity, size_t* ram_size,
                                   Mode* mode) {
    encoding::Whole whole{bytes};
    return inspect(bytes.size(), encoding::Whole::next, &whole, identity, ram_size, mode);
}
Snapshot::Status Snapshot::inspect(size_t size, PieceSource source, void* context, RomIdentity* identity,
                                   size_t* ram_size, Mode* mode) {
    Reader reader(source, context, size);
    Header header;
    const auto status = read_header(reader, size, header);
    if (status == Status::unknown || status == Status::version || status == Status::system) return status;
    if (identity) *identity = header.identity;
    if (ram_size) *ram_size = header.ram_size;
    if (mode) *mode = header.mode;
    if (status != Status::ok) return status;
    return reader.take(nullptr, header.payload) && reader.check() ? Status::ok : Status::damaged;
}
Snapshot::Status Snapshot::decode(std::span<const uint8_t> bytes) {
    encoding::Whole whole{bytes};
    return decode(bytes.size(), encoding::Whole::next, &whole);
}
Snapshot::Status Snapshot::decode(size_t size, PieceSource source, void* context) {
    valid_ = false;
    Reader reader(source, context, size);
    Header header;
    const auto status = read_header(reader, size, header);
    if (status != Status::ok) return status;
    if (!state_ || header.ram_size != ram_size_) return Status::incompatible;
    state_->mode = header.mode;
    fields(*state_, reader);
    reader.bytes({ram_, ram_size_});
    if (!reader.ok() || reader.position() != kHeader + header.payload || !reader.check() || !plausible(*state_))
        return Status::damaged;
    identity_ = header.identity;
    mode_ = header.mode;
    valid_ = true;
    return Status::ok;
}
}  // namespace gbx::gb
