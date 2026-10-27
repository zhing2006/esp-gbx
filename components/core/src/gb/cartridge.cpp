#include "gbx/gb/cartridge.h"

#include <algorithm>
#include <array>
#include <utility>

#include "gbx/gb/header.h"

namespace gbx::gb {
Cartridge::Support Cartridge::layout(std::span<const uint8_t> bytes, size_t size, Layout& out) {
    if (bytes.size() < 0x150) return Support::size;
    const auto type = bytes[0x147];
    Kind kind;
    bool has_ram = false;
    unsigned max_rom = 0, max_ram = 0;
    switch (type) {
    case 0: case 8: case 9: kind = Kind::plain; has_ram = type != 0; max_ram = 8192; break;
    case 1: case 2: case 3: kind = Kind::mbc1; has_ram = type != 1; max_rom = 6; max_ram = 32768; break;
    case 5: case 6: kind = Kind::mbc2; max_rom = 3; break;
    case 0x0f: case 0x10: case 0x11: case 0x12: case 0x13:
        // Pan Docs, MBC3 and MBC30: the MBC3 addresses up to 2 MiB of ROM
        // and 32 KiB of RAM, the MBC30 4 MiB and 64 KiB. The header gives no
        // other way to tell them apart, so a header that declares more than
        // the MBC3 addresses selects the MBC30. mbc3-tester's 4 MiB type 11
        // ROM needs all 256 banks this way.
        kind = Kind::mbc3; has_ram = type == 0x10 || type >= 0x12; max_rom = 7; max_ram = 65536; break;
    case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e:
        kind = Kind::mbc5; has_ram = type != 0x19 && type != 0x1c; max_rom = 8;
        max_ram = type >= 0x1c ? 65536 : 131072; break;
    default: return Support::type;
    }
    if (bytes[0x148] > max_rom || size != (size_t{32768} << bytes[0x148])) return Support::size;
    // Header code 01 is retained for legacy homebrew (2 KiB), although Pan
    // Docs notes no licensed cartridge used it. Unsupported sizes are rejected.
    constexpr size_t sizes[]{0, 2048, 8192, 32768, 131072, 65536};
    if (bytes[0x149] >= std::size(sizes)) return Support::ram;
    const auto ram_size = kind == Kind::mbc2 ? 512 : has_ram ? sizes[bytes[0x149]] : 0;
    if (kind != Kind::mbc2 && ram_size > max_ram) return Support::ram;
    out = {kind, ram_size};
    return Support::ok;
}
Cartridge::Support Cartridge::supports(std::span<const uint8_t> header, size_t size) {
    Layout layout_out;
    return layout(header, size, layout_out);
}
bool Cartridge::load(std::span<const uint8_t> bytes) {
    Layout layout_out;
    if (layout(bytes, bytes.size(), layout_out) != Support::ok) return false;
    const auto type = bytes[0x147];
    const auto [kind, ram_size] = layout_out;
    std::unique_ptr<uint8_t[], Release> ram(ram_size ? static_cast<uint8_t*>(platform::allocate_external(ram_size)) : nullptr);
    if (ram_size && !ram) return false;
    // Commit only after validation and allocation succeed. Memory's mapped
    // pointers and the cartridge state must remain valid on a failed load.
    kind_ = kind; ram_size_ = ram_size; ram_ = std::move(ram);
    wide_ = kind == Kind::mbc3 && (bytes[0x148] > 6 || ram_size > 32768);
    has_rtc_ = type == 0x0f || type == 0x10;
    battery_ = type == 0x03 || type == 0x06 || type == 0x09 || type == 0x0f || type == 0x10 || type == 0x13 ||
               type == 0x1b || type == 0x1e;
    rom_.attach(bytes); rom_mask_ = static_cast<uint16_t>(bytes.size() / 16384 - 1);
    rumble_ = type >= 0x1c && type <= 0x1e;
    // Pan Docs, MBC1M: a second Nintendo logo in bank 10 identifies the
    // alternative multicart wiring. Compare the complete 48-byte logo, not a
    // title or a game-specific whitelist. Limit detection to 1 MiB MBC1 carts.
    multicart_ = kind_ == Kind::mbc1 && bytes.size() == 1048576 &&
        std::equal(kNintendoLogo.begin(), kNintendoLogo.end(), bytes.begin() + 0x40104);
    if (ram_size_) std::fill_n(ram_.get(), ram_size_, static_cast<uint8_t>(nibble_ram() ? 0xf0 : 0));
    reset();
    return true;
}
void Cartridge::reset(const RtcState& clock, Tick now) {
    ram_written_ = 0; rtc_written_ = false;
    bank1_ = 1; bank2_ = mode_ = ram_select_ = 0; ram_enabled_ = kind_ == Kind::plain;
    rtc_.reset(clock, now);
    resolve();
}
void Cartridge::resolve() {
    unsigned low = 0, high = bank1_, ram = ram_select_;
    if (kind_ == Kind::mbc1) {
        const auto upper = bank2_ << (multicart_ ? 4 : 5);
        low = mode_ ? upper : 0;
        high = upper | ((bank1_ ? bank1_ : 1) & (multicart_ ? 15 : 31));
        ram = mode_ && rom_.bytes().size() <= 524288 ? bank2_ : 0;
    }
    rom_bank[0] = static_cast<uint16_t>(low & rom_mask_); rom_bank[1] = static_cast<uint16_t>(high & rom_mask_);
    ram_offset_ = ram_size_ ? (ram * 8192) & (ram_size_ - 1) : 0;
    if (ram_window()) ram_written_ |= 1u << (ram_offset_ / 8192);
}
void Cartridge::load_ram(std::span<const uint8_t> bytes) {
    const size_t n = std::min(bytes.size(), ram_size_);
    for (size_t i = 0; i < n; ++i) ram_[i] = static_cast<uint8_t>(nibble_ram() ? bytes[i] | 0xf0 : bytes[i]);
}
bool Cartridge::control(uint16_t address, uint8_t value, Tick now) {
    const auto old0 = rom_bank[0], old1 = rom_bank[1], old_ram = ram_bank();
    switch (kind_) {
    case Kind::plain: return false;
    case Kind::mbc1:
        if (address < 0x2000) ram_enabled_ = (value & 15) == 10;
        else if (address < 0x4000) bank1_ = value & 31;
        else if (address < 0x6000) bank2_ = value & 3;
        else mode_ = value & 1;
        break;
    case Kind::mbc2:
        if (address >= 0x4000) return false;
        if (address & 0x100) bank1_ = (value & 15) ? value & 15 : 1;
        else ram_enabled_ = (value & 15) == 10;
        break;
    case Kind::mbc3:
        if (address < 0x2000) ram_enabled_ = (value & 15) == 10;
        else if (address < 0x4000) {
            const unsigned bank = value & (wide_ ? 255 : 127);
            bank1_ = static_cast<uint16_t>(bank ? bank : 1);
        }
        else if (address < 0x6000) ram_select_ = value;
        else if (has_rtc_) rtc_.latch(value, now);
        break;
    case Kind::mbc5:
        if (address < 0x2000) ram_enabled_ = (value & 15) == 10;
        else if (address < 0x3000) bank1_ = (bank1_ & 256) | value;
        else if (address < 0x4000) bank1_ = (bank1_ & 255) | ((value & 1) << 8);
        else if (address < 0x6000) ram_select_ = value & (rumble_ ? 7 : 15);
        break;
    }
    resolve();
    return old0 != rom_bank[0] || old1 != rom_bank[1] || old_ram != ram_bank();
}
uint8_t Cartridge::read(uint16_t address) const {
    if (address < 0x8000) {
        const auto* window = rom_window(address >> 14);
        return window ? window[address & 0x3fff] : 0xff;
    }
    if (const auto* window = ram_window()) return window[(address - 0xa000) & (ram_window_size() - 1)];
    const auto reg = rtc_register();
    return reg >= 0 ? rtc_.read(reg) : 0xff;
}
void Cartridge::write_ram(uint16_t address, uint8_t value, Tick now) {
    if (auto* window = ram_window())
        window[(address - 0xa000) & (ram_window_size() - 1)] = static_cast<uint8_t>(nibble_ram() ? value | 0xf0 : value);
    else if (const auto reg = rtc_register(); reg >= 0) { rtc_.write(reg, value, now); rtc_written_ = true; }
}
}  // namespace gbx::gb
