#include "gbx/ui/vfd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gbx/platform.h"

namespace gbx::ui {

namespace {

constexpr const char* kTag = "ui.vfd";
constexpr int kMinPitch = 2;

// Python's round(): halves go to the even neighbour. The previews in
// docs/ui-layouts are drawn that way, so glyphs land on the same pixels.
int round_even(double v) {
    return static_cast<int>(std::nearbyint(v));
}

uint16_t blend565(uint16_t dst, uint16_t src, unsigned a) {
    if (a == 255) {
        return src;
    }
    unsigned na = 255 - a;
    unsigned r = (((src >> 11) & 31) * a + ((dst >> 11) & 31) * na + 127) / 255;
    unsigned g = (((src >> 5) & 63) * a + ((dst >> 5) & 63) * na + 127) / 255;
    unsigned b = ((src & 31) * a + (dst & 31) * na + 127) / 255;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

// Blends the source rectangle (sx, sy, w, h) of an image so that its top-left lands at screen (dx, dy).
void blit_part(Canvas& canvas, const ImageView& image, int sx, int sy, int w, int h, int dx, int dy) {
    int x0 = std::max(dx, canvas.x);
    int y0 = std::max(dy, canvas.y);
    int x1 = std::min(dx + w, canvas.x + canvas.width);
    int y1 = std::min(dy + h, canvas.y + canvas.height);
    int row_pixels = image.stride / 2;
    for (int y = y0; y < y1; ++y) {
        int iy = sy + (y - dy);
        const uint16_t* src = image.rgb + iy * row_pixels + sx + (x0 - dx);
        const uint8_t* alpha = image.alpha ? image.alpha + iy * image.width + sx + (x0 - dx) : nullptr;
        uint16_t* dst = canvas.pixels + (y - canvas.y) * canvas.stride + (x0 - canvas.x);
        for (int x = x0; x < x1; ++x, ++src, ++dst) {
            unsigned a = alpha ? *alpha++ : 255;
            if (a != 0) {
                *dst = blend565(*dst, *src, a);
            }
        }
    }
}

int fit_pitch(int pitch, double available, auto width_at) {
    while (pitch > kMinPitch && width_at(pitch) > available) {
        --pitch;
    }
    return pitch;
}

}  // namespace

void blit(Canvas& canvas, const ImageView& image) {
    blit_part(canvas, image, 0, 0, image.width, image.height, image.x, image.y);
}

bool VfdFont::load(const Pack& pack) {
    chars_ = field(pack.layout().font_chars);
    atlas_count_ = 0;
    for (const pack_format::Font& f : pack.fonts()) {
        if (atlas_count_ == static_cast<int>(std::size(atlases_))) {
            break;
        }
        char name[16];
        std::snprintf(name, sizeof name, "font.%u", static_cast<unsigned>(f.pitch));
        auto image = pack.image(name);
        if (!image || image->alpha == nullptr) {
            platform::log(platform::LogLevel::error, kTag, "font atlas %s is missing", name);
            return false;
        }
        ImageView view = *image;
        const size_t rgb = static_cast<size_t>(view.stride) * view.height, alpha = static_cast<size_t>(view.width) * view.height;
        copies_[atlas_count_].reset(static_cast<uint8_t*>(platform::allocate_external(rgb + alpha)));
        if (uint8_t* copy = copies_[atlas_count_].get()) {
            std::memcpy(copy, view.rgb, rgb);
            std::memcpy(copy + rgb, view.alpha, alpha);
            view.rgb = reinterpret_cast<const uint16_t*>(copy);
            view.alpha = copy + rgb;
        }
        atlases_[atlas_count_++] = {&f, view};
    }
    return atlas_count_ > 0;
}

void VfdFont::FreeExternal::operator()(uint8_t* memory) const {
    platform::free_external(memory);
}

int VfdFont::text_width(std::string_view text, int pitch) {
    return std::max(0, static_cast<int>(text.size()) * 6 * pitch - pitch);
}

const VfdFont::Atlas* VfdFont::atlas(int pitch) const {
    for (int i = 0; i < atlas_count_; ++i) {
        if (atlases_[i].font->pitch == pitch) {
            return &atlases_[i];
        }
    }
    return nullptr;
}

void VfdFont::draw(Canvas& canvas, std::string_view text, double x, double y, int pitch) const {
    const Atlas* a = atlas(pitch);
    if (a == nullptr) {
        return;
    }
    const pack_format::Font& f = *a->font;
    for (size_t n = 0; n < text.size(); ++n) {
        size_t k = chars_.find(text[n]);
        if (k == std::string_view::npos) {
            k = 0;  // the first glyph is a space
        }
        int col = static_cast<int>(k % f.columns);
        int row = static_cast<int>(k / f.columns);
        int px = round_even(x + static_cast<double>(n) * f.advance - f.margin);
        int py = round_even(y - f.margin);
        blit_part(canvas, a->image, col * f.cell_width, row * f.cell_height, f.cell_width, f.cell_height, px, py);
    }
}

std::string_view format_binding(std::string_view binding, FieldLookup lookup, const void* context,
                                std::span<char> out) {
    size_t n = 0;
    auto put = [&](std::string_view text) {
        for (char c : text) {
            if (n < out.size()) {
                out[n++] = c;
            }
        }
    };
    size_t i = 0;
    while (i < binding.size()) {
        if (binding[i] != '{') {
            put(binding.substr(i, 1));
            ++i;
            continue;
        }
        const size_t close = binding.find('}', i);
        if (close == std::string_view::npos) {
            break;
        }
        std::string_view inside = binding.substr(i + 1, close - i - 1);
        i = close + 1;
        // "name" or "name:.Nf"
        const size_t colon = inside.find(':');
        int decimals = 0;
        if (colon != std::string_view::npos && inside.size() > colon + 2 && inside[colon + 1] == '.') {
            decimals = inside[colon + 2] - '0';
        }
        const Reading reading = lookup(inside.substr(0, colon), context);
        switch (reading.state) {
            case Reading::State::uncollected: put("--"); break;
            case Reading::State::unsupported: put("N/A"); break;
            case Reading::State::stale: put("STALE"); break;
            case Reading::State::warmup: put("WARMUP"); break;
            case Reading::State::valid: {
                char number[24];
                int length = std::snprintf(number, sizeof number, "%.*f", decimals, reading.value);
                put({number, static_cast<size_t>(std::max(length, 0))});
                break;
            }
        }
    }
    return {out.data(), n};
}

std::string_view placeholder(std::string_view binding, std::span<char> out) {
    return format_binding(binding, [](std::string_view, const void*) { return Reading{}; }, nullptr, out);
}

void draw_cell(Canvas& canvas, const VfdFont& font, const pack_format::VfdCell& cell, std::string_view value,
               std::optional<std::string_view> unit, std::span<const std::string_view> lines) {
    using pack_format::UnitPosition;
    std::string_view unit_text = unit ? *unit : field(cell.unit);
    int up = cell.unit_pitch;
    bool beside = !unit_text.empty() && cell.unit_position == UnitPosition::right;

    auto width = [&](int p) {
        int u = std::min(up, p);
        int uw = unit_text.empty() ? 0 : VfdFont::text_width(unit_text, u);
        return VfdFont::text_width(value, p) + (beside ? 3 * u + uw : 0);
    };
    int p = fit_pitch(cell.pitch, cell.right - cell.left_limit, width);
    int u = std::min(up, p);
    double bottom = cell.top + 7.0 * cell.pitch;
    if (beside) {
        double ux = cell.right - VfdFont::text_width(unit_text, u);
        font.draw(canvas, unit_text, ux, bottom - 7 * u, u);
        font.draw(canvas, value, ux - 3 * u - VfdFont::text_width(value, p), bottom - 7 * p, p);
    } else {
        font.draw(canvas, value, cell.right - VfdFont::text_width(value, p), bottom - 7 * p, p);
        if (!unit_text.empty()) {
            font.draw(canvas, unit_text, cell.right - VfdFont::text_width(unit_text, u), bottom + 2 * u, u);
        }
    }
    const size_t count = std::min<size_t>(cell.line_count, lines.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& line = cell.lines[i];
        const std::string_view text = lines[i];
        if (text.empty()) continue;
        const bool right = line.align == pack_format::Align::right;
        double available = right ? line.x - (cell.rect[0] + 4) : cell.rect[0] + cell.rect[2] - 4 - line.x;
        int lp = fit_pitch(line.pitch, available, [&](int q) { return VfdFont::text_width(text, q); });
        double x = right ? line.x - VfdFont::text_width(text, lp) : line.x;
        font.draw(canvas, text, x, line.top + 7 * (line.pitch - lp), lp);
    }
}

}  // namespace gbx::ui
