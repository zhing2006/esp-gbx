#include "gbx/ui/pack.h"

#include <array>
#include <cstdio>
#include <cstring>

#include "gbx/platform.h"

namespace gbx::ui {

namespace {

constexpr const char* kTag = "ui.pack";

using namespace pack_format;

constexpr std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    return table;
}

constexpr auto kCrcTable = make_crc_table();

// CRC-32 as computed by zlib.crc32().
uint32_t crc32(std::span<const std::byte> data) {
    uint32_t c = 0xFFFFFFFFu;
    for (std::byte b : data) {
        c = kCrcTable[(c ^ static_cast<uint8_t>(b)) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

bool fits(std::span<const std::byte> data, uint64_t offset, uint64_t size) {
    return offset <= data.size() && size <= data.size() - offset;
}

template <typename T>
const T* at(std::span<const std::byte> data, uint32_t offset) {
    return reinterpret_cast<const T*>(data.data() + offset);
}

}  // namespace

bool Pack::open(std::span<const std::byte> data) {
    if (data.empty()) {
        platform::log(platform::LogLevel::error, kTag, "no asset pack on this platform");
        return false;
    }
    if (data.size() < sizeof(Header)) {
        platform::log(platform::LogLevel::error, kTag, "asset pack is smaller than its header");
        return false;
    }
    const auto* header = at<Header>(data, 0);
    if (std::memcmp(header->magic, kMagic, sizeof kMagic) != 0) {
        platform::log(platform::LogLevel::error, kTag, "invalid asset pack header");
        return false;
    }
    if (header->version != kPackVersion) {
        platform::log(platform::LogLevel::error, kTag, "asset pack version %u, expected %u",
                      static_cast<unsigned>(header->version), static_cast<unsigned>(kPackVersion));
        return false;
    }
    if (header->total_size < sizeof(Header) || header->total_size > data.size()) {
        platform::log(platform::LogLevel::error, kTag, "asset pack size %u does not fit the %u-byte buffer",
                      static_cast<unsigned>(header->total_size), static_cast<unsigned>(data.size()));
        return false;
    }
    data = data.first(header->total_size);
    uint64_t start = platform::monotonic_us();
    uint32_t crc = crc32(data.subspan(sizeof(Header)));
    if (crc != header->crc32) {
        platform::log(platform::LogLevel::error, kTag, "asset pack CRC mismatch, it may be partly written");
        return false;
    }
    if (!fits(data, header->entries_offset, uint64_t{header->entry_count} * sizeof(Entry))) {
        platform::log(platform::LogLevel::error, kTag, "asset pack entry table is out of range");
        return false;
    }
    data_ = data;
    header_ = header;
    entries_ = {at<Entry>(data, header->entries_offset), header->entry_count};
    for (const Entry& e : entries_) {
        if (!fits(data, e.offset, e.size) || e.offset % kAlign != 0) {
            platform::log(platform::LogLevel::error, kTag, "asset pack entry %.*s is out of range",
                          static_cast<int>(field(e.name).size()), field(e.name).data());
            header_ = nullptr;
            return false;
        }
    }

    const Entry* layout = find("layout");
    if (layout == nullptr || layout->type != EntryType::layout || layout->size < sizeof(Layout)) {
        platform::log(platform::LogLevel::error, kTag, "asset pack has no layout");
        header_ = nullptr;
        return false;
    }
    layout_ = at<Layout>(data, layout->offset);
    uint64_t records = sizeof(Layout) + uint64_t{layout_->control_count} * sizeof(Control) +
                       uint64_t{layout_->lamp_count} * sizeof(Lamp) + uint64_t{layout_->font_count} * sizeof(Font) +
                       uint64_t{layout_->cell_count} * sizeof(VfdCell);
    if (records != layout->size) {
        platform::log(platform::LogLevel::error, kTag, "asset pack layout has %u bytes, its counts need %u",
                      static_cast<unsigned>(layout->size), static_cast<unsigned>(records));
        header_ = nullptr;
        return false;
    }
    uint32_t offset = layout->offset + sizeof(Layout);
    controls_ = {at<Control>(data, offset), layout_->control_count};
    offset += layout_->control_count * sizeof(Control);
    lamps_ = {at<Lamp>(data, offset), layout_->lamp_count};
    offset += layout_->lamp_count * sizeof(Lamp);
    fonts_ = {at<Font>(data, offset), layout_->font_count};
    offset += layout_->font_count * sizeof(Font);
    cells_ = {at<VfdCell>(data, offset), layout_->cell_count};
    for (const VfdCell& cell : cells_) {
        if (cell.line_count > kMaxCellLines) {
            platform::log(platform::LogLevel::error, kTag, "VFD cell %.8s has %u lines", cell.id,
                          static_cast<unsigned>(cell.line_count));
            header_ = nullptr;
            return false;
        }
    }

    const Entry* menu = find("menu");
    if (menu == nullptr || menu->type != EntryType::menu || menu->size < sizeof(Menu)) {
        platform::log(platform::LogLevel::error, kTag, "asset pack has no menu theme");
        header_ = nullptr;
        return false;
    }
    menu_ = at<Menu>(data, menu->offset);
    records = sizeof(Menu) + uint64_t{menu_->color_count} * sizeof(Rgb) +
              uint64_t{menu_->palette_count} * sizeof(MenuPalette) + uint64_t{menu_->icon_count} * sizeof(MenuIcon);
    if (records != menu->size || menu_->color_count != kMenuColorCount) {
        platform::log(platform::LogLevel::error, kTag, "asset pack menu theme has %u bytes and %u colors",
                      static_cast<unsigned>(menu->size), static_cast<unsigned>(menu_->color_count));
        header_ = nullptr;
        return false;
    }
    offset = menu->offset + sizeof(Menu);
    menu_colors_ = {at<Rgb>(data, offset), menu_->color_count};
    offset += menu_->color_count * sizeof(Rgb);
    menu_palettes_ = {at<MenuPalette>(data, offset), menu_->palette_count};
    offset += menu_->palette_count * sizeof(MenuPalette);
    menu_icons_ = {at<MenuIcon>(data, offset), menu_->icon_count};
    const Entry* font = find("menu_font");
    if (font == nullptr || font->type != EntryType::font) {
        platform::log(platform::LogLevel::error, kTag, "asset pack has no menu font");
        header_ = nullptr;
        return false;
    }
    menu_font_ = data.subspan(font->offset, font->size);

    platform::log(platform::LogLevel::info, kTag, "asset pack for %.*s, %ux%u, %u entries, %u bytes, CRC in %u ms",
                  static_cast<int>(board().size()), board().data(), width(), height(),
                  static_cast<unsigned>(entries_.size()), static_cast<unsigned>(data.size()),
                  static_cast<unsigned>((platform::monotonic_us() - start) / 1000));
    return true;
}

std::string_view Pack::board() const {
    return field(header_->board);
}

const Entry* Pack::find(std::string_view name) const {
    for (const Entry& e : entries_) {
        if (field(e.name) == name) {
            return &e;
        }
    }
    return nullptr;
}

std::span<const int16_t> Pack::sound(std::string_view cue, uint32_t rate) const {
    char name[32];
    std::snprintf(name, sizeof name, "sound.%.*s", static_cast<int>(cue.size()), cue.data());
    const Entry* e = find(name);
    if (e == nullptr || e->type != EntryType::sound || e->size < sizeof(Sound)) return {};
    const auto* s = at<Sound>(data_, e->offset);
    if (sizeof(Sound) + uint64_t{s->frames} * 2 > e->size) {
        platform::log(platform::LogLevel::error, kTag, "%s is malformed", name);
        return {};
    }
    if (s->rate != rate) return {};
    return {reinterpret_cast<const int16_t*>(data_.data() + e->offset + sizeof(Sound)), s->frames};
}

std::optional<ImageView> Pack::image(std::string_view name) const {
    const Entry* e = find(name);
    if (e == nullptr || e->type != EntryType::image || e->size < sizeof(Image)) {
        return std::nullopt;
    }
    const auto* im = at<Image>(data_, e->offset);
    uint64_t pixels = uint64_t{im->width} * im->height;
    uint64_t bytes = uint64_t{im->stride} * im->height + (im->format == PixelFormat::rgb565a8 ? pixels : 0);
    if (im->stride < im->width * 2 || im->data_offset % kAlign != 0 || !fits(data_, im->data_offset, bytes)) {
        platform::log(platform::LogLevel::error, kTag, "image %.*s is malformed", static_cast<int>(name.size()),
                      name.data());
        return std::nullopt;
    }
    const std::byte* base = data_.data() + im->data_offset;
    ImageView view{
        .format = im->format,
        .x = im->x,
        .y = im->y,
        .width = im->width,
        .height = im->height,
        .stride = im->stride,
        .rgb = reinterpret_cast<const uint16_t*>(base),
        .alpha = nullptr,
    };
    if (im->format == PixelFormat::rgb565a8) {
        view.alpha = reinterpret_cast<const uint8_t*>(base + uint64_t{im->stride} * im->height);
    }
    return view;
}

}  // namespace gbx::ui
