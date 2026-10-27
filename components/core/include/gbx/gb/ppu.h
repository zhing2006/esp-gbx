#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include "gbx/common/scheduler.h"

namespace gbx::gb {
class Memory;
class Offload;
class Renderer;
inline constexpr unsigned kFramePixels = 160 * 144;
inline constexpr unsigned kPackedFrameBytes = kFramePixels / 4;

// Receives each frame the renderer completes, and the blank screen when the
// LCD turns off, on the worker thread of the other core. A display reads
// frames this way without the CPU owner, which would otherwise wait for the
// renderer and convert the colors on the emulating core.
class FrameSink {
public:
    // Where the frame goes: kFramePixels BGR555 pixels, row by row, or
    // nullptr to skip it.
    virtual uint16_t* frame_buffer() = 0;
    // frame_buffer() now holds frame `id`; a blank screen repeats the id.
    virtual void frame_ready(uint64_t id) = 0;
protected:
    ~FrameSink() = default;
};

// A CGB's PPU state. It lives in Memory's CGB block, see Memory::load(), so
// that the DMG's state, in and after the PPU, keeps its place.
struct PpuColor {
    // Palette RAM: eight BG palettes, then eight OBJ palettes, of four
    // little-endian BGR555 colors each.
    std::array<uint8_t, 128> palettes{};
    uint64_t dirty1 = ~uint64_t{0};  // VRAM bank 1 pages.
    bool palettes_dirty = true;
};

// All timing state belongs to the CPU owner. Renderer receives immutable jobs.
class Ppu {
public:
    Ppu();
    ~Ppu();
    // Rows go to the worker of offload, which outlives this PPU.
    bool initialize(Offload& offload);
    void reset(Memory& memory);
    void write(uint8_t address, uint8_t value);
    // A write to VRAM bank `bank` (1 only on a CGB) that reaches it.
    void write_vram(unsigned offset, uint8_t value, unsigned bank = 0);
    void write_vram_block(unsigned offset, const uint8_t* bytes, unsigned bank);
    void write_oam(unsigned offset, uint8_t value);
    // CGB palette RAM through BGPD and OBPD, at the index in BGPI or OBPI:
    // reads give FF and writes miss in mode 3, but a write still advances
    // an auto-incrementing index (Pan Docs, LCD Color Palettes).
    uint8_t read_palette(bool obj);
    void write_palette(bool obj, uint8_t value);
    // Whether no HBlank is about to come: the LCD is off or in mode 0.
    bool in_hblank();
    // An HBlank DMA started or stopped, see Memory::hdma_active().
    void hdma_changed();
    bool vram_blocked(bool write = false);
    bool oam_blocked(bool write = false);
    void finish();
    // Waits for the renderer, then sends it the frames from now on; nullptr
    // stops them.
    void set_sink(FrameSink* sink);
    std::span<const uint16_t> pixels();
    // GB mode only: the shades of the frame.
    std::span<const uint8_t> shades();
    // BGR555 colors of the four shades, lightest first, for the frames the
    // sink receives; each frame takes the colors set when it is delivered.
    // pixels() keeps the gray scale. Any task.
    void set_palette(const std::array<uint16_t, 4>& palette);
    // Whether the next frames are drawn; true after construction. Nothing of
    // a frame not drawn goes to the worker: VRAM and OAM changes wait for the
    // next drawn row, and the window line restarts every frame, so only the
    // pixels go missing. The front frame and its ID stay, and the sink sees
    // the next frame ID skip. The CPU owner sets it between frames, see
    // in_frame(). A state keeps it, so a frame resumed midway goes on as it
    // began.
    void set_drawing(bool drawing) { skipping_ = !drawing; }
    // Whether a frame is under way: the LCD is on and the PPU, as last
    // brought up to date, has not delivered the frame yet.
    bool in_frame() const;
    uint64_t frame_id();
    uint8_t stat();
    // LY as a CGB's reads see it, see ly_cgb(); a DMG's reads see LY as
    // it is.
    [[gnu::noinline]] uint8_t ly_cgb();
    // IF was written: the requests IF does not show yet stay only while
    // they settle, see kHblankSettle.
    void write_if();
    // The earliest time a LY=LYC STAT request may come without a write: when
    // LY next becomes LYC, or the next PPU event while it is. kNever while
    // the LCD is off.
    Tick next_lyc() const;
    // Time of the next line-144 event, which requests the VBlank interrupt
    // and completes a frame; kNever while the LCD is off.
    Tick next_vblank() const;
    static bool owns(Scheduler::Callback callback) { return callback == &event; }
    // Writes recorded in the row of the line in transfer: register, VRAM and
    // OAM writes in mode 3 and just after it.
    unsigned pending_changes() const;
private:
    friend struct SnapshotAccess;
    // Mode 3 as STAT and the bus see it, which ends after the HBlank source
    // rises.
    bool in_transfer() const;
    unsigned hblank_lead() const;
    // The end of the current line: its line phase, if no LCD write moves it.
    Tick line_end() const;
    // Ticks from the start of the current line to `time`, which is within it
    // or the next lines, as the PPU's events and accesses are.
    uint32_t offset(Tick time) const { return static_cast<uint32_t>(time - line_start_); }
    Memory* memory_ = nullptr;
    std::unique_ptr<Renderer> renderer_;
    uint16_t line_ = 153;
    uint8_t mode_ = 1;
    bool coincidence_ = true, stat_line_ = false;
    // The LY that LY=LYC compares, which lags LY, or kNoCompare while the
    // comparison has no result, see advance(). flag_ is STAT bit 2, whether
    // it equals LYC. coincidence_, the STAT line's LY=LYC source, keeps the
    // last result where the comparison has none at the start of a line.
    static constexpr uint16_t kNoCompare = 0x100;
    uint16_t compared_ = 0;
    bool flag_ = true;
    // The HBlank, OAM and line 144's mode 1 sources of the STAT line, which
    // do not follow the mode STAT reads, see kTransfer.
    bool hblank_irq_ = false, oam_irq_ = false, mode1_irq_ = false;
    // When the HBlank source last rose, at single speed, and whether that
    // requested the interrupt, see kHblankSettle.
    Tick hblank_at_ = kNever;
    bool hblank_requested_ = false;
    // The same for line 144's mode 1 source, which rises while LY=LYC may
    // still hold the line up (mode1_pending_), and the time of the phase or
    // write that update_stat() runs for.
    Tick mode1_at_ = kNever, stat_at_ = 0;
    bool mode1_requested_ = false, mode1_pending_ = false;
    bool first_line_ = false, blank_ = false, window_y_ = false;
    // VRAM reads are blocked from this time until the end of mode 3. Mode 2
    // sets it ahead, so the lock needs no event of its own.
    Tick lock_at_ = kNever;
    Tick line_start_ = 0, transfer_start_ = 0;
    unsigned transfer_dots_ = 172;
    // The objects the line in transfer selected, which an SCX write early
    // in mode 3 times the transfer by again, drawn or not.
    std::array<uint8_t, 10> objects_{};
    uint8_t object_count_ = 0;
    // LCDC and WX as mode 3 started, its fine scroll, and the number of
    // LCDC and WX writes since, see timing_.
    uint8_t transfer_lcdc_ = 0, transfer_wx_ = 0, fine_ = 0, timing_count_ = 0;
    // The first renderer dot the window may start at, when WY matched LY in
    // this mode 3; 0 otherwise. See check_wy().
    uint16_t window_from_ = 0;
    // A DMG whose window reached WX 166 at the end of a line, with WY
    // matched, shows it from the start of the next visible line, if it is
    // enabled as mode 3 starts, from its second tile on. The window line
    // advances as that line starts with the window enabled, and each time
    // the window is enabled before its mode 3, shown or not: Gambatte
    // window/on_screen/wxA6_* on dmg08. window_carry_: the next line
    // carries; carry_count_: the window lines it advanced by;
    // transfer_carry_: the line in mode 3 shows the window from its start.
    bool window_carry_ = false, transfer_carry_ = false;
    uint8_t carry_count_ = 0;
    // lyc: LY=LYC compares the new LY in VBlank, and on a CGB line 144's OAM
    // STAT source rises. ly153 and lyc_off: line 153's LY of 0 and the
    // comparison's gap before it compares 0, see advance().
    // edge: the changes at the start of a visible line, see kEdges.
    // ly0: a CGB's LY of 0 on line 153, a little after a DMG's. oam: the
    // OAM source rising before lines 1 to 143, see kOamEarly.
    // settle: IF shows the HBlank request, see kHblankSettle.
    enum class Phase : uint8_t { line, edge, transfer, hblank, vblank, ly153, lyc, lyc_off, ly0, oam, settle };
    // The next phase and its time. Only phases that need an event are
    // scheduled, see arm(); sync() applies the others once they are due.
    Phase phase_ = Phase::line;
    uint8_t edge_ = 0;  // The index in line_edges() of the next edge phase.
    Tick phase_at_ = kNever, armed_ = kNever;  // armed_: the scheduled phase.
    uint64_t dirty_ = ~uint64_t{0};
    bool oam_dirty_ = true;  // OAM changed since the last row copied it.
    bool skipping_ = false;  // see set_drawing()
    // The CGB's state fills padding, so the DMG's keeps its place. hdma_:
    // an HBlank DMA runs, see hdma_changed().
    bool hdma_ = false;
    // The LCDC bits that show the window: 0x21, but in CGB mode LCDC bit 0
    // gives the BG and window priority instead of turning them off (Pan
    // Docs, LCDC.0), so only bit 5.
    uint8_t window_bits_ = 0x21;
    PpuColor* cgb_ = nullptr;  // On a CGB, in either mode.
    // Bit i of line_objects_[y] marks OAM entry i as overlapping line y at
    // objects_height_. OAM Y writes and height changes rebuild it (0 marks it
    // stale), so the OAM scan does not read all 40 entries on every line.
    std::array<uint64_t, 144> line_objects_{};
    unsigned objects_height_ = 0;
    Tick size_at_ = 0;  // The last change of the OBJ height, see kScanStart.
    void* job_ = nullptr;
    // The LCDC and WX writes in mode 3 that change them, by the renderer's
    // dot, see time_transfer(). The CPU writes at most once every four dots
    // (LD (C),A at double speed), and mode 3 starts 81 dots into a line of
    // 456. After the fields each line uses, so that they share cache lines.
    struct Write { uint16_t dot; uint8_t address, value; };
    std::array<Write, (456 - 81 + 3) / 4> timing_{};
    void schedule(Tick when, Phase phase);
    const struct LineEdges& line_edges() const;
    void apply_edge();
    void arm();
    void sync();
    static void event(void* context);
    void skip(Tick now);
    void advance();
    [[gnu::noinline]] void advance_vblank();
    void update_stat(bool unseen = false);
    [[gnu::noinline]] void raise_stat(bool unseen);
    bool stat_high(uint8_t enables, bool hblank, bool coincidence) const;
    bool hblank_settling(Tick window) const;
    void settle_hblank(uint8_t old, bool coincidence, bool quirk);
    void compare_ly();
    void compare(uint16_t ly);
    void begin_line();
    unsigned select_objects(std::array<uint8_t, 10>& objects);
    unsigned select_objects(std::array<uint8_t, 10>& objects, unsigned height);
    void begin_transfer();
    void time_transfer();
    [[gnu::noinline]] void retime();
    uint16_t window_ly() const;
    void check_wy(bool written);
    [[gnu::noinline]] void window_matched();
    void end_transfer();
    void change(uint16_t address, uint8_t value);
    [[gnu::noinline]] void overflow(uint16_t address, uint8_t value);
    [[gnu::noinline]] void fill_cgb_row(void* row);
    void reset_palettes();
};
}  // namespace gbx::gb
