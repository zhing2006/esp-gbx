#include <algorithm>
#include <array>
#include <cstring>

#include "gbx/platform.h"
#include "gbx/ui/menu.h"
#include "menu_draw.h"
#include "src/misc/cache/instance/lv_image_cache.h"

namespace gbx::ui {

namespace {

constexpr const char* kTag = "ui.menu";
// Often enough for the marquee to move a pixel at a time.
constexpr uint32_t kApplyPeriodMs = 20;
// The selected ROM's name waits, scrolls to its end, waits and starts over.
constexpr uint64_t kMarqueeWaitUs = 1'000'000;
constexpr int kMarqueePixelsPerSecond = 24;

}  // namespace

struct MenuView::State {
    explicit State(const Pack& p) : pack(p), painter(theme, font) {}

    const Pack& pack;
    MenuTheme theme;
    MenuFont font;
    MenuPainter painter;
    std::array<int16_t, 4> window{};

    // Grid pixels: what the canvas shows, the next drawing, and the dimmed
    // game frame under the pause menu.
    uint16_t* drawn = nullptr;
    uint16_t* next = nullptr;
    uint16_t* backdrop = nullptr;

    std::mutex mutex;  // guards the wanted_ fields
    MenuScreen wanted;
    bool wanted_changed = false;
    std::array<uint16_t, MenuPainter::kWideThumbnailWidth * MenuPainter::kWideThumbnailHeight> frame{};
    int frame_width = 0, frame_height = 0;
    bool frame_changed = false;
    bool has_frame = false;
    // The SAVE / LOAD thumbnail as set_thumbnail() gave it, BGR555, and the
    // copy the drawing reads, with their sizes.
    uint16_t* source = nullptr;
    uint16_t* source_shown = nullptr;
    int source_width = 0, source_height = 0, shown_width = 0, shown_height = 0;
    bool source_changed = false;

    // LVGL task, or a caller holding the LVGL lock.
    MenuScreen shown;
    bool valid = false;  // drawn holds a drawing of shown
    int marquee = 0;
    uint64_t marquee_start = 0;

