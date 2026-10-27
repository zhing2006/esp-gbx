// Runs a GBA game from an instant save on the interpreter, one instruction
// at a time, and counts the control flow the ARM7 JIT has to follow:
// instructions, taken branches with a target in the instruction (static)
// and those with a computed one (dynamic), by region and site; and the data
// accesses by kind and by the region of their address, the base register's
// for block transfers:
//
//     gbx_gba_flow --bios FILE --state FILE [--frames N] ROM

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <set>
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

constexpr size_t kStateOffset = 64 + gbx::gba::kFramePixels * 2;

// The kinds of data access counted, and the address of the instruction's
// access, before it runs; false when it makes none.
enum Access : unsigned { kLiteral, kLoad, kStore, kBlockLoad, kBlockStore, kAccessKinds };
constexpr const char* kAccessNames[kAccessKinds] = {"PC-relative loads", "loads", "stores", "block loads",
                                                    "block stores"};
bool access_of(uint32_t opcode, bool thumb, uint32_t pc, const std::array<uint32_t, 16>& r, Access& kind,
               uint32_t& address, unsigned& words) {
    words = 1;
    if (thumb) {
        const uint32_t rb = (opcode >> 3) & 7, imm = (opcode >> 6) & 31;
        switch (opcode >> 11) {
        case 0x09: kind = kLiteral; address = ((pc + 4) & ~2u) + (opcode & 0xff) * 4; return true;
        case 0x0a: case 0x0b:
            kind = ((opcode >> 9) & 7) < 3 ? kStore : kLoad;
            address = r[rb] + r[(opcode >> 6) & 7];
            return true;
        case 0x0c: case 0x0d: case 0x0e: case 0x0f:
            kind = opcode & 0x800 ? kLoad : kStore;
            address = r[rb] + imm * (opcode & 0x1000 ? 1 : 4);
            return true;
        case 0x10: case 0x11: kind = opcode & 0x800 ? kLoad : kStore; address = r[rb] + imm * 2; return true;
        case 0x12: case 0x13: kind = opcode & 0x800 ? kLoad : kStore; address = r[13] + (opcode & 0xff) * 4; return true;
        case 0x16: case 0x17:
            if (((opcode >> 9) & 3) != 2) return false;
            kind = opcode & 0x800 ? kBlockLoad : kBlockStore;
            address = r[13];
            words = static_cast<unsigned>(std::popcount(opcode & 0x1ff));
            return true;
        case 0x18: case 0x19:
            kind = opcode & 0x800 ? kBlockLoad : kBlockStore;
            address = r[(opcode >> 8) & 7];
            words = static_cast<unsigned>(std::popcount(opcode & 0xff));
            return true;
        default: return false;
        }
    }
    const uint32_t rn = (opcode >> 16) & 15, base = rn == 15 ? pc + 8 : r[rn];
    if ((opcode & 0x0c000000) == 0x04000000) {
        kind = rn == 15 && (opcode & 0x00100000) ? kLiteral : opcode & 0x00100000 ? kLoad : kStore;
        address = base;
        return true;
    }
    if ((opcode & 0x0e000090) == 0x00000090 && (opcode & 0x60)) {
        kind = rn == 15 && (opcode & 0x00100000) ? kLiteral : opcode & 0x00100000 ? kLoad : kStore;
        address = base;
        return true;
    }
    if ((opcode & 0x0e000000) == 0x08000000) {
        kind = opcode & 0x00100000 ? kBlockLoad : kBlockStore;
        address = base;
        words = static_cast<unsigned>(std::popcount(opcode & 0xffff));
        return true;
    }
    return false;
}

