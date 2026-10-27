#include "gba_blocks.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <span>

#include "gbx/gba/system.h"
#include "gbx/platform.h"

namespace gbx::test {
namespace {
using namespace gba;

// The BIOS's IRQ vector as GBATEK "BIOS Interrupt handling" describes it:
// R0-R3, R12 and LR saved on the IRQ stack, a call to the handler whose
// address is at 3007FFCh (read at 3FFFFFCh), and the return. The rest of
// the BIOS is zero: the programs start from System::reset() and call none
// of its functions.
constexpr uint32_t kBiosIrq[]{
    0xea000000,  // 18: b 20
    0,           // 1c
    0xe92d500f,  // 20: stmfd sp!, {r0-r3, r12, lr}
    0xe3a00301,  // 24: mov r0, #0x04000000
    0xe28fe000,  // 28: add lr, pc, #0
    0xe510f004,  // 2c: ldr pc, [r0, #-4]
    0xe8bd500f,  // 30: ldmfd sp!, {r0-r3, r12, lr}
    0xe25ef004,  // 34: subs pc, lr, #4
};

// Blocks of several instructions while timer 0 interrupts every 256
// cycles: the IRQs come due inside blocks, which leave at the boundary and
// resume there after the handler. A sum over a loop, then a wait for 80
// IRQs, counted in IWRAM at 3000000h.
constexpr uint32_t kInterrupts[]{
    0xe3a04301,  // 8000000: mov r4, #0x04000000
    0xe28f0068,  // 8000004: add r0, pc, #104 (handler)
    0xe5040004,  // 8000008: str r0, [r4, #-4]
    0xe3a06403,  // 800000c: mov r6, #0x03000000
    0xe3a00000,  // 8000010: mov r0, #0
    0xe5860000,  // 8000014: str r0, [r6]
    0xe2845c02,  // 8000018: add r5, r4, #0x200
    0xe3a00008,  // 800001c: mov r0, #8
    0xe1c500b0,  // 8000020: strh r0, [r5] (IE: timer 0)
    0xe3a00001,  // 8000024: mov r0, #1
    0xe1c500b8,  // 8000028: strh r0, [r5, #8] (IME)
    0xe59f0064,  // 800002c: ldr r0, =0x00c0ff00
    0xe5840100,  // 8000030: str r0, [r4, #0x100] (TM0: reload FF00h, IRQ, start)
    0xe3a01000,  // 8000034: mov r1, #0
    0xe3a02000,  // 8000038: mov r2, #0
    0xe0811002,  // 800003c: loop: add r1, r1, r2
    0xe0213182,  // 8000040: eor r3, r1, r2, lsl #3
    0xe0811123,  // 8000044: add r1, r1, r3, lsr #2
    0xe0030292,  // 8000048: mul r3, r2, r2
    0xe0811003,  // 800004c: add r1, r1, r3
    0xe2822001,  // 8000050: add r2, r2, #1
    0xe35200c8,  // 8000054: cmp r2, #200
    0x1afffff7,  // 8000058: bne loop
    0xe5960000,  // 800005c: wait: ldr r0, [r6]
    0xe3500050,  // 8000060: cmp r0, #80
    0x3afffffc,  // 8000064: blo wait
    0xe3a00000,  // 8000068: mov r0, #0
    0xe5840100,  // 800006c: str r0, [r4, #0x100]
    0xeafffffe,  // 8000070: b .
    0xe3a00301,  // 8000074: handler: mov r0, #0x04000000
    0xe2800c02,  // 8000078: add r0, r0, #0x200
    0xe3a01008,  // 800007c: mov r1, #8
    0xe1c010b2,  // 8000080: strh r1, [r0, #2] (IF: timer 0)
    0xe3a00403,  // 8000084: mov r0, #0x03000000
    0xe5901000,  // 8000088: ldr r1, [r0]
    0xe2811001,  // 800008c: add r1, r1, #1
    0xe5801000,  // 8000090: str r1, [r0]
    0xe12fff1e,  // 8000094: bx lr
    0x00c0ff00,  // 8000098
};

// Code rewritten after it ran, in IWRAM and then in EWRAM, by the CPU and
// by DMA: a routine that returns 1, then 2 after its first word changed;
// one that rewrites its own next instruction, the one after and the one
// three ahead, of which only the last is not yet in the pipeline (ARM7TDMI
// TRM 6.1: an instruction is fetched two before it executes); then, in
// EWRAM, DMA 3 writes a routine's first word; then the first two again at
// the same RAM through a mirror of each, 3009000h and 2041000h (GBATEK
// "GBA Memory Map"). Each result is stored from 3000100h on.
constexpr uint32_t kRewrites[]{
    0xe3a0b403,  // 8000000: mov r11, #0x03000000
    0xe28bbc01,  // 8000004: add r11, r11, #0x100
    0xe3a06403,  // 8000008: mov r6, #0x03000000
    0xe2866a01,  // 800000c: add r6, r6, #0x1000
    0xe28fe000,  // 8000010: add lr, pc, #0
    0xea000014,  // 8000014: b region
    0xe3a06402,  // 8000018: mov r6, #0x02000000
    0xe2866a01,  // 800001c: add r6, r6, #0x1000
    0xe28fe000,  // 8000020: add lr, pc, #0
    0xea000010,  // 8000024: b region
    0xe28f00b4,  // 8000028: add r0, pc, #180 (simple)
    0xe8900006,  // 800002c: ldmia r0, {r1, r2}
    0xe2865c01,  // 8000030: add r5, r6, #0x100
    0xe8850006,  // 8000034: stmia r5, {r1, r2}
    0xe1a0e00f,  // 8000038: mov lr, pc
    0xe12fff15,  // 800003c: bx r5
    0xe48b0004,  // 8000040: str r0, [r11], #4
    0xe3a04301,  // 8000044: mov r4, #0x04000000
    0xe28440d4,  // 8000048: add r4, r4, #0xd4 (DMA3SAD)
    0xe28f0098,  // 800004c: add r0, pc, #152 (nine)
    0xe1a01005,  // 8000050: mov r1, r5
    0xe3a02361,  // 8000054: mov r2, #0x84000001 (one word, now)
    0xe8840007,  // 8000058: stmia r4, {r0, r1, r2}
    0xe1a0e00f,  // 800005c: mov lr, pc
    0xe12fff15,  // 8000060: bx r5
    0xe48b0004,  // 8000064: str r0, [r11], #4
    0xea00002e,  // 8000068: b mirrors
    0xe1a0a00e,  // 800006c: region: mov r10, lr
    0xe28f006c,  // 8000070: add r0, pc, #108 (simple)
    0xe8900006,  // 8000074: ldmia r0, {r1, r2}
    0xe8860006,  // 8000078: stmia r6, {r1, r2}
    0xe1a0e00f,  // 800007c: mov lr, pc
    0xe12fff16,  // 8000080: bx r6
    0xe48b0004,  // 8000084: str r0, [r11], #4
    0xe59f1088,  // 8000088: ldr r1, =0xe3a00002 (mov r0, #2)
    0xe5861000,  // 800008c: str r1, [r6]
    0xe1a0e00f,  // 8000090: mov lr, pc
    0xe12fff16,  // 8000094: bx r6
    0xe48b0004,  // 8000098: str r0, [r11], #4
    0xe2865040,  // 800009c: add r5, r6, #0x40
    0xe28f0048,  // 80000a0: add r0, pc, #72 (inblock)
    0xe3a0100a,  // 80000a4: mov r1, #10
    0xe4902004,  // 80000a8: copy: ldr r2, [r0], #4
    0xe4852004,  // 80000ac: str r2, [r5], #4
    0xe2511001,  // 80000b0: subs r1, r1, #1
    0x1afffffb,  // 80000b4: bne copy
    0xe2865040,  // 80000b8: add r5, r6, #0x40
    0xe59f1058,  // 80000bc: ldr r1, =0xe3a00004 (mov r0, #4)
    0xe59f2058,  // 80000c0: ldr r2, =0xe3a07006 (mov r7, #6)
    0xe59f3058,  // 80000c4: ldr r3, =0xe3a08008 (mov r8, #8)
    0xe1a0e00f,  // 80000c8: mov lr, pc
    0xe12fff15,  // 80000cc: bx r5
    0xe8ab0181,  // 80000d0: stmia r11!, {r0, r7, r8}
    0xe1a0e00f,  // 80000d4: mov lr, pc
    0xe12fff15,  // 80000d8: bx r5
    0xe8ab0181,  // 80000dc: stmia r11!, {r0, r7, r8}
    0xe1a0f00a,  // 80000e0: mov pc, r10
    0xe3a00001,  // 80000e4: simple: mov r0, #1
    0xe12fff1e,  // 80000e8: bx lr
    0xe3a00009,  // 80000ec: nine: mov r0, #9
    0xe50f1004,  // 80000f0: inblock: str r1, [pc, #-4] (the next one)
    0xe3a00003,  // 80000f4: mov r0, #3
    0xe58f2000,  // 80000f8: str r2, [pc] (the one after the next)
    0xe1a09009,  // 80000fc: mov r9, r9
    0xe3a07005,  // 8000100: mov r7, #5
    0xe58f3004,  // 8000104: str r3, [pc, #4] (three ahead)
    0xe1a09009,  // 8000108: mov r9, r9
    0xe1a09009,  // 800010c: mov r9, r9
    0xe3a08007,  // 8000110: mov r8, #7
    0xe12fff1e,  // 8000114: bx lr
    0xe3a00002,  // 8000118
    0xe3a00004,  // 800011c
    0xe3a07006,  // 8000120
    0xe3a08008,  // 8000124
    0xe3a06403,  // 8000128: mirrors: mov r6, #0x03000000
    0xe2866a09,  // 800012c: add r6, r6, #0x9000
    0xe28fe000,  // 8000130: add lr, pc, #0
    0xeaffffcc,  // 8000134: b region
    0xe3a06402,  // 8000138: mov r6, #0x02000000
    0xe2866a41,  // 800013c: add r6, r6, #0x41000
    0xe28fe000,  // 8000140: add lr, pc, #0
    0xeaffffc8,  // 8000144: b region
    0xeafffffe,  // 8000148: b .
};

// Long enough for both programs to reach their idle loops.
constexpr Tick kEnd = 100'000;
// The JIT's second run stops at the first boundary after every this many
// ticks, so that its blocks leave and resume at many of their boundaries.
constexpr Tick kQuantum = 7;
constexpr size_t kRomBytes = RomCache::kPageBytes;
constexpr size_t kEwramBytes = 256 * 1024;

// What a run ends with: the CPU, the time and the memory the programs use.
struct Final {
    Arm7Registers registers{};
    std::array<uint32_t, 2> pipeline{};
    Tick time = 0;
    std::array<uint8_t, 32768> iwram{};
    std::array<uint8_t, 0x400> io{};
    uint8_t* ewram = nullptr;  // kEwramBytes, external memory
};

uint32_t word(const Final& f, uint32_t address) {
    uint32_t value;
    std::memcpy(&value, f.iwram.data() + (address & 0x7fff), 4);
    return value;
}

uint32_t check_interrupts(const Final& f) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 200; ++i) {
        sum += i;
        sum += (sum ^ (i << 3)) >> 2;
        sum += i * i;
    }
    const auto& r = f.registers.r;
    return (r[1] == sum && r[2] == 200) | (word(f, 0x03000000) >= 80) << 1 | (f.pipeline[0] == 0xeafffffe) << 2;
}

