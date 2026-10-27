#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "gbx/ui/pack_format.h"

namespace gbx::ui {

// An image stored in the pack, already scaled and placed on the logical screen.
struct ImageView {
    pack_format::PixelFormat format;
    int x;
    int y;
    int width;
    int height;
    int stride;              // bytes per row of the RGB565 plane
    const uint16_t* rgb;     // RGB565 plane
    const uint8_t* alpha;    // width * height alpha values, or nullptr for opaque images
};

// Read-only view of an asset pack. The pack stays where it is, for example in
// the memory-mapped assets partition; nothing is copied.
class Pack {
public:
    // Checks the header, size, version and CRC. Returns false and logs the
    // reason if the pack cannot be used.
    bool open(std::span<const std::byte> data);

    int width() const { return header_->width; }
    int height() const { return header_->height; }
    std::string_view board() const;

    // The image entry with this name, or nothing if the pack has none.
    std::optional<ImageView> image(std::string_view name) const;

    const pack_format::Layout& layout() const { return *layout_; }
    std::span<const pack_format::Control> controls() const { return controls_; }
    std::span<const pack_format::Lamp> lamps() const { return lamps_; }
    std::span<const pack_format::Font> fonts() const { return fonts_; }
    std::span<const pack_format::VfdCell> cells() const { return cells_; }

    // The menus' theme, see pack_format::Menu, and the menu font's bytes.
    const pack_format::Menu& menu() const { return *menu_; }
    std::span<const pack_format::Rgb> menu_colors() const { return menu_colors_; }
    std::span<const pack_format::MenuPalette> menu_palettes() const { return menu_palettes_; }
    std::span<const pack_format::MenuIcon> menu_icons() const { return menu_icons_; }
    std::span<const std::byte> menu_font() const { return menu_font_; }

    // The samples of the sound entry "sound.<cue>" at `rate`, or nothing if
    // the pack has none at that rate.
    std::span<const int16_t> sound(std::string_view cue, uint32_t rate) const;

private:
    const pack_format::Entry* find(std::string_view name) const;

    std::span<const std::byte> data_;
    const pack_format::Header* header_ = nullptr;
    std::span<const pack_format::Entry> entries_;
    const pack_format::Layout* layout_ = nullptr;
    std::span<const pack_format::Control> controls_;
    std::span<const pack_format::Lamp> lamps_;
    std::span<const pack_format::Font> fonts_;
    std::span<const pack_format::VfdCell> cells_;
    const pack_format::Menu* menu_ = nullptr;
    std::span<const pack_format::Rgb> menu_colors_;
    std::span<const pack_format::MenuPalette> menu_palettes_;
    std::span<const pack_format::MenuIcon> menu_icons_;
    std::span<const std::byte> menu_font_;
};

// Length of a NUL-padded fixed-size string field.
template <size_t N>
std::string_view field(const char (&text)[N]) {
    size_t n = 0;
    while (n < N && text[n] != '\0') {
        ++n;
    }
    return {text, n};
}

}  // namespace gbx::ui
