// Boots a GBA game on the interpreter and plays a key script, writing PNG
// pictures at chosen frames, for finding the key presses that reach a scene:
//
//     gbx_gba_script --bios FILE --rom FILE --script FILE --out DIR
//                    [--shot N]... [--every N] [--after N]
//
// The script holds one event per line, "<frame> <keys>", from that frame on
// until the next event; keys are "none" or names from A B SELECT START RIGHT
// LEFT UP DOWN R L joined by '+'. "rom <name>" is ignored here, and
// "end <N>" ends the script before frame N. Frame 0 is the first frame after
// power-on, which skips the BIOS's start-up (System::reset()). The tool
// writes DIR/f<N>.png after frame N for each --shot and every --every frames,
// DIR/end.png after the script and DIR/window_end.png --after frames later
// (600 by default) with the last keys still held.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gbx/gba/system.h"

namespace {

std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> bytes;
    if (FILE* f = std::fopen(path, "rb")) {
        std::fseek(f, 0, SEEK_END);
        bytes.resize(static_cast<size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        std::fclose(f);
    }
    return bytes;
}

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(v >> s));
}

void chunk(std::vector<uint8_t>& out, const char* kind, const std::vector<uint8_t>& body) {
    put32(out, static_cast<uint32_t>(body.size()));
    const size_t start = out.size();
    out.insert(out.end(), kind, kind + 4);
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

// An RGB PNG of the BGR555 frame, with stored (uncompressed) deflate blocks.
bool write_png(const std::string& path, std::span<const uint16_t> pixels) {
    using gbx::gba::kScreenHeight;
    using gbx::gba::kScreenWidth;
    std::vector<uint8_t> raw;
    for (unsigned y = 0; y < kScreenHeight; ++y) {
        raw.push_back(0);
        for (unsigned x = 0; x < kScreenWidth; ++x) {
            const uint16_t p = pixels[y * kScreenWidth + x];
            for (const unsigned c : {p & 31u, (p >> 5) & 31u, (p >> 10) & 31u})
                raw.push_back(static_cast<uint8_t>(c << 3 | c >> 2));
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size();) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<ptrdiff_t>(pos), raw.begin() + static_cast<ptrdiff_t>(pos + n));
        pos += n;
    }
    uint32_t a = 1, b = 0;
    for (const uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, b << 16 | a);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> header;
    put32(header, kScreenWidth);
    put32(header, kScreenHeight);
    header.insert(header.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

struct Event {
    unsigned frame;
    uint16_t keys;
};

// KEYINPUT bit numbering, set bit = pressed.
bool parse_keys(const std::string& text, uint16_t& keys) {
    static const char* const names[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L"};
    keys = 0;
    if (text == "none") return true;
    std::stringstream parts(text);
    for (std::string name; std::getline(parts, name, '+');) {
        unsigned bit = 0;
        while (bit < 10 && name != names[bit]) ++bit;
        if (bit == 10) return false;
        keys |= static_cast<uint16_t>(1u << bit);
    }
    return true;
}

bool parse_script(const char* path, std::vector<Event>& events, unsigned& end) {
    const auto bytes = read_file(path);
    std::stringstream lines(std::string(bytes.begin(), bytes.end()));
    end = 0;
    for (std::string line; std::getline(lines, line);) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::stringstream words(line);
        std::string first, second;
        if (!(words >> first)) continue;
        if (first == "rom") continue;
        if (!(words >> second)) return false;
        if (first == "end") {
            end = static_cast<unsigned>(std::strtoul(second.c_str(), nullptr, 10));
            continue;
        }
        Event e{static_cast<unsigned>(std::strtoul(first.c_str(), nullptr, 10)), 0};
        if (!parse_keys(second, e.keys) || (!events.empty() && e.frame <= events.back().frame)) return false;
        events.push_back(e);
    }
    return end > 0;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace gbx::gba;
    const char *bios_path = nullptr, *rom_path = nullptr, *script_path = nullptr;
    std::string out = ".";
    std::set<unsigned> shots;
    unsigned every = 0, after = 600;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--bios") bios_path = argv[++i];
        else if (arg == "--rom") rom_path = argv[++i];
        else if (arg == "--script") script_path = argv[++i];
        else if (arg == "--out") out = argv[++i];
        else if (arg == "--shot") shots.insert(static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10)));
        else if (arg == "--every") every = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (arg == "--after") after = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    }
    std::vector<Event> events;
    unsigned end = 0;
    if (!bios_path || !rom_path || !script_path) {
        std::fprintf(stderr,
                     "usage: %s --bios FILE --rom FILE --script FILE --out DIR [--shot N]... [--every N] [--after N]\n",
                     argv[0]);
        return 2;
    }
    if (!parse_script(script_path, events, end)) {
        std::fprintf(stderr, "cannot read the script\n");
        return 2;
    }
    const auto bios = read_file(bios_path);
    RomCache cache;
    auto system = std::unique_ptr<System>(new (std::nothrow) System);
    if (bios.size() != kBiosBytes || !cache.open(rom_path, 64u << 20) || !system || !system->initialize() ||
        !system->load(std::span<const uint8_t, kBiosBytes>(bios.data(), kBiosBytes), cache)) {
        std::fprintf(stderr, "cannot start the game\n");
        return 1;
    }
    size_t next = 0;
    uint16_t keys = 0;
    for (unsigned frame = 0; frame < end + after; ++frame) {
        while (next < events.size() && events[next].frame <= frame) keys = events[next++].keys;
        system->set_buttons(keys);
        system->run_frame();
        std::string name;
        if (shots.count(frame) || (every && frame % every == 0)) {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "/f%05u.png", frame);
            name = buffer;
        }
        if (frame + 1 == end) name = "/end.png";
        if (frame + 1 == end + after) name = "/window_end.png";
        if (!name.empty() && !write_png(out + name, system->frame().pixels)) {
            std::fprintf(stderr, "cannot write %s%s\n", out.c_str(), name.c_str());
            return 1;
        }
    }
    return 0;
}
