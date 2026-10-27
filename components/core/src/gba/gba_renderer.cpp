// The GBA renderer, on the worker of the other core: BG modes 0-5, OBJs,
// windows, color effects and mosaic, a line at a time (GBATEK "GBA LCD
// Video Controller" and its sections named below). It reads only its jobs
// and its own copies of VRAM, palette RAM and OAM.

#include "renderer.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <type_traits>

#include "bits.h"
#include "gbx/platform.h"

namespace gbx::gba {
namespace {

constexpr size_t kVramBytes = 96 * 1024;
// A tile row read may run past VRAM's end, for an 8-bit OBJ tile 1023: it
// reads zeros there.
constexpr size_t kVramPadding = 64;
// GBATEK "LCD OBJ - OAM Attributes": width and height by shape and size.
constexpr uint8_t kObjSize[4][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
    {{0, 0}, {0, 0}, {0, 0}, {0, 0}},
};

int32_t s16(uint16_t v) { return static_cast<int16_t>(v); }
int32_t s28(uint32_t v) { return static_cast<int32_t>(v << 4) >> 4; }
uint16_t u16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

// The three channels side by side in a word: red at bit 0, blue at 10 and
// green at 21, each with room for the sum of two products by up to 16.
constexpr uint32_t kSpread = 0x03e07c1f;
// After a shift by 4, the integer parts, 6 bits each, and bit 5 of each.
constexpr uint32_t kSums = 0x07e0fc3f, kOver = 0x04008020;
[[gnu::always_inline]] inline uint32_t spread(uint16_t c) { return (c | uint32_t{c} << 16) & kSpread; }
[[gnu::always_inline]] inline uint16_t gather(uint32_t s) {
    return static_cast<uint16_t>((s & 0x7c1f) | ((s >> 16) & 0x3e0));
}

[[gnu::always_inline]] inline uint16_t blend(uint16_t a, uint16_t b, unsigned eva, unsigned evb) {
    uint32_t sum = ((spread(a) * eva + spread(b) * evb) >> 4) & kSums;
    // A channel above 31 becomes 31.
    const uint32_t over = sum & kOver;
    sum |= over - (over >> 5);
    return gather(sum);
}

// A loop the compiler keeps here: std::fill of halfwords is a call. Two
// halfwords a store where they are word aligned.
typedef uint32_t __attribute__((may_alias, aligned(4))) Word;
typedef uint16_t __attribute__((may_alias)) Half;
[[gnu::always_inline]] inline void fill16(uint16_t* out, unsigned count, uint16_t value) {
    if (count && (reinterpret_cast<uintptr_t>(out) & 2)) {
        *out++ = value;
        --count;
    }
    auto* words = reinterpret_cast<Word*>(out);
    const uint32_t pair = value * 0x10001u;
    for (unsigned i = 0; i < count / 2; ++i) words[i] = pair;
    if (count & 1) out[count - 1] = value;
}

// The sink's pixels: BGR555 to RGB565, green's new low bit repeating its
// top bit, two pixels a word. `count` is even and both are word aligned.
void to_rgb565(const uint16_t* in, uint16_t* out, size_t count) {
    const auto* from = reinterpret_cast<const Word*>(in);
    auto* to = reinterpret_cast<Word*>(out);
    for (size_t i = 0; i < count / 2; ++i) {
        const uint32_t c = from[i];
        to[i] = (c & 0x001f001fu) << 11 | (c & 0x03e003e0u) << 1 | ((c >> 4) & 0x00200020u) | ((c >> 10) & 0x001f001fu);
    }
}
[[gnu::always_inline]] inline uint16_t rgb565(uint16_t c) {
    return static_cast<uint16_t>((c & 0x1f) << 11 | (c & 0x3e0) << 1 | ((c >> 4) & 0x20) | (c >> 10));
}
// BGR555 to RGB565 in place on [from, to) of a word aligned line: two
// pixels a word between its odd ends.
void to_rgb565(uint16_t* line, unsigned from, unsigned to) {
    if (from < to && (from & 1)) line[from] = rgb565(line[from]), ++from;
    if (from < to && (to & 1)) --to, line[to] = rgb565(line[to]);
    to_rgb565(line + from, line + from, to - from);
}
// RGB565 that rgb565() made back to BGR555.
void to_bgr555(const uint16_t* in, uint16_t* out, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const unsigned c = in[i];
        out[i] = static_cast<uint16_t>((c >> 11) | ((c >> 6) & 31) << 5 | (c & 31) << 10);
    }
}

[[gnu::always_inline]] inline uint16_t brighten(uint16_t a, unsigned evy, bool up) {
    const uint32_t c = spread(a);
    if (up) return gather(c + (((spread(a ^ 0x7fff) * evy) >> 4) & kSpread));
    return gather(c - (((c * evy) >> 4) & kSpread));
}

// The color effects a pixel's top layer takes, see Renderer::acts_: alpha
// blending with the layer under it when that is a second target, else the
// brightness effect; a semi-transparent OBJ blends outside the windows'
// effects too.
constexpr uint8_t kAlpha = 1, kFade = 2, kSemiAlpha = 4;
// The color effect a layer's pixels take as paint() draws them without
// the stack, see Renderer::layer_fx_: none, alpha blending over the
// pixel's top layer, or the brightness effect. In a segment where no
// layer blends, the layers write RGB565, the line's own format, straight
// away: kFxConvert with that effect, which the BGs' colors in RGB565
// have taken already (bgc_, bright_).
constexpr uint8_t kFxNone = 0, kFxBlend = 1, kFxUp = 2, kFxDown = 3, kFxConvert = 4;

// Where paint() puts a layer's opaque pixels (PaintMode). Flat: into the
// line, with the layer's color effect `fx` if any. Tracked: the same, but
// alpha blending only over a top layer that is a second target, as acts
// records for each pixel, and `act` that of the layer. Stacked: over the
// pixel's top layer on the stack, with the layer's second target bit
// `tag` and effects `act`.
template <PaintMode kPaint>
struct Pen {
    static constexpr bool kStack = kPaint == PaintMode::stacked, kTracked = kPaint == PaintMode::tracked;
    uint16_t* out;
    uint32_t* stack;
    uint8_t* acts;
    uint16_t tag;
    uint8_t act;
    uint8_t fx;
    unsigned eva, evb, evy;
    [[gnu::always_inline]] uint16_t effect(uint16_t color, const uint16_t& under) const {
        if (!kTracked && (fx & kFxConvert)) return rgb565(fx & 3 ? brighten(color, evy, (fx & 3) == kFxUp) : color);
        if (kTracked || fx == kFxBlend) return blend(color, under, eva, evb);
        return brighten(color, evy, fx == kFxUp);
    }
    [[gnu::always_inline]] void put(unsigned x, uint16_t color) const {
        if constexpr (kStack) {
            stack[x] = stack[x] << 16 | color | tag;
            acts[x] = act;
        } else if constexpr (kTracked) {
            out[x] = fx && acts[x] ? effect(color, out[x]) : color;
            acts[x] = act;
        } else {
            out[x] = fx ? effect(color, out[x]) : color;
        }
    }
    // Eight opaque pixels from x on, color(i) the i-th; the stores to acts
    // last, as they could alias anything.
    template <typename Color>
    [[gnu::always_inline]] void put8(unsigned x, Color color) const {
        uint8_t* const pixel_acts = acts + x;
        if constexpr (kStack) {
            uint32_t* const pixels = stack + x;
#pragma GCC unroll 8
            for (unsigned i = 0; i < 8; ++i) pixels[i] = pixels[i] << 16 | color(i) | tag;
        } else {
            uint16_t* const pixels = out + x;
            if (fx) {
                for (unsigned i = 0; i < 8; ++i) {
                    const uint16_t c = color(i);
                    pixels[i] = !kTracked || pixel_acts[i] ? effect(c, pixels[i]) : c;
                }
            } else {
#pragma GCC unroll 8
                for (unsigned i = 0; i < 8; ++i) pixels[i] = color(i);
            }
            if constexpr (!kTracked) return;
        }
        if (!(x & 3)) {
            // acts is word aligned.
            auto* const words = reinterpret_cast<Word*>(pixel_acts);
            words[0] = words[1] = act * 0x01010101u;
            return;
        }
#pragma GCC unroll 8
        for (unsigned i = 0; i < 8; ++i) pixel_acts[i] = act;
    }
};

// Whether a word of tile data has a byte, or a nibble, of 0: a transparent
// pixel.
constexpr bool zero_byte(uint32_t v) { return (v - 0x01010101u) & ~v & 0x80808080u; }
constexpr bool zero_nibble(uint32_t v) { return (v - 0x11111111u) & ~v & 0x88888888u; }
#ifdef GBX_PROFILER
// The cycles and instructions of a part of the renderer's work, see
// RenderCounters.
struct PartSpan {
    unsigned part;
    uint32_t cycles = profile_cycles(), retired = profile_retired();
    ~PartSpan() {
        const uint32_t spent = profile_retired() - retired;
        render_counters.frame_part_retired[part] += spent;
        render_counters.part_retired[part].add(spent, kCountShift);
        render_counters.part_cycles[part].add(profile_cycles() - cycles, kCountShift);
    }
};
#define RENDER_PART(part) const PartSpan render_part_span{part}
#else
#define RENDER_PART(part)
#endif
// Bits [x, x + 8) of a line's bitmap, x below kScreenWidth, and setting
// them: they span two words only below the last.
[[gnu::always_inline]] inline uint32_t bits8(const uint32_t* bits, unsigned x) {
    const unsigned w = x >> 5, s = x & 31;
    uint32_t v = bits[w] >> s;
    if (s > 24) v |= bits[w + 1] << (32 - s);
    return v & 0xff;
}
[[gnu::always_inline]] inline void set8(uint32_t* bits, unsigned x, uint32_t mask) {
    const unsigned w = x >> 5, s = x & 31;
    bits[w] |= mask << s;
    if (s > 24) bits[w + 1] |= mask >> (32 - s);
}
// The nonzero nibbles of a row of 4-bit pixels, a bit each from the low one.
[[gnu::always_inline]] inline uint32_t opaque_nibbles(uint32_t row) {
    uint32_t t = row | (row >> 1);
    t = (t | (t >> 2)) & 0x11111111u;
    t = (t | (t >> 3)) & 0x03030303u;
    t = (t | (t >> 6)) & 0x000f000fu;
    return (t | (t >> 12)) & 0xff;
}
// std::byteswap is a call without Zbb.
constexpr uint32_t swap_bytes(uint32_t v) {
    v = (v >> 16) | (v << 16);
    return ((v >> 8) & 0x00ff00ffu) | ((v << 8) & 0xff00ff00u);
}

}  // namespace