// Bits 0-3: IWRAM, EWRAM, then each through its mirror; bit 4: DMA.
uint32_t check_rewrites(const Final& f) {
    constexpr uint32_t kRegion[]{1, 2, 3, 5, 8, 4, 6, 8};
    constexpr uint32_t kResults[]{0x03000100, 0x03000120, 0x03000148, 0x03000168};
    uint32_t mask = 0;
    for (unsigned region = 0; region < 4; ++region) {
        bool good = true;
        for (unsigned i = 0; i < 8; ++i) good &= word(f, kResults[region] + i * 4) == kRegion[i];
        mask |= good << region;
    }
    return mask | (word(f, 0x03000140) == 1 && word(f, 0x03000144) == 9) << 4;
}

class Fixture {
public:
    bool initialize() {
        // External: the JIT's code pool needs the internal RAM, and these
        // programs barely run in the BIOS.
        bios_ = static_cast<uint8_t*>(platform::allocate_external(kBiosBytes));
        rom_ = static_cast<uint8_t*>(platform::allocate_external(kRomBytes));
        for (auto* f : {&interpreter_, &jit_}) {
            f->ewram = static_cast<uint8_t*>(platform::allocate_external(kEwramBytes));
            if (!f->ewram) return false;
        }
        cache_.reset(new (std::nothrow) RomCache);
        if (!bios_ || !rom_ || !cache_) return false;
        std::memset(bios_, 0, kBiosBytes);
        std::memcpy(bios_ + 0x18, kBiosIrq, sizeof(kBiosIrq));
        return true;
    }
    ~Fixture() {
        platform::free_executable(reserved_);
        platform::free_external(bios_);
        platform::free_external(rom_);
        for (auto* f : {&interpreter_, &jit_}) platform::free_external(f->ewram);
    }

