#include "menu_draw.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

#include "gbx/platform.h"

namespace gbx::ui {

namespace {

using Screen = MenuScreen;
using pack_format::Rgb;

constexpr const char* kTag = "ui.menu";
constexpr std::string_view kEllipsis = "...";
// Insets of the first rows of a rounded corner, per radius.
constexpr int kCorners[5][4] = {{}, {1}, {2, 1}, {3, 1, 1}, {4, 2, 1, 1}};
constexpr Rgb kBlack{0, 0, 0, 0};
// Mixing amounts out of 256: the dim over the paused game, the dim under a
// dialog and the drop shadows of cards.
constexpr int kPauseDim = 184;   // 0.72
constexpr int kDialogDim = 141;  // 0.55
constexpr int kCardShadow = 90;  // 0.35
constexpr int kPauseShadow = 128;

std::string_view view(const char* text) {
    return {text, std::strlen(text)};
}

uint8_t expand5(unsigned v) {
    return static_cast<uint8_t>(v << 3 | v >> 2);
}

uint8_t expand6(unsigned v) {
    return static_cast<uint8_t>(v << 2 | v >> 4);
}

std::string_view rstrip(std::string_view t) {
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
    return t;
}

std::string_view lstrip(std::string_view t) {
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    return t;
}

// The buttons of the ERRORS page, of SAVE / LOAD and of WIRELESS GAMEPAD.
constexpr const char* kErrorButtons[] = {"CLEAR", "BACK"};
constexpr const char* kSlotButtons[] = {"SAVE", "LOAD"};
constexpr const char* kGamepadButtons[] = {"PAIR", "BACK"};

const char* save_text(Screen::Save save) {
    switch (save) {
        case Screen::Save::none: return "NONE";
        case Screen::Save::clean: return "CLEAN";
        case Screen::Save::dirty: return "DIRTY";
        case Screen::Save::writing: return "WRITING";
        case Screen::Save::error: return "ERROR";
    }
    return "";
}

}  // namespace

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | (b * 31 + 127) / 255);
}

namespace {

uint16_t bgr555_to_rgb565(uint16_t p) {
    return rgb565(expand5(p & 31), expand5((p >> 5) & 31), expand5(p >> 10 & 31));
}

}  // namespace

bool MenuTheme::load(const Pack& pack) {
    const auto& m = pack.menu();
    width = m.grid_width;
    height = m.grid_height;
    scale = m.scale;
    if (width <= 0 || height <= 0 || scale <= 0) {
        platform::log(platform::LogLevel::error, kTag, "menu grid %dx%d at %dx", width, height, scale);
        return false;
    }
    for (int i = 0; i < pack_format::kMenuColorCount; ++i) {
        rgb[i] = pack.menu_colors()[i];
        color[i] = rgb565(rgb[i].r, rgb[i].g, rgb[i].b);
    }
    palettes = pack.menu_palettes();
    icons = pack.menu_icons();
    return true;
}

void Surface::fill(int x, int y, int w, int h, uint16_t color) {
    const int x0 = std::max(x, clip_x0), y0 = std::max(y, clip_y0);
    const int x1 = std::min(x + w, clip_x1), y1 = std::min(y + h, clip_y1);
    for (int row = y0; row < y1; ++row) {
        std::fill(pixels + row * width + x0, pixels + row * width + std::max(x0, x1), color);
    }
}

void Surface::blend(int x, int y, int w, int h, Rgb color, int amount) {
    const int x0 = std::max(x, clip_x0), y0 = std::max(y, clip_y0);
    const int x1 = std::min(x + w, clip_x1), y1 = std::min(y + h, clip_y1);
    auto mix = [amount](int from, int to) { return static_cast<uint8_t>(from + ((to - from) * amount + 128) / 256); };
    for (int row = y0; row < y1; ++row) {
        uint16_t* p = pixels + row * width;
        uint16_t last_in = static_cast<uint16_t>(~p[std::min(x0, width - 1)]);
        uint16_t last_out = 0;
        for (int col = x0; col < x1; ++col) {
            // Areas are mostly of one color, so convert only on a change.
            if (p[col] != last_in) {
                last_in = p[col];
                last_out = rgb565(mix(expand5(last_in >> 11), color.r), mix(expand6((last_in >> 5) & 63), color.g),
                                  mix(expand5(last_in & 31), color.b));
            }
            p[col] = last_out;
        }
    }
}

void Surface::rrect(int x, int y, int w, int h, int r, uint16_t color) {
    const int radius = std::clamp(std::min({r, h / 2, w / 2}), 0, 4);
    const int* inset = kCorners[radius];
    for (int row = 0; row < h; ++row) {
        const int k = row < radius ? inset[row] : h - 1 - row < radius ? inset[h - 1 - row] : 0;
        fill(x + k, y + row, w - 2 * k, 1, color);
    }
}

void Surface::frame(int x, int y, int w, int h, int r, uint16_t line, uint16_t inside) {
    rrect(x, y, w, h, r, line);
    rrect(x + 1, y + 1, w - 2, h - 2, std::max(0, r - 1), inside);
}

