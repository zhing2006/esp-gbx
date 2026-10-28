// GBA states: capture, restore and the byte format, see gbx/gba/snapshot.h.

#include "gbx/gba/snapshot.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>
#include <type_traits>

#include "gbx/gba/system.h"
#include "gbx/platform.h"
#include "../common/encoding.h"
#include "../gb/offload.h"
#include "mixer.h"
#include "renderer.h"
#include "sound.h"

// An encoded state, little endian, with the GB's header (gb/snapshot.cpp):
//
//   0   8  magic "GBXSTATE"
//   8   2  format version, 1
//  10   2  system: 4, a GBA (the GB's states hold 1 to 3)
//  12  24  the ROM's identity, see rom_identity()
//  36   4  save memory bytes, r
//  40   4  payload bytes, n
//  44   n  payload: the fields of SnapshotState in the order fields() visits
//          them, then the r bytes of save memory
//  44+n 4  CRC-32 (the zlib polynomial) of everything before it
//
// In the payload, integers take their size, Tick 8 bytes, and bool one byte,
// 0 or 1; enums are one byte. Arrays of bytes are stored as they are, the
// frame as 16-bit integers. Three are packed: the line job under way only
// with as many pages and timed writes as it holds, and the renderer's
// copies of VRAM, palette RAM and OAM only in the pages that may differ
// from the memory's, see SnapshotAccess::capture().
//
// Any change to what a field means, or to the fields and their order, needs
// a new version: states of other versions are refused, never migrated. The
// state is not compressed: at about 500 KiB plus the save memory it takes
// the card a fraction of a second.
namespace gbx::gba {
namespace {
constexpr char kMagic[8] = {'G', 'B', 'X', 'S', 'T', 'A', 'T', 'E'};
constexpr uint16_t kVersion = 1;
constexpr uint16_t kSystem = 4;
constexpr size_t kHeader = 44, kTrailer = 4;
constexpr size_t kMaxBackup = 128 * 1024;
constexpr unsigned kTimedWrites = 256;  // Ppu::kMaxWrites
constexpr unsigned kPending = 1024;     // Mixer::kPending
// The state's pages: VRAM, palette RAM and OAM in 512-byte pages. The PPU's
// pages of palette RAM are smaller; to_state() and to_ppu() convert marks.
constexpr unsigned kPageBytes = 512, kVramPages = 192, kPalettePage = 192, kOamPage = 194, kPages = 196;
constexpr unsigned kDirtyWords = (kPages + 31) / 32;
// The bits of the last dirty word that name pages.
constexpr uint32_t kLastWord = (1u << (kPages % 32)) - 1;
static_assert(Ppu::kVramPages == kVramPages && Ppu::kDirtyWords == 8 && Ppu::kPalettePage == 6 * 32 &&
              Ppu::kOamPage == 7 * 32 && kPageBytes / Ppu::kColorPageBytes == 16);

std::array<uint32_t, kDirtyWords> to_state(const std::array<uint32_t, Ppu::kDirtyWords>& marks) {
    std::array<uint32_t, kDirtyWords> state{};
    std::copy_n(marks.begin(), 6, state.begin());
    const uint32_t colors = marks[6];
    state[6] = (colors & 0xffff ? 1u : 0u) | (colors >> 16 ? 2u : 0u) | (marks[7] & 3) << 2;
    return state;
}

std::array<uint32_t, Ppu::kDirtyWords> to_ppu(const std::array<uint32_t, kDirtyWords>& state) {
    std::array<uint32_t, Ppu::kDirtyWords> marks{};
    std::copy_n(state.begin(), 6, marks.begin());
    marks[6] = (state[6] & 1 ? 0xffffu : 0u) | (state[6] & 2 ? 0xffff0000u : 0u);
    marks[7] = (state[6] >> 2) & 3;
    return marks;
}

using encoding::get16;
using encoding::get32;
using encoding::put16;
using encoding::put32;
}  // namespace

// Everything a running GBA holds, as plain copies. Device classes keep their
// own layout for their hot paths; SnapshotAccess copies between the two.
struct SnapshotState {
    Arm7State cpu;
    // The devices' time, and the CPU's, which may be later: Memory::now().
    // A run brings the devices to the CPU at its start.
    Tick now, cpu_now;
    std::array<Tick, Scheduler::kSlots> events;  // Deadlines by slot, kNever for none.
    uint64_t frames, halt_cycles;
    bool irq_line, irq_unmasked, halted;
    uint32_t bios_latch, vram_obj_start, memory_control;
    std::array<uint32_t, kDirtyWords> dirty;
    std::array<uint8_t, 256 * 1024> ewram;
    std::array<uint8_t, 96 * 1024> vram;
    std::array<uint8_t, 32768> iwram;
    std::array<uint8_t, 1024> palette, oam;
    std::array<uint8_t, 0x400> io;
    uint16_t irq_flags, irq_recent, buttons;
    Tick irq_recent_time;
    struct History {
        std::array<Tick, 8> time;
        uint8_t values, count;
        bool before;
    };
    History irq_request, irq_master;
    Tick irq_request_rise, halt_entry, halt_tail, irq_stalled, irq_stall_from;
    bool irq_watch, halt_in_access;
    struct Timer {
        uint16_t reload, control, counter, held;
        Tick since, end, first, period, handled;
    };
    std::array<Timer, 4> timers;
    uint16_t timer_irq;
    Tick timer_irq_time, prescaler_phase;
    struct Dma {
        uint32_t source, destination, count;
        uint16_t control;
        Tick start;
        uint32_t latch;
        bool gamepak_sequential, eeprom, paused;
    };
    std::array<Dma, 4> dma;
    uint32_t dma_latch;
    uint8_t dma_pending;
    Tick dma_deferred;
    bool dma_bus, capture;
    struct Fifo {
        std::array<uint8_t, 32> data;
        uint8_t read, size;
        int8_t sample;
    };
    std::array<Fifo, 2> fifo;
    struct {
        Tick edge;
        uint8_t step, on, reported;
        std::array<uint16_t, 4> length;
        uint16_t shadow;
        uint8_t sweep_timer;
        bool sweep_enabled, negated;
    } psg;
    std::array<uint8_t, 16> wave_played;
    struct {
        uint32_t head, base, start;
        uint8_t paused, cycles;
        bool running, stalled;
    } prefetch;
    uint32_t gamepak_next;
    struct {
        uint8_t data, direction, control;
    } gpio;
    struct {
        RtcState clock;  // At `now`.
        uint8_t phase, pins, command, shift, bits, index, output;
        std::array<uint8_t, 7> buffer;
    } rtc;
    struct {
        BackupType type;
        uint8_t flash_state;
        bool flash_id, flash_erase;
        uint8_t flash_bank;
        uint64_t eeprom_shift;
        uint32_t eeprom_count;
        uint8_t eeprom_command;
        uint32_t eeprom_read_bit, eeprom_address;
        Tick eeprom_ready;
        bool eeprom_writing;
        uint64_t eeprom_buffer;
        uint32_t size;
        std::array<uint8_t, kMaxBackup> data;  // Encoded after the payload.
    } backup;
    struct {
        Tick frame_start;
        uint64_t frame;
        uint8_t affine_written;
        std::array<uint32_t, kDirtyWords> held;
        bool open;
    } ppu;
    // The job of the line under way, when ppu.open: its pages, copied when
    // the line started, and the writes timed in it.
    struct {
        uint8_t line, affine_written, pages, drawing;
        uint16_t writes;
        uint64_t frame;
        std::array<uint8_t, LineJob::kRegisterBytes> registers;
        std::array<uint8_t, kPages> numbers;
        std::array<uint8_t, kPages * kPageBytes> data;
        std::array<TimedWrite, kTimedWrites> timed;
    } job;
    // The renderer's copies, kept only in the pages of `differs`: those
    // written since the renderer last took them, held for a bitmap frame
    // not shown, or in the job under way. The others equal the memory's.
    struct {
        std::array<uint32_t, kDirtyWords> differs;
        std::array<uint8_t, 96 * 1024> vram;
        std::array<uint8_t, 1024> palette, oam;
        uint64_t frame_id;
        std::array<uint8_t, 6> enabled_lines;
        uint8_t shown;
        std::array<std::array<int32_t, 2>, 2> reference, mosaic_reference;
        std::array<uint16_t, kFramePixels> frame;
    } renderer;
    struct Square {
        Tick next;
        uint32_t spacing;
        uint8_t step;
    };
    struct Envelope {
        uint8_t volume, timer;
        bool running;
    };
    struct {
        std::array<uint8_t, 0x30> regs;
        std::array<std::array<uint8_t, 16>, 2> wave_ram;
        uint8_t on;
        std::array<Square, 2> squares;
        Tick wave_next;
        uint32_t wave_spacing;
        uint8_t wave_index, sample;
        Tick noise_next;
        uint32_t noise_spacing;
        uint16_t lfsr;
        bool noise_high;
        std::array<Envelope, 4> envelopes;
        std::array<uint8_t, 4> digital;
        std::array<int8_t, 2> fifo;
        int32_t level_left, level_right, held_left, held_right;
        Tick sample_at, now, base_time, chunk;
        uint64_t base, step;
        uint32_t rate;
        int32_t ppm;
        uint32_t first;
        std::array<std::array<int32_t, 2>, kPending> changes;
        int32_t sum_left, sum_right;
    } mixer;
};

namespace {
// The bytes of a renderer page in the state's copies.
std::span<uint8_t> renderer_page(SnapshotState& s, unsigned page) {
    auto& d = s.renderer;
    if (page < kVramPages) return std::span(d.vram).subspan(page * kPageBytes, kPageBytes);
    if (page < kOamPage) return std::span(d.palette).subspan((page - kPalettePage) * kPageBytes, kPageBytes);
    return std::span(d.oam).subspan((page - kOamPage) * kPageBytes, kPageBytes);
}
std::span<const uint8_t> renderer_page(const SnapshotState& s, unsigned page) {
    return renderer_page(const_cast<SnapshotState&>(s), page);
}

// Visits every field of a state once, in the encoding's order, so that the
// writer and the reader cannot disagree. `f.present(flag)` decides whether
// a field that is only sometimes there follows, `f.limit(count, capacity)`
// how many entries of an array do, and `f.mask(word)` which pages: the
// reader has just read the flag, count or word, and measuring the largest
// encoding takes every one as there.
template<class S, class F> void fields(S& s, F& f) {
    auto& c = s.cpu;
    for (auto& r : c.r) f(r);
    f(c.cpsr); f(c.pipeline[0]); f(c.pipeline[1]); f(c.sequential);
    for (auto& bank : c.r8) for (auto& r : bank) f(r);
    for (auto& bank : c.r13) for (auto& r : bank) f(r);
    for (auto& r : c.spsr) f(r);
    f(s.now); f(s.cpu_now);
    for (auto& event : s.events) f(event);
    f(s.frames); f(s.halt_cycles); f(s.irq_line); f(s.irq_unmasked); f(s.halted);
    f(s.bios_latch); f(s.vram_obj_start); f(s.memory_control);
    for (auto& word : s.dirty) f(word);
    f.bytes(s.ewram); f.bytes(s.vram); f.bytes(s.iwram); f.bytes(s.palette); f.bytes(s.oam); f.bytes(s.io);
    f(s.irq_flags); f(s.irq_recent); f(s.buttons); f(s.irq_recent_time);
    for (auto* h : {&s.irq_request, &s.irq_master}) {
        for (auto& time : h->time) f(time);
        f(h->values); f(h->count); f(h->before);
    }
    f(s.irq_request_rise); f(s.halt_entry); f(s.halt_tail); f(s.irq_stalled); f(s.irq_stall_from);
    f(s.irq_watch); f(s.halt_in_access);
    for (auto& t : s.timers) {
        f(t.reload); f(t.control); f(t.counter); f(t.held);
        f(t.since); f(t.end); f(t.first); f(t.period); f(t.handled);
    }
    f(s.timer_irq); f(s.timer_irq_time); f(s.prescaler_phase);
    for (auto& d : s.dma) {
        f(d.source); f(d.destination); f(d.count); f(d.control); f(d.start); f(d.latch);
        f(d.gamepak_sequential); f(d.eeprom); f(d.paused);
    }
    f(s.dma_latch); f(s.dma_pending); f(s.dma_deferred); f(s.dma_bus); f(s.capture);
    for (auto& fifo : s.fifo) { f.bytes(fifo.data); f(fifo.read); f(fifo.size); f(fifo.sample); }
    auto& p = s.psg;
    f(p.edge); f(p.step); f(p.on); f(p.reported);
    for (auto& length : p.length) f(length);
    f(p.shadow); f(p.sweep_timer); f(p.sweep_enabled); f(p.negated);
    f.bytes(s.wave_played);
    auto& q = s.prefetch;
    f(q.head); f(q.base); f(q.start); f(q.paused); f(q.cycles); f(q.running); f(q.stalled);
    f(s.gamepak_next);
    f(s.gpio.data); f(s.gpio.direction); f(s.gpio.control);
    auto& r = s.rtc;
    f.bytes(r.clock.datetime); f(r.clock.control); f(r.clock.subsecond);
    f(r.phase); f(r.pins); f(r.command); f(r.shift); f(r.bits); f(r.index); f(r.output); f.bytes(r.buffer);
    auto& b = s.backup;
    f(b.type); f(b.flash_state); f(b.flash_id); f(b.flash_erase); f(b.flash_bank);
    f(b.eeprom_shift); f(b.eeprom_count); f(b.eeprom_command); f(b.eeprom_read_bit); f(b.eeprom_address);
    f(b.eeprom_ready); f(b.eeprom_writing); f(b.eeprom_buffer);

    auto& v = s.ppu;
    f(v.frame_start); f(v.frame); f(v.affine_written);
    for (auto& word : v.held) f(word);
    f(v.open);
    if (f.present(v.open)) {
        auto& j = s.job;
        f(j.line); f(j.affine_written); f(j.drawing); f(j.frame); f.bytes(j.registers);
        f(j.pages);
        const size_t pages = f.limit(j.pages, kPages);
        f.bytes(std::span(j.numbers).first(pages));
        f.bytes(std::span(j.data).first(pages * kPageBytes));
        f(j.writes);
        for (size_t i = 0, n = f.limit(j.writes, kTimedWrites); i < n; ++i) {
            f(j.timed[i].cycle); f(j.timed[i].offset); f(j.timed[i].value);
        }
    }
    auto& d = s.renderer;
    for (unsigned w = 0; w < kDirtyWords; ++w) {
        f(d.differs[w]);
        for (uint32_t bits = f.mask(d.differs[w], w + 1 < kDirtyWords ? ~0u : kLastWord); bits; bits &= bits - 1)
            f.bytes(renderer_page(s, w * 32 + static_cast<unsigned>(std::countr_zero(bits))));
    }
    f(d.frame_id); f.bytes(d.enabled_lines); f(d.shown);
    for (auto& point : d.reference) { f(point[0]); f(point[1]); }
    for (auto& point : d.mosaic_reference) { f(point[0]); f(point[1]); }
    f.words(d.frame);

    auto& m = s.mixer;
    f.bytes(m.regs); f.bytes(m.wave_ram[0]); f.bytes(m.wave_ram[1]); f(m.on);
    for (auto& square : m.squares) { f(square.next); f(square.spacing); f(square.step); }
    f(m.wave_next); f(m.wave_spacing); f(m.wave_index); f(m.sample);
    f(m.noise_next); f(m.noise_spacing); f(m.lfsr); f(m.noise_high);
    for (auto& e : m.envelopes) { f(e.volume); f(e.timer); f(e.running); }
    f.bytes(m.digital); f(m.fifo[0]); f(m.fifo[1]);
    f(m.level_left); f(m.level_right); f(m.held_left); f(m.held_right);
    f(m.sample_at); f(m.now); f(m.base_time); f(m.chunk); f(m.base); f(m.step); f(m.rate); f(m.ppm); f(m.first);
    for (auto& change : m.changes) { f(change[0]); f(change[1]); }
    f(m.sum_left); f(m.sum_right);
}

class Writer : public encoding::Output {
public:
    using Output::Output;
    uint32_t mask(uint32_t word, uint32_t) const { return word; }
};

// Counts a payload: with `largest` the largest one, with every optional
// field there and every page and write, which reads no field; otherwise the
// one of the state it visits.
class Measure {
public:
    explicit Measure(bool largest) : largest_(largest) {}
    template<class T> void operator()(const T&) { size_ += encoding::width<T>(); }
    void bytes(std::span<const uint8_t> data) { size_ += data.size(); }
    void words(std::span<const uint16_t> data) { size_ += data.size() * 2; }
    bool present(bool flag) const { return largest_ || flag; }
    size_t limit(size_t count, size_t capacity) const { return largest_ ? capacity : count; }
    uint32_t mask(uint32_t word, uint32_t full) const { return largest_ ? full : word; }
    size_t size() const { return size_; }
private:
    bool largest_;
    size_t size_ = 0;
};

class Reader : public encoding::Input {
public:
    using Input::Input;
    uint32_t mask(uint32_t word, uint32_t full) {
        if (word & ~full) { fail(); return 0; }
        return word;
    }
};

// The header of an encoding.
struct Header {
    RomIdentity identity{};
    size_t backup = 0, payload = 0;
};
// Reads the header of an encoding of `size` bytes. It is filled in unless
// the encoding is unknown, of another system or of another version.
Snapshot::Status read_header(Reader& reader, size_t size, Header& out) {
    using Status = Snapshot::Status;
    uint8_t h[kHeader];
    if (size < kHeader || !reader.take(h, kHeader) || std::memcmp(h, kMagic, sizeof kMagic) != 0)
        return Status::unknown;
    if (get16(h + 10) != kSystem) return Status::system;
    if (get16(h + 8) != kVersion) return Status::version;
    std::copy_n(h + 12, out.identity.size(), out.identity.begin());
    out.backup = get32(h + 36);
    out.payload = get32(h + 40);
    if (out.backup > kMaxBackup || out.payload < out.backup || size != kHeader + out.payload + kTrailer)
        return Status::damaged;
    return Status::ok;
}

// Values that index arrays, divide or pick code paths. A state that passes
// its checksum was written by this version, so these hold unless it was
// written wrongly; they are checked so that such a state is refused rather
// than run.
bool plausible(const SnapshotState& s) {
    bool ok = s.events[kSlotIrq] == kNever && s.cpu_now >= s.now && (s.dirty.back() & ~kLastWord) == 0 &&
              (s.ppu.held.back() & ~kLastWord) == 0 && (s.vram_obj_start == 0x10000 || s.vram_obj_start == 0x14000) &&
              s.irq_request.count <= 8 && s.irq_master.count <= 8 && s.dma_pending < 16 && s.psg.step < 8 &&
              s.psg.sweep_timer <= 8 && s.rtc.phase <= 4 && s.rtc.command < 8 && s.rtc.bits < 8 &&
              s.backup.type <= BackupType::eeprom8k && s.backup.flash_state <= 7 && s.backup.flash_bank <= 1 &&
              s.backup.eeprom_command <= 3 && s.backup.eeprom_read_bit <= 68 && s.backup.eeprom_address < 1024 &&
              s.backup.size == Backup::size_of(s.backup.type) && s.mixer.wave_index < 64;
    for (const auto& t : s.timers) ok &= t.first == kNever || t.period != 0;
    for (const auto& fifo : s.fifo) ok &= fifo.read < 32 && fifo.size <= 32;
    for (const auto& square : s.mixer.squares) ok &= square.step < 8;
    for (const auto& e : s.mixer.envelopes) ok &= e.volume < 16;
    for (const uint8_t d : s.mixer.digital) ok &= d < 16;
    if (s.ppu.open) {
        const auto& j = s.job;
        ok &= j.line <= kVisibleLines && j.pages <= kPages && j.writes <= kTimedWrites;
        for (unsigned i = 0; i < j.pages && ok; ++i) ok &= j.numbers[i] < kPages;
        for (unsigned i = 0; i < j.writes && ok; ++i) ok &= j.timed[i].offset <= LineJob::kRegisterBytes - 2;
    }
    return ok;
}
}  // namespace

RomIdentity rom_identity(RomCache& rom) {
    RomIdentity id{};
    if (rom.size() < 0xc0) return id;
    for (unsigned i = 0; i < 18; ++i) id[i] = rom.byte(0xa0 + i);
    id[18] = rom.byte(0xbc);
    id[19] = rom.byte(0xbd);
    return id;
}

// Copies between the devices and a state. Only the owner calls it, and the
// worker on the other core is idle whenever it touches the renderer or the
// mixer, see System::capture() and System::restore().
struct SnapshotAccess {
    static void capture(System& system, SnapshotState& s);
    static void restore(System& system, const SnapshotState& s);
};

void SnapshotAccess::capture(System& system, SnapshotState& s) {
    static_assert(Ppu::kMaxWrites == kTimedWrites && Mixer::kPending == kPending);
    auto& m = system.memory;
    // The renderer's frame, whole in its frame_.
    m.ppu.pixels();
    s.cpu = system.cpu;
    s.now = m.clock.now;
    s.cpu_now = m.now();
    for (unsigned slot = 0; slot < Scheduler::kSlots; ++slot) s.events[slot] = m.clock.deadline(slot);
    s.frames = m.frames; s.halt_cycles = m.halt_cycles;
    s.irq_line = m.irq_line; s.irq_unmasked = m.irq_unmasked; s.halted = m.halted;
    s.bios_latch = m.bios_latch; s.vram_obj_start = m.vram_obj_start; s.memory_control = m.memory_control_;
    s.dirty = to_state(m.dirty);
    std::memcpy(s.ewram.data(), m.ewram, s.ewram.size());
    std::memcpy(s.vram.data(), m.vram, s.vram.size());
    s.iwram = m.iwram; s.palette = m.palette; s.oam = m.oam; s.io = m.io_;
    // VCOUNT and IF keep copies in I/O for the JIT, not part of the state.
    s.io[6] = s.io[7] = s.io[0x202] = s.io[0x203] = 0;
    s.irq_flags = m.irq_flags_; s.irq_recent = m.irq_recent_; s.buttons = m.buttons_;
    s.irq_recent_time = m.irq_recent_time_;
    const auto history = [](const Memory::IrqHistory& h) {
        return SnapshotState::History{h.time, h.values, h.count, h.before};
    };
    s.irq_request = history(m.irq_request_); s.irq_master = history(m.irq_master_);
    s.irq_request_rise = m.irq_request_rise_; s.halt_entry = m.halt_entry_; s.halt_tail = m.halt_tail_;
    s.irq_stalled = m.irq_stalled_; s.irq_stall_from = m.irq_stall_from_;
    s.irq_watch = m.irq_watch_; s.halt_in_access = m.halt_in_access_;
    for (unsigned i = 0; i < 4; ++i) {
        const auto& t = m.timers_[i];
        s.timers[i] = {t.reload, t.control, t.counter, t.held, t.since, t.end, t.first, t.period, t.handled};
        const auto& d = m.dma_[i];
        s.dma[i] = {d.source, d.destination, d.count, d.control, d.start, d.latch,
                    d.gamepak_sequential, d.eeprom, d.paused};
    }
    s.timer_irq = m.timer_irq_; s.timer_irq_time = m.timer_irq_time_; s.prescaler_phase = m.prescaler_phase_;
    s.dma_latch = m.dma_latch_; s.dma_pending = m.dma_pending_; s.dma_deferred = m.dma_deferred_;
    s.dma_bus = m.dma_bus_; s.capture = m.capture_;
    for (unsigned i = 0; i < 2; ++i) {
        const auto& f = m.fifo_[i];
        s.fifo[i] = {f.data, f.read, f.size, f.sample};
    }
    const auto& p = m.psg_;
    s.psg = {p.edge, p.step, p.on, p.reported, p.length, p.shadow, p.sweep_timer, p.sweep_enabled, p.negated};
    s.wave_played = m.wave_played_;
    const auto& q = m.prefetch_;
    s.prefetch = {q.head, q.base, q.start, q.paused, q.cycles, q.running, q.stalled};
    s.gamepak_next = m.gamepak_next_;
    s.gpio = {m.gpio_.data, m.gpio_.direction, m.gpio_.control};
    auto& rtc = m.rtc;
    s.rtc = {rtc.state(m.clock.now), static_cast<uint8_t>(rtc.phase_), rtc.pins_, rtc.command_, rtc.shift_,
             rtc.bits_, rtc.index_, rtc.output_, rtc.buffer_};
    const auto& b = m.backup;
    auto& sb = s.backup;
    sb.type = b.type_; sb.flash_state = static_cast<uint8_t>(b.flash_state_);
    sb.flash_id = b.flash_id_; sb.flash_erase = b.flash_erase_; sb.flash_bank = b.flash_bank_;
    sb.eeprom_shift = b.eeprom_shift_; sb.eeprom_count = b.eeprom_count_; sb.eeprom_command = b.eeprom_command_;
    sb.eeprom_read_bit = b.eeprom_read_bit_; sb.eeprom_address = b.eeprom_address_;
    sb.eeprom_ready = b.eeprom_ready_; sb.eeprom_writing = b.eeprom_writing_; sb.eeprom_buffer = b.eeprom_buffer_;
    sb.size = static_cast<uint32_t>(b.size());
    std::memcpy(sb.data.data(), b.data_, sb.size);

    const auto& v = m.ppu;
    s.ppu.frame_start = v.frame_start_; s.ppu.frame = v.frame_; s.ppu.affine_written = v.affine_written_;
    s.ppu.held = to_state(v.held_); s.ppu.open = v.open_;
    auto& d = s.renderer;
    for (unsigned w = 0; w < kDirtyWords; ++w) d.differs[w] = s.dirty[w] | s.ppu.held[w];
    const auto* renderer = v.renderer_.get();
    if (v.open_) {
        const auto& job = v.job_;
        auto& j = s.job;
        j.line = job.line; j.affine_written = job.affine_written; j.drawing = job.drawing;
        j.writes = job.writes; j.frame = job.frame; j.registers = job.registers;
        // The job's pages of palette RAM go into whole pages of the state:
        // the renderer's copy, which has every job before, and the job's.
        const uint8_t* payload = v.payload_ + job.payload;
        const uint8_t* data = payload + job.pages;
        std::array<uint8_t, 1024> palette{};
        if (renderer) palette = renderer->palette_;
        unsigned colors = 0, pages = 0;
        for (unsigned i = 0; i < job.pages; ++i) {
            const unsigned page = payload[i];
            if (page - Ppu::kPalettePage < Ppu::kOamPage - Ppu::kPalettePage) {
                const unsigned offset = (page - Ppu::kPalettePage) * Ppu::kColorPageBytes;
                std::memcpy(palette.data() + offset, data, Ppu::kColorPageBytes);
                colors |= 1u << (offset / kPageBytes);
                data += Ppu::kColorPageBytes;
                continue;
            }
            j.numbers[pages] = static_cast<uint8_t>(page < kVramPages ? page : kOamPage + page - Ppu::kOamPage);
            std::memcpy(j.data.data() + size_t{pages++} * kPageBytes, data, kPageBytes);
            data += kPageBytes;
        }
        for (unsigned half = 0; half < 2; ++half) {
            if (!(colors & (1u << half))) continue;
            j.numbers[pages] = static_cast<uint8_t>(kPalettePage + half);
            std::memcpy(j.data.data() + size_t{pages++} * kPageBytes, palette.data() + half * kPageBytes, kPageBytes);
        }
        j.pages = static_cast<uint8_t>(pages);
        std::copy_n(v.writes_.begin(), job.writes, j.timed.begin());
        for (unsigned i = 0; i < pages; ++i) d.differs[j.numbers[i] / 32] |= 1u << (j.numbers[i] % 32);
    }
    if (renderer) {
        for (unsigned w = 0; w < kDirtyWords; ++w) {
            for (uint32_t bits = d.differs[w]; bits; bits &= bits - 1) {
                const unsigned page = w * 32 + static_cast<unsigned>(std::countr_zero(bits));
                const uint8_t* from = page < kVramPages ? renderer->vram_ + page * kPageBytes
                                      : page < kOamPage ? renderer->palette_.data() + (page - kPalettePage) * kPageBytes
                                                        : renderer->oam_.data() + (page - kOamPage) * kPageBytes;
                std::memcpy(renderer_page(s, page).data(), from, kPageBytes);
            }
        }
        d.frame_id = renderer->frame_id_; d.enabled_lines = renderer->enabled_lines_; d.shown = renderer->shown_;
        d.reference = renderer->reference_; d.mosaic_reference = renderer->mosaic_reference_;
        std::copy_n(renderer->frame_, kFramePixels, d.frame.begin());
    }

    if (const auto* mixer = m.mixer_.get()) {
        auto& x = s.mixer;
        x.regs = mixer->regs_; x.wave_ram = mixer->wave_ram_; x.on = mixer->on_;
        for (unsigned i = 0; i < 2; ++i) {
            const auto& square = mixer->squares_[i];
            x.squares[i] = {square.next, square.spacing, square.step};
        }
        x.wave_next = mixer->wave_.next; x.wave_spacing = mixer->wave_.spacing; x.wave_index = mixer->wave_.index;
        x.sample = mixer->sample_;
        x.noise_next = mixer->noise_.next; x.noise_spacing = mixer->noise_.spacing;
        x.lfsr = mixer->noise_.lfsr; x.noise_high = mixer->noise_.high;
        for (unsigned i = 0; i < 4; ++i) {
            const auto& e = mixer->envelopes_[i];
            x.envelopes[i] = {e.volume, e.timer, e.running};
        }
        x.digital = mixer->digital_; x.fifo = mixer->fifo_;
        x.level_left = mixer->level_left_; x.level_right = mixer->level_right_;
        x.held_left = mixer->held_left_; x.held_right = mixer->held_right_;
        x.sample_at = mixer->sample_at_; x.now = mixer->now_; x.base_time = mixer->base_time_;
        x.chunk = mixer->chunk_; x.base = mixer->base_; x.step = mixer->step_; x.rate = mixer->rate_;
        x.ppm = mixer->ppm_; x.first = mixer->first_; x.changes = mixer->changes_;
        x.sum_left = mixer->sum_left_; x.sum_right = mixer->sum_right_;
    }
}

void SnapshotAccess::restore(System& system, const SnapshotState& s) {
    auto& m = system.memory;
    auto& v = m.ppu;
    // The worker finishes what it was handed, and stays idle while its
    // state is replaced. The sound up to now closes as a batch.
    m.flush_sound();
    m.finish_audio();
    v.finish();

    system.cpu = s.cpu;
    m.clock.restore(s.now, s.events, {&Ppu::event, &Memory::timer_event, &Memory::dma_event, &Memory::serial_event, nullptr},
                    {&v, &m, &m, &m, nullptr});
    m.frames = s.frames; m.halt_cycles = s.halt_cycles;
    m.irq_line = s.irq_line; m.irq_unmasked = s.irq_unmasked; m.halted = s.halted;
    m.bios_latch = s.bios_latch; m.vram_obj_start = s.vram_obj_start; m.memory_control_ = s.memory_control;
    m.dirty = to_ppu(s.dirty);
    std::memcpy(m.ewram, s.ewram.data(), s.ewram.size());
    std::memcpy(m.vram, s.vram.data(), s.vram.size());
    m.iwram = s.iwram; m.palette = s.palette; m.oam = s.oam; m.io_ = s.io;
    m.irq_flags_ = s.irq_flags; m.irq_recent_ = s.irq_recent; m.buttons_ = s.buttons;
    m.mirror_if();
    m.init_io_reads();
    m.irq_recent_time_ = s.irq_recent_time;
    const auto history = [](Memory::IrqHistory& h, const SnapshotState::History& x) {
        h.time = x.time; h.values = x.values; h.count = x.count; h.before = x.before;
    };
    history(m.irq_request_, s.irq_request); history(m.irq_master_, s.irq_master);
    m.irq_request_rise_ = s.irq_request_rise; m.halt_entry_ = s.halt_entry; m.halt_tail_ = s.halt_tail;
    m.irq_stalled_ = s.irq_stalled; m.irq_stall_from_ = s.irq_stall_from;
    m.irq_watch_ = s.irq_watch; m.halt_in_access_ = s.halt_in_access;
    // The stage is clocked again from the next access on.
    m.irq_next_ = 0;
#ifdef GBX_GBA_RELAXED_TIMING
    // The line follows IE & IF and IME at once, see Memory::update_irq().
    m.irq_watch_ = false;
    m.irq_line = (m.ie() & m.irq_flags_ & 0x3fff) && m.ime();
#endif
    for (unsigned i = 0; i < 4; ++i) {
        const auto& t = s.timers[i];
        m.timers_[i] = {t.reload, t.control, t.counter, t.held, t.since, t.end, t.first, t.period, t.handled};
        const auto& d = s.dma[i];
        m.dma_[i] = {d.source, d.destination, d.count, d.control, d.start, d.latch,
                     d.gamepak_sequential, d.eeprom, d.paused};
    }
    m.timer_irq_ = s.timer_irq; m.timer_irq_time_ = s.timer_irq_time; m.prescaler_phase_ = s.prescaler_phase;
    m.fifo_quiet_until_ = 0;
    m.dma_latch_ = s.dma_latch; m.dma_pending_ = s.dma_pending; m.dma_deferred_ = s.dma_deferred;
    m.dma_bus_ = s.dma_bus; m.capture_ = s.capture; m.dma_running_ = false;
    for (unsigned i = 0; i < 2; ++i) {
        const auto& f = s.fifo[i];
        m.fifo_[i] = {f.data, f.read, f.size, f.sample};
    }
    const auto& p = s.psg;
    m.psg_ = {p.edge, p.step, p.on, p.reported, p.length, p.shadow, p.sweep_timer, p.sweep_enabled, p.negated};
    m.wave_played_ = s.wave_played;
    // The wait states and the prefetch's enable from WAITCNT and the memory
    // control; a new generation of them retags the JIT's blocks. Then the
    // buffer's own state.
    m.update_waits();
    const auto& q = s.prefetch;
    m.prefetch_ = {q.head, q.base, q.start, q.paused, q.cycles, q.running, q.stalled};
    m.gamepak_next_ = s.gamepak_next;
    m.lazy_pipeline = 0;
    m.target = kNever;
    m.stop = false;
    m.deadline = s.cpu_now;
    m.left = 0;

    auto& rtc = m.rtc;
    rtc.set(s.rtc.clock, s.now);
    rtc.phase_ = static_cast<Rtc::Phase>(s.rtc.phase); rtc.pins_ = s.rtc.pins; rtc.command_ = s.rtc.command;
    rtc.shift_ = s.rtc.shift; rtc.bits_ = s.rtc.bits; rtc.index_ = s.rtc.index; rtc.output_ = s.rtc.output;
    rtc.buffer_ = s.rtc.buffer;
    // The clock may differ from the save file now, as the backup memory may.
    rtc.written_ = m.gpio_page_ != nullptr;
    m.gpio_ = {s.gpio.data, s.gpio.direction, s.gpio.control};
    if (m.gpio_page_) m.update_gpio();

    auto& b = m.backup;
    const auto& sb = s.backup;
    b.type_ = sb.type; b.flash_state_ = static_cast<Backup::FlashState>(sb.flash_state);
    b.flash_id_ = sb.flash_id; b.flash_erase_ = sb.flash_erase; b.flash_bank_ = sb.flash_bank;
    b.eeprom_shift_ = sb.eeprom_shift; b.eeprom_count_ = sb.eeprom_count; b.eeprom_command_ = sb.eeprom_command;
    b.eeprom_read_bit_ = sb.eeprom_read_bit; b.eeprom_address_ = sb.eeprom_address;
    b.eeprom_ready_ = sb.eeprom_ready; b.eeprom_writing_ = sb.eeprom_writing; b.eeprom_buffer_ = sb.eeprom_buffer;
    // The storage past the state's size is as erased, as it was when the
    // state was taken: what decides the type writes it.
    std::memcpy(b.data_, sb.data.data(), sb.size);
    std::memset(b.data_ + sb.size, 0xff, kMaxBackup - sb.size);
    // The whole save memory may differ from the save file now.
    b.mark_written();

    v.frame_start_ = s.ppu.frame_start; v.frame_ = s.ppu.frame; v.affine_written_ = s.ppu.affine_written;
    v.held_ = to_ppu(s.ppu.held); v.open_ = s.ppu.open;
    v.next_event_ = s.events[kSlotPpu];
    // Every job handed over is done: the payload ring starts again, with
    // the job under way at its start.
    v.payload_head_ = v.job_start_ = 0;
    v.reused_before_ = v.submitted_;
    if (s.ppu.open) {
        const auto& j = s.job;
        auto& job = v.job_;
        job.line = j.line; job.affine_written = j.affine_written; job.drawing = j.drawing;
        job.writes = j.writes; job.frame = j.frame; job.registers = j.registers;
        job.payload = 0;
        // A page of palette RAM is 16 of the PPU's.
        unsigned count = 0;
        for (unsigned i = 0; i < j.pages; ++i) count += j.numbers[i] - kPalettePage < 2 ? 16 : 1;
        uint8_t* numbers = v.payload_;
        uint8_t* data = numbers + count;
        unsigned n = 0;
        for (unsigned i = 0; i < j.pages; ++i) {
            const unsigned page = j.numbers[i];
            const uint8_t* from = j.data.data() + size_t{i} * kPageBytes;
            if (page - kPalettePage < 2) {
                for (unsigned k = 0; k < 16; ++k) numbers[n++] = static_cast<uint8_t>(Ppu::kPalettePage + (page - kPalettePage) * 16 + k);
                std::memcpy(data, from, kPageBytes);
            } else {
                numbers[n++] = static_cast<uint8_t>(page < kVramPages ? page : Ppu::kOamPage + page - kOamPage);
                std::memcpy(data, from, kPageBytes);
            }
            data += kPageBytes;
        }
        job.pages = static_cast<uint8_t>(count);
        v.payload_head_ = static_cast<size_t>(data - v.payload_);
        std::copy_n(j.timed.begin(), j.writes, v.writes_.begin());
    }
    v.quiet();
    v.mirror_vcount(m.clock.now);
    if (auto* renderer = v.renderer_.get()) {
        const auto& d = s.renderer;
        std::memcpy(renderer->vram_, m.vram, 96 * 1024);
        renderer->palette_ = m.palette;
        renderer->oam_ = m.oam;
        for (unsigned w = 0; w < kDirtyWords; ++w) {
            for (uint32_t bits = d.differs[w]; bits; bits &= bits - 1) {
                const unsigned page = w * 32 + static_cast<unsigned>(std::countr_zero(bits));
                uint8_t* to = page < kVramPages ? renderer->vram_ + page * kPageBytes
                              : page < kOamPage ? renderer->palette_.data() + (page - kPalettePage) * kPageBytes
                                                : renderer->oam_.data() + (page - kOamPage) * kPageBytes;
                std::memcpy(to, renderer_page(s, page).data(), kPageBytes);
            }
        }
        renderer->refresh();
        renderer->frame_id_ = d.frame_id; renderer->enabled_lines_ = d.enabled_lines; renderer->shown_ = d.shown;
        renderer->reference_ = d.reference; renderer->mosaic_reference_ = d.mosaic_reference;
        std::copy(d.frame.begin(), d.frame.end(), renderer->frame_);
    }

    // The flush above closed the open batch. The next one starts at the
    // restored time, and the next run hands it over.
    m.sound_end_ = s.now;
    if (auto* mixer = m.mixer_.get()) {
        const auto& x = s.mixer;
        mixer->regs_ = x.regs; mixer->wave_ram_ = x.wave_ram; mixer->on_ = x.on;
        for (unsigned i = 0; i < 2; ++i)
            mixer->squares_[i] = {x.squares[i].next, x.squares[i].spacing, x.squares[i].step};
        mixer->wave_ = {x.wave_next, x.wave_spacing, x.wave_index};
        mixer->sample_ = x.sample;
        mixer->noise_ = {x.noise_next, x.noise_spacing, x.lfsr, x.noise_high};
        for (unsigned i = 0; i < 4; ++i)
            mixer->envelopes_[i] = {x.envelopes[i].volume, x.envelopes[i].timer, x.envelopes[i].running};
        mixer->digital_ = x.digital; mixer->fifo_ = x.fifo;
        mixer->level_left_ = x.level_left; mixer->level_right_ = x.level_right;
        mixer->held_left_ = x.held_left; mixer->held_right_ = x.held_right;
        mixer->sample_at_ = x.sample_at; mixer->now_ = x.now; mixer->base_time_ = x.base_time;
        mixer->chunk_ = x.chunk; mixer->base_ = x.base; mixer->step_ = x.step; mixer->rate_ = x.rate;
        mixer->ppm_ = x.ppm; mixer->first_ = x.first; mixer->changes_ = x.changes;
        mixer->sum_left_ = x.sum_left; mixer->sum_right_ = x.sum_right;
        // As after a reset, the reader skips the frames not read yet.
        mixer->reset_at_.store(mixer->written_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        mixer->dropped_.store(0, std::memory_order_relaxed);
        // The producer's count of committed frames goes on from the
        // restored position, see Mixer::submit().
        mixer->scheduled_time_ = x.now;
        mixer->scheduled_position_ = x.base + (x.now - x.base_time) * x.step;
        mixer->scheduled_step_ = x.step; mixer->scheduled_rate_ = x.rate; mixer->scheduled_ppm_ = x.ppm;
        // The output rate and speed are the caller's, not the state's. A
        // state saved without one holds the FIFOs' samples the mixer heard
        // before.
        if (x.rate != m.audio_rate_) m.append_setting(sound::Event::rate, m.audio_rate_);
        if (!x.rate && m.audio_rate_) m.fifo_resume();
        if (x.ppm != m.audio_speed_) m.append_setting(sound::Event::speed, static_cast<uint32_t>(m.audio_speed_));
    }
}

bool System::capture(Snapshot& out) {
    if (!out.allocated() || !memory.rom) return false;
    // The sound up to now goes to the mixer; then the worker finishes it and
    // the lines, and stays idle while its state is copied.
    memory.flush_sound();
    memory.finish_audio();
    memory.ppu.finish();
    SnapshotAccess::capture(*this, *out.state_);
    out.identity_ = rom_identity(*memory.rom);
    out.valid_ = true;
    return true;
}

bool System::restore(const Snapshot& in, bool other_rom) {
    if (!in.valid() || !memory.rom) return false;
    if (!other_rom && in.identity_ != rom_identity(*memory.rom)) return false;
    SnapshotAccess::restore(*this, *in.state_);
    if (jit_) jit_->forget_ram_code();
    return true;
}

Snapshot::Snapshot() = default;
Snapshot::~Snapshot() { release(); }

void Snapshot::release() {
    platform::free_external(block_);
    block_ = nullptr;
    state_ = nullptr;
    valid_ = false;
}

bool Snapshot::allocate() {
    if (state_) return true;
    // External memory is only word aligned; the state holds 64-bit fields.
    constexpr size_t align = alignof(SnapshotState);
    block_ = platform::allocate_external(sizeof(SnapshotState) + align);
    if (!block_) return false;
    const auto base = (reinterpret_cast<uintptr_t>(block_) + align - 1) & ~uintptr_t{align - 1};
    state_ = new (reinterpret_cast<void*>(base)) SnapshotState{};
    return true;
}

BackupType Snapshot::backup_type() const { return state_ && valid_ ? state_->backup.type : BackupType::unknown; }

std::span<const uint8_t> Snapshot::backup() const {
    return state_ && valid_ ? std::span<const uint8_t>(state_->backup.data).first(state_->backup.size)
                            : std::span<const uint8_t>{};
}

std::span<const uint16_t> Snapshot::frame() const {
    return state_ && valid_ ? std::span<const uint16_t>(state_->renderer.frame) : std::span<const uint16_t>{};
}

size_t Snapshot::max_encoded() const {
    if (!state_) return 0;
    Measure measure(true);
    fields(*state_, measure);
    return kHeader + measure.size() + kMaxBackup + kTrailer;
}

size_t Snapshot::encoded_size() const {
    if (!valid_) return 0;
    Measure measure(false);
    fields(*state_, measure);
    return kHeader + measure.size() + backup().size() + kTrailer;
}

size_t Snapshot::encode(std::span<uint8_t> out) const {
    if (!valid_ || out.size() < max_encoded()) return 0;
    return encode(out, nullptr, nullptr);
}

size_t Snapshot::encode(std::span<uint8_t> buffer, PieceSink sink, void* context) const {
    const size_t size = encoded_size();
    if (!size) return 0;
    const size_t payload = size - kHeader - kTrailer;
    const auto backup = this->backup();
    uint8_t h[kHeader];
    std::memcpy(h, kMagic, sizeof kMagic);
    put16(h + 8, kVersion);
    put16(h + 10, kSystem);
    std::copy(identity_.begin(), identity_.end(), h + 12);
    put32(h + 36, static_cast<uint32_t>(backup.size()));
    put32(h + 40, static_cast<uint32_t>(payload));
    Writer writer(buffer, sink, context);
    writer.bytes(h);
    fields(*state_, writer);
    writer.bytes(backup);
    return writer.written() == kHeader + payload && writer.finish() ? size : 0;
}

Snapshot::Status Snapshot::inspect(std::span<const uint8_t> bytes, RomIdentity* identity) {
    encoding::Whole whole{bytes};
    return inspect(bytes.size(), encoding::Whole::next, &whole, identity);
}

Snapshot::Status Snapshot::inspect(size_t size, PieceSource source, void* context, RomIdentity* identity) {
    Reader reader(source, context, size);
    Header header;
    const auto status = read_header(reader, size, header);
    if (status == Status::unknown || status == Status::system || status == Status::version) return status;
    if (identity) *identity = header.identity;
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
    if (!state_) return Status::damaged;
    fields(*state_, reader);
    if (!reader.ok() || header.backup != Backup::size_of(state_->backup.type)) return Status::damaged;
    state_->backup.size = static_cast<uint32_t>(header.backup);
    reader.bytes(std::span(state_->backup.data).first(header.backup));
    if (!reader.ok() || reader.position() != kHeader + header.payload || !reader.check() || !plausible(*state_))
        return Status::damaged;
    identity_ = header.identity;
    valid_ = true;
    return Status::ok;
}

}  // namespace gbx::gba