struct Renderer::Registers {
    const uint8_t* bytes;
    uint16_t h(unsigned offset) const { return u16(bytes + offset); }
};

Renderer::Renderer() = default;

Renderer::~Renderer() {
    platform::free_external(vram_);
    platform::free_external(frame_);
}

bool Renderer::initialize() {
    if (!vram_) {
        vram_ = static_cast<uint8_t*>(platform::allocate_external(kVramBytes + kVramPadding));
        if (vram_) std::memset(vram_ + kVramBytes, 0, kVramPadding);
    }
    if (!frame_) frame_ = static_cast<uint16_t*>(platform::allocate_external(kFramePixels * 2));
    return vram_ && frame_;
}

void Renderer::reset() {
    std::memset(vram_, 0, kVramBytes);
    palette_.fill(0);
    oam_.fill(0);
    refresh();
    std::fill_n(frame_, kFramePixels, uint16_t{0x7fff});
    reference_ = mosaic_reference_ = {};
    enabled_lines_ = {};
    shown_ = 0;
    frame_id_ = 0;
}

void Renderer::refresh() {
    target_ = nullptr;
    sunk_ = {};
    drawn_ = false;
    palette_changed(0);
    palette_changed(1);
    obj_lines_ = {};
    obj_spans_ = {};
    obj_moved_.fill(~0u);
    obj_lines_stale_ = true;
}

// The lines only in a sink's buffer back to frame_: BGR555 from RGB565,
// whose low green bit to_rgb565() made from the top one.
void Renderer::gather_frame() {
    for (unsigned line = 0; line < kVisibleLines; ++line) {
        const uint16_t* row = sunk_[line];
        if (!row) continue;
        to_bgr555(row, frame_ + line * kScreenWidth, kScreenWidth);
        sunk_[line] = nullptr;
    }
}

// The sink's buffer found late takes the lines before `lines` from where
// they are.
void Renderer::fill_target(unsigned lines) {
    for (unsigned line = 0; line < lines; ++line) {
        uint16_t* to = target_ + line * kScreenWidth;
        if (sunk_[line]) std::memcpy(to, sunk_[line], kScreenWidth * 2);
        else to_rgb565(frame_ + line * kScreenWidth, to, kScreenWidth);
        sunk_[line] = to;
    }
}

// Bank 0 is the BGs' half of palette RAM, 1 the OBJs'.
void Renderer::palette_changed(unsigned bank) {
    const auto* colors = reinterpret_cast<const uint16_t*>(palette_.data()) + bank * 256;
    for (unsigned i = 0; i < 256; ++i) {
        const auto color = static_cast<uint16_t>(colors[i] & 0x7fff);
        if (bank) {
            obj4_[i] = i & 15 ? color : kTransparent;
            obj8_[i] = i ? color : kTransparent;
        } else {
            bg8_[i] = i ? color : kTransparent;
            bgc_[i] = rgb565(color);
        }
    }
    if (!bank) bright_key_ = kNoBright;
}

// A page of palette RAM, 16 colors, into the tables.
void Renderer::palette_page(unsigned page, const uint8_t* from) {
    const unsigned bank = page * Ppu::kColorPageBytes / 512, first = page * Ppu::kColorPageBytes / 2 % 256;
    std::memcpy(palette_.data() + page * Ppu::kColorPageBytes, from, Ppu::kColorPageBytes);
    for (unsigned i = 0; i < Ppu::kColorPageBytes / 2; ++i) {
        const auto color = static_cast<uint16_t>(u16(from + i * 2) & 0x7fff);
        if (bank) {
            obj4_[first + i] = i ? color : kTransparent;
            obj8_[first + i] = first + i ? color : kTransparent;
        } else {
            bg8_[first + i] = first + i ? color : kTransparent;
            bgc_[first + i] = rgb565(color);
            if (bright_key_ != kNoBright) bright_[first + i] = rgb565(brighten(color, bright_key_ & 31, bright_key_ >> 8));
        }
    }
}

void Renderer::draw(const LineJob& job, const uint8_t* payload) {
    const uint8_t* data = payload + job.pages;
    {
        RENDER_PART(kPartPages);
        for (unsigned i = 0; i < job.pages; ++i) {
            const unsigned page = payload[i];
            if (page - Ppu::kPalettePage < Ppu::kOamPage - Ppu::kPalettePage) {
                palette_page(page - Ppu::kPalettePage, data);
                data += Ppu::kColorPageBytes;
                continue;
            }
            if (page < Ppu::kVramPages) {
                std::memcpy(vram_ + page * Ppu::kPageBytes, data, Ppu::kPageBytes);
            } else {
                // The OBJs whose lines change, see obj_moved_: attribute 0
                // but X, and the size in attribute 1.
                const unsigned first = (page - Ppu::kOamPage) * (Ppu::kPageBytes / 8);
                uint8_t* const to = oam_.data() + first * 8;
                for (unsigned i = 0; i < Ppu::kPageBytes / 8; ++i) {
                    uint32_t before, after;
                    std::memcpy(&before, to + i * 8, 4);
                    std::memcpy(&after, data + i * 8, 4);
                    if (!((before ^ after) & 0xc000c3ffu)) continue;
                    obj_moved_[(first + i) >> 5] |= 1u << ((first + i) & 31);
                    obj_lines_stale_ = true;
                }
                std::memcpy(to, data, Ppu::kPageBytes);
            }
            data += Ppu::kPageBytes;
        }
    }
    if (job.line >= kVisibleLines) {
        ++frame_id_;
        // A frame 2X did not draw is not shown, as on the GB.
        if (sink_ && drawn_) {
            if (!target_ && (target_ = sink_->frame_buffer())) fill_target(kVisibleLines);
            sink_->frame_ready(frame_id_);
        }
        target_ = nullptr;
        drawn_ = false;
        return;
    }
    const unsigned line = job.line;
    drawn_ |= job.drawing;
    // The sink's buffer takes each line once it is done. The lines before
    // the one that found the buffer come from frame_.
    if (sink_ && drawn_ && !target_ && (target_ = sink_->frame_buffer()))
        fill_target(line);
    alignas(4) uint8_t regs[LineJob::kRegisterBytes];
    std::memcpy(regs, job.registers.data(), sizeof regs);
    const Registers r{regs};
    // GBATEK "LCD I/O BG Rotation/Scaling": the reference points are copied
    // at the start of the frame and when written.
    for (unsigned bg = 0; bg < 2; ++bg) {
        for (unsigned axis = 0; axis < 2; ++axis) {
            if (line == 0 || (job.affine_written & (1u << (bg * 2 + axis)))) {
                const unsigned offset = 0x28 + 0x10 * bg + 4 * axis;
                reference_[bg][axis] = s28(r.h(offset) | uint32_t{r.h(offset + 2)} << 16);
            }
        }
        const uint16_t control = r.h(0x0c + 2 * bg);
        const unsigned mosaic_v = control & 0x40 ? ((r.h(0x4c) >> 4) & 15) + 1 : 1;
        if (line % mosaic_v == 0) mosaic_reference_[bg] = reference_[bg];
    }
    latch_display(r.h(0), line);
    const uint16_t dispcnt = latched(r.h(0));
    std::memcpy(regs, &dispcnt, 2);
    const uint8_t* write = data;
    unsigned x = 0;
    objects_stale_ = true;
    for (unsigned i = 0; i < job.writes; ++i) {
        TimedWrite w;
        std::memcpy(&w, write + i * sizeof(TimedWrite), sizeof w);
        const unsigned at = std::min<unsigned>(kScreenWidth, w.cycle / 4);
        if (job.drawing && at > x) {
            render(regs, line, x, at);
            x = at;
        }
        const uint16_t value = w.offset == 0 ? latched(w.value) : w.value;
        std::memcpy(regs + w.offset, &value, 2);
        if (w.offset == 0 || w.offset == 0x4c) objects_stale_ = true;
    }
    if (job.drawing && x < kScreenWidth) render(regs, line, x, kScreenWidth);
    {
        RENDER_PART(kPartOutput);
        uint16_t* row = frame_ + line * kScreenWidth;
        uint16_t* to = target_ ? target_ + line * kScreenWidth : nullptr;
        if (to) {
            if (job.drawing) std::memcpy(to, line_.data(), kScreenWidth * 2);
            else if (sunk_[line]) std::memcpy(to, sunk_[line], kScreenWidth * 2);
            else to_rgb565(row, to, kScreenWidth);
            sunk_[line] = to;
        } else if (job.drawing) {
            to_bgr555(line_.data(), row, kScreenWidth);
            sunk_[line] = nullptr;
        }
        // The next line's place in the sink's buffer, while this job ends and
        // the next one draws.
        if (target_ && line + 1 < kVisibleLines)
            platform::preload_external(target_ + (line + 1) * kScreenWidth, kScreenWidth * 2);
    }
    // After each line the reference points move by PB and PD.
    for (unsigned bg = 0; bg < 2; ++bg) {
        reference_[bg][0] += s16(r.h(0x22 + 0x10 * bg));
        reference_[bg][1] += s16(r.h(0x26 + 0x10 * bg));
    }
}