MenuPainter::Fitted MenuPainter::fit(std::string_view t, int width) const {
    if (measure(t) <= width) {
        return {t, false};
    }
    const int room = width - measure(kEllipsis);
    int used = 0;
    size_t at = 0;
    while (at < t.size()) {
        size_t next = at;
        const int advance = font_.advance(MenuFont::decode(t, next));
        if (used + advance > room) {
            break;
        }
        used += advance;
        at = next;
    }
    return {rstrip(t.substr(0, at)), true};
}

int MenuPainter::glyphs(Surface& s, int x, int top, std::string_view t, uint16_t color) const {
    for (size_t at = 0; at < t.size();) {
        const uint32_t code = MenuFont::decode(t, at);
        MenuFont::Glyph g;
        if (!font_.glyph(code, g)) {
            // A box as tall as a CJK glyph stands in for a character the font lacks.
            s.fill(x + 1, top + 3, 10, 1, color);
            s.fill(x + 1, top + 13, 10, 1, color);
            s.fill(x + 1, top + 3, 1, 11, color);
            s.fill(x + 10, top + 3, 1, 11, color);
            x += MenuFont::kMissingAdvance;
            continue;
        }
        const int gx = x + g.left;
        const int gy = top + g.top;
        unsigned bit = 0;
        for (int r = 0; r < g.height; ++r) {
            const int py = gy + r;
            for (int col = 0; col < g.width; ++col, ++bit) {
                const int px = gx + col;
                if ((g.bits[bit >> 3] >> (7 - (bit & 7)) & 1) && px >= s.clip_x0 && px < s.clip_x1 && py >= s.clip_y0 &&
                    py < s.clip_y1) {
                    s.pixels[py * s.width + px] = color;
                }
            }
        }
        x += g.advance;
    }
    return x;
}

int MenuPainter::text(Surface& s, int x, int top, std::string_view t, uint16_t color, int width, Align align) const {
    Fitted f{t, false};
    if (width != kNoWidth) {
        f = fit(t, width);
    }
    if (align != Align::left) {
        const int total = measure(f.text) + (f.cut ? measure(kEllipsis) : 0);
        x -= align == Align::right ? total : total / 2;
    }
    x = glyphs(s, x, top, f.text, color);
    return f.cut ? glyphs(s, x, top, kEllipsis, color) : x;
}

int MenuPainter::wrap(std::string_view t, int width, std::span<std::string_view> lines) const {
    size_t count = 0;
    auto push = [&](std::string_view line) {
        if (count < lines.size()) lines[count++] = line;
    };
    size_t start = 0;
    for (;;) {
        const size_t end = std::min(t.find('\n', start), t.size());
        const std::string_view paragraph = t.substr(start, end - start);
        // Words keep their trailing space; each CJK character is a word of its own.
        size_t line = 0;  // the line so far is paragraph[line, word)
        size_t word = 0;
        // Adds the word paragraph[word, word_end), or breaks the line before it.
        auto add = [&](size_t word_end) {
            if (line < word && measure(rstrip(paragraph.substr(line, word_end - line))) > width) {
                push(rstrip(paragraph.substr(line, word - line)));
                line = word_end - lstrip(paragraph.substr(word, word_end - word)).size();
            }
            word = word_end;
        };
        for (size_t at = 0; at < paragraph.size();) {
            const size_t here = at;
            const uint32_t code = MenuFont::decode(paragraph, at);
            if (code >= 0x2e80 && code != MenuFont::kInvalid) {
                if (word < here) add(here);
                add(at);
            } else if (code == ' ') {
                add(at);
            }
        }
        if (word < paragraph.size()) add(paragraph.size());
        push(rstrip(paragraph.substr(line, paragraph.size() - line)));
        if (end >= t.size()) break;
        start = end + 1;
    }
    return static_cast<int>(count);
}

void MenuPainter::icon(Surface& s, std::string_view name, int x, int y, uint16_t color, int zoom) const {
    for (const auto& i : theme_.icons) {
        if (field(i.id) != name) continue;
        for (int row = 0; row < 16; ++row) {
            for (int col = 0; col < 16; ++col) {
                if (i.rows[row] >> (15 - col) & 1) s.fill(x + col * zoom, y + row * zoom, zoom, zoom, color);
            }
        }
        return;
    }
}

void MenuPainter::header(Surface& s, std::string_view title, std::string_view path, std::string_view right) const {
    s.fill(0, 0, theme_.width, kHeader, c(MenuColor::header));
    const int x = text(s, 6, 0, title, c(MenuColor::header_ink));
    int right_x = theme_.width - 6;
    if (!right.empty()) {
        right_x = text(s, theme_.width - 6, 0, right, c(MenuColor::header_muted), kNoWidth, Align::right) -
                  measure(right) - 8;
    }
    if (!path.empty()) {
        text(s, x + 6, 0, path, c(MenuColor::header_muted), right_x - x - 6);
    }
}