// Whether the instruction's branch target is part of it: B, BL, and in
// Thumb B, conditional B and BL.
bool static_branch(uint32_t opcode, bool thumb) {
    if (!thumb) return ((opcode >> 25) & 7) == 5;
    return (opcode >> 12) == 0xd || (opcode >> 11) == 0x1c || (opcode >> 11) == 0x1e || (opcode >> 11) == 0x1f;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace gbx::gba;
    const char *bios_path = nullptr, *state_path = nullptr, *rom_path = nullptr;
    unsigned frames = 60;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--bios" && i + 1 < argc) bios_path = argv[++i];
        else if (arg == "--state" && i + 1 < argc) state_path = argv[++i];
        else if (arg == "--frames" && i + 1 < argc) frames = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else rom_path = argv[i];
    }
    if (!bios_path || !state_path || !rom_path) {
        std::fprintf(stderr, "usage: %s --bios FILE --state FILE [--frames N] ROM\n", argv[0]);
        return 2;
    }
    const auto bios = read_file(bios_path);
    const auto slot = read_file(state_path);
    if (bios.size() != kBiosBytes || slot.size() < kStateOffset + 4) return 1;
    uint32_t state_bytes;
    std::memcpy(&state_bytes, slot.data() + 52, 4);
    RomCache cache;
    auto system = std::unique_ptr<System>(new (std::nothrow) System);
    Snapshot snapshot;
    if (!cache.open(rom_path, 64u << 20) || !system || !system->initialize() ||
        !system->load(std::span<const uint8_t, kBiosBytes>(bios.data(), kBiosBytes), cache) || !snapshot.allocate() ||
        snapshot.decode({slot.data() + kStateOffset, state_bytes}) != Snapshot::Status::ok ||
        !system->restore(snapshot)) {
        std::fprintf(stderr, "cannot start the game from the slot\n");
        return 1;
    }
    Memory& m = system->memory;
    struct Site {
        uint64_t count = 0;
        std::set<uint32_t> targets;
        uint32_t opcode = 0;
        bool thumb = false;
    };
    std::map<uint32_t, Site> dynamic_sites;
    // Instructions by address, with HOT set in the environment.
    std::map<uint32_t, std::pair<uint64_t, uint32_t>> hot;
    const bool count_hot = std::getenv("HOT");
    std::array<uint64_t, 16> instructions{}, statics{}, dynamics{}, backward{};
    std::array<std::array<uint64_t, 16>, kAccessKinds> accesses{}, words{};
    uint64_t exceptions = 0, total = 0;
    const gbx::Tick end = system->now() + gbx::Tick{frames} * kFrameCycles;
    while (system->now() < end) {
        const bool thumb = system->cpu.cpsr & 0x20;
        const uint32_t length = thumb ? 2 : 4;
        const uint32_t pc = system->cpu.r[15] - 2 * length;
        const uint32_t opcode = m.peek(pc, thumb ? 1 : 2);
        if (std::getenv("WATCH") && pc == std::strtoul(std::getenv("WATCH"), nullptr, 16) && total % 1000 < 3) {
            const auto& r = system->cpu.r;
            std::printf("at %08x: r0 %08x r1 %08x r2 %08x r3 %08x r4 %08x r5 %08x r6 %08x r11 %08x r13 %08x\n", pc,
                        r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[11], r[13]);
        }
        Access kind;
        uint32_t address;
        unsigned count;
        if (!system->memory.halted && access_of(opcode, thumb, pc, system->cpu.r, kind, address, count)) {
            ++accesses[kind][(address >> 24) & 15];
            words[kind][(address >> 24) & 15] += count;
        }
        // A step in HALT runs no instruction.
        const bool halted = system->memory.halted;
        const gbx::Tick before = system->now();
        system->run_until(system->now() + 1);
        if (halted) continue;
        if (count_hot) {
            auto& entry = hot[pc | (thumb ? 1 : 0)];
            entry.first += system->now() - before;
            entry.second = opcode;
        }
        const bool now_thumb = system->cpu.cpsr & 0x20;
        const uint32_t next = system->cpu.r[15] - 2 * (now_thumb ? 2 : 4);
        const unsigned region = (pc >> 24) & 15;
        ++instructions[region];
        ++total;
        if (next == pc + length || next == pc) continue;
        if (next == 0x18 || next == 0x08 || next == 0x128) {
            ++exceptions;
            continue;
        }
        if (static_branch(opcode, thumb)) {
            ++statics[region];
            if (next < pc && pc - next < 256) ++backward[region];
        } else {
            ++dynamics[region];
            auto& site = dynamic_sites[pc];
            ++site.count;
            if (site.targets.size() < 64) site.targets.insert(next);
            site.opcode = opcode;
            site.thumb = thumb;
        }
    }
    std::printf("%llu instructions in %u frames, %llu exceptions\n", static_cast<unsigned long long>(total), frames,
                static_cast<unsigned long long>(exceptions));
    for (unsigned r = 0; r < 16; ++r) {
        if (!instructions[r]) continue;
        std::printf("region %x: %8.0f instructions, %7.0f static (%7.0f short backward), %7.0f dynamic branches a frame\n", r,
                    double(instructions[r]) / frames, double(statics[r]) / frames, double(backward[r]) / frames,
                    double(dynamics[r]) / frames);
    }
    for (unsigned k = 0; k < kAccessKinds; ++k) {
        std::printf("%s a frame by region:", kAccessNames[k]);
        for (unsigned r = 0; r < 16; ++r) {
            if (!accesses[k][r]) continue;
            std::printf(" %x %.0f", r, double(accesses[k][r]) / frames);
            if (k >= kBlockLoad) std::printf(" (%.1f words)", double(words[k][r]) / double(accesses[k][r]));
        }
        std::printf("\n");
    }
    std::vector<std::pair<uint32_t, const Site*>> sorted;
    for (const auto& [pc, site] : dynamic_sites) sorted.push_back({pc, &site});
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second->count > b.second->count; });
    for (size_t i = 0; i < sorted.size() && i < 25; ++i) {
        const auto& [pc, site] = sorted[i];
        std::printf("%08x %s %08x %8.0f a frame, %zu%s targets\n", pc, site->thumb ? "T" : "A", site->opcode,
                    double(site->count) / frames, site->targets.size(), site->targets.size() >= 64 ? "+" : "");
    }
    if (count_hot) {
        std::vector<std::pair<uint32_t, std::pair<uint64_t, uint32_t>>> by_time(hot.begin(), hot.end());
        std::sort(by_time.begin(), by_time.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
        const double cycles = double(frames) * kFrameCycles;
        for (size_t i = 0; i < by_time.size() && i < 60; ++i)
            std::printf("%08x %08x %5.2f%% of the time\n", by_time[i].first, by_time[i].second.second,
                        100.0 * double(by_time[i].second.first) / cycles);
    }
    return 0;
}