// nba-hw-test ppu/dispcnt-latch: a BG or the OBJ layer enabled in DISPCNT
// shows from the third line that starts with it enabled, and forced blank
// ends likewise; disabling and forced blank take effect at once. Bits set
// at line 0 count as settled, as games write DISPCNT during VBlank.
void Renderer::latch_display(uint16_t dispcnt, unsigned line) {
    const unsigned bits = ((dispcnt >> 8) & 0x1f) | (dispcnt & 0x80 ? 0 : 0x20);
    for (unsigned i = 0; i < 6; ++i) {
        uint8_t& lines = enabled_lines_[i];
        if (!(bits & (1u << i))) lines = 0;
        else if (line == 0) lines = kLatchLines;
        else if (lines < kLatchLines) ++lines;
    }
    shown_ = 0;
    for (unsigned i = 0; i < 6; ++i)
        if (enabled_lines_[i] >= kLatchLines) shown_ |= static_cast<uint8_t>(1u << i);
}

uint16_t Renderer::latched(uint16_t dispcnt) const {
    dispcnt &= static_cast<uint16_t>(~0x1f00u | (shown_ & 0x1f) << 8);
    if (!(shown_ & 0x20)) dispcnt |= 0x80;
    return dispcnt;
}

void Renderer::render(const uint8_t* registers, unsigned line, unsigned from, unsigned to) {
    const Registers r{registers};
    const uint16_t dispcnt = r.h(0);
    uint16_t* out = line_.data();
    if (dispcnt & 0x80) {
        // Forced blank: white (GBATEK "LCD I/O Display Control").
        fill16(out + from, to - from, 0xffff);
        return;
    }
    const unsigned mode = dispcnt & 7;
    // The OBJs of the rest of the line at once: their registers stay.
    if (objects_stale_) {
        RENDER_PART(kPartObjects);
        objects(r, line, from, kScreenWidth);
        objects_stale_ = false;
    }
    bool effects;
    {
        RENDER_PART(kPartSetup);
        effects = setup(r, line, from, to);
    }
    {
        RENDER_PART(kPartPaint);
        paint_windows(r, line, mode, from, to, out, effects);
    }
    if (r.h(2) & 1) {
        // Green swap: each pair of pixels exchanges green (GBATEK "LCD I/O
        // Display Control"), bits 5-10 in RGB565.
        for (unsigned x = (from + 1) & ~1u; x + 1 < to; x += 2) {
            const uint16_t a = out[x], b = out[x + 1];
            out[x] = static_cast<uint16_t>((a & ~0x7e0) | (b & 0x7e0));
            out[x + 1] = static_cast<uint16_t>((b & ~0x7e0) | (a & 0x7e0));
        }
    }
}

// The BGs' colors in RGB565 after the brightness effect of setup().
const uint16_t* Renderer::bright_colors() {
    const uint32_t key = evy_ | (effect_ == 2 ? 0x100u : 0);
    if (key != bright_key_) {
        const auto* colors = reinterpret_cast<const uint16_t*>(palette_.data());
        for (unsigned i = 0; i < 256; ++i) bright_[i] = rgb565(brighten(colors[i] & 0x7fff, evy_, effect_ == 2));
        bright_key_ = key;
    }
    return bright_.data();
}