void MenuPainter::footer(Surface& s, const Screen& m) const {
    std::string_view t = view(m.footer);
    s.fill(0, kFooter, theme_.width, 1, c(MenuColor::rule));
    const int width = theme_.width - 12;
    // Fields two spaces apart, such as a ROM's title, MBC and RAM, drop from
    // the end before the text ends in an ellipsis.
    for (size_t gap; measure(t) > width && (gap = t.rfind("  ")) != std::string_view::npos && gap > 0;) {
        t = t.substr(0, gap);
    }
    const MenuColor color = m.footer_alert ? MenuColor::alert : m.footer_warning ? MenuColor::dirty : MenuColor::muted;
    text(s, 6, kFooter + 1, t, c(color), width);
}

uint16_t MenuPainter::ink_of(Screen::Ink i) const {
    switch (i) {
        case Screen::Ink::muted: break;
        case Screen::Ink::disabled: return c(MenuColor::disabled);
        case Screen::Ink::alert: return c(MenuColor::alert);
        case Screen::Ink::warning: return c(MenuColor::dirty);
    }
    return c(MenuColor::muted);
}

void MenuPainter::badge(Surface& s, int x, int y, Screen::Badge b, bool enabled, bool selected) const {
    constexpr int w = 24, h = 13;
    const char* label = b == Screen::Badge::gba ? "GBA" : b == Screen::Badge::gbc ? "GBC" : "GB";
    const MenuColor machine = b == Screen::Badge::gba ? MenuColor::gba
                              : b == Screen::Badge::gbc ? MenuColor::gbc
                                                        : MenuColor::gb;
    uint16_t ink;
    if (selected) {
        s.rrect(x, y, w, h, 2, c(MenuColor::accent_ink));
        ink = c(MenuColor::accent);
    } else {
        s.rrect(x, y, w, h, 2, c(enabled ? machine : MenuColor::disabled));
        ink = c(MenuColor::badge_ink);
    }
    text(s, x + (w - measure(label) + 1) / 2, y - 2, label, ink);
}

void MenuPainter::scrollbar(Surface& s, int first, int total) const {
    if (total <= kRows) {
        return;
    }
    const int top = kHeader + 2;
    const int height = kRows * kRow - 4;
    s.fill(theme_.width - 4, top, 2, height, c(MenuColor::rule));
    const int thumb = std::max(8, height * kRows / total);
    const int at = top + (height - thumb) * first / (total - kRows);
    s.fill(theme_.width - 4, at, 2, thumb, c(MenuColor::muted));
}

void MenuPainter::triangle(Surface& s, int x, int y, bool left, uint16_t color) const {
    constexpr int kLeft[] = {1, 3, 5}, kRight[] = {5, 3, 1};
    for (int i = 0; i < 3; ++i) {
        const int h = left ? kLeft[i] : kRight[i];
        s.fill(x + i, y + (5 - h) / 2, 1, h, color);
    }
}

void MenuPainter::backdrop(std::span<const uint16_t> frame, int width, int height, uint16_t* out) const {
    const int w = theme_.width, h = theme_.height;
    std::fill(out, out + w * h, uint16_t{0});
    if (frame.size() >= static_cast<size_t>(width) * height) {
        const int x0 = (w - width) / 2, y0 = (h - height) / 2;
        for (int y = 0; y < height; ++y) {
            if (y0 + y < 0 || y0 + y >= h) continue;
            for (int x = 0; x < width; ++x) {
                if (x0 + x < 0 || x0 + x >= w) continue;
                const uint16_t p = frame[y * width + x];
                out[(y0 + y) * w + x0 + x] = rgb565(expand5(p & 31), expand5((p >> 5) & 31), expand5(p >> 10));
            }
        }
    }
    Surface s(out, w, h);
    s.blend(0, 0, w, h, theme_.rgb[static_cast<int>(MenuColor::scrim)], kPauseDim);
}

void MenuPainter::draw(const Screen& m, Surface& s, const uint16_t* backdrop, const uint16_t* thumbnail,
                       int marquee) const {
    switch (m.kind) {
        case Screen::Kind::home: home(m, s); break;
        case Screen::Kind::roms: roms(m, s, marquee); break;
        case Screen::Kind::message: message(m, s); break;
        case Screen::Kind::settings: settings(m, s); break;
        case Screen::Kind::pause: pause(m, s, backdrop); break;
        case Screen::Kind::errors: errors(m, s); break;
        case Screen::Kind::loading: loading(m, s); break;
        case Screen::Kind::slots: slots(m, s, thumbnail); break;
        case Screen::Kind::gamepad: gamepad(m, s); break;
    }
    if (m.dialog.shown) {
        dialog(m, s);
    }
}

MenuPainter::HomeCards MenuPainter::home_cards(const Screen&) const {
    // Room for four cards; with fewer, the cards keep their places and those
    // below the last stay empty.
    constexpr int kPlaces = 4;
    HomeCards cards{0, 26, 4};
    cards.top = kHeader + (kFooter - kHeader - kPlaces * cards.height - (kPlaces - 1) * cards.gap) / 2;
    return cards;
}

