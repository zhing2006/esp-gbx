#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace gbx::ui {

// The menu font as tools/ui/menu_font.py packs it: proportional 12 px glyphs
// on a 16 px line, read in place from the asset pack.
class MenuFont {
public:
    static constexpr int kLine = 16;
    // A character the font lacks, or a byte that is not UTF-8, is drawn as a
    // box as wide as a CJK character.
    static constexpr int kMissingAdvance = 12;

    struct Glyph {
        uint8_t advance = 0;
        int8_t left = 0;
        int8_t top = 0;  // rows below the top of the line
        uint8_t width = 0;
        uint8_t height = 0;
        const uint8_t* bits = nullptr;  // width * height bits, row by row, MSB first
    };

    bool load(std::span<const std::byte> data);

    // False when the font has no glyph for `code`.
    bool glyph(uint32_t code, Glyph& out) const;
    int advance(uint32_t code) const;
    int measure(std::string_view text) const;

    // The next character of UTF-8 text at `at`, which moves past it. A byte
    // that does not start a valid sequence is one character of its own,
    // returned as kInvalid.
    static constexpr uint32_t kInvalid = 0xffffffff;
    static uint32_t decode(std::string_view text, size_t& at);

private:
    struct Range {
        uint32_t first;
        uint32_t count;
        uint32_t index;
    };
    const uint8_t* lookup(uint32_t code) const;

    std::span<const Range> ranges_;
    const uint32_t* offsets_ = nullptr;
    const uint8_t* glyphs_ = nullptr;
    size_t glyph_bytes_ = 0;
    uint32_t glyph_count_ = 0;
    // ASCII, which most text is, without the range search.
    std::array<const uint8_t*, 128> ascii_{};
};

}  // namespace gbx::ui