// GBATEK "LCD I/O BG Control", "LCD I/O BG Scrolling", "LCD VRAM BG Screen
// Data Format (BG Map)" and "LCD VRAM Character Data". Tile data past the
// 64 KiB of BG VRAM is drawn transparent; GBATEK "GBA Unpredictable
// Things" describes it only for one layer.
template <PaintMode kPaint>
void Renderer::text_bg(const Registers& r, unsigned n, unsigned line, unsigned from, unsigned to, uint16_t* out,
                       const uint8_t* shows) {
    const uint16_t control = r.h(0x08 + 2 * n);
    const unsigned hofs = r.h(0x10 + 4 * n) & 0x1ff, vofs = r.h(0x12 + 4 * n) & 0x1ff;
    const uint32_t char_base = ((control >> 2) & 3) * 0x4000, screen_base = ((control >> 8) & 31) * 0x800;
    const bool color256 = control & 0x80;
    const unsigned size = control >> 14;
    const unsigned width_mask = (size & 1) ? 511 : 255, height_mask = (size & 2) ? 511 : 255;
    const uint16_t mosaic = r.h(0x4c);
    const unsigned mh = control & 0x40 ? (mosaic & 15) + 1 : 1, mv = control & 0x40 ? ((mosaic >> 4) & 15) + 1 : 1;
    const unsigned y = line - line % mv;
    const unsigned py = (y + vofs) & height_mask;
    // A pixel of the BG, where it is opaque and its window shows it.
    const unsigned bit = 1u << n;
    const Pen<kPaint> pen{out, stack_.data(), acts_.data(), tags_[n], act(kPaint, n), fx_[n], eva_, evb_, evy_};
    const auto put = [&](unsigned x, uint16_t color) {
        if (!(color & kTransparent) && (!shows || (shows[x] & bit))) pen.put(x, color);
    };
    uint32_t row_base = screen_base + ((py & 255) >> 3) * 64;
    if (size == 3) row_base += (py >> 8) * 0x1000;
    else if (size == 2) row_base += (py >> 8) * 0x800;
    if (mh > 1) {
        const uint16_t* pal = reinterpret_cast<const uint16_t*>(palette_.data());
        auto pixel = [&](unsigned px) -> uint16_t {
            const uint32_t block = size & 1 ? (px >> 8) * 0x800 : 0;
            const uint16_t entry = u16(vram_ + ((row_base + block + ((px & 255) >> 3) * 2) & 0x1ffff) % kVramBytes);
            const unsigned tile = entry & 0x3ff;
            const unsigned tx = entry & 0x400 ? 7 - (px & 7) : px & 7;
            const unsigned ty = entry & 0x800 ? 7 - (py & 7) : py & 7;
            unsigned index;
            if (color256) {
                const uint32_t address = char_base + tile * 64 + ty * 8 + tx;
                if (address >= 0x10000) return kTransparent;
                index = vram_[address];
            } else {
                const uint32_t address = char_base + tile * 32 + ty * 4 + tx / 2;
                if (address >= 0x10000) return kTransparent;
                index = (vram_[address] >> (4 * (tx & 1))) & 15;
                if (index) index += (entry >> 12) * 16;
            }
            return index ? static_cast<uint16_t>(pal[index] & 0x7fff) : kTransparent;
        };
        for (unsigned x = from; x < to; ++x) put(x, pixel(((x - x % mh) + hofs) & width_mask));
        return;
    }
    // A tile at a time: one map entry and one row of tile data for up to
    // 8 pixels, `count` of them from column `first` of the tile. The map
    // entries lie below 11840h, inside VRAM; tile data at 64 KiB and past
    // is transparent.
    const unsigned ty = py & 7;
    const uint32_t straight = color256 ? ty * 8 : ty * 4, flipped = color256 ? (7 - ty) * 8 : (7 - ty) * 4;
    // The tiles' colors, RGB565 where the line takes them as they are
    // (kFxConvert); an index of 0 is transparent.
    const bool direct = kPaint == Paint::flat && (pen.fx & kFxConvert);
    const uint16_t* const colors = !direct ? bg8_.data() : pen.fx & 3 ? bright_colors() : bgc_.data();
    Pen<kPaint> tiles = pen;
    if (direct) tiles.fx = kFxNone;
    const auto put_index = [&](unsigned x, const uint16_t* palette, unsigned index) __attribute__((always_inline)) {
        if (index && (!shows || (shows[x] & bit))) tiles.put(x, palette[index]);
    };
    const auto tile = [&]<bool k256>(unsigned at, uint16_t entry, unsigned first, unsigned count)
                          __attribute__((always_inline)) {
        const uint32_t address = char_base + (entry & 0x3ff) * (k256 ? 64 : 32) + (entry & 0x800 ? flipped : straight);
        if (address >= 0x10000) return;
        const bool flip = entry & 0x400;
        if constexpr (k256) {
            // The row as two words, its pixels in reverse order when flipped.
            uint32_t low, high;
            std::memcpy(&low, vram_ + address, 4);
            std::memcpy(&high, vram_ + address + 4, 4);
            if (!(low | high)) return;
            if (flip) {
                const uint32_t swapped = swap_bytes(low);
                low = swap_bytes(high);
                high = swapped;
            }
            if (count == 8 && !shows && !zero_byte(low) && !zero_byte(high)) {
                tiles.put8(at, [&](unsigned i) { return colors[((i < 4 ? low : high) >> (8 * (i & 3))) & 255]; });
            } else {
                uint8_t indexes[8];
                std::memcpy(indexes, &low, 4);
                std::memcpy(indexes + 4, &high, 4);
                for (unsigned i = 0; i < count; ++i) put_index(at + i, colors, indexes[first + i]);
            }
            return;
        }
        uint32_t row;
        std::memcpy(&row, vram_ + address, 4);
        if (!row) return;
        if (flip) {
            // The nibbles in reverse order.
            row = (row >> 16) | (row << 16);
            row = ((row >> 8) & 0x00ff00ff) | ((row << 8) & 0xff00ff00);
            row = ((row >> 4) & 0x0f0f0f0f) | ((row << 4) & 0xf0f0f0f0);
        }
        // Color 0 of the bank is transparent.
        const uint16_t* bank = colors + (entry >> 12) * 16;
        if (kPaint == Paint::flat && count == 8 && !shows && !tiles.fx && row == 0xffffffffu) {
            // Opaque throughout: no pixel needs a test.
            fill16(out + at, 8, bank[15]);
        } else if (count == 8 && !shows && !zero_nibble(row)) {
            tiles.put8(at, [&](unsigned i) { return bank[(row >> (4 * i)) & 15]; });
        } else if (count == 8) {
            put_index(at, bank, row & 15);
            put_index(at + 1, bank, (row >> 4) & 15);
            put_index(at + 2, bank, (row >> 8) & 15);
            put_index(at + 3, bank, (row >> 12) & 15);
            put_index(at + 4, bank, (row >> 16) & 15);
            put_index(at + 5, bank, (row >> 20) & 15);
            put_index(at + 6, bank, (row >> 24) & 15);
            put_index(at + 7, bank, row >> 28);
        } else {
            row >>= 4 * first;
            for (unsigned i = 0; i < count; ++i, row >>= 4) put_index(at + i, bank, row & 15);
        }
    };
    // The tiles of [x, end) within one 256-pixel block of the map, whose
    // entries lie side by side from `map` on; the first from its column
    // `first`.
    const auto run = [&]<bool k256>(unsigned x, unsigned end, const Half* map, unsigned first)
                         __attribute__((always_inline)) {
        while (x < end) {
            const unsigned count = std::min(8 - first, end - x);
            tile.template operator()<k256>(x, *map++, first, count);
            x += count;
            first = 0;
        }
    };
    for (unsigned x = from; x < to;) {
        const unsigned px = (x + hofs) & width_mask;
        const uint32_t block = size & 1 ? (px >> 8) * 0x800 : 0;
        const auto* map = reinterpret_cast<const Half*>(vram_ + row_base + block) + ((px & 255) >> 3);
        const unsigned end = x + std::min(256 - (px & 255), to - x);
        if (color256) run.template operator()<true>(x, end, map, px & 7);
        else run.template operator()<false>(x, end, map, px & 7);
        x = end;
    }
}

// GBATEK "LCD I/O BG Rotation/Scaling" and "LCD VRAM Bitmap BG Modes".
template <PaintMode kPaint>
void Renderer::affine_bg(const Registers& r, unsigned n, unsigned mode, unsigned from, unsigned to, uint16_t* out,
                         const uint8_t* shows) {
    const uint16_t control = r.h(0x08 + 2 * n);
    const unsigned base = 0x20 + 0x10 * (n - 2);
    const int32_t pa = s16(r.h(base)), pc = s16(r.h(base + 4));
    const uint16_t mosaic = r.h(0x4c);
    const unsigned mh = control & 0x40 ? (mosaic & 15) + 1 : 1, mv = control & 0x40 ? ((mosaic >> 4) & 15) + 1 : 1;
    const auto& ref = mv > 1 ? mosaic_reference_[n - 2] : reference_[n - 2];
    const uint16_t dispcnt = r.h(0);
    const uint32_t frame = (dispcnt & 0x10) && (mode == 4 || mode == 5) ? 0xa000 : 0;
    // As text_bg()'s.
    const unsigned bit = 1u << n;
    const Pen<kPaint> pen{out, stack_.data(), acts_.data(), tags_[n], act(kPaint, n), fx_[n], eva_, evb_, evy_};
    const auto put = [&](unsigned x, uint16_t color) {
        if (!(color & kTransparent) && (!shows || (shows[x] & bit))) pen.put(x, color);
    };
    const int32_t size = 128 << (control >> 14);
    const uint32_t char_base = ((control >> 2) & 3) * 0x4000, screen_base = ((control >> 8) & 31) * 0x800;
    const bool wrap = control & 0x2000;
    // The texel of a pixel; out of the BG, a coordinate is negative or too
    // large, so one unsigned compare checks both ends.
    const auto texel = [&](int32_t tx, int32_t ty) -> uint16_t {
        switch (mode) {
        case 1: case 2: {
            if (wrap) {
                tx &= size - 1;
                ty &= size - 1;
            } else if (static_cast<uint32_t>(tx) >= static_cast<uint32_t>(size) ||
                       static_cast<uint32_t>(ty) >= static_cast<uint32_t>(size)) {
                return kTransparent;
            }
            const unsigned tile = vram_[(screen_base + (ty >> 3) * (size >> 3) + (tx >> 3)) % kVramBytes];
            const uint32_t address = char_base + tile * 64 + (ty & 7) * 8 + (tx & 7);
            return address < 0x10000 ? bg8_[vram_[address]] : kTransparent;
        }
        case 3:
            if (static_cast<uint32_t>(tx) >= 240 || static_cast<uint32_t>(ty) >= 160) return kTransparent;
            return static_cast<uint16_t>(u16(vram_ + (ty * 240 + tx) * 2) & 0x7fff);
        case 4:
            if (static_cast<uint32_t>(tx) >= 240 || static_cast<uint32_t>(ty) >= 160) return kTransparent;
            return bg8_[vram_[frame + ty * 240 + tx]];
        default:
            if (static_cast<uint32_t>(tx) >= 160 || static_cast<uint32_t>(ty) >= 128) return kTransparent;
            return static_cast<uint16_t>(u16(vram_ + frame + (ty * 160 + tx) * 2) & 0x7fff);
        }
    };
    if (mh > 1) {
        for (unsigned x = from; x < to; ++x) {
            const auto mx = static_cast<int32_t>(x - x % mh);
            put(x, texel((ref[0] + pa * mx) >> 8, (ref[1] + pc * mx) >> 8));
        }
        return;
    }
    // Without mosaic each pixel steps the point by PA and PC. A loop for
    // each mode keeps the mode's test out of the pixels; locals keep the
    // tables in registers, as the stores to out could alias the members.
    int32_t px = ref[0] + pa * static_cast<int32_t>(from), py = ref[1] + pc * static_cast<int32_t>(from);
    const uint8_t* const vram = vram_;
    const uint16_t* const palette = bg8_.data();
    if (mode >= 3 && pa == 0x100 && pc == 0) {
        // Not rotated or scaled across: one row of the bitmap, a texel a
        // pixel.
        const int32_t ty = py >> 8;
        const unsigned width = mode == 5 ? 160 : 240, height = mode == 5 ? 128 : 160;
        if (static_cast<uint32_t>(ty) >= height) return;
        // The pixels whose texel is in the row: [inside, outside).
        const int32_t tx = px >> 8;
        const int32_t first = std::clamp<int32_t>(static_cast<int32_t>(from) - tx, static_cast<int32_t>(from),
                                                  static_cast<int32_t>(to));
        const int32_t last = std::clamp<int32_t>(static_cast<int32_t>(width) + static_cast<int32_t>(from) - tx,
                                                 first, static_cast<int32_t>(to));
        const auto inside = static_cast<unsigned>(first), outside = static_cast<unsigned>(last);
        const int32_t shift = tx - static_cast<int32_t>(from);
        if (mode == 4) {
            const uint8_t* row = vram + frame + ty * 240 + shift;
            for (unsigned x = inside; x < outside; ++x) put(x, palette[row[x]]);
        } else {
            const uint8_t* row = vram + frame + (ty * static_cast<int32_t>(width) + shift) * 2;
            for (unsigned x = inside; x < outside; ++x) put(x, static_cast<uint16_t>(u16(row + x * 2) & 0x7fff));
        }
        return;
    }
    const auto run = [&](auto mode_constant) {
        constexpr unsigned kMode = decltype(mode_constant)::value;
        for (unsigned x = from; x < to; ++x, px += pa, py += pc) {
            int32_t tx = px >> 8, ty = py >> 8;
            uint16_t color;
            if constexpr (kMode == 2) {
                if (wrap) {
                    tx &= size - 1;
                    ty &= size - 1;
                }
                if (static_cast<uint32_t>(tx) < static_cast<uint32_t>(size) &&
                    static_cast<uint32_t>(ty) < static_cast<uint32_t>(size)) {
                    const unsigned tile = vram[(screen_base + (ty >> 3) * (size >> 3) + (tx >> 3)) % kVramBytes];
                    const uint32_t address = char_base + tile * 64 + (ty & 7) * 8 + (tx & 7);
                    color = address < 0x10000 ? palette[vram[address]] : kTransparent;
                } else {
                    color = kTransparent;
                }
            } else if constexpr (kMode == 3) {
                color = static_cast<uint32_t>(tx) < 240 && static_cast<uint32_t>(ty) < 160
                            ? static_cast<uint16_t>(u16(vram + (ty * 240 + tx) * 2) & 0x7fff)
                            : kTransparent;
            } else if constexpr (kMode == 4) {
                color = static_cast<uint32_t>(tx) < 240 && static_cast<uint32_t>(ty) < 160
                            ? palette[vram[frame + ty * 240 + tx]]
                            : kTransparent;
            } else {
                color = static_cast<uint32_t>(tx) < 160 && static_cast<uint32_t>(ty) < 128
                            ? static_cast<uint16_t>(u16(vram + frame + (ty * 160 + tx) * 2) & 0x7fff)
                            : kTransparent;
            }
            put(x, color);
        }
    };
    switch (mode) {
    case 1: case 2: run(std::integral_constant<unsigned, 2>{}); break;
    case 3: run(std::integral_constant<unsigned, 3>{}); break;
    case 4: run(std::integral_constant<unsigned, 4>{}); break;
    default: run(std::integral_constant<unsigned, 5>{}); break;
    }
}