void MenuPainter::home(const Screen& m, Surface& s) const {
    const int W = theme_.width;
    s.fill(0, 0, W, theme_.height, c(MenuColor::paper));
    header(s, view(m.title), {}, view(m.status));
    const HomeCards cards = home_cards(m);
    const int pad = (cards.height - kLine) / 2;
    int y = cards.top;
    for (int i = 0; i < m.row_count; ++i) {
        const auto& e = m.rows[i];
        uint16_t ink, detail;
        if (i == m.selected) {
            s.rrect(8, y, W - 16, cards.height, 3, bar(e.enabled));
            ink = detail = c(MenuColor::accent_ink);
        } else {
            s.frame(8, y, W - 16, cards.height, 3, c(MenuColor::rule), c(MenuColor::card));
            ink = c(e.enabled ? MenuColor::ink : MenuColor::disabled);
            detail = c(e.enabled ? MenuColor::muted : MenuColor::disabled);
        }
        icon(s, view(e.icon), 16, y + pad, ink);
        text(s, 40, y + pad, view(e.label), ink);
        if (e.detail[0]) {
            text(s, W - 18, y + pad, view(e.detail), detail, kNoWidth, Align::right);
        }
        y += cards.height + cards.gap;
    }
    footer(s, m);
}

int MenuPainter::rom_right(const Screen& m) const {
    return m.total <= kRows ? theme_.width - 6 : theme_.width - 8;
}

int MenuPainter::rom_name_right(const Screen& m, const Screen::Row& row) const {
    const int tag_x = rom_right(m) - 4;
    return row.detail[0] ? tag_x - measure(view(row.detail)) - 8 : tag_x;
}

int MenuPainter::marquee_range(const Screen& m) const {
    if (m.kind != Screen::Kind::roms || m.selected < 0 || m.selected >= m.row_count || m.dialog.shown) {
        return 0;
    }
    const auto& row = m.rows[m.selected];
    return std::max(0, measure(view(row.label)) - (rom_name_right(m, row) - 34));
}

void MenuPainter::roms(const Screen& m, Surface& s, int marquee) const {
    s.fill(0, 0, theme_.width, theme_.height, c(MenuColor::paper));
    header(s, view(m.title), view(m.path), view(m.status));
    const int right = rom_right(m);
    for (int row = 0; row < m.row_count; ++row) {
        const auto& item = m.rows[row];
        const int y = kHeader + row * kRow;
        const bool sel = row == m.selected;
        uint16_t ink, side;
        if (sel) {
            s.rrect(2, y + 1, right - 2, kRow - 2, 2, bar(item.enabled));
            ink = side = c(MenuColor::accent_ink);
        } else {
            ink = c(item.enabled ? MenuColor::ink : MenuColor::disabled);
            side = c(item.enabled ? MenuColor::muted : MenuColor::disabled);
        }
        const int name_right = rom_name_right(m, item);
        if (item.badge == Screen::Badge::folder) {
            icon(s, "folder", 8, y + 1, sel ? c(MenuColor::accent_ink) : c(MenuColor::muted));
        } else {
            badge(s, 6, y + 3, item.badge, item.enabled, sel);
        }
        if (sel && marquee > 0) {
            // The selected name scrolls inside its room instead of ending in an ellipsis.
            Surface clipped = s;
            clipped.clip_x0 = 34;
            clipped.clip_x1 = std::max(34, name_right);
            glyphs(clipped, 34 - marquee, y + 1, view(item.label), ink);
        } else {
            text(s, 34, y + 1, view(item.label), ink, name_right - 34);
        }
        if (item.detail[0]) {
            text(s, right - 4, y + 1, view(item.detail), side, kNoWidth, Align::right);
        }
    }
    scrollbar(s, m.first, m.total);
    footer(s, m);
}

void MenuPainter::message(const Screen& m, Surface& s) const {
    s.fill(0, 0, theme_.width, theme_.height, c(MenuColor::paper));
    header(s, view(m.title), {}, view(m.status));
    notice(m, s, c(MenuColor::ink));
}

void MenuPainter::notice(const Screen& m, Surface& s, uint16_t heading) const {
    const int W = theme_.width;
    icon(s, view(m.icon), W / 2 - 16, 34, c(MenuColor::muted), 2);
    text(s, W / 2, 74, view(m.heading), heading, W - 40, Align::center);
    std::array<std::string_view, 3> lines;
    const int n = wrap(view(m.body), W - 40, lines);
    for (int i = 0; i < n; ++i) {
        text(s, W / 2, 94 + i * kLine, lines[i], c(MenuColor::muted), kNoWidth, Align::center);
    }
}

// A choice's pill: its label, after a 2x2 chip of the palette's four colors
// when it has a swatch.
int MenuPainter::choice_width(const Screen::Row& row, int k) const {
    return measure(view(row.choices[k])) + 12 + (row.swatches[k] >= 0 ? 6 : 0);
}

int MenuPainter::choice_x(const Screen::Row& row, int k) const {
    int x = setting_right();
    for (int j = row.choice_count - 1; j >= k; --j) {
        x -= choice_width(row, j);
        if (j > k) x -= 3;
    }
    return x;
}

