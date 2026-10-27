// Compiles the code a GBA game runs from an instant save with the ARM7
// JIT's compiler and reports the size of the native code, for work on the
// code's density:
//
//     gbx_jit_size --bios FILE --state FILE [--frames N] [--dump DIR] ROM
//
// The interpreter runs the game and samples where it is every 97 cycles; a
// block is compiled at each place it stopped, as the JIT would on a miss
// there. The calls into the gateway take fixed-size slots, so the stubs'
// addresses here, which are made up, do not change the size. --dump writes
// the hottest blocks' code for objdump -b binary -m riscv:rv32.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "gbx/gba/system.h"
#include "gba/jit/arm7_compiler.h"

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

}  // namespace

int main(int argc, char** argv) {
    using namespace gbx::gba;
    const char *bios_path = nullptr, *state_path = nullptr, *rom_path = nullptr, *dump = nullptr;
    unsigned frames = 60;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--bios" && i + 1 < argc) bios_path = argv[++i];
        else if (arg == "--state" && i + 1 < argc) state_path = argv[++i];
        else if (arg == "--frames" && i + 1 < argc) frames = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (arg == "--dump" && i + 1 < argc) dump = argv[++i];
        else rom_path = argv[i];
    }
    if (!bios_path || !state_path || !rom_path) {
        std::fprintf(stderr, "usage: %s --bios FILE --state FILE [--frames N] [--dump DIR] ROM\n", argv[0]);
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
    // Where the interpreter stopped: R15 and CPSR, as a block's key.
    std::map<std::pair<uint32_t, uint32_t>, unsigned> samples;
    const gbx::Tick end = system->now() + gbx::Tick{frames} * kFrameCycles;
    while (system->now() < end) {
        system->run_until(system->now() + 97);
        ++samples[{system->cpu.r[15], system->cpu.cpsr & 0x3f}];
    }
    // AT=<R15 in hex>: that place's block first, as if it had the most
    // samples, for dumping a block that starts where the JIT starts it.
    if (const char* at = std::getenv("AT")) {
        const auto r15 = static_cast<uint32_t>(std::strtoul(at, nullptr, 16));
        uint32_t cpsr = 0x1f;
        for (const auto& [key, count] : samples)
            if (key.first >> 24 == r15 >> 24) cpsr = key.second;
        samples[{r15, cpsr}] += 1u << 30;
    }
    Memory& m = system->memory;
    if (dump) {
        if (FILE* f = std::fopen((std::string(dump) + "/iwram.bin").c_str(), "wb")) {
            std::fwrite(m.iwram.data(), 1, m.iwram.size(), f);
            std::fclose(f);
        }
    }
    Arm7Jit::Stubs stubs{};
    auto* fake = reinterpret_cast<uintptr_t*>(&stubs);
    for (size_t i = 0; i < sizeof stubs / sizeof(uintptr_t); ++i) fake[i] = 0x40000000u + i * 64;
    const Arm7CodeSource code{&m, [](const void* context, uint32_t address, bool thumb) {
        return const_cast<Memory*>(static_cast<const Memory*>(context))->peek(address, thumb ? 1 : 2);
    }};
    std::vector<uint32_t> hot(2048), cold(1024), output(3072);
    std::vector<gbx::jit::Relocation> relocations(512);
    struct Block {
        uint32_t r15, cpsr;
        unsigned samples, instructions;
        size_t bytes;
        std::vector<uint32_t> words;
    };
    std::vector<Block> blocks;
    std::set<uint32_t> covered;  // Instructions the blocks cover.
    uint64_t weighted_bytes = 0, weighted_instructions = 0;
    for (const auto& [key, count] : samples) {
        const uint32_t r15 = key.first, cpsr = key.second;
        const unsigned region = r15 >> 24;
        arm7_compiler::MappedTarget target;
        target.region = static_cast<uint8_t>(region);
        target.ewram_cycles = m.cycles_n[2];
        target.code_n = m.cycles_n[region & 15];
        target.code_s = m.cycles_s[region & 15];
        target.halfword_s = m.cycles_s[region & 15][1];
        target.prefetch = m.prefetch_enabled;
        auto* compiler = new arm7_compiler::Compiler(hot, cold, stubs, code, r15, cpsr, &target);
        size_t calls = 0;
        const auto words = compiler->compile(Arm7Jit::kMaxBlock, output, relocations, calls);
        const unsigned instructions = compiler->instructions();
        const uint32_t code_end = compiler->code_end();
        delete compiler;
        const uint32_t length = cpsr & 0x20 ? 2 : 4;
        for (uint32_t a = r15 - 2 * length; a < code_end; a += length) covered.insert(a);
        if (words.empty() || !instructions) continue;
        blocks.push_back({r15, cpsr, count, instructions, words.size() * 4, {words.begin(), words.end()}});
        weighted_bytes += uint64_t{count} * words.size() * 4;
        weighted_instructions += uint64_t{count} * instructions;
    }
    std::sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) { return a.samples > b.samples; });
    size_t bytes = 0, instructions = 0;
    for (const auto& b : blocks) {
        bytes += b.bytes;
        instructions += b.instructions;
    }
    std::printf("%zu blocks, %zu instructions, %zu bytes: %.1f bytes an instruction, %.1f weighted by samples\n",
                blocks.size(), instructions, bytes, double(bytes) / instructions,
                double(weighted_bytes) / weighted_instructions);
    std::printf("%zu distinct instructions\n", covered.size());
    std::map<unsigned, std::array<size_t, 3>> regions;  // blocks, instructions, bytes
    for (const auto& b : blocks) {
        auto& r = regions[b.r15 >> 24];
        ++r[0];
        r[1] += b.instructions;
        r[2] += b.bytes;
    }
    for (const auto& [region, r] : regions)
        std::printf("region %x: %zu blocks, %zu instructions, %zu bytes\n", region, r[0], r[1], r[2]);
    for (size_t i = 0; i < blocks.size() && i < 20; ++i) {
        const auto& b = blocks[i];
        std::printf("%08x %s %5u samples %3u instructions %5zu bytes\n", b.r15, b.cpsr & 0x20 ? "T" : "A", b.samples,
                    b.instructions, b.bytes);
        if (dump && i < 5) {
            const std::string path = std::string(dump) + "/block" + std::to_string(i) + ".bin";
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(b.words.data(), 4, b.words.size(), f);
                std::fclose(f);
            }
        }
    }
    return 0;
}