// The OBJs whose rows cross each visible line: their visible lines are the
// height, twice that for a double-size affine OBJ, from Y on, wrapping at
// 256 (GBATEK "LCD OBJ - OAM Attributes").
void Renderer::list_objects() {
    for (unsigned word = 0; word < 4; ++word) {
        for (uint32_t moved = obj_moved_[word]; moved; moved &= moved - 1) {
            const unsigned i = word * 32 + lowest_bit(moved);
            const uint32_t bit = 1u << (i & 31);
            auto& [y, count] = obj_spans_[i];
            for (unsigned dy = 0; dy < count; ++dy) {
                const unsigned line = (y + dy) & 0xff;
                if (line < kVisibleLines) obj_lines_[line][word] &= ~bit;
            }
            const uint8_t* entry = oam_.data() + i * 8;
            const uint16_t a0 = u16(entry), a1 = u16(entry + 2);
            const bool affine = a0 & 0x100;
            y = static_cast<uint8_t>(a0 & 0xff);
            count = 0;
            if (!affine && (a0 & 0x200)) continue;
            const unsigned h = kObjSize[a0 >> 14][a1 >> 14][1];
            count = static_cast<uint8_t>(affine && (a0 & 0x200) ? 2 * h : h);
            for (unsigned dy = 0; dy < count; ++dy) {
                const unsigned line = (y + dy) & 0xff;
                if (line < kVisibleLines) obj_lines_[line][word] |= bit;
            }
        }
    }
    obj_moved_ = {};
    obj_lines_stale_ = false;
}