void MenuPainter::settings(const Screen& m, Surface& s) const {
    const int W = theme_.width;
    s.fill(0, 0, W, theme_.height, c(MenuColor::paper));
    header(s, view(m.title), {}, view(m.status));
    for (int i = 0; i < m.row_count; ++i) {
        const auto& it = m.rows[i];
        const int y = setting_y(i);
        const bool sel = i == m.selected;
        const bool on = it.enabled;
        if (sel) {
            s.rrect(4, y + 2, W - 8, 18, 2, bar(on));
        }
        const uint16_t ink = c(sel ? MenuColor::accent_ink : on ? MenuColor::ink : MenuColor::disabled);
        text(s, kSettingLeft, y + 3, view(it.label), ink);
        const int right = setting_right();
        if (it.setting == Screen::Setting::level) {
            char value[8];
            if (it.unsupported && !on) {
                std::snprintf(value, sizeof value, "N/A");
            } else {
                std::snprintf(value, sizeof value, "%d%%", it.value);
            }
            text(s, right, y + 3, value, ink, kNoWidth, Align::right);
            const int bar_right = right - 34;
            const int x = level_x(right);
            if (sel && on) {
                triangle(s, x - 7, y + 9, true, c(MenuColor::accent_ink));
                triangle(s, bar_right + 4, y + 9, false, c(MenuColor::accent_ink));
            }
            const int lit = it.unsupported ? 0 : it.value / 10;
            for (int k = 0; k < 10; ++k) {
                MenuColor color;
                if (sel) {
                    color = k < lit ? MenuColor::accent_ink : on ? MenuColor::accent_dim : MenuColor::disabled;
                } else if (!on) {
                    color = k < lit ? MenuColor::disabled : MenuColor::rule;
                } else {
                    color = k < lit ? MenuColor::accent : MenuColor::rule;
                }
                s.fill(x + k * 6, y + 7, 5, 9, c(color));
            }
            continue;
        }
        for (int k = 0; k < it.choice_count; ++k) {
            const int x = choice_x(it, k);
            const int w = choice_width(it, k);
            const bool chosen = k == it.value;
            const bool off = it.off >> k & 1;
            MenuColor fill, line, tink;
            if (!on) {
                fill = chosen ? MenuColor::disabled : MenuColor::paper;
                line = chosen ? MenuColor::disabled : MenuColor::rule;
                tink = chosen ? MenuColor::paper : MenuColor::disabled;
            } else if (off) {
                fill = sel ? MenuColor::accent : MenuColor::paper;
                line = sel ? MenuColor::accent_dim : MenuColor::rule;
                tink = sel ? MenuColor::accent_dim : MenuColor::disabled;
            } else if (sel) {
                fill = chosen ? MenuColor::accent_ink : MenuColor::accent;
                line = chosen ? MenuColor::accent_ink : MenuColor::accent_dim;
                tink = chosen ? MenuColor::accent : MenuColor::accent_ink;
            } else {
                fill = chosen ? MenuColor::ink : MenuColor::paper;
                line = chosen ? MenuColor::ink : MenuColor::rule;
                tink = chosen ? MenuColor::paper : MenuColor::ink;
            }
            s.frame(x, y + 4, w, 14, 3, c(line), c(fill));
            int tx = x + 6;
            if (const int p = it.swatches[k]; p >= 0 && p < static_cast<int>(theme_.palettes.size())) {
                // Lightest at the top left, darkest at the bottom right. A
                // chip that cannot be chosen fades into its pill.
                for (int shade = 0; shade < 4; ++shade) {
                    const auto* rgb = theme_.palettes[p].shades[shade];
                    s.fill(x + 4 + shade % 2 * 3, y + 8 + shade / 2 * 3, 3, 3, rgb565(rgb[0], rgb[1], rgb[2]));
                }
                if (!on || off) s.blend(x + 4, y + 8, 6, 6, theme_.rgb[static_cast<int>(fill)], 154);
                tx = x + 13;
            }
            text(s, tx, y + 3, view(it.choices[k]), c(tink));
        }
    }
    footer(s, m);
}

MenuPainter::PauseBox MenuPainter::pause_box(const Screen& m) const {
    PauseBox b{};
    b.w = 168;
    b.h = kHeader + 4 + m.row_count * kRow + 4 + 1 + kLine + 2;
    b.x = (theme_.width - b.w) / 2;
    b.y = (theme_.height - b.h) / 2;
    b.top = b.y + kHeader + 4;
    return b;
}

