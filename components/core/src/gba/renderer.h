#pragma once

#include <array>
#include <cstdint>
#include <new>
#include <span>

#include "gbx/gba/ppu.h"
#include "gbx/platform.h"

namespace gbx::gba {

// How Renderer::paint() draws the layers of a segment: each pixel's final
// color into the line, as with no color effects or those that setup() lets
// paint() apply as it goes (flat, tracked), or the top two layers of each
// pixel onto the stack for resolve() (stacked). See gba_renderer.cpp.
enum class PaintMode : uint8_t { flat, tracked, stacked };

// Draws the lines the PPU hands over, on the worker of the other core,
// from its own copies of VRAM, palette RAM and OAM, which each job's pages
// bring up to date (GBATEK "GBA LCD Video Controller").
class Renderer {
public:
    // Its copies of palette RAM and OAM and its line buffers are touched
    // for every pixel: internal RAM while there is room.
    static void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
        return platform::allocate_internal(bytes);
    }
    static void operator delete(void* memory) noexcept { platform::free_internal(memory); }
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    bool initialize();
    void reset();
    // A job and its payload, see LineJob.
    void draw(const LineJob& job, const uint8_t* payload);
    // Rebuilds what the renderer derives from its copies of palette RAM and
    // OAM after they changed outside draw(), as a restore changes them, and
    // starts the frame's lines in the sink's buffer afresh.
    void refresh();
    // The frame as drawn so far, in frame_, once the lines only in the
    // sink's buffers are copied back, see sunk_.
    std::span<const uint16_t> pixels() {
        gather_frame();
        return {frame_, kFramePixels};
    }
    uint64_t frame_id() const { return frame_id_; }
    void set_sink(FrameSink* sink) {
        gather_frame();
        sink_ = sink;
        target_ = nullptr;
    }

private:
    friend struct SnapshotAccess;
    // A layer's pixel: a BGR555 color, or kTransparent.
    static constexpr uint16_t kTransparent = 0x8000;
    struct Registers;
    // Lines a layer or the display must have been enabled to show.
    static constexpr uint8_t kLatchLines = 3;
    void palette_changed(unsigned bank);
    void palette_page(unsigned page, const uint8_t* from);
    void list_objects();
    void latch_display(uint16_t dispcnt, unsigned line);
    uint16_t latched(uint16_t dispcnt) const;
    void render(const uint8_t* registers, unsigned line, unsigned from, unsigned to);
    using Paint = PaintMode;
    // A BG's pixels where they are opaque and `shows` (window_, or nullptr
    // without windows) shows the BG, see paint().
    template <Paint kPaint>
    void text_bg(const Registers& r, unsigned n, unsigned line, unsigned from, unsigned to, uint16_t* out,
                 const uint8_t* shows);
    template <Paint kPaint>
    void affine_bg(const Registers& r, unsigned n, unsigned mode, unsigned from, unsigned to, uint16_t* out,
                   const uint8_t* shows);
    void objects(const Registers& r, unsigned line, unsigned from, unsigned to);
    // The color effects, windows and BG order of a segment; true when a
    // color effect may apply, so that paint() keeps two layers a pixel.
    bool setup(const Registers& r, unsigned line, unsigned from, unsigned to);
    // The layers of `shown` (bits 0-3 the BGs, 4 the OBJs) where `shows`,
    // if not nullptr, shows them; with bit 5 the layers take the effects
    // that kPaint applies.
    template <Paint kPaint>
    void paint(const Registers& r, unsigned line, unsigned mode, unsigned from, unsigned to, uint16_t* out,
               unsigned shown, const uint8_t* shows);
    // paint() and, with `effects` (setup()), resolve() of a segment
    // through its windows: span by span between the edges of window 0 and
    // 1, each span with the layers and effects its window shows, else
    // pixel by pixel.
    void paint_windows(const Registers& r, unsigned line, unsigned mode, unsigned from, unsigned to, uint16_t* out,
                       bool effects);
    template <bool kWindows>
    void resolve(unsigned from, unsigned to, uint16_t* out);