// Narrows [lo, hi) to the steps i with 0 <= start + step * i < limit.
static void inside(int32_t start, int32_t step, int32_t limit, unsigned& lo, unsigned& hi) {
    // The coordinates stay within 2^25 and the steps within 2^15, so 32
    // bits hold every term: RV32 divides those in hardware, 64 bits in a
    // libgcc call.
    const auto floor_div = [](int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    int32_t first, last;  // i in [first, last)
    if (step == 0) {
        first = 0;
        last = start >= 0 && start < limit ? static_cast<int32_t>(hi) : 0;
    } else if (step > 0) {
        first = -floor_div(start, step);  // ceil(-start / step)
        last = floor_div(limit - 1 - start, step) + 1;
    } else {
        first = floor_div(start - limit, -step) + 1;
        last = floor_div(start, -step) + 1;
    }
    lo = static_cast<unsigned>(std::clamp<int32_t>(first, static_cast<int32_t>(lo), static_cast<int32_t>(hi)));
    hi = static_cast<unsigned>(std::clamp<int32_t>(last, static_cast<int32_t>(lo), static_cast<int32_t>(hi)));
}

// GBATEK "LCD OBJ - Overview", "LCD OBJ - OAM Attributes", "LCD OBJ - OAM
// Rotation/Scaling Parameters" and "LCD OBJ - VRAM Character (Tile)
// Mapping". OBJs are drawn in OAM order and the first opaque pixel stays:
// a lower OAM index is in front. The line's OBJ cycles run out at 1210, or
// 954 with H-Blank Interval Free; drawing stops at the first OBJ that does
// not fit.
void Renderer::objects(const Registers& r, unsigned line, unsigned from, unsigned to) {
    obj_priorities_ = 0;
    obj_semi_ = false;
    obj_window_ = false;
    obj_bits_ = {};
    obj_semi_bits_ = {};
    obj_window_bits_ = {};
    obj_covered_ = {};
    uint32_t* const covered_bits = obj_covered_.data();
    const uint16_t dispcnt = r.h(0);
    if (!(dispcnt & 0x1000)) return;
    if (obj_lines_stale_) list_objects();
    const int budget = dispcnt & 0x20 ? 954 : 1210;
    const bool one_dim = dispcnt & 0x40, bitmap = (dispcnt & 7) >= 3;
    const uint16_t mosaic = r.h(0x4c);
    const unsigned omh = ((mosaic >> 8) & 15) + 1, omv = ((mosaic >> 12) & 15) + 1;
    int used = 0;
    for (unsigned word = 0; word < 4; ++word) {
        for (uint32_t bits = obj_lines_[line][word]; bits; bits &= bits - 1) {
            const unsigned i = word * 32 + lowest_bit(bits);
            const uint8_t* entry = oam_.data() + i * 8;
            const uint16_t a0 = u16(entry), a1 = u16(entry + 2), a2 = u16(entry + 4);
            const bool affine = a0 & 0x100;
            const unsigned shape = a0 >> 14;
            const int w = kObjSize[shape][a1 >> 14][0], h = kObjSize[shape][a1 >> 14][1];
            const bool twice = affine && (a0 & 0x200);
            const int bw = twice ? 2 * w : w, bh = twice ? 2 * h : h;
            const int cost = affine ? 10 + 2 * bw : w;
            if (used + cost > budget) return;
            used += cost;
            const unsigned tile_name = a2 & 0x3ff;
            if (bitmap && tile_name < 512) continue;
            const unsigned y = a0 & 0xff, x = a1 & 0x1ff;
            // The OBJ's columns that land in [from, to): one run, as an OBJ
            // is narrower than the 272 columns off the screen.
            unsigned begin = 0, end = 0;
            if (x >= from && x < to) {
                end = std::min<unsigned>(static_cast<unsigned>(bw), to - x);
            } else {
                begin = (from - x) & 0x1ff;
                end = std::min<unsigned>(static_cast<unsigned>(bw), begin + (to - from));
            }
            if (begin >= end) continue;
            const unsigned first_x = (x + begin) & 0x1ff;
            const bool obj_mosaic = a0 & 0x1000;
            const unsigned sy = obj_mosaic ? line - line % omv : line;
            // Blocks one pixel wide are the pixels themselves.
            const bool wide_mosaic = obj_mosaic && omh > 1;
            const int dy = static_cast<int>((sy - y) & 0xff);
            const bool color256 = a0 & 0x2000;
            const unsigned mode = (a0 >> 10) & 3;
            const unsigned priority = (a2 >> 10) & 3;
            const bool semi = mode == 1;
            const uint16_t* bank = color256 ? obj8_.data() : obj4_.data() + (a2 >> 12) * 16;
            uint32_t* const drawn_bits = obj_bits_[priority].data();
            bool drawn = false;
            // Whether an OBJ in front has an opaque pixel at sx.
            const auto covered = [&](unsigned sx) __attribute__((always_inline)) {
                return covered_bits[sx >> 5] >> (sx & 31) & 1;
            };
            // An opaque pixel at sx where no OBJ in front has one.
            const auto keep = [&](unsigned sx, uint16_t color) __attribute__((always_inline)) {
                obj_[sx] = color;
                const uint32_t bit = 1u << (sx & 31);
                drawn_bits[sx >> 5] |= bit;
                covered_bits[sx >> 5] |= bit;
                if (semi) obj_semi_bits_[sx >> 5] |= bit;
                drawn = true;
            };
            // A pixel the OBJ has at sx, color 0 being transparent.
            const auto put = [&](unsigned sx, uint16_t color) {
                if (color == kTransparent) return;
                if (mode == 2) {
                    obj_window_bits_[sx >> 5] |= 1u << (sx & 31);
                    obj_window_ = true;
                } else if (!covered(sx)) {
                    keep(sx, color);
                }
            };
            // The color at (tx, ty) within the OBJ.
            const auto color_at = [&](int tx, int ty) __attribute__((always_inline)) -> uint16_t {
                if (color256) {
                    const unsigned tile = one_dim ? tile_name + ((ty / 8) * (w / 8) + tx / 8) * 2
                                                  : (tile_name & ~1u) + (ty / 8) * 32 + (tx / 8) * 2;
                    return bank[vram_[0x10000 + (tile & 0x3ff) * 32 + (ty % 8) * 8 + tx % 8]];
                }
                const unsigned tile = one_dim ? tile_name + (ty / 8) * (w / 8) + tx / 8 : tile_name + (ty / 8) * 32 + tx / 8;
                return bank[(vram_[0x10000 + (tile & 0x3ff) * 32 + (ty % 8) * 4 + (tx % 8) / 2] >> (4 * (tx & 1))) & 15];
            };
            if (affine) {
                // The texture coordinates in 8.8 fixed point, ((PA * (col -
                // cx) + PB * (row - cy)) >> 8) + w / 2 and the same with PC
                // and PD: from column 0 on, a column at a time, or with
                // mosaic from the column each block of pixels takes.
                const uint8_t* group = oam_.data() + ((a1 >> 9) & 31) * 32;
                const int32_t pa = s16(u16(group + 6)), pb = s16(u16(group + 14)), pc = s16(u16(group + 22)),
                              pd = s16(u16(group + 30));
                const int cx = bw / 2, cy = bh / 2;
                const int32_t u0 = pb * (dy - cy) + (w / 2) * 256 - pa * cx, v0 = pd * (dy - cy) + (h / 2) * 256 - pc * cx;
                // The color at texture coordinates u and v, transparent
                // outside the OBJ.
                const auto texel = [&](int32_t u, int32_t v) __attribute__((always_inline)) -> uint16_t {
                    const auto tx = static_cast<unsigned>(u >> 8), ty = static_cast<unsigned>(v >> 8);
                    if (tx >= static_cast<unsigned>(w) || ty >= static_cast<unsigned>(h)) return kTransparent;
                    return color_at(static_cast<int>(tx), static_cast<int>(ty));
                };
                const unsigned count = end - begin;
                if (!wide_mosaic) {
                    // Only the columns whose coordinates both fall inside.
                    int32_t u = u0 + pa * static_cast<int>(begin), v = v0 + pc * static_cast<int>(begin);
                    unsigned lo = 0, hi = count;
                    inside(u, pa, w * 256, lo, hi);
                    inside(v, pc, h * 256, lo, hi);
                    u += pa * static_cast<int32_t>(lo);
                    v += pc * static_cast<int32_t>(lo);
                    // color_at() with the OBJ's layout worked out once: a
                    // row of its tiles is 1 << shift tile numbers on.
                    const unsigned shift = one_dim ? lowest_bit(static_cast<uint32_t>(w)) - 3 + (color256 ? 1 : 0) : 5;
                    const unsigned base = color256 && !one_dim ? tile_name & ~1u : tile_name;
                    const uint8_t* const tiles = vram_ + 0x10000;
                    // A word of the bitmaps at a time: the pixels the OBJs
                    // in front cover, and those this one adds.
                    const auto run = [&]<bool k256>() __attribute__((always_inline)) {
                        for (unsigned sx = first_x + lo, last = first_x + hi; sx < last;) {
                            const unsigned word = sx >> 5, stop = std::min(last, word * 32 + 32);
                            const uint32_t taken = mode == 2 ? 0 : covered_bits[word];
                            uint32_t fresh = 0;
                            for (; sx < stop; ++sx, u += pa, v += pc) {
                                const uint32_t bit = 1u << (sx & 31);
                                if (taken & bit) continue;
                                const auto tx = static_cast<unsigned>(u >> 8), ty = static_cast<unsigned>(v >> 8);
                                const unsigned tile = base + ((ty >> 3) << shift) + ((tx >> 3) << (k256 ? 1 : 0));
                                const uint8_t* row = tiles + (tile & 0x3ff) * 32 + (ty & 7) * (k256 ? 8 : 4);
                                const unsigned index = k256 ? row[tx & 7] : (row[(tx & 7) >> 1] >> (4 * (tx & 1))) & 15;
                                if (!index) continue;
                                fresh |= bit;
                                if (mode != 2) obj_[sx] = bank[index];
                            }
                            if (!fresh) continue;
                            if (mode == 2) {
                                obj_window_bits_[word] |= fresh;
                                obj_window_ = true;
                                continue;
                            }
                            drawn_bits[word] |= fresh;
                            covered_bits[word] |= fresh;
                            if (semi) obj_semi_bits_[word] |= fresh;
                            drawn = true;
                        }
                    };
                    if (color256) run.template operator()<true>();
                    else run.template operator()<false>();
                } else {
                    // A block of pixels takes the color of its first column.
                    unsigned phase = first_x % omh;  // sx % omh
                    for (unsigned sx = first_x, last = first_x + count; sx < last; phase = 0) {
                        const unsigned run = std::min(omh - phase, last - sx);
                        const auto col = static_cast<int32_t>((sx - phase - x) & 0x1ff);
                        const uint16_t color = texel(u0 + pa * col, v0 + pc * col);
                        if (color != kTransparent) {
                            for (unsigned k = sx; k < sx + run; ++k)
                                if (mode == 2 || !covered(k)) put(k, color);
                        }
                        sx += run;
                    }
                }
            } else if (wide_mosaic) {
                // Each block of pixels takes its first column's.
                const int ty = a1 & 0x2000 ? h - 1 - dy : dy;
                unsigned phase = first_x % omh;  // sx % omh
                for (unsigned sx = first_x, last = first_x + (end - begin); sx < last;
                     ++sx, phase = phase + 1 == omh ? 0 : phase + 1) {
                    if (mode != 2 && covered(sx)) continue;
                    const auto col = static_cast<int>((sx - phase - x) & 0x1ff);
                    const int tx = a1 & 0x1000 ? w - 1 - col : col;
                    if (tx < 0 || tx >= w) continue;
                    put(sx, color_at(tx, ty));
                }
            } else {
                // A row of tile data at a time.
                const bool hflip = a1 & 0x1000;
                const int ty = a1 & 0x2000 ? h - 1 - dy : dy;
                const unsigned row_tiles = one_dim ? static_cast<unsigned>(w / 8) : 32;
                for (unsigned ix = begin; ix < end;) {
                    const unsigned tx = hflip ? static_cast<unsigned>(w) - 1 - ix : ix;
                    const unsigned within = tx & 7;
                    const unsigned count = std::min(hflip ? within + 1 : 8 - within, end - ix);
                    const unsigned sx = first_x + (ix - begin);
                    ix += count;
                    if (color256) {
                        const unsigned tile = one_dim ? tile_name + ((ty / 8) * row_tiles + tx / 8) * 2
                                                      : (tile_name & ~1u) + (ty / 8) * row_tiles + (tx / 8) * 2;
                        const uint8_t* row = vram_ + 0x10000 + (tile & 0x3ff) * 32 + (ty % 8) * 8;
                        for (unsigned k = 0; k < count; ++k) put(sx + k, bank[row[hflip ? within - k : within + k]]);
                    } else {
                        const unsigned tile = tile_name + (ty / 8) * row_tiles + tx / 8;
                        uint32_t row;
                        std::memcpy(&row, vram_ + 0x10000 + (tile & 0x3ff) * 32 + (ty % 8) * 4, 4);
                        if (!row) continue;
                        // The pixels in screen order from the low nibble:
                        // flipped, the row's nibbles reversed.
                        if (hflip) {
                            row = swap_bytes(row);
                            row = ((row >> 4) & 0x0f0f0f0fu) | ((row & 0x0f0f0f0fu) << 4);
                        }
                        row >>= 4 * (hflip ? 7 - within : within);
                        // The row's opaque pixels, color 0 alone being
                        // transparent in a 16-color bank.
                        const uint32_t opaque = opaque_nibbles(row) & ((1u << count) - 1);
                        if (mode == 2) {
                            set8(obj_window_bits_.data(), sx, opaque);
                            obj_window_ |= opaque != 0;
                            continue;
                        }
                        const uint32_t fresh = opaque & ~bits8(covered_bits, sx);
                        if (!fresh) continue;
                        if (fresh == 0xff) {
#pragma GCC unroll 8
                            for (unsigned k = 0; k < 8; ++k) obj_[sx + k] = bank[(row >> (4 * k)) & 15];
                        } else {
                            for (uint32_t m = fresh; m; m &= m - 1) {
                                const unsigned k = lowest_bit(m);
                                obj_[sx + k] = bank[(row >> (4 * k)) & 15];
                            }
                        }
                        set8(drawn_bits, sx, fresh);
                        set8(covered_bits, sx, fresh);
                        if (semi) set8(obj_semi_bits_.data(), sx, fresh);
                        drawn = true;
                    }
                }
            }
            if (drawn) {
                obj_priorities_ |= static_cast<uint8_t>(1u << priority);
                obj_semi_ |= semi;
            }
        }
    }
}

// Windows, priorities and color effects (GBATEK "LCD I/O Window Feature",
// "LCD I/O Color Special Effects"). A window whose X1 > X2 or Y1 > Y2 wraps
// around (Tonc, gbadoc; confirmed by the user in task 1-2). Each pixel
// shows its top two layers for the effects: BGs by priority, the lower
// number in front, and an OBJ in front of the BGs of its priority.
bool Renderer::setup(const Registers& r, unsigned line, unsigned from, unsigned to) {
    const uint16_t dispcnt = r.h(0);
    const unsigned mode = dispcnt & 7;
    static constexpr uint8_t kValid[8] = {0xf, 0x7, 0xc, 0x4, 0x4, 0x4, 0, 0};
    const unsigned enabled = (dispcnt >> 8) & kValid[mode];
    objs_ = (dispcnt & 0x1000) && obj_priorities_;
    const uint16_t bldcnt = r.h(0x50), bldalpha = r.h(0x52), bldy = r.h(0x54);
    eva_ = std::min(16u, bldalpha & 31u);
    evb_ = std::min(16u, (bldalpha >> 8) & 31u);
    evy_ = std::min(16u, bldy & 31u);
    effect_ = (bldcnt >> 6) & 3;
    backdrop_ = u16(palette_.data()) & 0x7fff;
    windows_ = dispcnt & 0xe000;
    if (windows_) {
        // Outside, then the OBJ window, window 1 and window 0 over it.
        const uint16_t winin = r.h(0x48), winout = r.h(0x4a);
        std::memset(window_.data() + from, winout & 0x3f, to - from);
        cut_count_ = 0;
        window_pixels_ = (dispcnt & 0x8000) && obj_window_;
        if (window_pixels_) {
            const auto inside = static_cast<uint8_t>((winout >> 8) & 0x3f);
            for (unsigned x = from; x < to; ++x)
                if (obj_window_bits_[x >> 5] >> (x & 31) & 1) window_[x] = inside;
        }
        for (int w = 1; w >= 0; --w) {
            if (!(dispcnt & (0x2000 << w))) continue;
            const uint16_t h = r.h(0x40 + 2 * w), v = r.h(0x44 + 2 * w);
            const unsigned y1 = v >> 8, y2 = v & 0xff;
            if (!(y1 <= y2 ? (line >= y1 && line < y2) : (line >= y1 || line < y2))) continue;
            const unsigned x1 = h >> 8, x2 = std::min<unsigned>(h & 0xff, kScreenWidth);
            const auto inside = static_cast<uint8_t>((winin >> (8 * w)) & 0x3f);
            const auto span = [&](unsigned begin, unsigned end) {
                begin = std::max(begin, from);
                end = std::min(end, to);
                if (begin >= end) return;
                std::memset(window_.data() + begin, inside, end - begin);
                // Only the cuts inside the segment, at most two a window:
                // a window that wraps around starts one span at `from`
                // and ends the other at `to`.
                if (begin > from) cuts_[cut_count_++] = static_cast<uint8_t>(begin);
                if (end < to) cuts_[cut_count_++] = static_cast<uint8_t>(end);
            };
            if (x1 <= (h & 0xff)) {
                span(x1, x2);
            } else {
                span(x1, kScreenWidth);
                span(0, x2);
            }
        }
    }
    const unsigned first = bldcnt & 0x3f, second = (bldcnt >> 8) & 0x3f;
    layer_count_ = 0;
    for (unsigned priority = 0; priority < 4; ++priority) {
        for (unsigned n = 0; n < 4; ++n) {
            if ((enabled & (1u << n)) && (r.h(0x08 + 2 * n) & 3) == priority)
                layers_[layer_count_++] = {static_cast<uint8_t>(priority), static_cast<uint8_t>(n)};
        }
    }
    // Alpha blending with EVA 16 and EVB 0 leaves every color as it is, for
    // semi-transparent OBJs too, which games set to turn blending off, and
    // so does a brightness effect with EVY 0. A semi-transparent OBJ blends
    // only over a second target, and then takes no brightness effect
    // (GBATEK "LCD I/O Color Special Effects"): without second targets, or
    // with EVA 16 and EVB 0 where the OBJs take no brightness effect, it
    // looks as an opaque one does.
    const bool identity = eva_ == 16 && evb_ == 0;
    const bool unblended = effect_ == 1 && identity;
    const bool semi = objs_ && obj_semi_ && second && (!identity || (effect_ >= 2 && (first & 0x10) && evy_));
    semi_ = semi;
    if (!semi && !(effect_ == 1 && first && second && !unblended) && !(effect_ >= 2 && first && evy_)) return false;
    for (unsigned layer = 0; layer < 6; ++layer) {
        const bool target = first >> layer & 1;
        tags_[layer] = static_cast<uint16_t>((second >> layer & 1) << 15);
        layer_acts_[layer] = static_cast<uint8_t>((effect_ == 1 && target && !unblended ? kAlpha : 0) |
                                                  (effect_ >= 2 && target ? kFade : 0));
    }
    tags_[6] = tags_[4];
    layer_acts_[6] = static_cast<uint8_t>(layer_acts_[4] | (semi ? kSemiAlpha : 0));
    // Effects that paint() can apply as it draws each layer, back to front,
    // without the stack. Flat: the brightness effect, which takes only the
    // top layer, or alpha blending of a single first target BG over layers
    // that are all second targets, the backdrop's included. Tracked: alpha
    // blending of first targets and semi-transparent OBJs where none of
    // those is a second target, so that what they blend over is a layer
    // as it was drawn; acts tells whether it is a second target. The OBJ
    // window's effects change pixel by pixel and keep the stack, as do the
    // rest.
    paint_ = Paint::stacked;
    if (window_pixels_) return true;
    layer_fx_ = {};
    const unsigned present = enabled | (objs_ ? 0x10u : 0) | 0x20u, firsts = first & present;
    if (effect_ >= 2) {
        // A semi-transparent OBJ over a second target takes no brightness
        // effect.
        if (semi) return true;
        for (unsigned layer = 0; layer < 6; ++layer)
            if (first >> layer & 1) layer_fx_[layer] = effect_ == 2 ? kFxUp : kFxDown;
        paint_ = Paint::flat;
        return true;
    }
    // Without semi-transparent OBJs that blend, the effect is 1 here.
    if (!semi && (!firsts || (firsts < 0x10 && !(firsts & (firsts - 1)) && !(present & ~firsts & ~second)))) {
        if (firsts) layer_fx_[lowest_bit(firsts)] = kFxBlend;
        paint_ = Paint::flat;
        return true;
    }
    const unsigned blending = effect_ == 1 && !unblended ? firsts : 0;
    if (!(blending & second) && !((semi || (blending & 0x10)) && (second & 0x10))) {
        for (unsigned layer = 0; layer < 5; ++layer)
            if (blending >> layer & 1) layer_fx_[layer] = kFxBlend;
        paint_ = Paint::tracked;
    }
    return true;
}

// Back to front: the backdrop, then for each priority from 3 to 0 its BGs
// from the highest number down, each drawing its opaque pixels that its
// window shows, then the OBJ pixels of that priority, so that an OBJ is in
// front of the BGs of its priority and behind those of a lower one. With
// Paint::stacked each pixel keeps its top two layers for resolve(); the
// backdrop starts as both, so that it blends with itself as the top layer.
template <PaintMode kPaint>
void Renderer::paint(const Registers& r, unsigned line, unsigned mode, unsigned from, unsigned to, uint16_t* out,
                     unsigned shown, const uint8_t* shows) {
    if constexpr (kPaint == Paint::stacked) {
        const uint32_t backdrop = backdrop_ | tags_[5];
        std::fill(stack_.begin() + from, stack_.begin() + to, backdrop << 16 | backdrop);
        std::memset(acts_.data() + from, layer_acts_[5], to - from);
    } else {
        // With bit 5 of `shown` the layers take their effects, layer_fx_.
        fx_ = shown & 0x20 ? layer_fx_ : decltype(fx_){};
        uint16_t backdrop = backdrop_;
        if (kPaint == Paint::flat && fx_[5]) backdrop = brighten(backdrop, evy_, fx_[5] == kFxUp);
        if (kPaint == Paint::flat && std::find(fx_.begin(), fx_.end(), kFxBlend) == fx_.end()) {
            for (auto& fx : fx_) fx |= kFxConvert;
            backdrop = rgb565(backdrop);
        }
        fill16(out + from, to - from, backdrop);
        if (kPaint == Paint::tracked) std::memset(acts_.data() + from, act(kPaint, 5), to - from);
    }
    const Pen<kPaint> obj{out, stack_.data(), acts_.data(), tags_[4], act(kPaint, 4), fx_[4], eva_, evb_, evy_};
    // Semi-transparent OBJs blend outside the windows' effects too.
    const Pen<kPaint> semi{out,  stack_.data(), acts_.data(), tags_[6], act(kPaint, 6),
                           kPaint == Paint::tracked ? kFxBlend : kFxNone, eva_, evb_, evy_};
    unsigned k = layer_count_;
    for (int priority = 3; priority >= 0; --priority) {
        for (; k && layers_[k - 1].priority == static_cast<unsigned>(priority); --k) {
            const unsigned n = layers_[k - 1].number;
            if (!(shown >> n & 1)) continue;
            RENDER_PART(kPartLayers);
            if (mode == 0 || (mode == 1 && n < 2)) text_bg<kPaint>(r, n, line, from, to, out, shows);
            else affine_bg<kPaint>(r, n, mode, from, to, out, shows);
        }
        if (!objs_ || !(obj_priorities_ >> priority & 1) || !(shown & 0x10)) continue;
        RENDER_PART(kPartObjLayer);
        // The OBJ pixels of this priority in [from, to).
        const auto& bits = obj_bits_[priority];
        for (unsigned w = from >> 5, last = (to - 1) >> 5; w <= last; ++w) {
            uint32_t pending = bits[w];
            if (w == from >> 5) pending &= ~0u << (from & 31);
            if (w == last) pending &= ~0u >> (31 - ((to - 1) & 31));
            // Semi-transparent OBJs blend only when the stack or acts tells
            // what is under them.
            uint32_t semis = 0;
            if constexpr (kPaint != Paint::flat) {
                semis = pending & obj_semi_bits_[w];
                pending &= ~semis;
            }
            // The OBJs' pixels mostly come in runs: a run of bits at a time.
            const auto runs = [&](uint32_t pending, const Pen<kPaint>& pen) __attribute__((always_inline)) {
                while (pending) {
                    const unsigned first = lowest_bit(pending);
                    // Adding the lowest bit carries through the run.
                    const uint32_t after = pending + (pending & (0u - pending));
                    const unsigned end = w * 32 + (after ? lowest_bit(after) : 32);
                    pending &= after;
                    for (unsigned x = w * 32 + first; x < end; ++x)
                        if (!shows || (shows[x] & 0x10)) pen.put(x, obj_[x]);
                }
            };
            runs(pending, obj);
            runs(semis, semi);
        }
    }
    // The line's format, unless the layers wrote it already.
    if (kPaint == Paint::tracked || (kPaint == Paint::flat && !(fx_[5] & kFxConvert))) {
        RENDER_PART(kPartConvert);
        to_rgb565(out, from, to);
    }
}

void Renderer::paint_windows(const Registers& r, unsigned line, unsigned mode, unsigned from, unsigned to,
                             uint16_t* out, bool effects) {
    if (!windows_) {
        if (!effects) {
            paint<Paint::flat>(r, line, mode, from, to, out, 0x1f, nullptr);
        } else if (paint_ == Paint::flat) {
            paint<Paint::flat>(r, line, mode, from, to, out, 0x3f, nullptr);
        } else if (paint_ == Paint::tracked) {
            paint<Paint::tracked>(r, line, mode, from, to, out, 0x3f, nullptr);
        } else {
            paint<Paint::stacked>(r, line, mode, from, to, out, 0x1f, nullptr);
            resolve<false>(from, to, out);
        }
        return;
    }
    const uint8_t* const window = window_.data();
    if (window_pixels_) {
        if (!effects) {
            paint<Paint::flat>(r, line, mode, from, to, out, 0x1f, window);
            return;
        }
        paint<Paint::stacked>(r, line, mode, from, to, out, 0x1f, window);
        resolve<true>(from, to, out);
        return;
    }
    // Window 0 and 1 change what shows only where they begin and end: the
    // spans between those cuts, joined where they show the same.
    std::array<uint8_t, std::tuple_size_v<decltype(cuts_)> + 1> cuts;
    unsigned count = 0;
    for (unsigned i = 0; i < cut_count_; ++i) {
        const uint8_t cut = cuts_[i];
        unsigned k = count++;
        for (; k && cuts[k - 1] > cut; --k) cuts[k] = cuts[k - 1];
        cuts[k] = cut;
    }
    cuts[count] = static_cast<uint8_t>(to);
    unsigned begin = from;
    for (unsigned i = 0; i <= count; ++i) {
        const unsigned end = cuts[i];
        if (end < to && window[end] == window[begin]) continue;
        // A span outside the windows' effects keeps one layer, unless a
        // semi-transparent OBJ blends there; inside, every pixel may take
        // the effects.
        const uint8_t shown = window[begin];
        if (effects && paint_ == Paint::flat) {
            paint<Paint::flat>(r, line, mode, begin, end, out, shown, nullptr);
        } else if (effects && paint_ == Paint::tracked && ((shown & 0x20) || semi_)) {
            paint<Paint::tracked>(r, line, mode, begin, end, out, shown, nullptr);
        } else if (effects && (shown & 0x20)) {
            paint<Paint::stacked>(r, line, mode, begin, end, out, shown & 0x1f, nullptr);
            resolve<false>(begin, end, out);
        } else if (effects && semi_) {
            paint<Paint::stacked>(r, line, mode, begin, end, out, shown & 0x1f, nullptr);
            resolve<true>(begin, end, out);
        } else {
            paint<Paint::flat>(r, line, mode, begin, end, out, shown & 0x1f, nullptr);
        }
        begin = end;
    }
}

// The color effects on the top layer of each pixel that paint<true>() left
// on the stack: alpha blending with the layer under it, or the brightness
// effect. Outside the windows' effects only a semi-transparent OBJ blends.
template <bool kWindows>
void Renderer::resolve(unsigned from, unsigned to, uint16_t* out) {
    const uint32_t* const stack = stack_.data();
    const uint8_t* const acts = acts_.data();
    const uint8_t* const window = window_.data();
    const unsigned eva = eva_, evb = evb_, evy = evy_;
    const bool up = effect_ == 2;
    for (unsigned x = from; x < to; ++x) {
        const uint32_t pixels = stack[x];
        unsigned act = acts[x];
        if (kWindows && !(window[x] & 0x20)) act &= kSemiAlpha;
        auto color = static_cast<uint16_t>(pixels & 0x7fff);
        if (act) {
            // Bit 31: the layer under it is a second target.
            if ((act & (kAlpha | kSemiAlpha)) && (pixels >> 31))
                color = blend(color, static_cast<uint16_t>(pixels >> 16), eva, evb);
            else if (act & kFade)
                color = brighten(color, evy, up);
        }
        out[x] = rgb565(color);
    }
}

}  // namespace gbx::gba