void MenuPainter::pause(const Screen& m, Surface& s, const uint16_t* backdrop) const {
    const int W = theme_.width, H = theme_.height;
    if (backdrop) {
        std::copy(backdrop, backdrop + W * H, s.pixels);
    } else {
        s.fill(0, 0, W, H, 0);
        s.blend(0, 0, W, H, theme_.rgb[static_cast<int>(MenuColor::scrim)], kPauseDim);
    }
    const PauseBox b = pause_box(m);
    s.blend(b.x + 2, b.y + 2, b.w, b.h, kBlack, kPauseShadow);
    s.rrect(b.x, b.y, b.w, b.h, 3, c(MenuColor::paper));
    s.rrect(b.x, b.y, b.w, kHeader, 3, c(MenuColor::header));
    s.fill(b.x, b.y + kHeader - 3, b.w, 3, c(MenuColor::header));
    text(s, b.x + 6, b.y, view(m.title), c(MenuColor::header_ink));
    for (int i = 0; i < m.row_count; ++i) {
        const auto& it = m.rows[i];
        const int ry = b.top + i * kRow;
        const bool sel = i == m.selected;
        if (sel) {
            s.rrect(b.x + 3, ry + 1, b.w - 6, kRow - 2, 2, bar(it.enabled));
        }
        const uint16_t ink = c(sel ? MenuColor::accent_ink : it.enabled ? MenuColor::ink : MenuColor::disabled);
        text(s, b.x + 10, ry + (kRow - kLine) / 2, view(it.label), ink);
        if (it.detail[0]) {
            text(s, b.x + b.w - 10, ry + (kRow - kLine) / 2, view(it.detail),
                 sel ? c(MenuColor::accent_ink) : ink_of(it.detail_ink), kNoWidth, Align::right);
        }
    }
    int sy = b.top + m.row_count * kRow + 4;
    s.fill(b.x + 6, sy, b.w - 12, 1, c(MenuColor::rule));
    sy += 1;
    text(s, b.x + 10, sy, "SAVE", c(MenuColor::muted));
    const MenuColor tone = m.save == Screen::Save::clean   ? MenuColor::clean
                           : m.save == Screen::Save::error ? MenuColor::error
                           : m.save == Screen::Save::none  ? MenuColor::muted
                                                           : MenuColor::dirty;
    const std::string_view state = save_text(m.save);
    const int end = b.x + b.w - 10;
    text(s, end, sy, state, c(tone), kNoWidth, Align::right);
    s.rrect(end - measure(state) - 9, sy + 5, 6, 6, 1, c(tone));
}

void MenuPainter::errors(const Screen& m, Surface& s) const {
    const int W = theme_.width;
    s.fill(0, 0, W, theme_.height, c(MenuColor::paper));
    header(s, view(m.title), {}, view(m.status));
    int y = kHeader + 4;
    for (int i = 0; i < m.row_count; ++i) {
        const auto& e = m.rows[i];
        const std::string_view reason = view(e.label);
        // An entry takes up to two lines; the second ends in an ellipsis
        // when the reason needs more.
        std::array<std::string_view, 3> lines;
        int n = std::min(wrap(reason, W - 44, lines), 3);
        if (y + std::min(n, 2) * kLine > kFooter - 2) break;
        text(s, 30, y, view(e.detail), c(MenuColor::muted), kNoWidth, Align::right);
        text(s, 38, y, lines[0], c(MenuColor::ink));
        if (n > 1) {
            const size_t rest = static_cast<size_t>(lines[1].data() - reason.data());
            text(s, 38, y + kLine, reason.substr(rest), c(MenuColor::ink), W - 44);
        }
        y += std::min(n, 2) * kLine + 2;
    }
    s.fill(0, kFooter, W, 1, c(MenuColor::rule));
    buttons(s, W / 2, kFooter + 2, kErrorButtons, m.selected);
}

void MenuPainter::gamepad(const Screen& m, Surface& s) const {
    const int W = theme_.width;
    s.fill(0, 0, W, theme_.height, c(MenuColor::paper));
    header(s, view(m.title), {}, view(m.status));
    notice(m, s, c(m.gamepad.alert ? MenuColor::alert : MenuColor::ink));
    s.fill(0, kFooter, W, 1, c(MenuColor::rule));
    const bool enabled[] = {m.gamepad.pair, true};
    buttons(s, W / 2, kFooter + 2, kGamepadButtons, m.selected, enabled);
}

void MenuPainter::loading(const Screen& m, Surface& s) const {
    const int W = theme_.width;
    s.fill(0, 0, W, theme_.height, c(MenuColor::paper));
    icon(s, "cartridge", W / 2 - 16, 34, c(MenuColor::header), 2);
    text(s, W / 2, 74, view(m.heading), c(MenuColor::ink), kNoWidth, Align::center);
    text(s, W / 2, 90, view(m.name), c(MenuColor::muted), W - 40, Align::center);
    const int bw = 120;
    const int bx = (W - bw) / 2;
    s.rrect(bx, 114, bw, 5, 2, c(MenuColor::rule));
    s.rrect(bx, 114, std::max(5, bw * std::clamp(m.progress, 0, 100) / 100), 5, 2, c(MenuColor::accent));
}

