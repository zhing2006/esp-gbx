#pragma once

// The renderer on the worker of the other core and the scanline rows it
// receives. Only ppu.cpp drives it; snapshot.cpp copies its state while the
// worker is idle.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>

#include "gbx/gb/mode.h"
#include "gbx/gb/ppu.h"
#include "offload.h"

namespace gbx::gb {
// The rows are local to each file that includes this, as they were when the
// renderer lived in ppu.cpp: the compiler then keeps the renderer's code and
// its place in the hot code as before. Only the layout is shared.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#endif
namespace {
constexpr std::array<uint16_t, 4> kGrays{0x7fff, 0x56b5, 0x294a, 0};
constexpr uint16_t kWhite = 0x7fff;
// Each byte with its bits in reverse order, for horizontally flipped CGB tiles.
constexpr auto kReverse = [] {
    std::array<uint8_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i)
        for (unsigned bit = 0; bit < 8; ++bit) if (i & (1u << bit)) table[i] |= static_cast<uint8_t>(0x80u >> bit);
    return table;
}();
// Color numbers of four pixels, the leftmost in the low byte. Bits 0..3 of
// the index are a nibble of the low bitplane, bits 4..7 of the high one.
constexpr auto kNibbleColors = [] {
    std::array<uint32_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i)
        for (unsigned k = 0; k < 4; ++k)
            table[i] |= (((i >> (3 - k)) & 1) | (((i >> (7 - k)) & 1) << 1)) << (k * 8);
    return table;
}();
// A write during the line: VRAM bank 0 from 8000, bank 1 from A000 (offset
// 2000 of the row's VRAM), OAM and the PPU registers.
struct Change { uint16_t dot, address; uint8_t value; };
struct Row {
    enum class Kind : uint8_t { line, frame, clear } kind = Kind::line;
    Mode mode = Mode::dmg;  // Frame and clear rows only.
    uint8_t y = 0, window_line = 0, count = 0;
    // double_speed: the CPU's speed while the line is in transfer.
    bool window_y = false, blank = false, double_speed = false;
    // The first dot the window may start at, when WY matched LY in mode 3,
    // see Ppu::check_wy(); 0 otherwise. window_carry: the window shows from
    // the start of the line; window_count: the window lines it advanced by
    // first. See Ppu::window_carry_.
    uint16_t window_from = 0;
    bool window_carry = false;
    uint8_t window_count = 0;
    // dirty: VRAM bank 0 pages; dirty1: bank 1 pages, on a CGB.
    uint64_t id = 0, dirty = 0, dirty1 = 0;
    std::array<uint8_t, 12> registers{};
    // oam is valid only when oam_changed; the renderer keeps the last copy.
    bool oam_changed = false;
    std::array<uint8_t, 160> oam;
    std::array<uint8_t, 10> objects{};
    // Only dirty 128-byte pages are copied or read. Jobs never borrow live
    // VRAM. Bank 1 follows bank 0.
    std::array<uint8_t, 16384> vram;
    // CGB palette RAM, valid only when palettes_changed, like oam.
    bool palettes_changed = false;
    std::array<uint8_t, 128> palettes;
    std::array<Change, 192> changes{};
    unsigned changes_size = 0;
    unsigned fine = 0;
};
// The timeline of a line in mode 3, in the renderer's dots (see
// Ppu::change()): the OBJ fetches and the window restart pause the pixels,
// pixel x leaves at delayed(18 + fine + x), and mode 3 ends with pixel 159.
// reg(index, dot) gives a register (index from FF40) as of a dot, so writes
// in mode 3 count from the dot they land on. The renderer draws from it, and
// the PPU times mode 3 from it when LCDC or WX changes in mode 3.
struct Timeline {
    struct Pause { int start, duration; bool window = false; };
    std::array<Pause, 11> pauses{};
    unsigned pause_count = 0;
    std::array<int, 10> object_fetch{};
    // The window's first pixel, -1 if it does not start, the dot it starts
    // at and its WX - 7.
    int window_trigger = -1, window_trigger_dot = 0, wx = 0;
    int delayed(int dot) const {
        for (unsigned i = 0; i < pause_count; ++i)
            if (dot >= pauses[i].start) dot += pauses[i].duration;
        return dot;
    }
};
// What decides the window of a line: WY matched LY (y) and the first dot
// it may start at (from), see Ppu::check_wy(); it shows from the start of
// the line (carry), see Ppu::window_carry_; a DMG's PPU (dmg); and the LCDC
// bits that show it.
struct WindowInputs { bool y = false, carry = false, dmg = false; int from = 0; unsigned bits = 0x21; };
// objects: the line's objects, sorted as Ppu::select_objects() does, with
// their OAM entries in oam. A window that shows from the start of the line
// restarts nowhere, and the OBJ fetches keep the BG's tile grid. A DMG's
// window at WX 166 adds 2 dots less the fine scroll, see transfer_length().
template<class Reg> void make_timeline(Timeline& t, const uint8_t* oam, const std::array<uint8_t, 10>& objects,
                                       unsigned count, int fine, const WindowInputs& w, Reg reg) {
    const bool window_y = w.y && !w.carry;
    const int window_from = w.from;
    const unsigned window_bits = w.bits;
    t.object_fetch.fill(-1);
    int last_tile = -100;
    for (unsigned i = 0; i < count; ++i) {
        const auto* obj = oam + objects[i] * 4;
        const int ox = obj[1], coordinate = ox - 8 + fine;
        if (ox >= 168) continue;
        const int start = t.delayed(18 + coordinate);
        if (!(reg(0, start) & 2)) continue;
        const int tile = coordinate >= 0 ? coordinate / 8 : -1;
        const int penalty = ox == 0 ? (last_tile == -1 ? 6 : 11) :
            6 + (tile == last_tile ? 0 : std::max(0, 5 - (coordinate & 7)));
        // Turning the objects off during the fetch ends it the dot before:
        // Gambatte sprites/sprite_late_disable_* and late_disable_* on
        // dmg08, at OBJ X 18 to 1B.
        int stop = penalty;
        for (int d = 1; d < penalty; ++d)
            if (!(reg(0, start + d) & 2)) { stop = d - 1; break; }
        t.pauses[t.pause_count++] = {start, stop};
        if (stop == penalty) t.object_fetch[i] = start + penalty - 2;
        last_tile = tile;
    }
    int wx = reg(11, 11) - 7;
    if (window_y && wx < 0) {
        const int trigger = t.delayed(18 + wx);
        if (trigger >= window_from && reg(11, trigger - 1) == wx + 7 &&
            (reg(0, trigger - 1) & window_bits) == window_bits)
            { t.window_trigger = 0; t.window_trigger_dot = trigger - 1; }
    }
    if (window_y && t.window_trigger < 0) {
        for (int x = 0; x < 160; ++x) {
            const int dot = t.delayed(18 + fine + x);
            if (dot >= window_from && reg(11, dot - 1) == x + 7 && (reg(0, dot + 1) & window_bits) == window_bits) {
                const bool late = (reg(0, dot) & window_bits) != window_bits;
                t.window_trigger = x + late; wx = t.window_trigger; t.window_trigger_dot = dot + late; break;
            }
        }
    }
    t.wx = wx;
    if (t.window_trigger < 0) return;
    int start = 18 + fine + t.window_trigger;
    for (unsigned i = 0; i < t.pause_count; ++i)
        if (start > t.pauses[i].start) start += t.pauses[i].duration;
    const int duration = wx == -7 && fine ? 7 : w.dmg && wx == 159 ? std::max(0, 2 - fine) : 6;
    for (unsigned i = 0; i < t.pause_count; ++i) if (t.pauses[i].start >= start) t.pauses[i].start += duration;
    for (auto& fetch : t.object_fetch) if (fetch >= start) fetch += duration;
    t.pauses[t.pause_count++] = {start, duration, true};
    for (unsigned i = 1; i < t.pause_count; ++i)
        for (unsigned j = i; j && t.pauses[j - 1].start > t.pauses[j].start; --j)
            std::swap(t.pauses[j - 1], t.pauses[j]);
}
}  // namespace

