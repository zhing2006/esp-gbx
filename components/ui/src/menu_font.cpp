#include "menu_font.h"

#include <cstring>

#include "gbx/platform.h"

namespace gbx::ui {

namespace {

constexpr const char* kTag = "ui.font";
constexpr char kMagic[8] = {'G', 'B', 'X', 'F', 'O', 'N', 'T', '\0'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeader = 20;  // magic, u16 version, u8 line height, u8 ascent, u32 ranges, u32 glyphs
constexpr size_t kGlyphHeader = 5;

uint32_t u32(const std::byte* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

}  // namespace

bool MenuFont::load(std::span<const std::byte> data) {
    if (data.size() < kHeader || std::memcmp(data.data(), kMagic, sizeof kMagic) != 0) {
        platform::log(platform::LogLevel::error, kTag, "no menu font");
        return false;
    }
    uint16_t version;
    std::memcpy(&version, data.data() + 8, sizeof version);
    const auto line = static_cast<uint8_t>(data[10]);
    const uint32_t range_count = u32(data.data() + 12);
    const uint32_t glyph_count = u32(data.data() + 16);
    const uint64_t tables = kHeader + uint64_t{range_count} * sizeof(Range) + uint64_t{glyph_count} * 4;
    if (version != kVersion || line != kLine || tables > data.size()) {
        platform::log(platform::LogLevel::error, kTag, "menu font version %u, line %u, does not fit", version, line);
        return false;
    }
    // The pack aligns entries to 64 bytes, so the tables are word aligned.
    ranges_ = {reinterpret_cast<const Range*>(data.data() + kHeader), range_count};
    offsets_ = reinterpret_cast<const uint32_t*>(data.data() + kHeader + range_count * sizeof(Range));
    glyphs_ = reinterpret_cast<const uint8_t*>(data.data() + tables);
    glyph_bytes_ = data.size() - tables;
    glyph_count_ = glyph_count;
    for (const Range& r : ranges_) {
        if (uint64_t{r.index} + r.count > glyph_count) {
            platform::log(platform::LogLevel::error, kTag, "menu font range out of bounds");
            ranges_ = {};
            return false;
        }
    }
    for (uint32_t code = 0; code < ascii_.size(); ++code) {
        ascii_[code] = nullptr;
        ascii_[code] = lookup(code);
    }
    return true;
}

const uint8_t* MenuFont::lookup(uint32_t code) const {
    if (code < ascii_.size() && ascii_[code]) {
        return ascii_[code];
    }
    size_t lo = 0;
    size_t hi = ranges_.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const Range& r = ranges_[mid];
        if (code < r.first) {
            hi = mid;
        } else if (code >= r.first + r.count) {
            lo = mid + 1;
        } else {
            const uint32_t offset = offsets_[r.index + code - r.first];
            if (offset + kGlyphHeader > glyph_bytes_) {
                return nullptr;
            }
            const uint8_t* g = glyphs_ + offset;
            const size_t bits = (size_t{g[3]} * g[4] + 7) / 8;
            return offset + kGlyphHeader + bits <= glyph_bytes_ ? g : nullptr;
        }
    }
    return nullptr;
}

bool MenuFont::glyph(uint32_t code, Glyph& out) const {
    const uint8_t* g = code == kInvalid ? nullptr : lookup(code);
    if (!g) {
        return false;
    }
    out = {g[0], static_cast<int8_t>(g[1]), static_cast<int8_t>(g[2]), g[3], g[4], g + kGlyphHeader};
    return true;
}

int MenuFont::advance(uint32_t code) const {
    const uint8_t* g = code == kInvalid ? nullptr : lookup(code);
    return g ? g[0] : kMissingAdvance;
}

int MenuFont::measure(std::string_view text) const {
    int width = 0;
    for (size_t at = 0; at < text.size();) {
        width += advance(decode(text, at));
    }
    return width;
}

uint32_t MenuFont::decode(std::string_view text, size_t& at) {
    const auto byte = [&](size_t i) { return static_cast<uint8_t>(text[i]); };
    const uint8_t lead = byte(at);
    if (lead < 0x80) {
        ++at;
        return lead;
    }
    int length = 0;
    uint32_t code = 0;
    uint32_t lowest = 0;  // overlong encodings are not valid
    if ((lead & 0xe0) == 0xc0) {
        length = 2, code = lead & 0x1f, lowest = 0x80;
    } else if ((lead & 0xf0) == 0xe0) {
        length = 3, code = lead & 0x0f, lowest = 0x800;
    } else if ((lead & 0xf8) == 0xf0) {
        length = 4, code = lead & 0x07, lowest = 0x10000;
    }
    if (length == 0 || at + length > text.size()) {
        ++at;
        return kInvalid;
    }
    for (int i = 1; i < length; ++i) {
        const uint8_t next = byte(at + i);
        if ((next & 0xc0) != 0x80) {
            ++at;
            return kInvalid;
        }
        code = code << 6 | (next & 0x3f);
    }
    if (code < lowest || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
        ++at;
        return kInvalid;
    }
    at += length;
    return code;
}

}  // namespace gbx::ui