void MenuPainter::slots(const Screen& m, Surface& s, const uint16_t* thumbnail) const {
    s.fill(0, 0, theme_.width, theme_.height, c(MenuColor::paper));
    header(s, view(m.title), view(m.path), view(m.status));
    const auto& sl = m.slots;
    const int right = slot_right(m);
    for (int i = 0; i < m.row_count; ++i) {
        const auto& it = m.rows[i];
        const int y = kSlotTop + i * kSlotRow;
        const bool sel = i == m.selected;
        const bool on_bar = sel && sl.button < 0;
        uint16_t ink, side;
        if (on_bar) {
            // While a write is under way nothing else can start: the bar is gray.
            s.rrect(2, y + 1, right - 2, kSlotRow - 2, 2, bar(!sl.busy));
            ink = side = c(MenuColor::accent_ink);
        } else {
            if (sel) {
                s.frame(2, y + 1, right - 2, kSlotRow - 2, 2, c(MenuColor::accent), c(MenuColor::paper));
            }
            ink = c(MenuColor::ink);
            side = ink_of(it.detail_ink);
        }
        text(s, 10, y + 1, view(it.label), ink, right - 14);
        if (it.progress >= 0) {
            const int bw = right - 20;
            s.rrect(10, y + 20, bw, 5, 2, c(on_bar ? MenuColor::disabled : MenuColor::rule));
            s.rrect(10, y + 20, std::max(5, bw * std::clamp<int>(it.progress, 0, 100) / 100), 5, 2,
                    c(on_bar ? MenuColor::accent_ink : MenuColor::accent));
        } else {
            text(s, 10, y + 14, view(it.detail), side, right - 14);
        }
    }
    int tw, th;
    thumbnail_size(m, tw, th);
    const int tx = thumbnail_x(m);
    s.frame(tx, kThumbnailY, tw / 2 + 2, th / 2 + 2, 0, c(MenuColor::rule), c(MenuColor::card));
    if (sl.thumbnail && thumbnail) {
        // Half size, from the top left pixel of each 2x2; a view at 2x draws
        // the full size over it.
        for (int y = 0; y < th / 2; ++y) {
            const uint16_t* row = thumbnail + 2 * y * tw;
            for (int x = 0; x < tw / 2; ++x) {
                s.fill(tx + 1 + x, kThumbnailY + 1 + y, 1, 1, bgr555_to_rgb565(row[2 * x]));
            }
        }
    }
    buttons(s, slot_buttons_x(m), kSlotButtonsY, kSlotButtons, sl.button, sl.enabled);
    footer(s, m);
}

bool MenuPainter::thumbnail_at(const Screen& m, int& x, int& y) const {
    if (m.kind != Screen::Kind::slots || !m.slots.thumbnail || m.dialog.shown) return false;
    x = thumbnail_x(m) + 1;
    y = kThumbnailY + 1;
    return true;
}

void MenuPainter::thumbnail(const uint16_t* pixels, int width, int height, uint16_t* out, int stride) {
    for (int y = 0; y < height; ++y) {
        const uint16_t* row = pixels + y * width;
        uint16_t* o = out + y * stride;
        // Areas are mostly of one color, so convert only on a change.
        uint16_t last_in = static_cast<uint16_t>(~row[0]), last_out = 0;
        for (int x = 0; x < width; ++x) {
            if (row[x] != last_in) last_out = bgr555_to_rgb565(last_in = row[x]);
            o[x] = last_out;
        }
    }
}

MenuPainter::DialogBox MenuPainter::dialog_box(const Screen& m) const {
    const auto& d = m.dialog;
    DialogBox b{};
    b.w = 208;
    for (int i = 0; i < d.line_count; ++i) {
        const int room = static_cast<int>(b.lines.size()) - b.line_count;
        if (d.lines[i].label[0]) {
            if (room > 0) {
                b.lines[b.line_count] = {};
                b.source[b.line_count++] = i;
            }
            continue;
        }
        const int n = wrap(view(d.lines[i].text), b.w - 16, std::span(b.lines).subspan(b.line_count, room));
        for (int k = 0; k < n; ++k) b.source[b.line_count + k] = i;
        b.line_count += n;
    }
    b.h = kHeader + 6 + b.line_count * kLine + 6 + (d.button_count ? 20 : 0) + (d.progress >= 0 ? 12 : 0);
    b.x = (theme_.width - b.w) / 2;
    b.y = (theme_.height - b.h) / 2;
    b.top = b.y + kHeader;
    b.buttons_y = b.top + 6 + b.line_count * kLine + 6 + (d.progress >= 0 ? 12 : 0);
    return b;
}

int MenuPainter::buttons_width(std::span<const char* const> labels, std::span<int> widths) const {
    int total = 0;
    for (size_t i = 0; i < labels.size(); ++i) {
        widths[i] = measure(view(labels[i])) + 16;
        total += widths[i] + (i ? 6 : 0);
    }
    return total;
}

void MenuPainter::buttons(Surface& s, int cx, int y, std::span<const char* const> labels, int selected,
                          std::span<const bool> enabled) const {
    std::array<int, 2> widths{};
    int x = cx - buttons_width(labels, widths) / 2;
    for (size_t i = 0; i < labels.size(); ++i) {
        const bool on = i >= enabled.size() || enabled[i];
        uint16_t ink;
        if (static_cast<int>(i) == selected) {
            s.rrect(x, y, widths[i], 14, 3, bar(on));
            ink = c(MenuColor::accent_ink);
        } else {
            s.frame(x, y, widths[i], 14, 3, c(MenuColor::rule), c(MenuColor::card));
            ink = c(on ? MenuColor::ink : MenuColor::disabled);
        }
        text(s, x + 8, y - 1, view(labels[i]), ink);
        x += widths[i] + 6;
    }
}

MenuHit MenuPainter::hit_buttons(int x, int y, int cx, int top, std::span<const char* const> labels) const {
    std::array<int, 2> widths{};
    int bx = cx - buttons_width(labels, widths) / 2;
    for (size_t i = 0; i < labels.size(); ++i) {
        // A little more than the pill, which is small for a finger.
        if (x >= bx - 3 && x < bx + widths[i] + 3 && y >= top - 4 && y < top + 18) {
            return {MenuHit::Target::button, static_cast<int>(i), 0};
        }
        bx += widths[i] + 6;
    }
    return {};
}