    // The SAVE / LOAD thumbnail at full size over the scaled grid: what the
    // canvas shows and the next one, and the top left cell it is at.
    uint16_t* thumbnail = nullptr;
    uint16_t* thumbnail_next = nullptr;
    bool thumbnail_shown = false;
    int thumbnail_x = 0;
    int thumbnail_y = 0;
    int thumbnail_width = 0;
    int thumbnail_height = 0;
};

MenuView::MenuView() = default;

MenuView::~MenuView() {
    delete state_;
}

bool MenuView::create(const Pack& pack, std::span<const int16_t, 4> window) {
    state_ = new State(pack);
    State& st = *state_;
    std::copy(window.begin(), window.end(), st.window.begin());
    if (!st.theme.load(pack) || !st.font.load(pack.menu_font())) {
        return false;
    }
    const int k = st.theme.scale;
    if (st.theme.width * k != window[2] || st.theme.height * k != window[3]) {
        platform::log(platform::LogLevel::error, kTag, "menu grid %dx%d at %dx does not fill the %dx%d window",
                      st.theme.width, st.theme.height, k, window[2], window[3]);
        return false;
    }
    const size_t bytes = size_t{2} * st.theme.width * st.theme.height;
    st.drawn = static_cast<uint16_t*>(platform::allocate_external(bytes));
    st.next = static_cast<uint16_t*>(platform::allocate_external(bytes));
    st.backdrop = static_cast<uint16_t*>(platform::allocate_external(bytes));
    constexpr size_t kThumbnailBytes = size_t{2} * MenuPainter::kWideThumbnailWidth * MenuPainter::kWideThumbnailHeight;
    st.thumbnail = static_cast<uint16_t*>(platform::allocate_external(kThumbnailBytes));
    st.thumbnail_next = static_cast<uint16_t*>(platform::allocate_external(kThumbnailBytes));
    st.source = static_cast<uint16_t*>(platform::allocate_external(kThumbnailBytes));
    st.source_shown = static_cast<uint16_t*>(platform::allocate_external(kThumbnailBytes));
    buffer_ = lv_draw_buf_create(window[2], window[3], LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (!st.drawn || !st.next || !st.backdrop || !st.thumbnail || !st.thumbnail_next || !st.source ||
        !st.source_shown || !buffer_) {
        platform::log(platform::LogLevel::error, kTag, "no memory for the menus");
        return false;
    }
    lv_draw_buf_clear(buffer_, nullptr);
    canvas_ = lv_canvas_create(lv_screen_active());
    lv_canvas_set_draw_buf(canvas_, buffer_);
    lv_obj_set_pos(canvas_, window[0], window[1]);
    timer_ = lv_timer_create([](lv_timer_t* t) { static_cast<MenuView*>(lv_timer_get_user_data(t))->apply(); },
                             kApplyPeriodMs, this);
    return true;
}

void MenuView::destroy() {
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (canvas_) {
        lv_obj_delete(canvas_);
        canvas_ = nullptr;
    }
    if (buffer_) {
        lv_image_cache_drop(buffer_);
        lv_draw_buf_destroy(buffer_);
        buffer_ = nullptr;
    }
    if (state_) {
        platform::free_external(state_->drawn);
        platform::free_external(state_->next);
        platform::free_external(state_->backdrop);
        platform::free_external(state_->thumbnail);
        platform::free_external(state_->thumbnail_next);
        platform::free_external(state_->source);
        platform::free_external(state_->source_shown);
        delete state_;
        state_ = nullptr;
    }
}

void MenuView::show(const MenuScreen& screen) {
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    state_->wanted = screen;
    state_->wanted_changed = true;
}

void MenuView::set_frame(std::span<const uint16_t> pixels, int width, int height) {
    const size_t n = static_cast<size_t>(width) * height;
    if (!state_ || n > state_->frame.size() || pixels.size() < n) return;
    std::lock_guard lock(state_->mutex);
    std::copy_n(pixels.begin(), n, state_->frame.begin());
    state_->frame_width = width;
    state_->frame_height = height;
    state_->frame_changed = true;
}

void MenuView::set_thumbnail(std::span<const uint16_t> pixels, int width, int height) {
    const size_t n = static_cast<size_t>(width) * height;
    constexpr size_t kMax = size_t{MenuPainter::kWideThumbnailWidth} * MenuPainter::kWideThumbnailHeight;
    if (!state_ || !state_->source || n > kMax || pixels.size() < n) return;
    std::lock_guard lock(state_->mutex);
    std::copy_n(pixels.begin(), n, state_->source);
    state_->source_width = width;
    state_->source_height = height;
    state_->source_changed = true;
}

void MenuView::draw_now() {
    if (!state_) return;
    apply();
    lv_refr_now(nullptr);
}

MenuHit MenuView::hit(const MenuScreen& screen, float x, float y) const {
    if (!state_) return {};
    const State& st = *state_;
    const float gx = (x - st.window[0]) / st.theme.scale;
    const float gy = (y - st.window[1]) / st.theme.scale;
    if (gx < 0 || gy < 0 || gx >= st.theme.width || gy >= st.theme.height) return {};
    return st.painter.hit(screen, static_cast<int>(gx), static_cast<int>(gy));
}

bool MenuView::capture(std::span<uint16_t> out, int& width, int& height) const {
    if (!state_) return false;
    const State& st = *state_;
    width = st.theme.width;
    height = st.theme.height;
    const size_t pixels = size_t{1} * width * height;
    if (!st.valid || out.size() < pixels) return false;
    std::copy_n(st.drawn, pixels, out.begin());
    return true;
}

void MenuView::apply() {
    State& st = *state_;
    bool redraw = !st.valid;
    {
        std::lock_guard lock(st.mutex);
        if (st.frame_changed) {
            st.painter.backdrop(st.frame, st.frame_width, st.frame_height, st.backdrop);
            st.frame_changed = false;
            st.has_frame = true;
            redraw = true;
        }
        if (st.source_changed) {
            std::swap(st.source, st.source_shown);
            st.shown_width = st.source_width;
            st.shown_height = st.source_height;
            st.source_changed = false;
            redraw = true;
        }
        if (st.wanted_changed) {
            const auto& old_row = st.shown.rows[std::clamp(st.shown.selected, 0, MenuScreen::kMaxItems - 1)];
            const auto& new_row = st.wanted.rows[std::clamp(st.wanted.selected, 0, MenuScreen::kMaxItems - 1)];
            if (st.wanted.kind != st.shown.kind || st.wanted.selected != st.shown.selected ||
                st.wanted.first != st.shown.first || std::strcmp(old_row.label, new_row.label) != 0) {
                st.marquee = 0;
                st.marquee_start = platform::monotonic_us();
            }
            st.shown = st.wanted;
            st.wanted_changed = false;
            redraw = true;
        }
    }
    if (const int range = st.painter.marquee_range(st.shown); range > 0) {
        const uint64_t scroll = uint64_t{1'000'000} * range / kMarqueePixelsPerSecond;
        const uint64_t t = (platform::monotonic_us() - st.marquee_start) % (kMarqueeWaitUs * 2 + scroll);
        const int offset = t < kMarqueeWaitUs           ? 0
                           : t < kMarqueeWaitUs + scroll ? static_cast<int>((t - kMarqueeWaitUs) * range / scroll)
                                                         : range;
        if (offset != st.marquee) {
            st.marquee = offset;
            redraw = true;
        }
    }
    if (!redraw) return;

    const int w = st.theme.width;
    const int h = st.theme.height;
    Surface surface(st.next, w, h);
    // A thumbnail of the other size, from before the screen changed, is not
    // drawn.
    int kThumbnailW, kThumbnailH;
    MenuPainter::thumbnail_size(st.shown, kThumbnailW, kThumbnailH);
    const bool fits = kThumbnailW == st.shown_width && kThumbnailH == st.shown_height;
    st.painter.draw(st.shown, surface, st.has_frame ? st.backdrop : nullptr, fits ? st.source_shown : nullptr,
                    st.marquee);
    // Copy only the rectangle that changed to the canvas, which LVGL and the
    // display then copy on.
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (int y = 0; y < h; ++y) {
        const uint16_t* a = st.next + y * w;
        const uint16_t* b = st.drawn + y * w;
        if (st.valid && std::memcmp(a, b, w * sizeof(uint16_t)) == 0) continue;
        int first = 0, last = w;
        if (st.valid) {
            while (a[first] == b[first]) ++first;
            while (a[last - 1] == b[last - 1]) --last;
        }
        x0 = std::min(x0, first), x1 = std::max(x1, last);
        y0 = std::min(y0, y), y1 = y + 1;
    }
    std::swap(st.next, st.drawn);
    st.valid = true;

    const int k = st.theme.scale;
    // The grid holds the thumbnail at half size; at 2x its pixels are drawn
    // one screen pixel each over the scaled cells.
    int tx = 0, ty = 0;
    const bool thumbnail = k == 2 && fits && st.painter.thumbnail_at(st.shown, tx, ty);
    if (st.thumbnail_shown && (!thumbnail || tx != st.thumbnail_x || ty != st.thumbnail_y ||
                               kThumbnailW != st.thumbnail_width)) {
        // Scale the grid's cells back over where it was.
        x0 = std::min(x0, st.thumbnail_x), x1 = std::max(x1, st.thumbnail_x + st.thumbnail_width / 2);
        y0 = std::min(y0, st.thumbnail_y), y1 = std::max(y1, st.thumbnail_y + st.thumbnail_height / 2);
    }
    bool thumbnail_changed = false;
    if (thumbnail) {
        MenuPainter::thumbnail(st.source_shown, kThumbnailW, kThumbnailH, st.thumbnail_next, kThumbnailW);
        // Scaling the cells under it covers it, so it is drawn again then too.
        const bool covered = x0 < tx + kThumbnailW / 2 && x1 > tx && y0 < ty + kThumbnailH / 2 && y1 > ty;
        thumbnail_changed = covered || !st.thumbnail_shown || tx != st.thumbnail_x || ty != st.thumbnail_y ||
                            kThumbnailW != st.thumbnail_width ||
                            std::memcmp(st.thumbnail, st.thumbnail_next, size_t{2} * kThumbnailW * kThumbnailH) != 0;
        std::swap(st.thumbnail, st.thumbnail_next);
    }
    st.thumbnail_shown = thumbnail;
    st.thumbnail_x = tx;
    st.thumbnail_y = ty;
    st.thumbnail_width = kThumbnailW;
    st.thumbnail_height = kThumbnailH;
    if (x0 >= x1 && !thumbnail_changed) return;

    const auto stride = buffer_->header.stride / sizeof(uint16_t);
    auto* canvas = reinterpret_cast<uint16_t*>(buffer_->data);
    for (int y = y0; y < y1; ++y) {
        uint16_t* out = canvas + y * k * stride + x0 * k;
        const uint16_t* in = st.drawn + y * w;
        if (k == 2) {
            // LVGL aligns the canvas to a word. At 2x, every source pixel's
            // destination offset is also word-aligned, including partial rows.
            auto* pairs = reinterpret_cast<uint32_t*>(out);
            for (int x = x0; x < x1; ++x) pairs[x - x0] = uint32_t{in[x]} * 0x10001u;
        } else {
            for (int x = x0; x < x1; ++x) {
                std::fill_n(out + (x - x0) * k, k, in[x]);
            }
        }
        for (int r = 1; r < k; ++r) {
            std::memcpy(out + r * stride, out, (x1 - x0) * k * sizeof(uint16_t));
        }
    }
    if (x0 < x1) {
        const lv_area_t area{st.window[0] + x0 * k, st.window[1] + y0 * k, st.window[0] + x1 * k - 1,
                             st.window[1] + y1 * k - 1};
        lv_obj_invalidate_area(canvas_, &area);
    }
    if (thumbnail_changed) {
        for (int y = 0; y < kThumbnailH; ++y) {
            std::copy_n(st.thumbnail + y * kThumbnailW, kThumbnailW, canvas + (ty * k + y) * stride + tx * k);
        }
        const lv_area_t area{st.window[0] + tx * k, st.window[1] + ty * k, st.window[0] + tx * k + kThumbnailW - 1,
                             st.window[1] + ty * k + kThumbnailH - 1};
        lv_obj_invalidate_area(canvas_, &area);
    }
}

}  // namespace gbx::ui