    // Bits 0-2: the JIT's CPU, time and memory equal the interpreter's at
    // the end; bit 3: the same when the JIT's run stops every kQuantum
    // ticks; bit 4: the JIT compiled blocks and ran nothing on the
    // interpreter. Bits 5 and up: the program's results, from the
    // interpreter's run.
    uint32_t run(std::span<const uint32_t> program, uint32_t (*check)(const Final&)) {
        std::memset(rom_, 0, kRomBytes);
        std::memcpy(rom_, program.data(), program.size_bytes());
        if (!cache_->attach({rom_, kRomBytes})) return 0;
        uint32_t mask = 0;
        if (!execute(Backend::interpreter, interpreter_)) return 0;
        auto system = start(Backend::jit);
        if (!system) return 0;
        system->run_until(kEnd);
        keep(*system, jit_);
        mask |= compare();
        system->load(std::span<const uint8_t, kBiosBytes>(bios_, kBiosBytes), *cache_);
        for (Tick time = system->now(); time < kEnd;) time = system->run_until(std::min(time + kQuantum, kEnd));
        keep(*system, jit_);
        if (compare() == 7) mask |= 1u << 3;
        const auto& fallbacks = system->jit()->fallbacks();
        if (std::all_of(fallbacks.begin() + 1, fallbacks.end(), [](uint64_t n) { return !n; }) &&
            system->jit()->compiled_blocks())
            mask |= 1u << 4;
        // The code pool is retired before the next system takes it.
        system.reset();
        return mask | check(interpreter_) << 5;
    }

private:
    // The JIT's block before the first system, kept for each system after
    // it, as the product does, so that the systems' internal RAM goes
    // elsewhere.
    std::unique_ptr<System> start(Backend backend) {
        if (backend == Backend::jit && !reserved_.words)
            reserved_ = platform::allocate_executable(gba::Arm7Jit::reserved_bytes({}));
        auto system = std::unique_ptr<System>(new (std::nothrow) System);
        const bool ready = system && system->initialize() &&
                           system->load(std::span<const uint8_t, kBiosBytes>(bios_, kBiosBytes), *cache_) &&
                           system->select_backend(backend, &reserved_);
        if (!ready) return nullptr;
        return system;
    }
    bool execute(Backend backend, Final& out) {
        auto system = start(backend);
        if (!system) return false;
        system->run_until(kEnd);
        keep(*system, out);
        return true;
    }
    static void keep(System& system, Final& out) {
        system.memory.ppu.finish();
        out.registers = arm7_registers(system.cpu);
        out.pipeline = system.cpu.pipeline;
        out.time = system.now();
        std::copy(system.memory.iwram.begin(), system.memory.iwram.end(), out.iwram.begin());
        std::memcpy(out.ewram, system.memory.ewram_bytes().data(), kEwramBytes);
        system.memory.io_snapshot(out.io);
    }
    uint32_t compare() const {
        const auto& a = interpreter_;
        const auto& b = jit_;
        const bool cpu = !std::memcmp(&a.registers, &b.registers, sizeof(a.registers)) && a.pipeline == b.pipeline;
        const bool memory = a.iwram == b.iwram && a.io == b.io && !std::memcmp(a.ewram, b.ewram, kEwramBytes);
        return cpu | (a.time == b.time) << 1 | memory << 2;
    }

    uint8_t* bios_ = nullptr;
    uint8_t* rom_ = nullptr;
    std::unique_ptr<RomCache> cache_;
    platform::ExecutableMemory reserved_{};
    Final interpreter_, jit_;
};

}  // namespace

bool gba_blocks(std::array<uint32_t, 2>& results) {
    // Final holds two IWRAM copies: off the caller's stack.
    auto fixture = std::unique_ptr<Fixture>(new (std::nothrow) Fixture);
    if (!fixture || !fixture->initialize()) return false;
    results[0] = fixture->run(kInterrupts, check_interrupts);
    results[1] = fixture->run(kRewrites, check_rewrites);
    return true;
}

}  // namespace gbx::test