void MenuPainter::dialog(const Screen& m, Surface& s) const {
    const int W = theme_.width, H = theme_.height;
    const auto& d = m.dialog;
    s.blend(0, 0, W, H, theme_.rgb[static_cast<int>(MenuColor::scrim)], kDialogDim);
    const DialogBox b = dialog_box(m);
    const uint16_t tone = c(d.tone == Screen::Tone::alert ? MenuColor::alert : MenuColor::header);
    s.blend(b.x + 2, b.y + 2, b.w, b.h, kBlack, kCardShadow);
    s.rrect(b.x, b.y, b.w, b.h, 3, c(MenuColor::paper));
    s.rrect(b.x, b.y, b.w, kHeader, 3, tone);
    s.fill(b.x, b.y + kHeader - 3, b.w, 3, tone);
    text(s, b.x + 6, b.y, view(d.title), c(MenuColor::header_ink), b.w - 12);
    for (int n = 0; n < b.line_count; ++n) {
        const int y = b.top + 6 + n * kLine;
        const auto& line = d.lines[b.source[n]];
        if (line.label[0]) {
            text(s, b.x + 8, y, view(line.label), c(MenuColor::muted));
            text(s, b.x + 44, y, view(line.text), c(MenuColor::ink), b.w - 52);
        } else {
            text(s, b.x + 8, y, b.lines[n], c(MenuColor::ink));
        }
    }
    if (d.progress >= 0) {
        const int by = b.top + 6 + b.line_count * kLine + 6;
        s.rrect(b.x + 24, by, b.w - 48, 5, 2, c(MenuColor::rule));
        s.rrect(b.x + 24, by, std::max(5, (b.w - 48) * std::clamp(d.progress, 0, 100) / 100), 5, 2,
                c(MenuColor::accent));
    }
    const char* labels[2] = {d.buttons[0], d.buttons[1]};
    buttons(s, W / 2, b.buttons_y, std::span(labels, std::clamp(d.button_count, 0, 2)), d.selected);
}

MenuHit MenuPainter::hit(const Screen& m, int x, int y) const {
    using Target = MenuHit::Target;
    const int W = theme_.width;
    if (x < 0 || x >= W || y < 0 || y >= theme_.height) return {};
    if (m.dialog.shown) {
        const auto& d = m.dialog;
        const char* labels[2] = {d.buttons[0], d.buttons[1]};
        return hit_buttons(x, y, W / 2, dialog_box(m).buttons_y, std::span(labels, std::clamp(d.button_count, 0, 2)));
    }
    switch (m.kind) {
        case Screen::Kind::home: {
            const HomeCards cards = home_cards(m);
            const int half_gap = cards.gap / 2;
            for (int i = 0; i < m.row_count; ++i) {
                const int cy = cards.top + i * (cards.height + cards.gap);
                if (x >= 8 && x < W - 8 && y >= cy - half_gap && y < cy + cards.height + half_gap) {
                    return {Target::row, i, 0};
                }
            }
            break;
        }
        case Screen::Kind::roms: {
            const int row = (y - kHeader) / kRow;
            if (y >= kHeader && y < kFooter && row < m.row_count) return {Target::row, row, 0};
            break;
        }
        case Screen::Kind::settings: {
            for (int i = 0; i < m.row_count; ++i) {
                const int ry = setting_y(i);
                if (y < ry || y >= ry + kSettingRow) continue;
                const auto& it = m.rows[i];
                if (it.setting == Screen::Setting::level) {
                    const int lx = level_x(setting_right());
                    if (x >= lx - 8 && x < lx + 60 + 8) {
                        return {Target::segment, i, (std::clamp((x - lx) / 6, 0, 9) + 1) * 10};
                    }
                } else {
                    for (int k = 0; k < it.choice_count; ++k) {
                        const int cx = choice_x(it, k);
                        if (x >= cx - 1 && x < cx + choice_width(it, k) + 2) return {Target::choice, i, k};
                    }
                }
                return {Target::row, i, 0};
            }
            break;
        }
        case Screen::Kind::pause: {
            const PauseBox b = pause_box(m);
            for (int i = 0; i < m.row_count; ++i) {
                const int ry = b.top + i * kRow;
                if (x >= b.x && x < b.x + b.w && y >= ry && y < ry + kRow) return {Target::row, i, 0};
            }
            break;
        }
        case Screen::Kind::errors: return hit_buttons(x, y, W / 2, kFooter + 2, kErrorButtons);
        case Screen::Kind::gamepad: return hit_buttons(x, y, W / 2, kFooter + 2, kGamepadButtons);
        case Screen::Kind::slots: {
            if (const MenuHit b = hit_buttons(x, y, slot_buttons_x(m), kSlotButtonsY, kSlotButtons);
                b.target != Target::none) {
                return b;
            }
            const int row = (y - kSlotTop) / kSlotRow;
            if (x >= 2 && x < slot_right(m) && y >= kSlotTop && row < m.row_count) return {Target::row, row, 0};
            break;
        }
        case Screen::Kind::message:
        case Screen::Kind::loading: break;
    }
    return {};
}

}  // namespace gbx::ui
