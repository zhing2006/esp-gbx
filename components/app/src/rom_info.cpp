#include "rom_info.h"

#include <algorithm>
#include <cstdio>

#include "gbx/gb/cartridge.h"
#include "gbx/gb/header.h"
#include "gbx/gba/rtc.h"

namespace gbx::app {
namespace {

// Pan Docs, The Cartridge Header, 0147.
const char* mbc_name(uint8_t type) {
    switch (type) {
        case 0x00: case 0x08: case 0x09: return "NO MBC";
        case 0x01: case 0x02: case 0x03: return "MBC1";
        case 0x05: case 0x06: return "MBC2";
        case 0x0b: case 0x0c: case 0x0d: return "MMM01";
        case 0x0f: case 0x10: return "MBC3+RTC";
        case 0x11: case 0x12: case 0x13: return "MBC3";
        case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: return "MBC5";
        case 0x20: return "MBC6";
        case 0x22: return "MBC7";
        case 0xfc: return "CAMERA";
        case 0xfd: return "TAMA5";
        case 0xfe: return "HUC3";
        case 0xff: return "HUC1";
        default: return nullptr;
    }
}

// Pan Docs, The Cartridge Header, 0149.
const char* ram_name(uint8_t code) {
    switch (code) {
        case 1: return "2K";
        case 2: return "8K";
        case 3: return "32K";
        case 4: return "128K";
        case 5: return "64K";
        default: return nullptr;
    }
}

// GBATEK, GBA Cartridge Header: B2 is 96h, and BD the complement check of
// A0-BC.
bool is_gba(std::span<const uint8_t> h) {
    if (h.size() < 0xc0 || h[0xb2] != 0x96) return false;
    uint8_t check = 0;
    for (size_t i = 0xa0; i <= 0xbc; ++i) check = static_cast<uint8_t>(check - h[i]);
    return static_cast<uint8_t>(check - 0x19) == h[0xbd];
}

}  // namespace

void rom_title(std::span<const uint8_t, 16> bytes, char (&out)[17]) {
    // Pan Docs, The Cartridge Header, 0134-0143: newer games use 013F-0142
    // for a manufacturer code of four uppercase letters or digits, and 0143
    // for the CGB flag. Pokemon Gold is POKEMON_GLD with AAUE; Yellow's
    // title runs into 013F, so a code needs all four characters.
    size_t length = 16;
    if (bytes[15] & 0x80) {
        length = 15;
        if (std::all_of(bytes.begin() + 11, bytes.begin() + 15,
                        [](uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); })) {
            length = 11;
        }
    }
    size_t n = 0;
    for (size_t i = 0; i < length && bytes[i]; ++i) {
        if (bytes[i] >= 0x20 && bytes[i] < 0x7f) out[n++] = static_cast<char>(bytes[i]);
    }
    while (n && out[n - 1] == ' ') --n;
    out[n] = '\0';
}

RomInfo parse_rom(std::span<const uint8_t> header, int64_t size) {
    RomInfo info;
    // The CGB boot ROM checks only the first 0x18 bytes of the logo.
    if (header.size() >= kRomHeaderBytes &&
        std::equal(gb::kNintendoLogo.begin(), gb::kNintendoLogo.begin() + 0x18, header.begin() + 0x104)) {
        const uint8_t cgb = header[0x143];
        info.machine = (cgb & 0xc0) == 0xc0 ? RomInfo::Machine::gbc_only
                       : cgb & 0x80         ? RomInfo::Machine::gbc
                                            : RomInfo::Machine::gb;
        info.type = header[0x147];
        info.ram_code = header[0x149];
        rom_title(header.subspan<0x134, 16>(), info.title);
        if (size > kMaxRomBytes) {
            info.problem = RomInfo::Problem::too_large;
        } else {
            switch (gb::Cartridge::supports(header, static_cast<size_t>(size))) {
                case gb::Cartridge::Support::ok: info.problem = RomInfo::Problem::none; break;
                case gb::Cartridge::Support::type: info.problem = RomInfo::Problem::type; break;
                case gb::Cartridge::Support::size: info.problem = RomInfo::Problem::size; break;
                case gb::Cartridge::Support::ram: info.problem = RomInfo::Problem::ram; break;
            }
        }
    } else if (is_gba(header)) {
        info.machine = RomInfo::Machine::gba;
        info.problem = size > kMaxGbaRomBytes ? RomInfo::Problem::too_large : RomInfo::Problem::none;
        gba_title(header.subspan<0xa0, 12>(), info.title);
        // GBATEK, GBA Cartridge Header: a 4 character game code at AC.
        for (size_t i = 0; i < 4; ++i) {
            const uint8_t c = header[0xac + i];
            info.code[i] = c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
        }
        info.rtc = gba::cartridge_has_rtc(header.subspan<0xac, 4>());
    }
    return info;
}

void gba_title(std::span<const uint8_t, 12> bytes, char (&out)[17]) {
    size_t n = 0;
    for (size_t i = 0; i < bytes.size() && bytes[i]; ++i) {
        if (bytes[i] >= 0x20 && bytes[i] < 0x7f) out[n++] = static_cast<char>(bytes[i]);
    }
    while (n && out[n - 1] == ' ') --n;
    out[n] = '\0';
}

void describe_rom(const RomInfo& info, char* out, size_t size) {
    if (info.machine == RomInfo::Machine::gba) {
        std::snprintf(out, size, "%s%s%s%s", info.title, info.title[0] ? "  " : "", info.code, info.rtc ? "  RTC" : "");
        return;
    }
    const char* mbc = mbc_name(info.type);
    char type[8];
    if (!mbc) {
        std::snprintf(type, sizeof type, "TYPE %02X", info.type);
        mbc = type;
    }
    const int n = std::snprintf(out, size, "%s%s%s", info.title, info.title[0] ? "  " : "", mbc);
    const bool mbc2 = info.type == 0x05 || info.type == 0x06;
    if (const char* ram = ram_name(info.ram_code); ram && !mbc2 && n >= 0 && static_cast<size_t>(n) < size) {
        std::snprintf(out + n, size - n, "  RAM %s", ram);
    }
}

void describe_problem(const RomInfo& info, char* out, size_t size) {
    switch (info.problem) {
        case RomInfo::Problem::none: std::snprintf(out, size, "%s", ""); break;
        case RomInfo::Problem::not_rom:
            std::snprintf(out, size, "Not a ROM: the header is not valid.");
            break;
        case RomInfo::Problem::too_large:
            std::snprintf(out, size, "The file is larger than %d MiB.",
                          info.machine == RomInfo::Machine::gba ? 32 : 8);
            break;
        case RomInfo::Problem::type:
            if (const char* mbc = mbc_name(info.type)) {
                std::snprintf(out, size, "%s cartridges cannot run yet.", mbc);
            } else {
                std::snprintf(out, size, "Cartridge type %02X cannot run.", info.type);
            }
            break;
        case RomInfo::Problem::size:
            std::snprintf(out, size, "The file size and header do not match.");
            break;
        case RomInfo::Problem::ram: std::snprintf(out, size, "The header has an invalid RAM size."); break;
    }
}

}  // namespace gbx::app