// Rows travel to the worker on the other core through the Offload queue,
// or on one core are drawn as they are submitted; see offload.h for the
// handoff and who waits when the ring is full.
class Renderer {
public:
    explicit Renderer(Offload& offload) : offload_(offload) {
        offload_.attach(Offload::rows, kCapacity, consume, this);
    }
    Row& acquire() {
        auto& row = rows_[offload_.acquire(Offload::rows) % kCapacity];
        // A CGB's rows also set dirty1 and palettes_changed, see
        // Ppu::fill_cgb_row(); a DMG's leave them unread.
        row.kind = Row::Kind::line; row.changes_size = 0; row.dirty = 0;
        return row;
    }
    void submit(bool now = false) { offload_.submit(Offload::rows, now); }
    void command(Row::Kind kind, uint64_t id, Mode mode) {
        auto& r = acquire(); r.kind = kind; r.id = id; r.mode = mode; submit(true);
    }
    void finish() { offload_.finish(Offload::rows); }
    // The caller has finished the renderer, so it reads a frame the renderer
    // no longer writes. Shades are converted once per requested frame; a
    // CGB's frames are in color already.
    std::span<const uint16_t> pixels() {
        if (mode_ != Mode::dmg) return color_frames_[front_];
        if (converted_ != frames_) {
            const auto& shades = shades_[front_];
            for (unsigned i = 0; i < kFramePixels; ++i) pixels_[i] = kGrays[shades[i]];
            converted_ = frames_;
        }
        return pixels_;
    }
    std::span<const uint8_t> shades() const { return shades_[front_]; }
    uint64_t id() const { return id_; }
    // The caller has finished the renderer, which reads sink_ only while it
    // consumes a row submitted after this.
    void set_sink(FrameSink* sink) { sink_ = sink; }
    void set_palette(uint64_t packed) { palette_colors_.store(packed, std::memory_order_relaxed); }
private:
    friend struct SnapshotAccess;
    // Inline, each row is consumed as it is submitted, see Offload.
    static constexpr unsigned kCapacity = Offload::kInline ? 2 : 16;
    Offload& offload_;
    // Renderer side.
    unsigned front_ = 0;
    uint32_t frames_ = 0;  // Front-buffer changes, which invalidate pixels_.
    // Of the latest frame or clear row. Line rows have none: the PPU's reset
    // sends a clear row for the mode it loaded.
    Mode mode_ = Mode::dmg;
    std::array<Row, kCapacity> rows_;
    std::array<uint8_t, 16384> vram_{};
    std::array<uint8_t, 160> oam_{};
    std::array<std::array<uint8_t, kFramePixels>, 2> shades_{};
    // A CGB's frames, in either mode, and its palette RAM as 32 BG colors
    // and 32 OBJ colors; bit 15 is ignored (Pan Docs, LCD Color Palettes).
    std::array<std::array<uint16_t, kFramePixels>, 2> color_frames_{};
    std::array<uint16_t, 64> cgb_colors_{};
    // The colors of two pixels in each BG palette, the left one in the low
    // half, 16 per palette: the left pixel's color number plus 4 times the
    // right one's.
    std::array<uint32_t, 128> cgb_pairs_{};
    // Converted on the caller's core: BGR555 colors of the front buffer when
    // frames_ was converted_.
    std::array<uint16_t, kFramePixels> pixels_{};
    uint32_t converted_ = ~uint32_t{0};
    uint64_t id_ = 0;
    uint8_t window_line_ = 0;
    // Values of registers changed in mode 3, by dot, see raster(). Changes
    // are recorded up to 8 dots after the longest mode 3.
    static constexpr unsigned kDots = 512;
    std::array<std::array<uint8_t, kDots>, 12> values_;
    // kNibbleColors mapped through palette_ (BGP); -1 before the first line.
    int palette_ = -1;
    std::array<uint32_t, 256> palette_shades_{};
    FrameSink* sink_ = nullptr;
    // The sink's four colors, shade 0 in the low 16 bits.
    std::atomic<uint64_t> palette_colors_{pack(kGrays)};
    static constexpr uint64_t pack(const std::array<uint16_t, 4>& c) {
        return c[0] | uint64_t{c[1]} << 16 | uint64_t{c[2]} << 32 | uint64_t{c[3]} << 48;
    }
    static void consume(void* context, uint32_t index) {
        auto& self = *static_cast<Renderer*>(context);
        auto& r = self.rows_[index % kCapacity];
        if (r.kind == Row::Kind::line) { self.render(r); return; }
        self.mode_ = r.mode;
        if (r.kind == Row::Kind::frame) { self.front_ ^= 1; ++self.frames_; self.id_ = r.id; }
        else {
            // The LCD turned off: the screen is white.
            if (r.mode == Mode::dmg) for (auto& frame : self.shades_) frame.fill(0);
            else
                for (auto& frame : self.color_frames_) {
                    // One white line, copied over the rest.
                    std::fill_n(frame.data(), 160, kWhite);
                    for (unsigned y = 1; y < 144; ++y) std::memcpy(frame.data() + y * 160, frame.data(), 320);
                }
            ++self.frames_; self.id_ = r.id;
        }
        if (self.sink_) self.deliver();
    }
    void deliver() {
        auto* out = sink_->frame_buffer();
        if (!out) return;
        if (mode_ != Mode::dmg) {
            std::memcpy(out, color_frames_[front_].data(), sizeof(color_frames_[front_]));
            sink_->frame_ready(id_);
            return;
        }
        if (sink_->shades()) {
            std::memcpy(out, shades_[front_].data(), sizeof(shades_[front_]));
            sink_->frame_ready(id_);
            return;
        }
        const uint64_t packed = palette_colors_.load(std::memory_order_relaxed);
        const std::array<uint16_t, 4> palette{static_cast<uint16_t>(packed), static_cast<uint16_t>(packed >> 16),
                                              static_cast<uint16_t>(packed >> 32), static_cast<uint16_t>(packed >> 48)};
        const auto& shades = shades_[front_];
        for (unsigned i = 0; i < kFramePixels; ++i) out[i] = palette[shades[i]];
        sink_->frame_ready(id_);
    }
    // The BGR555 colors of a compatibility-mode line: BGP's shades index BG
    // palette 0, OBP0's and OBP1's OBJ palettes 0 and 1 (Pan Docs, Power Up
    // Sequence, Compatibility palettes). shade holds the shade, plus 4 for
    // OBP0 and 8 for OBP1.
    void compatible(const Row& r, const std::array<uint8_t, 160>& shade) {
        std::array<uint16_t, 12> lookup;
        std::copy_n(cgb_colors_.begin(), 4, lookup.begin());
        std::copy_n(cgb_colors_.begin() + 32, 8, lookup.begin() + 4);
        auto* out = color_frames_[front_ ^ 1].data() + r.y * 160;
        for (unsigned x = 0; x < 160; ++x) out[x] = lookup[shade[x]];
    }
    // Rows without changes in mode 3 decode whole tile rows: kNibbleColors
    // turns a nibble of each bitplane into four pixels, and palette_shades_
    // into their shades. The lines are built in local buffers and copied out.
    // GB mode and compatibility mode, which draws the same shades, share it.
    void segment(Row& r) {
        const bool compat = mode_ == Mode::compat;
        if (r.blank) {
            if (!compat) std::memset(shades_[front_ ^ 1].data() + r.y * 160, 0, 160);
            else std::fill_n(color_frames_[front_ ^ 1].data() + r.y * 160, 160, kWhite);
            return;
        }
        const auto* regs = r.registers.data();
        const auto lcdc = regs[0];
        alignas(4) std::array<uint8_t, 160> bg{}, shade{};
        if (lcdc & 1) {
            if (palette_ != regs[7]) {
                palette_ = regs[7];
                for (unsigned i = 0; i < 256; ++i) {
                    uint32_t word = 0;
                    for (unsigned k = 0; k < 32; k += 8)
                        word |= ((palette_ >> (((kNibbleColors[i] >> k) & 3) * 2)) & 3) << k;
                    palette_shades_[i] = word;
                }
            }
            // Up to 21 tiles each: one more than the screen for a fine offset.
            std::array<uint32_t, 42> tile_colors, tile_shades;
            const auto decode = [&](unsigned map, unsigned y, unsigned column, unsigned tile) {
                const auto index = vram_[map + (y / 8) * 32 + column];
                const unsigned address = ((lcdc & 0x10) ? index * 16 : 0x1000 + static_cast<int8_t>(index) * 16) + (y & 7) * 2;
                const unsigned low = vram_[address], high = vram_[address + 1];
                const unsigned left = (low >> 4) | (high & 0xf0), right = (low & 15) | ((high & 15) << 4);
                tile_colors[tile * 2] = kNibbleColors[left]; tile_colors[tile * 2 + 1] = kNibbleColors[right];
                tile_shades[tile * 2] = palette_shades_[left]; tile_shades[tile * 2 + 1] = palette_shades_[right];
            };
            const auto copy = [&](unsigned first, unsigned x, unsigned count) {
                std::memcpy(bg.data() + x, reinterpret_cast<const uint8_t*>(tile_colors.data()) + first, count);
                std::memcpy(shade.data() + x, reinterpret_cast<const uint8_t*>(tile_shades.data()) + first, count);
            };
            // The window covers the line from split; screen x of the window's
            // pixel 0 is WX - 7, which may be left of the screen.
            const int wx = static_cast<int>(regs[11]) - 7;
            const bool window = r.window_y && (lcdc & 0x20) && regs[11] < 167;
            const unsigned split = window ? std::max(wx, 0) : 160;
            const unsigned scx = regs[3], fine = scx & 7, y = (r.y + regs[2]) & 255;
            for (unsigned tile = 0; tile < (split + fine + 7) / 8; ++tile)
                decode(lcdc & 8 ? 0x1c00 : 0x1800, y, ((scx >> 3) + tile) & 31, tile);
            copy(fine, 0, split);
            if (split < 160) {
                const unsigned first = split - wx;
                for (unsigned tile = 0; tile < (first + 160 - split + 7) / 8; ++tile)
                    decode(lcdc & 0x40 ? 0x1c00 : 0x1800, r.window_line, tile, tile);
                copy(first, split, 160 - split);
            }
        }
        if (lcdc & 2) {
            std::array<bool, 160> claimed{};
            for (unsigned i = 0; i < r.count; ++i) {
                const auto* obj = oam_.data() + r.objects[i] * 4;
                int oy = static_cast<int>(r.y) + 16 - obj[0];
                const int height = lcdc & 4 ? 16 : 8;
                if (oy < 0 || oy >= height) continue;
                if (obj[3] & 0x40) oy = height - 1 - oy;
                const unsigned address = (obj[2] & (height == 16 ? 0xfe : 0xff)) * 16 + oy * 2;
                const unsigned low = vram_[address], high = vram_[address + 1];
                const int start = std::max(0, obj[1] - 8), end = std::min(160, static_cast<int>(obj[1]));
                const unsigned palette = regs[(obj[3] & 0x10) ? 9 : 8];
                const unsigned tag = compat ? (obj[3] & 0x10 ? 8 : 4) : 0;
                for (int x = start; x < end; ++x) {
                    if (claimed[x]) continue;
                    const unsigned ox = x + 8 - obj[1], bit = obj[3] & 0x20 ? ox : 7 - ox;
                    const unsigned color = ((low >> bit) & 1) | (((high >> bit) & 1) << 1);
                    if (!color) continue;
                    // Resolve OBJ priority before BG priority: an obscured
                    // high-priority OBJ still masks all lower-priority OBJs.
                    claimed[x] = true;
                    if (!(obj[3] & 0x80) || bg[x] == 0) shade[x] = static_cast<uint8_t>(((palette >> (color * 2)) & 3) | tag);
                }
            }
        }
        if (!compat) std::memcpy(shades_[front_ ^ 1].data() + r.y * 160, shade.data(), 160);
        else compatible(r, shade);
    }
    // A CGB-mode line without changes in mode 3. Tiles are decoded into
    // colors through cgb_pairs_, two pixels at a time. For OBJs, each BG pixel
    // also keeps its color number, its attribute's palette and priority: bits
    // 0-1, 2-4 and 5. The attributes are in VRAM bank 1 at the tile's map
    // address (Pan Docs, BG Map Attributes); OBJs take their attributes' bank
    // and palette, and resolve among themselves by OAM index (Object Priority
    // and Conflicts).
    void segment_cgb(Row& r) {
        auto* out = color_frames_[front_ ^ 1].data() + r.y * 160;
        if (r.blank) { std::fill_n(out, 160, kWhite); return; }
        const auto* regs = r.registers.data();
        const auto lcdc = regs[0];
        const bool objects = (lcdc & 2) && r.count;
        alignas(4) std::array<uint8_t, 160> bg;
        // Up to 21 tiles each: one more than the screen for a fine offset.
        std::array<uint32_t, 84> tile_pixels;
        std::array<uint32_t, 42> tile_colors;
        const auto decode = [&](unsigned map, unsigned y, unsigned column, unsigned tile) {
            const unsigned at = map + (y / 8) * 32 + column;
            const unsigned index = vram_[at], attributes = vram_[0x2000 + at];
            const unsigned row = attributes & 0x40 ? 7 - (y & 7) : y & 7;
            const unsigned address = (attributes & 8 ? 0x2000 : 0) +
                ((lcdc & 0x10) ? index * 16 : 0x1000 + static_cast<int8_t>(index) * 16) + row * 2;
            unsigned low = vram_[address], high = vram_[address + 1];
            if (attributes & 0x20) { low = kReverse[low]; high = kReverse[high]; }
            const unsigned left = (low >> 4) | (high & 0xf0), right = (low & 15) | ((high & 15) << 4);
            const uint32_t numbers[2] = {kNibbleColors[left], kNibbleColors[right]};
            const auto* pairs = reinterpret_cast<const char*>(cgb_pairs_.data() + (attributes & 7) * 16);
            auto* pixels = tile_pixels.data() + tile * 4;
            for (unsigned half = 0; half < 2; ++half) {
                // Bits 2-5 of bytes 0 and 2 hold a pixel's color number plus
                // 4 times its right neighbor's: their entry's offset in pairs.
                const uint32_t offsets = numbers[half] << 2 | numbers[half] >> 4;
                pixels[half * 2] = *reinterpret_cast<const uint32_t*>(pairs + (offsets & 0x3c));
                pixels[half * 2 + 1] = *reinterpret_cast<const uint32_t*>(pairs + ((offsets >> 16) & 0x3c));
            }
            if (!objects) return;
            const uint32_t extra = (((attributes & 7) << 2) | ((attributes & 0x80) >> 2)) * 0x01010101u;
            tile_colors[tile * 2] = numbers[0] | extra; tile_colors[tile * 2 + 1] = numbers[1] | extra;
        };
        const auto copy = [&](unsigned first, unsigned x, unsigned count) {
            std::memcpy(out + x, reinterpret_cast<const uint8_t*>(tile_pixels.data()) + first * 2, count * 2);
            if (objects) std::memcpy(bg.data() + x, reinterpret_cast<const uint8_t*>(tile_colors.data()) + first, count);
        };
        const int wx = static_cast<int>(regs[11]) - 7;
        const bool window = r.window_y && (lcdc & 0x20) && regs[11] < 167;
        const unsigned split = window ? std::max(wx, 0) : 160;
        const unsigned scx = regs[3], fine = scx & 7, y = (r.y + regs[2]) & 255;
        for (unsigned tile = 0; tile < (split + fine + 7) / 8; ++tile)
            decode(lcdc & 8 ? 0x1c00 : 0x1800, y, ((scx >> 3) + tile) & 31, tile);
        copy(fine, 0, split);
        if (split < 160) {
            // The window's pixels left of the screen; split is 0 then.
            const unsigned first = wx < 0 ? -wx : 0;
            for (unsigned tile = 0; tile < (first + 160 - split + 7) / 8; ++tile)
                decode(lcdc & 0x40 ? 0x1c00 : 0x1800, r.window_line, tile, tile);
            copy(first, split, 160 - split);
        }
        if (!objects) return;
        // The selected objects by OAM index; at most ten.
        std::array<uint8_t, 10> order = r.objects;
        for (unsigned i = 1; i < r.count; ++i)
            for (unsigned j = i; j && order[j - 1] > order[j]; --j) std::swap(order[j - 1], order[j]);
        std::array<bool, 160> claimed{};
        const int height = lcdc & 4 ? 16 : 8;
        for (unsigned i = 0; i < r.count; ++i) {
            const auto* obj = oam_.data() + order[i] * 4;
            int oy = static_cast<int>(r.y) + 16 - obj[0];
            if (oy < 0 || oy >= height) continue;
            if (obj[3] & 0x40) oy = height - 1 - oy;
            const unsigned address = (obj[3] & 8 ? 0x2000 : 0) + (obj[2] & (height == 16 ? 0xfe : 0xff)) * 16 + oy * 2;
            const unsigned low = vram_[address], high = vram_[address + 1];
            const int start = std::max(0, obj[1] - 8), end = std::min(160, static_cast<int>(obj[1]));
            const unsigned palette = 32 + (obj[3] & 7) * 4;
            for (int x = start; x < end; ++x) {
                if (claimed[x]) continue;
                const unsigned ox = x + 8 - obj[1], bit = obj[3] & 0x20 ? ox : 7 - ox;
                const unsigned color = ((low >> bit) & 1) | (((high >> bit) & 1) << 1);
                if (!color) continue;
                claimed[x] = true;
                // LCDC bit 0 off gives every OBJ priority; otherwise BG
                // colors 1-3 stay in front when the BG or OBJ attribute asks.
                if (!(lcdc & 1) || !(bg[x] & 3) || !((bg[x] & 0x20) || (obj[3] & 0x80))) out[x] = cgb_colors_[palette + color];
            }
        }
    }
    // CGB mode, or GB and compatibility mode, which draws the same shades.
    // In CGB mode, background[] and objects[] hold the color number in bits
    // 0-1, the palette in bits 2-4 and the priority bit in bit 5.
    template<bool cgb> unsigned raster(Row& r) {
        // The window needs BG enable too, except in CGB mode (Pan Docs, LCDC.0).
        constexpr unsigned kWindow = cgb ? 0x20 : 0x21;
        // Sample tile-map, bitplane and output stages independently. The
        // timeline advances by tiles, OBJ stalls and register writes, not dots.
        // Sources: Pan Docs Scrolling / Window / Pixel FIFO, and Mealybug's
        // m3_* tests and DMG-blob reference frames.
        // Registers changed in mode 3 are looked up in values_, which holds
        // each one's value by dot; VRAM and OAM changes are rare and scanned.
        uint16_t changed_registers = 0;
        bool video_changes = false, object_changes = false;
        std::array<uint16_t, 12> from{};
        auto value = r.registers;
        const auto fill = [&](unsigned index, unsigned to) {
            std::memset(values_[index].data() + from[index], value[index], to - from[index]);
            from[index] = static_cast<uint16_t>(to);
        };
        for (unsigned i = 0; i < r.changes_size; ++i) {
            const auto& c = r.changes[i];
            if (c.address >= 0xff40) {
                const unsigned index = c.address - 0xff40;
                changed_registers |= 1u << index;
                fill(index, std::min<unsigned>(c.dot, kDots));
                value[index] = c.value;
            }
            else if (c.address >= 0xfe00) object_changes = true;
            else video_changes = true;
        }
        for (auto set = changed_registers; set; set &= set - 1) fill(std::countr_zero(set), kDots);
        const auto reg = [&](unsigned index, int dot) {
            if (!(changed_registers & (1u << index)) || dot < 0) return r.registers[index];
            return values_[index][std::min(dot, static_cast<int>(kDots) - 1)];
        };
        const auto video = [&](unsigned offset, int dot) {
            auto value = vram_[offset];
            if (video_changes) for (unsigned i = 0; i < r.changes_size; ++i) {
                const auto& c = r.changes[i];
                if (c.dot > dot) break;
                if (c.address == 0x8000 + offset) value = c.value;
            }
            return value;
        };
        const auto object_byte = [&](unsigned offset, int dot) {
            auto value = oam_[offset];
            if (object_changes) for (unsigned i = 0; i < r.changes_size; ++i) {
                const auto& c = r.changes[i];
                if (c.dot > dot) break;
                if (c.address == 0xfe00 + offset) value = c.value;
            }
            return value;
        };
        Timeline t;
        const WindowInputs inputs{r.window_y, r.window_carry, mode_ == Mode::dmg, r.window_from, kWindow};
        make_timeline(t, oam_.data(), r.objects, r.count, static_cast<int>(r.fine), inputs, reg);
        const auto& pauses = t.pauses;
        const unsigned pause_count = t.pause_count;
        const auto& object_fetch = t.object_fetch;
        const int window_trigger = t.window_trigger, window_trigger_dot = t.window_trigger_dot, wx = t.wx;
        const auto delayed = [&](int dot) { return t.delayed(dot); };
        const auto fetch_time = [&](int base, int stage, bool epoch, bool first) {
            int start = base, dot = base + stage;
            for (unsigned i = 0; i < pause_count; ++i) {
                const auto pause = pauses[i];
                if (epoch && pause.window) continue;
                const bool in_flight = !(epoch && first) && !pause.window && start - 1 <= pause.start;
                if (dot >= pause.start && !(in_flight && dot <= pause.start + pause.duration - 6))
                    dot += pause.duration;
                if (!in_flight) start += pause.duration;
            }
            return dot;
        };
        // A window carried from the line before, the only one left of -7,
        // keeps the BG's cadence, a tile ahead.
        const auto fetch_base = [&](int origin, int ordinal, bool epoch) {
            if (epoch && origin < -7) return 3 + ordinal * 8;
            return epoch ? 19 + static_cast<int>(r.fine) + origin + ordinal * 8 - (ordinal ? 2 : 0) :
                11 + ordinal * 8;
        };
        std::array<int16_t, 160> window_pixels{}, window_origins{};
        std::array<uint8_t, 160> window_rows{};
        std::array<bool, 160> window_sources{}, window_epochs{};
        bool window_epoch = false;
        unsigned window_uses = 0;
        uint8_t window_row = r.window_line;
        int window_pixel = r.fine, window_origin = 0, trigger_wx = -1, disabled_at = 0;
        const auto disabled_from = [&](int dot) {
            for (unsigned i = 0; i < r.changes_size; ++i) {
                const auto& c = r.changes[i];
                if (c.dot >= dot && c.address == 0xff40 && !(c.value & 0x20)) return static_cast<int>(c.dot);
            }
            return 1000;
        };
        if (r.window_carry) {
            // From its second tile, through the fine scroll, on the window
            // line counted as the line started, see Ppu::window_carry_.
            window_epoch = true; window_origin = -8 - static_cast<int>(r.fine); window_pixel = -window_origin;
            trigger_wx = r.registers[11] - 7; window_row = r.window_line;
            disabled_at = disabled_from(0);
        }
        for (int x = 0; x < 160; ++x) {
            const int dot = delayed(18 + r.fine + x);
            const int position = reg(11, dot) - 7;
            const bool enabled = r.window_y && dot >= r.window_from && (reg(0, dot) & kWindow) == kWindow;
            const bool active = window_epoch && dot < disabled_at;
            if (x == window_trigger || (!active && enabled && position == x)) {
                window_epoch = true; window_origin = x == window_trigger ? wx : position;
                window_pixel = x - window_origin; trigger_wx = window_origin;
                window_row = static_cast<uint8_t>(r.window_line + window_uses++);
                disabled_at = disabled_from(x == window_trigger ? window_trigger_dot : dot);
            }
            if (window_epoch) {
                const int ordinal = window_pixel / 8;
                window_sources[x] = fetch_time(fetch_base(window_origin, ordinal, true), 0, true, !ordinal) < disabled_at;
            }
            window_epochs[x] = window_epoch;
            window_pixels[x] = static_cast<int16_t>(window_pixel++);
            window_rows[x] = window_row;
            window_origins[x] = static_cast<int16_t>(window_origin);
            if (x != window_trigger && r.window_y && x == position && !(window_pixels[x] & 7) &&
                ((!enabled && !active) || (active && position != trigger_wx))) {
                window_pixels[x] = -2; --window_pixel; trigger_wx = position;
            }
        }
        std::array<uint8_t, 160> background{}, objects{};
        const unsigned scx_bias = mode_ == Mode::dmg ? 257 : 256;
        for (unsigned x = 0; x < 160;) {
            if (window_pixels[x] == -2) { background[x++] = 0; continue; }
            const bool win = window_sources[x], epoch = window_epochs[x];
            const int px = window_pixels[x];
            const int ordinal = px / 8;
            // A window restart fetches its first tile in six dots. Subsequent
            // tiles resume the eight-dot cadence used by the background.
            const int base = fetch_base(window_origins[x], ordinal, epoch);
            const auto fetch_dot = [&](int stage) {
                return fetch_time(base, stage, epoch, !ordinal);
            };
            const int map_dot = fetch_dot(0), low_dot = fetch_dot(2), high_dot = fetch_dot(4);
            const unsigned map = reg(0, map_dot) & (win ? 0x40 : 8) ? 0x1c00 : 0x1800;
            const unsigned map_y = win ? window_rows[x] : (r.y + reg(2, map_dot)) & 255;
            // The tile column counts pixels from SCX as it is at the fetch,
            // fine bits included, so an SCX write in mode 3 can move the
            // last tiles by one less: AGE m3-bg-scx, verified on DMG-C and
            // CPU CGB E, where the count runs one pixel ahead on the DMG.
            const unsigned map_x = win ? ordinal : ((reg(3, map_dot) + ordinal * 8 + scx_bias - r.fine) >> 3) & 31;
            const unsigned at = map + (map_y / 8) * 32 + (map_x & 31);
            const auto tile = video(at, map_dot);
            // CGB mode: the attribute at the same map address in bank 1.
            const unsigned attributes = cgb ? video(0x2000 + at, map_dot) : 0;
            const auto plane = [&](int dot, unsigned high) {
                const unsigned y = win ? window_rows[x] : r.y + reg(2, dot);
                const unsigned row = attributes & 0x40 ? 7 - (y & 7) : y & 7;
                const unsigned address = (attributes & 8 ? 0x2000 : 0) + (reg(0, dot) & 0x10 ? tile * 16 :
                    0x1000 + static_cast<int8_t>(tile) * 16) + row * 2 + high;
                const unsigned value = video(address, dot);
                return attributes & 0x20 ? kReverse[value] : value;
            };
            // A CGB that switches to the 8800 tile data in the dot before the
            // low bitplane fetch reads the tile number as that plane: AGE
            // m3-bg-lcdc, verified on CPU CGB E in both modes. The same write
            // at double speed, m3-bg-lcdc-ds, does not.
            const bool tile_as_plane = mode_ != Mode::dmg && !r.double_speed && (reg(0, low_dot - 2) & 0x10) &&
                !(reg(0, low_dot - 1) & 0x10);
            const unsigned low = tile_as_plane ? tile : plane(low_dot, 0), high = plane(high_dot, 1);
            const unsigned extra = ((attributes & 7) << 2) | ((attributes & 0x80) >> 2);
            unsigned end = std::min(160u, x + 8 - (px & 7));
            for (unsigned next = x + 1; next < end; ++next)
                if (window_pixels[next] != px + static_cast<int>(next - x) || window_sources[next] != win ||
                    window_origins[next] != window_origins[x]) { end = next; break; }
            for (unsigned bit = 7 - (px & 7); x < end; ++x, --bit)
                background[x] = static_cast<uint8_t>(((low >> bit) & 1) | (((high >> bit) & 1) << 1) | extra);
        }
        // In CGB mode the objects resolve by OAM index, otherwise by X, in
        // which order they were selected (see Ppu::select_objects()).
        std::array<uint8_t, 10> order{};
        for (unsigned i = 0; i < r.count; ++i) order[i] = static_cast<uint8_t>(i);
        if constexpr (cgb)
            for (unsigned i = 1; i < r.count; ++i)
                for (unsigned j = i; j && r.objects[order[j - 1]] > r.objects[order[j]]; --j) std::swap(order[j - 1], order[j]);
        for (unsigned k = 0; k < r.count; ++k) {
            const unsigned i = order[k];
            if (object_fetch[i] < 0) continue;
            const unsigned offset = r.objects[i] * 4;
            // OAM selection/position precedes transfer. Fetch tile/attributes
            // from the row's timestamped DMA writes, never from live memory.
            const std::array<uint8_t, 4> obj{oam_[offset], oam_[offset + 1],
                object_byte(offset + 2, object_fetch[i] - 4), object_byte(offset + 3, object_fetch[i] - 4)};
            const auto plane = [&](unsigned high) {
                const int height = reg(0, object_fetch[i] + high * 2) & 4 ? 16 : 8;
                int y = (static_cast<int>(r.y) + 16 - obj[0]) & (height - 1);
                if (obj[3] & 0x40) y = height - 1 - y;
                const unsigned bank = cgb && (obj[3] & 8) ? 0x2000 : 0;
                const unsigned address = bank + (obj[2] & (height == 16 ? 0xfe : 0xff)) * 16 + y * 2;
                return video(address + high, object_fetch[i] + high * 2);
            };
            const unsigned low = plane(0), high = plane(1);
            for (int x = std::max(0, obj[1] - 8), end = std::min(160, static_cast<int>(obj[1])); x < end; ++x) {
                const int bit = obj[3] & 0x20 ? x + 8 - obj[1] : 7 - (x + 8 - obj[1]);
                const unsigned color = ((low >> bit) & 1) | (((high >> bit) & 1) << 1);
                if (!color || objects[x]) continue;
                if constexpr (cgb) objects[x] = static_cast<uint8_t>(color | (obj[3] & 7) << 2 | (obj[3] & 0x80 ? 0x20 : 0));
                else objects[x] = static_cast<uint8_t>(color | (obj[3] & 0x10 ? 4 : 0) | (obj[3] & 0x80 ? 8 : 0));
            }
        }
        if constexpr (cgb) {
            auto* out = color_frames_[front_ ^ 1].data() + r.y * 160;
            for (unsigned x = 0; x < 160; ++x) {
                const int dot = delayed(18 + r.fine + x);
                const auto lcdc = reg(0, dot + (x == 0));
                const unsigned bg = background[x], obj = objects[x];
                uint16_t pixel = cgb_colors_[(bg >> 2 & 7) * 4 + (bg & 3)];
                if ((lcdc & 2) && obj && (!(lcdc & 1) || !(bg & 3) || !((bg & 0x20) || (obj & 0x20))))
                    pixel = cgb_colors_[32 + (obj >> 2 & 7) * 4 + (obj & 3)];
                out[x] = r.blank ? kWhite : pixel;
            }
        } else {
            // Compatibility mode collects the shades and which palette RAM they
            // index, see compatible().
            const bool compat = mode_ == Mode::compat;
            std::array<uint8_t, 160> shade;
            auto* out = compat ? shade.data() : shades_[front_ ^ 1].data() + r.y * 160;
            for (unsigned x = 0; x < 160; ++x) {
                const int dot = delayed(18 + r.fine + x);
                const auto lcdc = reg(0, dot + (x == 0));
                unsigned color = lcdc & 1 ? background[x] : 0, palette = 7;
                if ((lcdc & 2) && objects[x] && (!(objects[x] & 8) || !color)) {
                    color = objects[x] & 3; palette = objects[x] & 4 ? 9 : 8;
                }
                // On a DMG a palette write in the following dot mixes into this
                // pixel, but after a fine scroll the first pixel takes the
                // palette of the following dot, and so does every pixel on a
                // CGB. AGE m3-bg-bgp, verified on DMG-C and CPU CGB E, and
                // Mealybug m3_bgp_change.
                auto mapping = reg(palette, dot + 1);
                if (!compat && !(x == 0 && r.fine)) {
                    const auto now = reg(palette, dot);
                    mapping = now == mapping ? now : now | mapping;
                }
                out[x] = r.blank ? 0 : (mapping >> (color * 2)) & 3;
                if (compat) out[x] |= static_cast<uint8_t>((palette - 7) * 4);
            }
            if (compat) {
                if (r.blank) std::fill_n(color_frames_[front_ ^ 1].data() + r.y * 160, 160, kWhite);
                else compatible(r, shade);
            }
        }
        return window_uses;
    }
    void render(Row& r) {
        if (!r.y) window_line_ = 0;
        else window_line_ = static_cast<uint8_t>(window_line_ + r.window_count);
        r.window_line = window_line_;
        for (auto dirty = r.dirty; dirty; dirty &= dirty - 1) {
            const auto offset = std::countr_zero(dirty) * 128;
            std::memcpy(vram_.data() + offset, r.vram.data() + offset, 128);
        }
        if (r.oam_changed) oam_ = r.oam;
        if (mode_ == Mode::dmg) {
            if (r.changes_size || r.window_from || r.window_carry) {
                window_line_ = static_cast<uint8_t>(window_line_ + raster<false>(r));
                return;
            }
            window_line_ += r.window_y && (r.registers[0] & 0x21) == 0x21 && r.registers[11] < 167;
            segment(r);
            return;
        }
        render_color(r);
    }
    // Both CGB modes, kept apart from GB mode's path.
    [[gnu::noinline]] void render_color(Row& r) {
        for (auto dirty = r.dirty1; dirty; dirty &= dirty - 1) {
            const auto offset = 0x2000 + std::countr_zero(dirty) * 128;
            std::memcpy(vram_.data() + offset, r.vram.data() + offset, 128);
        }
        if (r.palettes_changed) {
            unsigned stale = 0;
            for (unsigned i = 0; i < 64; ++i) {
                const uint16_t color = (r.palettes[i * 2] | r.palettes[i * 2 + 1] << 8) & 0x7fff;
                if (i < 32 && color != cgb_colors_[i]) stale |= 1u << (i / 4);
                cgb_colors_[i] = color;
            }
            for (; stale; stale &= stale - 1) {
                const unsigned palette = std::countr_zero(stale);
                const auto* c = cgb_colors_.data() + palette * 4;
                for (unsigned i = 0; i < 16; ++i)
                    cgb_pairs_[palette * 16 + i] = c[i & 3] | uint32_t{c[i >> 2]} << 16;
            }
        }
        const bool cgb = mode_ == Mode::cgb;
        if (r.changes_size || r.window_from) {
            window_line_ = static_cast<uint8_t>(window_line_ + (cgb ? raster<true>(r) : raster<false>(r)));
            return;
        }
        const unsigned window = cgb ? 0x20 : 0x21;
        window_line_ += r.window_y && (r.registers[0] & window) == window && r.registers[11] < 167;
        if (cgb) segment_cgb(r);
        else segment(r);
    }
};
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
}  // namespace gbx::gb
