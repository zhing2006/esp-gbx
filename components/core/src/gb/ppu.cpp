#include "gbx/gb/ppu.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstring>
#include <new>

#include "gbx/gb/header.h"
#include "gbx/gb/memory.h"
#include "offload.h"
#include "renderer.h"

namespace gbx::gb {
namespace {
// Pan Docs Rendering: fine scroll, six-dot window restart, and 6..11 dots
// per OBJ, with LCDC and WX as mode 3 starts. No per-dot FIFO runs on
// either host core. window_bits: the LCDC bits that show the window, see
// Ppu::window_bits_. carry: the window shows from the start of the line,
// see Ppu::window_carry_; it restarts nowhere then, and the OBJ fetches
// keep the BG's tile grid. A DMG's window at WX 166 adds no restart, but
// the line takes at least 174 dots: Gambatte window/m2int_wxA6_*m3stat and
// m2int_wxA6_m0irq on dmg08, with SCX 0, 2, 3 and 5.
[[gnu::always_inline]] inline unsigned transfer_length(const Memory& m, std::span<const uint8_t> objects,
                                                       bool window_y, bool carry, unsigned bits, unsigned lcdc,
                                                       unsigned fine, unsigned wx) {
    const bool window = window_y && (lcdc & bits) == bits && wx < 167;
    const bool edge = window && wx == 166 && m.mode() == Mode::dmg;
    const bool restart = window && !edge && !carry;
    unsigned dots = 172 + fine + (restart ? 6 : edge && fine < 2 ? 2 - fine : 0);
    if (!(lcdc & 2)) return dots;
    int last_tile = -100;
    for (auto object : objects) {
        int x = m.oam[object * 4 + 1];
        if (x >= 168) continue;
        if (x == 0) { dots += last_tile == -1 ? 6 : 11; last_tile = -1; continue; }
        int coordinate = x - 8 + fine;
        if (restart && x - 8 >= static_cast<int>(wx) - 7) coordinate = x - 8 - (static_cast<int>(wx) - 7) + 256;
        const int tile = coordinate >= 0 ? coordinate / 8 : -1;
        dots += 6;
        if (tile != last_tile) dots += std::max(0, 5 - (coordinate & 7));
        last_tile = tile;
    }
    return dots;
}

// PPU timing, in ticks from the start of a line, where LY changes. The CPU
// sees it through its accesses, at the access edge 6 ticks into an M-cycle,
// and through interrupts, which it takes at the first M-cycle boundary at
// or after the request, or, waking from HALT, at the first boundary at
// least kHaltWake after it (System::boundary()). A write to LCDC that
// enables the LCD starts line 0 at its access edge, and so does the boot
// ROM's, so lines start 2 ticks before an M-cycle boundary in either case.
// Reads then fall on the line start and every 8 ticks after it: a change
// is seen at the first read at or after it, and only the 8-tick window it
// falls in can be told. The windows below come from mooneye
// lcdon_timing-GS and lcdon_write_timing-GS (reads after enabling the LCD),
// and the points in them, and the interrupt times, from the cases named at
// each, verified on DMG-C and CPU CGB E unless said otherwise.
constexpr Tick kLine = 912, kFirstLine = 904;
// Mode 3 starts 162 ticks into a line, and 156 into the first line after
// the LCD is enabled. Mode 0 is readable when the transfer ends, but the
// HBlank source of the STAT line rises kHblankLead before: AGE stat-int and
// halt-m0-interrupt time the mode 0 interrupt, from a NOP slide and against
// HALT, 10 ticks before the next line's mode 2 is readable in 8-tick steps
// of SCX, and mooneye hblank_ly_scx_timing-GS and intr_2_mode0_timing
// against HALT. At double speed the CPU sees mode 0 one tick later and the
// interrupt lead is 1 tick (AGE spsw-mode0-cgbBCE and stat-int on CPU CGB
// E), matching the odd SCX values a 4-tick M-cycle tells apart.
constexpr Tick kTransfer = 162, kFirstTransfer = 156, kHblankLead = 5, kDoubleHblankLead = 1, kDoubleRead = 1;
// The OAM source of the STAT line rises 6 ticks before lines 1 to 143
// start, where the HBlank source falls: AGE stat-int takes the mode 2
// interrupt from a NOP slide at the boundary 6 ticks before the line, and
// mooneye intr_2_mode0_timing, intr_2_mode3_timing and
// intr_2_mode0_timing_sprites wake from HALT on it one M-cycle later. On a
// DMG it falls when mode 2 is readable, so a STAT write in mode 2 raises
// nothing (AGE stat-int), and LY=LYC takes over without a gap (mooneye
// stat_irq_blocking). A CGB's falls at the same time; its mode 2 is
// readable 4 ticks into the line (AGE stat-int at double speed).
constexpr Tick kOamEarly = 6, kModeTwo = 8, kCgbModeTwo = 4, kOamEnd = 8;
// Line 0 raises the OAM source and reads mode 2 later, 2 ticks into the
// line, until 8, or 6 on a CGB: mooneye intr_1_2_timing-GS against HALT,
// and Gambatte's lyc153int_m2irq and m2enable on dmg08 and cgb04c.
constexpr Tick kZeroOam = 2, kZeroOamEnd = 8, kCgbZeroOamEnd = 6;
// LY=LYC compares the new LY one tick into a line, for the flag and the
// interrupt. Gambatte's lycint and lycEnable cases on dmg08 take the LY=LYC
// interrupt in the same M-cycle as the line start, and those at double
// speed on cgb04c one tick into it.
constexpr Tick kCompare = 1;
// Line 144: the mode 1 source of the STAT line rises 2 ticks before the
// line starts, where the HBlank source falls (AGE stat-int from a NOP
// slide; mooneye intr_1_2_timing-GS against HALT), and the VBlank
// interrupt, mode 1 and, on a DMG, the OAM source come 4 ticks into it
// (mooneye vblank_stat_intr-GS, di_timing-GS and halt_ime1_timing2-GS,
// where HALT and a NOP slide take VBlank at the same boundary). A CGB's OAM
// source rises with LY=LYC, before VBlank (mooneye
// misc/ppu/vblank_stat_intr-C).
constexpr Tick kVblank = 4, kMode1Early = 2;
// Line 153 reads LY 0 2 ticks in, but compares 153 then, nothing from 10
// and 0 from 18. Gambatte ly0/lycint152_ly153_1-3 read LY 152, 153 and 0
// one M-cycle apart, timed from the LYC=152 request, on dmg08; TCAGBD 8.9.1
// describes the gap.
constexpr Tick kLy153 = 2, kLycOff = 10, kLyc0 = 18;
// A CPU CGB E reads LY 153 until 10 ticks into line 153, at both speeds:
// AGE ly-cgbE, ly-ncmE and lcd-align-ly-cgbE read 153 at 8 ticks and 0 at
// 10 across their LCD alignments.
constexpr Tick kCgbLy0 = 10;
// The renderer's dot 0, before mode 3 starts: the dots that changes in mode 3
// carry count from there, see Ppu::change().
constexpr Tick kRenderLead = 10;
// WY matching LY in mode 3 lets the window start only at pixels it would
// start at this many dots after the match, see Ppu::check_wy().
constexpr Tick kWindowLead = 2, kCgbWindowLead = 6;
// The HBlank source's interrupt request, at single speed, reaches IF as
// reads see it kHblankSettle after the CPU can take it: Gambatte
// m2int_m0irq, m0int_m0irq and lyc0int_m0irq read IF without it 1 tick
// before mode 0 is readable and with it 7 ticks after, in steps of SCX, on
// dmg08 and cgb04c. Until then a DMG's STAT write, or until kLycSettle its
// LYC write, still decides whether the source's rise raises the STAT line:
// Gambatte m0enable/disable_*, lycdisable_ff41_* and lycdisable_ff45_* on
// dmg08, which turn a source off around it. An IF write until
// kIfWriteSettle leaves the request in IF: m2int_m0irq_scx3_ifw and
// m2int_m0irq_scx4_ifw on dmg08 and cgb04c. Line 144's mode 1 source,
// rising while LY=LYC holds the line up, raises it if LY=LYC falls within
// kHblankSettle, and IF shows that only then: Gambatte m1/lycint143_m1irq*
// and m1/lycint_m1intirq* on dmg08 and cgb04c.
constexpr Tick kHblankSettle = 6, kLycSettle = 5, kIfWriteSettle = 4;
// Mode 2 checks OAM entry i against LY with the OBJ height as of kScanStart
// + 4i ticks into the line: Gambatte sprites/late_sizechange* on dmg08,
// which change LCDC bit 2 around entries 0, 1, 2, 9 and 39.
constexpr Tick kScanStart = 4;
}  // namespace

// The changes from the start of a visible line until mode 3, in ticks from
// it; changes at the same time apply together. Lines 1 to 143 raised the
// OAM source before they started, see Phase::oam.
enum : uint8_t { kOamOn = 1, kMode2 = 2, kOamOff = 4, kCompareLy = 8 };
struct LineEdge { uint8_t at, effects; };
struct LineEdges { std::array<LineEdge, 4> edges; uint8_t count; };
namespace {
LineEdges make_edges(std::initializer_list<LineEdge> list) {
    LineEdges e{};
    for (auto edge : list) {
        unsigned i = 0;
        while (i < e.count && e.edges[i].at < edge.at) ++i;
        if (i < e.count && e.edges[i].at == edge.at) { e.edges[i].effects |= edge.effects; continue; }
        for (unsigned j = e.count; j > i; --j) e.edges[j] = e.edges[j - 1];
        e.edges[i] = edge; ++e.count;
    }
    return e;
}
// [cgb][line 0]
const LineEdges kEdges[2][2] = {
    {make_edges({{uint8_t(kCompare), kCompareLy}, {uint8_t(kModeTwo), kMode2}, {uint8_t(kOamEnd), kOamOff}}),
     make_edges({{uint8_t(kCompare), kCompareLy}, {uint8_t(kZeroOam), kOamOn | kMode2},
                 {uint8_t(kZeroOamEnd), kOamOff}})},
    {make_edges({{uint8_t(kCompare), kCompareLy}, {uint8_t(kCgbModeTwo), kMode2}, {uint8_t(kOamEnd), kOamOff}}),
     make_edges({{uint8_t(kCompare), kCompareLy}, {uint8_t(kZeroOam), kOamOn | kMode2},
                 {uint8_t(kCgbZeroOamEnd), kOamOff}})},
};
}  // namespace

Ppu::Ppu() = default;
Ppu::~Ppu() = default;
bool Ppu::initialize(Offload& offload) {
    if (!renderer_) renderer_.reset(new (std::nothrow) Renderer(offload));
    return renderer_ != nullptr;
}
void Ppu::reset(Memory& m) {
    if (job_) { renderer_->submit(); job_ = nullptr; }
    if (renderer_) { renderer_->command(Row::Kind::clear, 0, m.mode()); renderer_->finish(); }
    memory_ = &m; line_ = 153; mode_ = 1; coincidence_ = flag_ = true; compared_ = 0; stat_line_ = false;
    hblank_irq_ = oam_irq_ = mode1_irq_ = hblank_requested_ = mode1_requested_ = mode1_pending_ = false;
    hblank_at_ = mode1_at_ = kNever;
    first_line_ = blank_ = window_y_ = window_carry_ = transfer_carry_ = hdma_ = false;
    carry_count_ = edge_ = 0;
    window_bits_ = m.mode() == Mode::cgb ? 0x20 : 0x21; cgb_ = m.ppu_color(); lock_at_ = kNever; oam_dirty_ = true;
    line_start_ = transfer_start_ = 0; transfer_dots_ = 0; dirty_ = ~uint64_t{0}; objects_height_ = 0;
    reset_palettes();
    // The boot ROM leaves line 153 ending 2 ticks before an M-cycle boundary,
    // as its write to LCDC started the lines, see kTransfer.
    schedule(318, Phase::line); arm();
}
// The palettes the CGB boot ROM leaves (Pan Docs, LCD Color Palettes): every
// BG color white, the OBJ colors uninitialized, here zero. In compatibility
// mode, BG palette 0 and OBJ palettes 0 and 1 hold the compatibility
// palettes, see compatibility_palettes().
void Ppu::reset_palettes() {
    if (!cgb_) return;
    *cgb_ = {};
    auto& palettes = cgb_->palettes;
    for (unsigned i = 0; i < 64; i += 2) { palettes[i] = 0xff; palettes[i + 1] = 0x7f; }
    if (memory_->mode() != Mode::compat) return;
    const auto colors = compatibility_palettes(memory_->rom());
    for (unsigned i = 0; i < 12; ++i) {
        const unsigned at = i < 4 ? i * 2 : 64 + (i - 4) * 2;
        palettes[at] = static_cast<uint8_t>(colors[i]); palettes[at + 1] = static_cast<uint8_t>(colors[i] >> 8);
    }
}
void Ppu::schedule(Tick when, Phase phase) { phase_ = phase; phase_at_ = when; }
const LineEdges& Ppu::line_edges() const { return kEdges[cgb_ != nullptr][line_ == 0]; }
Tick Ppu::line_end() const { return line_start_ + (first_line_ ? kFirstLine : kLine); }
// Schedules the first phase from phase_ on that needs an event: one that
// may raise the STAT interrupt line, as the OAM source before a line and on
// line 0, LY=LYC and the mode 0 edge do while their source is enabled, and
// mode 0 while an HBlank DMA runs. Line, mode 3, VBlank and settle phases
// always run as events, and so does the CGB's line-144 OAM phase, once a
// frame, so that the DMG's phases need no check for it. The phases skipped
// follow at fixed times from the start of the line.
void Ppu::arm() {
    const auto& m = *memory_;
    const auto enables = m.io[0x41];
    auto phase = phase_;
    auto when = phase_at_;
    switch (phase) {
    case Phase::edge: {
        // Line 0 compares LY=LYC too, but LY has been 0 since line 153, so
        // that compare raises nothing.
        unsigned raising = enables & 0x20 ? kOamOn : 0;
        if ((enables & 0x40) && m.io[0x44] == m.io[0x45]) raising |= kCompareLy;
        const auto& line = line_edges();
        unsigned edge = edge_;
        if (raising) while (edge < line.count && !(line.edges[edge].effects & raising)) ++edge;
        else edge = line.count;
        if (edge == line.count) { phase = Phase::transfer; when = line_start_ + kTransfer; }
        else if (edge != edge_) when = line_start_ + line.edges[edge].at;
        break;
    }
    case Phase::hblank:
        if ((enables & 8) || hdma_) break;
        phase = Phase::oam; when = line_end() - (line_ < 143 ? kOamEarly : kMode1Early);
        [[fallthrough]];
    case Phase::oam:
        if (!(enables & (line_ == 143 ? 0x10 : 0x20))) { phase = Phase::line; when = line_end(); }
        break;
    case Phase::lyc_off:
        phase = Phase::lyc; when = line_start_ + kLyc0;
        [[fallthrough]];
    case Phase::lyc:
        // Line 144's LY=LYC phase also settles its mode 1 source, see
        // compare_ly().
        if (((enables & 0x40) && m.io[0x44] == m.io[0x45]) || mode1_pending_ ||
            (cgb_ && line_ == 144 && (enables & 0x20)))
            break;
        if (line_ == 144) { phase = Phase::vblank; when = line_start_ + kVblank; }
        else { phase = Phase::line; when = line_start_ + kLine; }
        break;
    default: break;
    }
    armed_ = when;
    memory_->clock.schedule(0, when, event, this);
}
// Applies the phases before the scheduled one that are due by now, in order,
// as their events would have. They only change the mode, the LY=LYC flag,
// the window line and the VRAM lock, or lower the STAT interrupt line. Every
// access that observes those syncs first. Device tests that clear the
// scheduler stop the PPU, since the scheduled phase never comes.
void Ppu::sync() {
    for (const auto now = memory_->clock.now; phase_at_ < armed_ && phase_at_ <= now;) advance();
}
void Ppu::event(void* context) {
    auto& ppu = *static_cast<Ppu*>(context);
    const auto now = ppu.memory_->clock.now;
    if (ppu.phase_at_ < now) ppu.skip(now);
    while (ppu.phase_at_ <= now) ppu.advance();
    ppu.arm();
}
// Applies the phases before the scheduled one at `now`. None of them could
// raise the STAT line, see arm(), and the scheduled phase updates it again,
// so on a visible line the edges before mode 3, and the phases from mode 0
// to the next line, need only their lasting changes, not a step each.
void Ppu::skip(Tick now) {
    auto& m = *memory_;
    if (phase_ == Phase::edge && offset(now) == kTransfer) {
        // Mode 3 sets the mode, the VRAM lock and the OAM source; only
        // LY=LYC is left, if its edge is, see apply_edge().
        if (!edge_) {
            if (!line_) window_y_ = false;
            compared_ = m.io[0x44]; flag_ = coincidence_ = compared_ == m.io[0x45];
        }
        phase_ = Phase::transfer; phase_at_ = now;
        return;
    }
    if (line_ < 143 && offset(now) == (first_line_ ? kFirstLine : kLine)) {
        if (phase_ == Phase::hblank) {
            // end_transfer(), with no HBlank DMA or HBlank source.
            mode_ = 0; lock_at_ = kNever;
            window_carry_ = !cgb_ && window_y_ && m.io[0x4b] == 166;
            hblank_at_ = m.double_speed() ? kNever : phase_at_; hblank_requested_ = false;
            phase_ = Phase::oam;
        }
        if (phase_ == Phase::oam) { hblank_irq_ = false; oam_irq_ = true; phase_ = Phase::line; phase_at_ = now; }
        if (phase_ == Phase::line) return;
    }
    while (phase_at_ < now) advance();
}
uint8_t Ppu::stat() {
    sync();
    return (memory_->io[0x41] & 0x78) | 0x80 | (in_transfer() ? 3 : mode_) | (flag_ ? 4 : 0);
}
// Mode 0 comes as the HBlank source rises, kHblankLead before it is
// readable, see kTransfer.
unsigned Ppu::hblank_lead() const {
    return static_cast<unsigned>(memory_->double_speed() ? kDoubleHblankLead : kHblankLead);
}
bool Ppu::in_transfer() const {
    if (mode_ != 0) return mode_ == 3;
    const Tick end = transfer_start_ + transfer_dots_ * 2 + (memory_->double_speed() ? kDoubleRead : 0);
    return memory_->clock.now < end;
}
// A CPU CGB E reading LY in the last tick before it increments sees the old
// and the new value ANDed: AGE lcd-align-ly-cgbE, verified on CPU CGB E,
// reads LY & (LY + 1) 2 ticks before the change and the old LY 4 ticks
// before, at both speeds.
uint8_t Ppu::ly_cgb() {
    const auto& m = *memory_;
    const uint8_t ly = m.io[0x44];
    // Only phases that run as events change LY and the line, see next_lyc(),
    // so they need no sync().
    if ((m.io[0x40] & 0x80) && line_ < 153 && line_end() - m.clock.now <= 2) return static_cast<uint8_t>(ly & (ly + 1));
    return ly;
}
// LY=LYC compares a new LY kCompare into its line, at an edge or the lyc
// phase, and 0 late in line 153, see advance(); never before LY becomes LYC.
// Only line and LY 153 phases change LY, and both always run as events. The
// current line ends at line_end(); every later line takes kLine.
Tick Ppu::next_lyc() const {
    const auto& m = *memory_;
    if (!(m.io[0x40] & 0x80)) return kNever;
    const unsigned lyc = m.io[0x45];
    if (m.io[0x44] == lyc) return m.clock.deadline(0);
    if (lyc > 153) return kNever;
    // The line whose comparison gives LYC: line 153 compares 0. On line 153,
    // LY is 0 or 153 in turn; the other value is a frame away.
    const unsigned target = lyc ? lyc : 153;
    unsigned ahead = (target + 154 - line_) % 154;
    if (!ahead && lyc) ahead = 154;
    const Tick start = ahead ? line_end() + Tick{ahead - 1} * kLine : line_start_;
    return start + (lyc ? kCompare : kLyc0);
}
// Whether the STAT line is up with these enables, the HBlank source and
// LY=LYC as given and the other sources as they are. A CGB raises line
// 144's OAM source after LY=LYC, a phase before VBlank, see Phase::lyc. The
// enable comes first, so that a disabled OAM source checks neither.
[[gnu::always_inline]] inline bool Ppu::stat_high(uint8_t enables, bool hblank, bool coincidence) const {
    return (coincidence && (enables & 0x40)) || (hblank && (enables & 8)) ||
        ((mode_ == 1 || mode1_irq_) && (enables & 0x10)) ||
        ((enables & 0x20) && (oam_irq_ || (line_ == 144 && mode_ == 1) ||
                              (cgb_ && line_ == 144 && phase_ == Phase::vblank)));
}
// unseen: a rise the CPU can take before IF shows it, see kHblankSettle.
// Without an enabled source the line stays low, which most lines of most
// games see.
[[gnu::always_inline]] inline void Ppu::update_stat(bool unseen) {
    if (memory_->io[0x41] & 0x78) raise_stat(unseen);
    else stat_line_ = false;
}
void Ppu::raise_stat(bool unseen) {
    const bool high = stat_high(memory_->io[0x41], hblank_irq_, coincidence_);
    if (high && !stat_line_) {
        if (unseen) memory_->request_unseen(2);
        else memory_->request_interrupt(2);
    }
    stat_line_ = high;
}
bool Ppu::hblank_settling(Tick window) const {
    return hblank_at_ != kNever && memory_->clock.now < hblank_at_ + window;
}
void Ppu::write_if() {
    auto& m = *memory_;
    if (!m.unseen_interrupts) return;
    const Tick now = m.clock.now;
    const bool kept = (hblank_requested_ && hblank_settling(kIfWriteSettle)) ||
        (mode1_requested_ && mode1_at_ != kNever && now < mode1_at_ + kIfWriteSettle);
    if (!kept) m.unseen_interrupts = 0;
}
// A DMG's STAT or LYC write while the HBlank source's rise settles, see
// kHblankSettle: the STAT line as written decides it. old and coincidence:
// the enables and LY=LYC before the write; quirk: a STAT write, which acts
// as if FF were written first (see write()).
void Ppu::settle_hblank(uint8_t old, bool coincidence, bool quirk) {
    auto& m = *memory_;
    const auto enables = m.io[0x41];
    // Raised by the write itself, then by the HBlank source.
    const bool line = stat_high(old, false, coincidence);
    const bool raised =
        !line && ((quirk && stat_high(0x78, false, coincidence)) || stat_high(enables, false, coincidence_));
    if (raised) m.request_interrupt(2);
    const bool rises = (enables & 8) && !raised && !stat_high(enables, false, coincidence_);
    if (rises && !hblank_requested_) {
        m.request_unseen(2);
        if (phase_ == Phase::oam) schedule(hblank_at_ + kHblankSettle, Phase::settle);
    }
    if (!rises && !raised && hblank_requested_ && (m.unseen_interrupts & 2)) {
        m.unseen_interrupts &= static_cast<uint8_t>(~2); ++m.interrupt_generation;
    }
    hblank_requested_ = rises;
    stat_line_ = stat_high(enables, hblank_irq_, coincidence_);
}
void Ppu::compare_ly() {
    compare(memory_->io[0x44]);
    // LY=LYC fell while line 144's mode 1 source settles, see
    // kHblankSettle: the source raises the line.
    if (mode1_pending_ && stat_line_ && stat_at_ < mode1_at_ + kHblankSettle &&
        !stat_high(memory_->io[0x41] & ~0x10, hblank_irq_, coincidence_)) {
        memory_->request_unseen(2); mode1_requested_ = true;
    }
    mode1_pending_ = false;
}
void Ppu::compare(uint16_t ly) { compared_ = ly; flag_ = coincidence_ = ly == memory_->io[0x45]; update_stat(); }
// The edges at the current phase's time, then the next edge or mode 3.
void Ppu::apply_edge() {
    const auto& line = line_edges();
    const unsigned effects = line.edges[edge_].effects;
    if (++edge_ < line.count) schedule(line_start_ + line.edges[edge_].at, Phase::edge);
    else schedule(line_start_ + kTransfer, Phase::transfer);
    if (effects & kOamOn) oam_irq_ = true;
    if (effects & kOamOff) oam_irq_ = false;
    if (effects & kMode2) { mode_ = 2; lock_at_ = line_start_ + 156; }
    // WY matching LY counts from line 0's compare on (Gambatte window/late_wy,
    // which writes WY at the start of line 0, on dmg08 and cgb04c).
    if ((effects & kCompareLy) && line_ == 0) window_y_ = false;
    if (effects & kCompareLy) compare_ly();
    else update_stat();
}
// A VBlank line's mode 1; line 144 requests the VBlank interrupt.
void Ppu::begin_line() {
    auto& m = *memory_;
    mode_ = 1; hblank_irq_ = false; update_stat();
    if (line_ != 144) return;
    // Line 144's mode 1 request has settled, see compare_ly().
    if (mode1_requested_) m.show_interrupts(2);
    m.request_interrupt(1); ++m.frames;
    if (renderer_ && !skipping_) renderer_->command(Row::Kind::frame, m.frames, m.mode());
    blank_ = false;
    schedule(line_start_ + kLine, Phase::line);
}
unsigned Ppu::select_objects(std::array<uint8_t, 10>& objects) {
    const auto& m = *memory_;
    const unsigned height = m.io[0x40] & 4 ? 16 : 8;
    // The height changed while mode 2 checked the entries.
    const Tick scan = line_start_ + kScanStart;
    if (size_at_ > scan && size_at_ < scan + 160) return select_objects(objects, height);
    if (objects_height_ != height) {
        line_objects_.fill(0);
        for (unsigned i = 0; i < 40; ++i) {
            const int top = static_cast<int>(m.oam[i * 4]) - 16;
            for (int y = std::max(top, 0), end = std::min(top + static_cast<int>(height), 144); y < end; ++y)
                line_objects_[y] |= uint64_t{1} << i;
        }
        objects_height_ = height;
    }
    // The first ten objects overlapping LY in OAM order.
    assert(m.io[0x44] < line_objects_.size());
    unsigned count = 0;
    for (auto set = line_objects_[m.io[0x44]]; set && count < 10; set &= set - 1)
        objects[count++] = static_cast<uint8_t>(std::countr_zero(set));
    // Stable insertion sort: DMG resolves smaller X first, then OAM index.
    for (unsigned i = 1; i < count; ++i) {
        const auto object = objects[i]; unsigned j = i;
        while (j && m.oam[objects[j - 1] * 4 + 1] > m.oam[object * 4 + 1]) {
            objects[j] = objects[j - 1]; --j;
        }
        objects[j] = object;
    }
    return count;
}
// The entries checked before the height changed take the old one.
unsigned Ppu::select_objects(std::array<uint8_t, 10>& objects, unsigned height) {
    const auto& m = *memory_;
    const int ly = m.io[0x44];
    const Tick scan = line_start_ + kScanStart;
    unsigned count = 0;
    for (unsigned i = 0; i < 40 && count < 10; ++i) {
        const int top = static_cast<int>(m.oam[i * 4]) - 16;
        const int h = static_cast<int>(scan + 4 * i < size_at_ ? height ^ 24 : height);
        if (ly >= top && ly < top + h) objects[count++] = static_cast<uint8_t>(i);
    }
    for (unsigned i = 1; i < count; ++i) {
        const auto object = objects[i]; unsigned j = i;
        while (j && m.oam[objects[j - 1] * 4 + 1] > m.oam[object * 4 + 1]) { objects[j] = objects[j - 1]; --j; }
        objects[j] = object;
    }
    return count;
}
void Ppu::begin_transfer() {
    auto& m = *memory_;
    mode_ = 3; lock_at_ = m.clock.now; oam_irq_ = hblank_irq_ = false; update_stat();
    const auto count = select_objects(objects_);
    object_count_ = static_cast<uint8_t>(count);
    transfer_start_ = m.clock.now;
    transfer_lcdc_ = m.io[0x40]; transfer_wx_ = m.io[0x4b]; fine_ = m.io[0x43] & 7; timing_count_ = 0;
    // WY matching LY, see check_wy(): mode 3 starts long after a CGB
    // compares this line, and after a DMG's LY=LYC compare.
    window_from_ = 0;
    if (!window_y_ && (cgb_ ? line_ : compared_) == m.io[0x4a]) window_y_ = true;
    transfer_carry_ = window_carry_ && (transfer_lcdc_ & window_bits_) == window_bits_;
    const auto counted = carry_count_;
    window_carry_ = false; carry_count_ = 0;
    // No LCDC or WX write retimes it yet, see time_transfer().
    transfer_dots_ = transfer_length(m, std::span<const uint8_t>(objects_).first(count), window_y_, transfer_carry_,
                                     window_bits_, transfer_lcdc_, fine_, transfer_wx_);
    if (renderer_ && !skipping_) {
        auto& r = renderer_->acquire(); job_ = &r;
        r.y = static_cast<uint8_t>(line_); r.window_y = window_y_; r.window_from = 0;
        r.window_carry = transfer_carry_; r.window_count = counted;
        r.blank = blank_; r.objects = objects_; r.count = static_cast<uint8_t>(count);
        r.double_speed = m.double_speed();
        r.oam_changed = oam_dirty_;
        if (oam_dirty_) { r.oam = m.oam; oam_dirty_ = false; }
        std::memcpy(r.registers.data(), m.io.data() + 0x40, 12);
        r.fine = m.io[0x43] & 7; r.dirty = dirty_;
        for (auto dirty = dirty_; dirty; dirty &= dirty - 1) {
            const auto offset = std::countr_zero(dirty) * 128;
            std::memcpy(r.vram.data() + offset, m.vram.data() + offset, 128);
        }
        dirty_ = 0;
        if (cgb_) fill_cgb_row(&r);
    }
    schedule(m.clock.now + (transfer_dots_ * 2 - hblank_lead()), Phase::hblank);
}
// The length of mode 3 from what the line had as it started, and the
// change in the timeline the LCDC and WX writes since then make, which the
// renderer draws from too (make_timeline()).
void Ppu::time_transfer() {
    const auto objects = std::span<const uint8_t>(objects_).first(object_count_);
    transfer_dots_ = transfer_length(*memory_, objects, window_y_, transfer_carry_, window_bits_, transfer_lcdc_, fine_,
                                     transfer_wx_);
    if (timing_count_ || window_from_) retime();
}
void Ppu::retime() {
    const auto& m = *memory_;
    const auto end = [&](bool changes) {
        const auto reg = [&](unsigned index, int dot) {
            uint8_t value = index ? transfer_wx_ : transfer_lcdc_;
            if (changes)
                for (unsigned i = 0; i < timing_count_; ++i)
                    if (timing_[i].dot <= dot && timing_[i].address == (index ? 0x4b : 0x40)) value = timing_[i].value;
            return value;
        };
        Timeline t;
        const WindowInputs window{window_y_, transfer_carry_, !cgb_, changes ? window_from_ : 0, window_bits_};
        make_timeline(t, m.oam.data(), objects_, object_count_, static_cast<int>(fine_), window, reg);
        return t.delayed(18 + static_cast<int>(fine_) + 159);
    };
    transfer_dots_ = static_cast<unsigned>(static_cast<int>(transfer_dots_) + end(true) - end(false));
}
// The LY that WY compares with. A DMG compares the LY that LY=LYC does
// (compared_), and a CGB the next line's from 2 ticks before it starts,
// with no result in the 6 ticks before that. Gambatte's window/arg/late_wy
// cases, on dmg08 and cgb04c, write WY around the start of a line and in
// mode 3.
uint16_t Ppu::window_ly() const {
    if (!cgb_) return compared_;
    const Tick now = memory_->clock.now, end = line_end();
    if (now >= end - 2) return static_cast<uint16_t>(line_ + 1);
    return now >= end - 8 ? kNoCompare : line_;
}
// Once WY matches LY, the window shows on the lines that follow until line 0
// (Pan Docs, Window). Mode 3 checks the WY it starts with and a write
// checks the old WY and the new; LY changes in between only in mode 0 and
// 2, so no match goes unseen. A write that matches in mode 3 lets the
// window start only at the pixels it reaches kWindowLead after the write.
void Ppu::check_wy(bool written) {
    auto& m = *memory_;
    if (window_y_ || !(m.io[0x40] & 0x80) || window_ly() != m.io[0x4a]) return;
    window_y_ = true;
    if (written && mode_ == 3) window_matched();
}
void Ppu::window_matched() {
    auto& m = *memory_;
    window_from_ = static_cast<uint16_t>((m.clock.now - transfer_start_ + kRenderLead) / 2 +
                                         (cgb_ ? kCgbWindowLead : kWindowLead));
    if (job_) { auto& r = *static_cast<Row*>(job_); r.window_y = true; r.window_from = window_from_; }
    time_transfer();
    schedule(transfer_start_ + (transfer_dots_ * 2 - hblank_lead()), Phase::hblank); arm();
}
// VRAM bank 1 follows bank 0 in the row. The palettes cannot change in mode
// 3 (Pan Docs, LCD Color Palettes), so the row carries them only when they
// changed since the last one.
void Ppu::fill_cgb_row(void* job) {
    auto& r = *static_cast<Row*>(job);
    auto& c = *cgb_;
    r.dirty1 = c.dirty1;
    for (auto dirty = c.dirty1; dirty; dirty &= dirty - 1) {
        const auto offset = std::countr_zero(dirty) * 128;
        std::memcpy(r.vram.data() + 0x2000 + offset, memory_->vram1() + offset, 128);
    }
    c.dirty1 = 0;
    r.palettes_changed = c.palettes_dirty;
    if (c.palettes_dirty) { r.palettes = c.palettes; c.palettes_dirty = false; }
}
void Ppu::end_transfer() {
    // Retain the row until the line boundary so trailing output-stage writes
    // can still be recorded without scheduling another per-line callback.
    const Tick at = phase_at_;
    mode_ = 0; lock_at_ = kNever; hblank_irq_ = true;
    window_carry_ = !cgb_ && window_y_ && memory_->io[0x4b] == 166;
    const bool was = stat_line_, settles = !memory_->double_speed();
    update_stat(settles);
    hblank_at_ = settles ? at : kNever; hblank_requested_ = settles && !was && stat_line_;
    // IF shows the request once it settles, see kHblankSettle.
    if (hblank_requested_) schedule(at + kHblankSettle, Phase::settle);
    else schedule(line_end() - (line_ < 143 ? kOamEarly : kMode1Early), Phase::oam);
    // An HBlank DMA moves a block at the start of mode 0 (Pan Docs, HBlank
    // DMA); the mode 0 phase is an event while one runs, see arm().
    if (hdma_) memory_->hdma_block(true);
}
void Ppu::advance() {
    auto& m = *memory_;
    stat_at_ = phase_at_;
    // The times are explained at kTransfer.
    switch (phase_) {
    case Phase::transfer: begin_transfer(); break;
    case Phase::hblank: end_transfer(); break;
    case Phase::edge: apply_edge(); break;
    case Phase::settle:
        if (hblank_requested_) m.show_interrupts(2);
        schedule(line_end() - (line_ < 143 ? kOamEarly : kMode1Early), Phase::oam); break;
    case Phase::oam:
        if (line_ == 143) { advance_vblank(); break; }
        // The next line's OAM source rises as the HBlank source falls, see
        // kOamEarly.
        hblank_irq_ = false; oam_irq_ = true; update_stat();
        schedule(line_end(), Phase::line); break;
    case Phase::line:
        if (job_) { renderer_->submit(); job_ = nullptr; }
        line_ = (line_ + 1) % 154; m.io[0x44] = static_cast<uint8_t>(line_);
        line_start_ = m.clock.now; first_line_ = false;
        if (line_ >= 144) { advance_vblank(); break; }
        // LY=LYC drops until mode 2 compares again, except into line 0: LY
        // has been 0 since line 153, so with LYC=0 the STAT line stays up
        // and the interrupt does not come a second time. Gambatte
        // lyc0int_m0irq_1 and lyc0int_m0irq_ds_1, verified on dmg08 and
        // cgb04c, see no request after line 153's until line 1's mode 0.
        // Shantae's LYC=0 handler runs into line 6, and a second request
        // there turns the objects off for the frame.
        if (line_) { compared_ = kNoCompare; flag_ = false; }
        else mode1_irq_ = false;
        if (window_carry_ && (m.io[0x40] & window_bits_) == window_bits_) ++carry_count_;
        edge_ = 0; schedule(line_start_ + kCompare, Phase::edge);
        update_stat();
        break;
    default: advance_vblank(); break;
    }
}
// The phases from line 143's mode 1 source to line 0, a few a frame.
void Ppu::advance_vblank() {
    auto& m = *memory_;
    switch (phase_) {
    case Phase::oam:
        // Line 144's mode 1 source, which settles while LY=LYC holds the
        // line up, see compare_ly().
        hblank_irq_ = false; mode1_irq_ = true; update_stat();
        mode1_at_ = m.double_speed() ? kNever : phase_at_; mode1_requested_ = false;
        mode1_pending_ = mode1_at_ != kNever && (m.io[0x41] & 0x10) &&
            stat_high(m.io[0x41] & ~0x10, false, coincidence_);
        schedule(line_end(), Phase::line); break;
    case Phase::line:
        compared_ = kNoCompare; flag_ = false;
        // Line 144's mode 1 source rose before the line, see kMode1Early,
        // and VBlank comes a little later, see kVblank.
        if (line_ == 144) { mode1_irq_ = true; hblank_irq_ = false; update_stat(); }
        else begin_line();
        schedule(line_start_ + (line_ == 153 ? kLy153 : kCompare), line_ == 153 ? Phase::ly153 : Phase::lyc);
        break;
    case Phase::vblank: begin_line(); break;
    case Phase::ly153:
        // LY reads 0, but the comparison still takes 153. A CGB's LY reads
        // 0 a little later, see kCgbLy0.
        if (!cgb_) m.io[0x44] = 0;
        compare(153);
        schedule(line_start_ + (cgb_ ? kCgbLy0 : kLycOff), cgb_ ? Phase::ly0 : Phase::lyc_off); break;
    case Phase::ly0: m.io[0x44] = 0; schedule(line_start_ + kLycOff, Phase::lyc_off); break;
    case Phase::lyc_off: compare(kNoCompare); schedule(line_start_ + kLyc0, Phase::lyc); break;
    // Phase::lyc. Line 144's VBlank follows; on a CGB, the OAM STAT source
    // has risen by then, see stat_high().
    default:
        if (line_ == 144) schedule(line_start_ + kVblank, Phase::vblank);
        else schedule(line_start_ + kLine, Phase::line);
        compare_ly();
        break;
    }
}
Tick Ppu::next_vblank() const {
    const auto& m = *memory_;
    if (!(m.io[0x40] & 0x80)) return kNever;
    if (phase_ == Phase::vblank) return m.clock.deadline(0);
    if (line_ == 144 && phase_ == Phase::lyc) return line_start_ + kVblank;
    // Every line takes kLine, except a first line after the LCD is enabled,
    // see line_end(). The event is kVblank into line 144.
    if (line_ < 144) return line_end() + Tick{143u - line_} * kLine + kVblank;
    return line_start_ + Tick{154u - line_ + 144} * kLine + kVblank;
}
bool Ppu::in_frame() const {
    // Line 144 delivers the frame kVblank in, at the VBlank event.
    return (memory_->io[0x40] & 0x80) &&
        (line_ < 144 || (line_ == 144 && (phase_ == Phase::vblank || phase_ == Phase::lyc)));
}
bool Ppu::vram_blocked(bool write) {
    sync();
    return write ? in_transfer() : in_transfer() || memory_->clock.now >= lock_at_;
}
bool Ppu::oam_blocked(bool write) {
    if (!(memory_->io[0x40] & 0x80)) return false;
    sync();
    if (in_transfer()) return true;
    if (!write) return mode_ == 2 || phase_ == Phase::edge;
    return mode_ == 2 && memory_->clock.now < line_start_ + 156;
}
uint8_t Ppu::read_palette(bool obj) {
    const auto index = memory_->io[obj ? 0x6a : 0x68];
    if ((memory_->io[0x40] & 0x80) && (sync(), in_transfer())) return 0xff;
    return cgb_->palettes[(obj ? 64 : 0) + (index & 63)];
}
void Ppu::write_palette(bool obj, uint8_t value) {
    auto& index = memory_->io[obj ? 0x6a : 0x68];
    if (!(memory_->io[0x40] & 0x80) || (sync(), !in_transfer())) {
        auto& byte = cgb_->palettes[(obj ? 64 : 0) + (index & 63)];
        if (byte != value) { byte = value; cgb_->palettes_dirty = true; }
    }
    if (index & 0x80) index = static_cast<uint8_t>(0x80 | ((index + 1) & 63));
}
bool Ppu::in_hblank() {
    if (!(memory_->io[0x40] & 0x80)) return true;
    sync();
    return mode_ == 0 && line_ < 144;
}
void Ppu::hdma_changed() {
    hdma_ = memory_->hdma_active();
    if (!(memory_->io[0x40] & 0x80)) return;
    sync(); arm();
}
void Ppu::change(uint16_t address, uint8_t value) {
    if (!job_) return;
    if (address == 0xff45 || address == 0xff4a) return;
    sync();
    const Tick dot = (memory_->clock.now - transfer_start_ + kRenderLead) / 2;
    if (mode_ != 3 && (address < 0xff40 || dot >= transfer_dots_ + 8)) return;
    auto& r = *static_cast<Row*>(job_);
    if (r.changes_size == r.changes.size() && address < 0xc000) { overflow(address, value); return; }
    // At most one CPU and one DMA write per four dots; 192 exceeds the
    // maximum for the longest scanline. Debug writes must obey the same bound.
    assert(r.changes_size < r.changes.size());
    r.changes[r.changes_size++] = {static_cast<uint16_t>(dot), address, value};
}
// Only the CGB's general-purpose DMA writes VRAM in mode 3 faster than a row
// has room for, as it ignores the PPU (Pan Docs, General-Purpose DMA). The
// bytes past the row's room count as written before the line.
void Ppu::overflow(uint16_t address, uint8_t value) {
    auto& r = *static_cast<Row*>(job_);
    const unsigned offset = address - 0x8000, page = offset / 128;
    auto& dirty = page < 64 ? r.dirty : r.dirty1;
    const uint64_t bit = uint64_t{1} << (page & 63);
    if (dirty & bit) { r.vram[offset] = value; return; }
    const auto* source = page < 64 ? memory_->vram.data() : memory_->vram1();
    std::memcpy(r.vram.data() + page * 128, source + (page & 63) * 128, 128);
    dirty |= bit;
}
// A bank 1 change goes into the row as an address from A000, which no other
// change uses.
void Ppu::write_vram(unsigned offset, uint8_t value, unsigned bank) {
    if (bank) {
        memory_->vram1()[offset] = value; cgb_->dirty1 |= uint64_t{1} << (offset / 128);
        change(static_cast<uint16_t>(0xa000 + offset), value);
        return;
    }
    memory_->vram[offset] = value; dirty_ |= uint64_t{1} << (offset / 128);
    change(static_cast<uint16_t>(0x8000 + offset), value);
}
// Sixteen bytes at one time, from offset, a multiple of 16: the first syncs
// the PPU, and the others need a change of their own only in mode 3.
void Ppu::write_vram_block(unsigned offset, const uint8_t* bytes, unsigned bank) {
    write_vram(offset, bytes[0], bank);
    if (job_ && mode_ == 3) {
        for (unsigned i = 1; i < 16; ++i) write_vram(offset + i, bytes[i], bank);
        return;
    }
    std::memcpy((bank ? memory_->vram1() : memory_->vram.data()) + offset + 1, bytes + 1, 15);
}
void Ppu::write_oam(unsigned offset, uint8_t value) {
    memory_->oam[offset] = value; change(static_cast<uint16_t>(0xfe00 + offset), value);
    oam_dirty_ = true;
    if (!(offset & 3)) objects_height_ = 0;
}
void Ppu::write(uint8_t a, uint8_t value) {
    auto& m = *memory_;
    if (a == 0x44) return;
    sync();
    stat_at_ = m.clock.now;
    if (a == 0x41) {
        // Pan Docs, Spurious STAT interrupts: on a DMG a write acts as if FF
        // were written, then the value. Gambatte's lycEnable and m2enable
        // cases on dmg08 fit an instant better than the M-cycle Pan Docs
        // gives. A CGB, in either mode, has no such quirk. A DMG's write
        // while the HBlank source settles decides that too, see
        // kHblankSettle.
        const auto old = m.io[a];
        m.io[a] = value & 0x78;
        if (!(m.io[0x40] & 0x80)) return;
        if (!cgb_ && hblank_irq_ && hblank_settling(kHblankSettle)) settle_hblank(old, coincidence_, true);
        else {
            if (!cgb_) { const auto written = m.io[a]; m.io[a] = 0x78; update_stat(); m.io[a] = written; }
            update_stat();
        }
        arm();
        return;
    }
    const auto previous = m.io[a];
    if (previous != value) change(0xff00 + a, value);
    if (a == 0x40 && ((previous ^ value) & 4)) size_at_ = m.clock.now;
    if (a == 0x4a) check_wy(false);
    m.io[a] = value;
    if (a == 0x4a) { check_wy(true); return; }
    if (a == 0x43 && mode_ == 3 && m.clock.now - transfer_start_ + kRenderLead < 20) {
        // The first tile fetch latches fine SCX before pixels start leaving
        // the pipeline. Mealybug m3_scx_low_3_bits accepts a write at dot 92,
        // but not at dot 96 (mode 3 becomes readable at dot 84). A frame not
        // drawn has no row, but the timing is the same.
        if (job_) {
            auto& row = *static_cast<Row*>(job_);
            row.fine = value & 7;
            row.registers[3] = static_cast<uint8_t>((row.registers[3] & 0xf8) | row.fine);
        }
        fine_ = value & 7; time_transfer();
        schedule(transfer_start_ + (transfer_dots_ * 2 - hblank_lead()), Phase::hblank); arm();
    }
    // LCDC and WX in mode 3 move the window and the OBJ fetches, see
    // time_transfer().
    if ((a == 0x4b || (a == 0x40 && (value & 0x80))) && previous != value && mode_ == 3 &&
        timing_count_ < timing_.size()) {
        timing_[timing_count_++] = {static_cast<uint16_t>((m.clock.now - transfer_start_ + kRenderLead) / 2), a, value};
        time_transfer();
        schedule(transfer_start_ + (transfer_dots_ * 2 - hblank_lead()), Phase::hblank); arm();
    }
    // Enabling the window before the line it carries into shows counts a
    // window line, see window_carry_.
    if (a == 0x40 && window_carry_ && (value & window_bits_) == window_bits_ &&
        (previous & window_bits_) != window_bits_)
        ++carry_count_;
    if (a == 0x45) {
        if (!(m.io[0x40] & 0x80)) return;
        if (!cgb_ && hblank_irq_ && hblank_settling(kLycSettle)) {
            const bool coincidence = coincidence_;
            flag_ = coincidence_ = compared_ == value;
            settle_hblank(m.io[0x41], coincidence, false);
        } else compare(compared_);
        arm();
        return;
    }
    if (a != 0x40 || !((previous ^ value) & 0x80)) return;
    if (job_) { renderer_->submit(); job_ = nullptr; }
    line_ = 0; m.io[0x44] = 0; mode_ = 0; lock_at_ = kNever;
    line_start_ = transfer_start_ = m.clock.now; transfer_dots_ = 0; window_y_ = m.io[0x4a] == 0;
    window_carry_ = transfer_carry_ = hblank_requested_ = mode1_requested_ = mode1_pending_ = false; carry_count_ = 0;
    hblank_at_ = mode1_at_ = kNever;
    if (value & 0x80) {
        // The first line reads mode 0, but its HBlank source stays low until
        // its transfer ends: Gambatte enable_display/ly0_m0irq_* and
        // enable_display_ly0_m0irq_trigger enable the mode 0 interrupt early
        // in that line, on dmg08 and cgb04c.
        first_line_ = blank_ = true; hblank_irq_ = oam_irq_ = mode1_irq_ = false;
        phase_ = Phase::transfer; compare_ly(); schedule(line_start_ + kFirstTransfer, Phase::transfer); arm();
    } else {
        // LY=LYC and its interrupt latch retain their state while LCD is off.
        // A request still settling shows at once.
        m.clock.cancel(0); phase_at_ = kNever; m.show_interrupts(2);
        if (renderer_) renderer_->command(Row::Kind::clear, m.frames, m.mode());
    }
}
void Ppu::finish() { if (renderer_) renderer_->finish(); }
void Ppu::set_sink(FrameSink* sink) {
    if (!renderer_) return;
    // The row being filled is not submitted yet, so the worker is idle.
    renderer_->finish();
    renderer_->set_sink(sink);
}
std::span<const uint16_t> Ppu::pixels() { return renderer_ ? renderer_->pixels() : std::span<const uint16_t>{}; }
std::span<const uint8_t> Ppu::shades() { return renderer_ ? renderer_->shades() : std::span<const uint8_t>{}; }
uint64_t Ppu::frame_id() { return renderer_ ? renderer_->id() : 0; }
void Ppu::set_palette(const std::array<uint16_t, 4>& palette) {
    if (renderer_) renderer_->set_palette(palette[0] | uint64_t{palette[1]} << 16 | uint64_t{palette[2]} << 32 |
                                          uint64_t{palette[3]} << 48);
}
}  // namespace gbx::gb
