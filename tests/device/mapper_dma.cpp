#include "mapper_dma.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <memory>
#include <new>

#include "gbx/gb/system.h"

namespace gbx::test {
namespace {
using namespace gb;
bool execute(System& s, unsigned count) {
    while (count) {
        const auto r = s.step_block(kTicksPerSecond, count);
        if (!r.instructions || r.instructions > count || r.fallback != JitFallback::none) return false;
        count -= r.instructions;
    }
    return true;
}
void start(System& s, uint16_t pc) {
    s.reset(); s.memory.clock.reset(); s.cpu = {}; s.cpu.pc = pc; s.cpu.sp = 0xfffe;
}
uint32_t bank_code(System& s) {
    // Pan Docs MBC3: register 2000 selects the physical ROM bank. Returning
    // to a still-resident bank must reuse its translation.
    start(s, 0x4200);
    uint32_t result = 0;
    if (execute(s, 3) && s.cpu.a == 0x11 && s.cpu.b == 1) result |= 1;
    s.memory.poke(0x2000, 2);
    if (execute(s, 3) && s.cpu.a == 0x12 && s.cpu.b == 2) result |= 2;
    const auto compiled = s.jit() ? s.jit()->compiled_blocks() : 0;
    s.memory.poke(0x2000, 1);
    if (execute(s, 3) && s.cpu.a == 0x11 && s.cpu.b == 3 &&
        (!s.jit() || s.jit()->compiled_blocks() == compiled)) result |= 4;
    // CB at 3FFF depends on the newly mapped byte at 4000. With B=80,
    // RLC B yields 01, whereas RRC B yields 40 (SM83 opcode definitions).
    s.cpu.pc = 0x3fff; s.cpu.b = 0x80;
    if (execute(s, 1) && s.cpu.b == 1) result |= 8;
    s.memory.poke(0x2000, 2); s.cpu.pc = 0x3fff; s.cpu.b = 0x80;
    if (execute(s, 1) && s.cpu.b == 0x40) result |= 16;
    // Seven NOPs, the crossing CB, LD A,n and INC D span both ROM windows.
    // The second window changes both an opcode and an immediate operand.
    for (unsigned bank = 1; bank <= 2; ++bank) {
        s.memory.poke(0x2000, bank);
        s.cpu.pc = 0x3ff8; s.cpu.b = 0x80; s.cpu.d = 0;
        const auto time = s.memory.clock.now;
        if (execute(s, 10) && s.cpu.pc == 0x4004 && s.cpu.a == 0x30 + bank &&
            s.cpu.b == (bank == 1 ? 1 : 0x40) && s.cpu.d == 1 &&
            s.memory.clock.now == time + 96) result |= 16u << bank;
    }
    start(s, 0x4200);
    bool linked = true;
    for (unsigned turn = 0; turn < 3; ++turn) {
        const unsigned bank = turn == 1 ? 2 : 1;
        s.memory.poke(0x2000, bank);
        const auto target = s.memory.clock.now + 96;
        const auto r = s.run_until(target);
        linked &= r.end == RunEnd::target && r.instructions == 6 && r.time == target &&
            s.cpu.pc == 0x4200 && s.cpu.a == 0x10 + bank && s.cpu.b == (turn + 1) * 2;
    }
    if (linked) result |= 128;
    // One run switches ROM banks itself: code at 0300 in bank 0 calls 4300
    // in bank 1, which does INC B, and in bank 2, which does INC C, five
    // times each, then jumps to 4400 in bank 1, which selects bank 2 and
    // goes on at 4405 in it: INC E there, not bank 1's INC D (Pan Docs
    // MBC3: the switch takes effect for the next access).
    start(s, 0x0300);
    s.cpu.d = 5;
    const auto end = s.run_until(s.memory.clock.now + 4096);
    if (end.end == RunEnd::target && s.cpu.pc == 0x4406 && s.cpu.b == 5 && s.cpu.c == 5 && s.cpu.d == 0 &&
        s.cpu.e == 1 && s.memory.cartridge.rom_bank[1] == 2) result |= 256;
    return result;
}
uint32_t ram_code(System& s) {
    // Pan Docs MBC3: RAM banks retain independent contents; disabling RAM
    // disconnects it, and writes while disabled cannot alter its contents.
    start(s, 0xa000); s.memory.poke(0, 0x0a);
    constexpr uint8_t code[]{0x04,0x18,0xfd}; // INC B; JR -3.
    for (unsigned bank = 0; bank < 2; ++bank) {
        s.memory.poke(0x4000, bank);
        for (unsigned i = 0; i < sizeof(code); ++i) s.memory.poke(0xa000 + i, code[i]);
        s.memory.poke(0xa000, bank ? 0x0c : 0x04); // INC C or INC B.
    }
    uint32_t result = 0;
    if (execute(s, 2) && s.cpu.c == 1 && !s.cpu.b) result |= 1;
    s.memory.poke(0x4000, 0);
    if (execute(s, 2) && s.cpu.b == 1 && s.cpu.c == 1) result |= 2;
    s.memory.poke(0xa000, 0x14); // Replace already translated INC B with INC D.
    if (execute(s, 2) && s.cpu.d == 1 && s.cpu.b == 1) result |= 4;
    s.memory.poke(0, 0); s.memory.poke(0xa000, 0x1c);
    if (s.memory.read(0xa000) == 0xff) result |= 8;
    s.memory.poke(0, 0x0a);
    if (execute(s, 2) && s.cpu.d == 2 && !s.cpu.e) result |= 16;
    s.memory.poke(0x4000, 8); // Unimplemented RTC must not alias SRAM.
    s.memory.poke(0xa000, 0x1c); s.memory.poke(0x4000, 0);
    if (execute(s, 2) && s.cpu.d == 3 && !s.cpu.e) result |= 32;
    // Rejected images must leave the old cartridge executable, including its
    // RAM ownership, page pointers and controller type. Use different storage
    // so a failed load cannot silently replace the borrowed ROM image either.
    auto rejected = std::unique_ptr<std::array<uint8_t, 32768>>(new (std::nothrow) std::array<uint8_t, 32768>{});
    if (!rejected) return result;
    const auto ram = s.memory.cartridge.ram();
    const auto rom = s.memory.rom();
    const auto page = s.memory.pages[0xa000 >> 7];
    bool preserved = true;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        // Invalid RAM header, then a RAM capacity unsupported by MBC1.
        (*rejected)[0x147] = attempt ? 2 : 5;
        (*rejected)[0x149] = attempt ? 4 : 0xff;
        const auto pc = s.cpu.pc;
        const auto time = s.memory.clock.now;
        preserved &= !s.load_rom(*rejected) && s.cpu.pc == pc && s.memory.clock.now == time &&
            s.memory.rom().data() == rom.data() && s.memory.rom().size() == rom.size() &&
            s.memory.cartridge.ram().data() == ram.data() && s.memory.cartridge.ram().size() == ram.size() &&
            !s.memory.cartridge.nibble_ram() && s.memory.pages[0xa000 >> 7].read == page.read &&
            s.memory.pages[0xa000 >> 7].write == page.write;
        if (!preserved) return result;
        preserved &= execute(s, 2) && s.cpu.d == 4 + attempt && !s.cpu.e;
    }
    if (preserved) result |= 64;
    return result;
}
uint32_t dma_access_limits(System& s) {
    // GB Complete Technical Reference 8.1 separates external and video buses.
    // mooneye oam_dma_start requires external fetches during VRAM DMA.
    uint32_t result = 0;
    for (unsigned video = 0; video < 2; ++video) {
        start(s, 0xff81);
        s.memory.vram.fill(0xaa);
        s.memory.wram.fill(0x55);
        s.memory.poke(0xff80, 0x7e); // LD A,(HL).
        s.memory.poke(0xff81, 0x70); // LD (HL),B.
        s.cpu.b = 0x99;
        s.cpu.set_hl(video ? 0x8000 : 0xc000);
        s.memory.write(0xff46, video ? 0x80 : 0xc0);
        s.memory.clock.advance(32);
        // Only the write is checked on the source bus. Its data must remain
        // unchanged; a read from these uniform source bytes proves nothing.
        if (execute(s, 1) &&
            s.memory.peek(video ? 0x8000 : 0xc000) == (video ? 0xaa : 0x55)) result |= 1u << (video * 3);
        s.cpu.pc = 0xff80; s.cpu.set_hl(video ? 0xc000 : 0x8000);
        if (execute(s, 2) && s.cpu.a == (video ? 0x55 : 0xaa) &&
            s.memory.peek(video ? 0xc000 : 0x8000) == 0x99) result |= 2u << (video * 3);
        s.cpu.pc = 0xff80; s.cpu.set_hl(0xfe00);
        if (execute(s, 2) && s.cpu.a == 0xff && s.memory.oam[0] == (video ? 0xaa : 0x55)) result |= 4u << (video * 3);
    }
    return result;
}
}
// Blocks of LD A,n; two LD (HL),A; JR -6 in bank 0, more blocks than the
// JIT's code pool has slots, see Sm83Jit::initialize().
constexpr unsigned kStoredBlocks = 2304, kStoredSize = 6;
constexpr uint16_t kStoredCode = 0x800;
uint32_t stored_code(System& s, std::array<uint8_t, 65536>& rom) {
    // Code run again after many other blocks gives the same results, and a
    // reloaded ROM runs its own code at the same addresses and banks. With
    // the JIT, the first blocks are evicted, then run from their copies in
    // the second level rather than compiled; a reload drops the copies. Two
    // loops take 2 * (2 + 2 * 2 + 3) M-cycles (Pan Docs instruction timings).
    const auto loops = [&](uint8_t flip) {
        bool good = true;
        for (unsigned i = 0; i < kStoredBlocks; ++i) {
            const auto pc = static_cast<uint16_t>(kStoredCode + i * kStoredSize);
            const auto value = static_cast<uint8_t>(i ^ flip);
            s.cpu.pc = pc; s.cpu.set_hl(0xc000);
            const auto target = s.memory.clock.now + 18 * 8;
            const auto r = s.run_until(target);
            good &= r.end == RunEnd::target && r.time == target && r.instructions == 8 && s.cpu.pc == pc &&
                s.cpu.a == value && s.memory.peek(0xc000) == value;
        }
        return good;
    };
    start(s, kStoredCode);
    auto* jit = s.jit();
    uint32_t result = 0;
    const auto evicted = jit ? jit->evictions() : 0;
    if (loops(0)) result |= 1;
    const auto compiled = jit ? jit->compiled_blocks() : 0, stored = jit ? jit->stored_entries() : 0;
    if (loops(0) && (!jit || (jit->evictions() > evicted && jit->compiled_blocks() == compiled &&
                              jit->stored_entries() > stored))) result |= 2;
    for (unsigned i = 0; i < kStoredBlocks; ++i) rom[kStoredCode + i * kStoredSize + 1] ^= 0x80;
    bool reloaded = s.load_rom(rom);
    start(s, kStoredCode);
    reloaded &= loops(0x80);
    for (unsigned i = 0; i < kStoredBlocks; ++i) rom[kStoredCode + i * kStoredSize + 1] ^= 0x80;
    if (s.load_rom(rom) && reloaded) result |= 4;
    return result;
}
// In bank 3: a ring of blocks, each INC B; JP to the next, and a chain of
// INC C; JP to the next that ends in JR -2. Banks 1 and 2 hold LD A,n; JR -2
// with different n at kBanked. Pan Docs timings: INC r 1 M-cycle, JP nn 4,
// LD r,n 2, JR 3.
constexpr uint16_t kRing = 0x4400, kChain = 0x5000, kBanked = 0x6000;
constexpr unsigned kRingBlocks = 24, kChainBlocks = 48;
// A JIT whose pool holds 8 blocks and whose second level, a copy of the
// gateway and about 8 KiB of blocks, wraps within the chain: blocks move
// between the levels all the time, and new copies overwrite dropped ones.
constexpr Sm83Jit::Capacity kSmall{4096, 8, 16 * 1024, 1024};
uint32_t two_levels(System& s) {
    // Rounds of 72 ring blocks that alternately stop after an INC B and
    // after the JP, so that the next one resumes inside a block.
    uint32_t cycles = 0;  // In the ring, from its first block.
    const auto ring = [&](unsigned rounds) {
        s.memory.poke(0x2000, 3);
        s.cpu.pc = static_cast<uint16_t>(kRing + cycles / 5 % kRingBlocks * 8 + (cycles % 5 ? 1 : 0));
        bool good = true;
        for (unsigned i = 0; i < rounds; ++i) {
            const unsigned length = cycles % 5 ? 364 : 361;
            const auto target = s.memory.clock.now + length * 8;
            const auto r = s.run_until(target);
            cycles += length;
            const unsigned blocks = cycles / 5, inside = cycles % 5 ? 1 : 0;
            good &= r.end == RunEnd::target && r.time == target && r.instructions == 145 &&
                s.cpu.pc == kRing + blocks % kRingBlocks * 8 + inside && s.cpu.b == static_cast<uint8_t>(blocks + inside);
        }
        return good;
    };
    start(s, kRing);
    auto* jit = s.jit();
    const auto evicted = jit ? jit->evictions() : 0, stored = jit ? jit->stored_entries() : 0;
    const auto restored = jit ? jit->restored_blocks() : 0, dropped = jit ? jit->store_drops() : 0;
    uint32_t result = 0;
    if (ring(40)) result |= 1;
    // New blocks push the oldest copies, the ring's, out of the second level.
    s.cpu.pc = kChain; s.cpu.c = 0;
    const auto target = s.memory.clock.now + (kChainBlocks * 5 + 3) * 8;
    const auto r = s.run_until(target);
    if (r.end == RunEnd::target && r.time == target && r.instructions == kChainBlocks * 2 + 1 &&
        s.cpu.pc == kChain + kChainBlocks * 4 && s.cpu.c == kChainBlocks) result |= 2;
    if (ring(20)) result |= 4;
    // The same address in two banks, with the ring in between to evict it.
    bool banked = true;
    for (unsigned i = 0; i < 6; ++i) {
        const unsigned bank = 1 + i % 2;
        s.memory.poke(0x2000, bank);
        s.cpu.pc = kBanked;
        const auto until = s.memory.clock.now + 5 * 8;
        const auto run = s.run_until(until);
        banked &= run.end == RunEnd::target && run.time == until && run.instructions == 2 &&
            s.cpu.pc == kBanked + 2 && s.cpu.a == 0x40 + bank && ring(2);
    }
    if (banked) result |= 8;
    // The JIT moved blocks both ways and dropped copies.
    if (!jit || (jit->evictions() > evicted && jit->stored_entries() > stored &&
                 jit->restored_blocks() > restored && jit->store_drops() > dropped)) result |= 16;
    return result;
}
bool mapper_dma(std::array<uint32_t, 10>& results) {
    auto rom = std::unique_ptr<std::array<uint8_t, 65536>>(new (std::nothrow) std::array<uint8_t, 65536>{});
    auto s = std::unique_ptr<System>(new (std::nothrow) System);
    if (!rom || !s) return false;
    (*rom)[0x147] = 0x13; (*rom)[0x148] = 1; (*rom)[0x149] = 3;
    (*rom)[0x3fff] = 0xcb; (*rom)[0x4000] = 0; (*rom)[0x8000] = 8;
    for (unsigned bank = 1; bank <= 2; ++bank) {
        const auto offset = bank * 16384;
        (*rom)[offset + 1] = 0x3e; (*rom)[offset + 2] = 0x30 + bank; (*rom)[offset + 3] = 0x14;
    }
    for (unsigned bank = 0; bank < 4; ++bank) {
        const std::array<uint8_t, 5> code{0x3e,static_cast<uint8_t>(0x10 + bank),0x04,0x18,0xfb};
        std::copy(code.begin(), code.end(), rom->begin() + bank * 16384 + 0x200);
    }
    for (unsigned i = 0; i < kStoredBlocks; ++i) {
        const std::array<uint8_t, kStoredSize> code{0x3e,static_cast<uint8_t>(i),0x77,0x77,0x18,0xfa};
        std::copy(code.begin(), code.end(), rom->begin() + kStoredCode + i * kStoredSize);
    }
    const auto place = [&](unsigned bank, uint16_t address, std::initializer_list<uint8_t> code) {
        std::copy(code.begin(), code.end(), rom->begin() + bank * 16384 + (address - 0x4000));
    };
    for (unsigned i = 0; i < kRingBlocks; ++i) {
        const auto next = static_cast<uint16_t>(kRing + (i + 1) % kRingBlocks * 8);
        place(3, static_cast<uint16_t>(kRing + i * 8), {0x04, 0xc3, static_cast<uint8_t>(next), static_cast<uint8_t>(next >> 8)});
    }
    for (unsigned i = 0; i < kChainBlocks; ++i) {
        const auto next = static_cast<uint16_t>(kChain + (i + 1) * 4);
        place(3, static_cast<uint16_t>(kChain + i * 4), {0x0c, 0xc3, static_cast<uint8_t>(next), static_cast<uint8_t>(next >> 8)});
    }
    place(3, kChain + kChainBlocks * 4, {0x18, 0xfe});
    for (unsigned bank = 1; bank <= 2; ++bank) place(bank, kBanked, {0x3e, static_cast<uint8_t>(0x40 + bank), 0x18, 0xfe});
    // Bank switches inside a run, see bank_code(): LD A,1; LD (2000),A;
    // CALL 4300; LD A,2; LD (2000),A; CALL 4300; DEC D; JR NZ,0300; LD A,1;
    // LD (2000),A; JP 4400.
    std::copy_n(std::initializer_list<uint8_t>{0x3e, 1, 0xea, 0x00, 0x20, 0xcd, 0x00, 0x43, 0x3e, 2, 0xea, 0x00, 0x20,
                                                0xcd, 0x00, 0x43, 0x15, 0x20, 0xed, 0x3e, 1, 0xea, 0x00, 0x20, 0xc3,
                                                0x00, 0x44}.begin(), 27, rom->begin() + 0x300);
    for (unsigned bank = 1; bank <= 2; ++bank) {
        // INC B or INC C; RET. LD A,2; LD (2000),A; INC D or INC E; JR $.
        place(bank, 0x4300, {static_cast<uint8_t>(bank == 1 ? 0x04 : 0x0c), 0xc9});
        place(bank, 0x4400, {0x3e, 2, 0xea, 0x00, 0x20, static_cast<uint8_t>(bank == 1 ? 0x14 : 0x1c), 0x18, 0xfe});
    }
    if (!s->load_rom(*rom)) return false;
    for (unsigned native = 0; native < 2; ++native) {
        if (!s->select_backend(native ? Backend::jit : Backend::interpreter)) return false;
        results[native] = bank_code(*s);
        results[2 + native] = ram_code(*s);
        results[4 + native] = dma_access_limits(*s);
        results[6 + native] = stored_code(*s, *rom);
        if (s->jit() && std::any_of(s->jit()->fallbacks().begin(), s->jit()->fallbacks().end(),
                                  [](uint64_t count) { return count != 0; })) return false;
    }
    s.reset();
    s.reset(new (std::nothrow) System);
    if (!s) return false;
    s->set_jit_capacity(kSmall);
    if (!s->load_rom(*rom)) return false;
    for (unsigned native = 0; native < 2; ++native) {
        if (!s->select_backend(native ? Backend::jit : Backend::interpreter)) return false;
        results[8 + native] = two_levels(*s);
        if (s->jit() && std::any_of(s->jit()->fallbacks().begin(), s->jit()->fallbacks().end(),
                                  [](uint64_t count) { return count != 0; })) return false;
    }
    return true;
}
}  // namespace gbx::test