    uint8_t* vram_ = nullptr;   // 96 KiB and a zeroed row past it, external memory.
    alignas(4) std::array<uint8_t, 1024> palette_{}, oam_{};
    // The palettes as the layers look colors up: without bit 15, and
    // kTransparent for color 0 of each 16-color bank (obj4_) or of the 256
    // (bg8_, obj8_); the BGs' colors also in RGB565 (bgc_). The BGs' tiles
    // test the index for color 0. palette_changed() rebuilds a half of
    // palette RAM.
    std::array<uint16_t, 256> bg8_{}, bgc_{}, obj4_{}, obj8_{};
    // The BGs' colors in RGB565 after a brightness effect, for EVY and
    // the direction in bright_key_ (bits 0-4 and 8), see bright_colors().
    static constexpr uint32_t kNoBright = ~0u;
    std::array<uint16_t, 256> bright_{};
    uint32_t bright_key_ = kNoBright;
    const uint16_t* bright_colors();
    // The OBJs on each visible line, a bit each by OAM index, so that
    // objects() looks only at those. Once OAM changed an OBJ's Y, shape,
    // size or its affine and double size or disable bits, it is in
    // obj_moved_, and list_objects() moves it from the lines in
    // obj_spans_ (the first, and how many) to those it takes now; every
    // OBJ after refresh().
    std::array<std::array<uint32_t, 4>, kVisibleLines> obj_lines_{};
    std::array<uint32_t, 4> obj_moved_{};
    std::array<std::array<uint8_t, 2>, 128> obj_spans_{};
    bool obj_lines_stale_ = true;
    uint16_t* frame_ = nullptr;  // kFramePixels, external memory.
    FrameSink* sink_ = nullptr;
    // The sink's buffer for the frame being drawn, which takes each line as
    // frame_ does, so that the end of the frame copies nothing; nullptr
    // until a line finds one.
    uint16_t* target_ = nullptr;
    // Whether a line of the frame was drawn.
    bool drawn_ = false;
    // Lines drawn into a sink's buffer and not into frame_, so that each
    // line is written to external memory once: the row in the buffer, or
    // nullptr where frame_ has the line. The buffer stays as it was after
    // frame_ready() until the next one, as every line of a frame given a
    // buffer goes into it.
    std::array<const uint16_t*, kVisibleLines> sunk_{};
    void gather_frame();
    void fill_target(unsigned lines);
    // The line being drawn, in internal RAM: RGB565, the sink's format,
    // once paint() is done with a segment of it; a segment whose layers
    // take color effects is BGR555 until then.
    alignas(4) std::array<uint16_t, kScreenWidth> line_{};
    uint64_t frame_id_ = 0;
    // Lines that started with BG0-3, OBJ and the display (forced blank off)
    // enabled, up to kLatchLines, and those that show, bits 0-5.
    std::array<uint8_t, 6> enabled_lines_{};
    uint8_t shown_ = 0;
    // Internal reference points of BG2 and BG3 (GBATEK "LCD I/O BG
    // Rotation/Scaling"), and those of the line where the current vertical
    // mosaic block began.
    std::array<std::array<int32_t, 2>, 2> reference_{}, mosaic_reference_{};
    // The OBJ layer's colors where obj_covered_ marks them.
    std::array<uint16_t, kScreenWidth> obj_{};
    // The OBJ priorities that have a pixel in the segment objects() drew,
    // a bit each, and whether one is semi-transparent.
    uint8_t obj_priorities_ = 0;
    bool obj_semi_ = false;
    bool obj_window_ = false;  // An OBJ of the OBJ window has a pixel.
    // The pixels objects() drew, a bit each: by their priority, those of
    // semi-transparent OBJs, and those of the OBJ window.
    using LineBits = std::array<uint32_t, (kScreenWidth + 31) / 32>;
    std::array<LineBits, 4> obj_bits_{};
    LineBits obj_semi_bits_{}, obj_window_bits_{};
    // The pixels of obj_ that an OBJ drew, whatever its priority: the rest
    // of obj_ is left as it was.
    LineBits obj_covered_{};
    // The OBJs to draw again for the rest of the line: at its start, and
    // after a write to DISPCNT or MOSAIC within it.
    bool objects_stale_ = true;
    // The layers each pixel shows: bits 0-3 BG0-3, 4 OBJ and 5 the effects.
    std::array<uint8_t, kScreenWidth> window_{};
    // Where window 0 and 1 begin and end inside the segment, cut_count_ of
    // them, unless the OBJ window shows pixels (window_pixels_), which
    // paint_windows() then checks one by one.
    std::array<uint8_t, 4> cuts_{};
    unsigned cut_count_ = 0;
    bool window_pixels_ = false;
    // The enabled BGs front to back, by priority and then number.
    struct Layer {
        uint8_t priority;
        uint8_t number;
    };
    std::array<Layer, 4> layers_{};
    unsigned layer_count_ = 0;
    bool objs_ = false, windows_ = false;
    bool semi_ = false;  // A semi-transparent OBJ blends, see setup().
    uint16_t backdrop_ = 0;
    unsigned effect_ = 0, eva_ = 0, evb_ = 0, evy_ = 0;
    // With color effects, the top two layers of each pixel: the top one's
    // color in bits 0-14, the one under it in bits 16-30, and in bit 15 and
    // 31 whether each is a second target. A layer drawn over a pixel moves
    // its top one down.
    alignas(4) std::array<uint32_t, kScreenWidth> stack_{};
    // What the top layer of each pixel takes, a kAlpha, kFade and kSemiAlpha
    // set (renderer.cpp); with Paint::tracked, whether it is a second
    // target.
    alignas(4) std::array<uint8_t, kScreenWidth> acts_{};
    // The second target bit, as on stack_, and the effects each layer
    // takes: BG0-3, the OBJs, the backdrop, then semi-transparent OBJs.
    std::array<uint16_t, 7> tags_{};
    std::array<uint8_t, 7> layer_acts_{};
    // How setup()'s effects let paint() draw without the stack: the
    // effect each layer then takes (BG0-3, the OBJs, the backdrop), and
    // those of the segment paint() draws, see renderer.cpp.
    Paint paint_ = Paint::stacked;
    std::array<uint8_t, 6> layer_fx_{}, fx_{};
    // What a layer's pixels leave in acts_ (see there).
    uint8_t act(Paint paint, unsigned layer) const {
        return paint == Paint::tracked ? static_cast<uint8_t>(tags_[layer] >> 15) : layer_acts_[layer];
    }
};

}  // namespace gbx::gba
