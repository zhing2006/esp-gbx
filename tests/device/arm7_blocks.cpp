#include "arm7_blocks.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <numeric>
#include <span>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gbx/gba/arm7.h"
#include "gbx/gba/bus.h"
#include "gbx/gba/jit/arm7_jit.h"

namespace gbx::test {
namespace {
using namespace gba;

// Each program runs from 0x100 in 64 KiB of RAM with these vectors: UND
// reads CPSR into R5 and returns with MOVS PC, LR; SWI reads SPSR into R4,
// stores User mode's R8-R14 below its stack with STMDB^ and returns with
// LDMFD SP!, {R0-R3, PC}^.
constexpr uint32_t kVectors[]{
    0xeafffffe,  // 00: b .
    0xea000005,  // 04: b und_handler
    0xea000006,  // 08: b swi_handler
    0, 0, 0, 0, 0,
    0xe10f5000,  // 20: mrs r5, cpsr
    0xe1b0f00e,  // 24: movs pc, lr
    0xe14f4000,  // 28: mrs r4, spsr
    0xe92d400f,  // 2c: stmfd sp!, {r0-r3, lr}
    0xe3a00099,  // 30: mov r0, #0x99
    0xe94d7f00,  // 34: stmdb sp, {r8-r14}^
    0xe8fd800f,  // 38: ldmfd sp!, {r0-r3, pc}^
};

// A loop and the flags of ARM data processing: conditions on the flags of
// CMP, SUBS, TST and CMN, carries in and out, RRX, a shift by a register,
// and multiplies with flags.
constexpr uint32_t kFlags[]{
    0xe3a00000,  // 100: mov r0, #0
    0xe3a0100a,  // 104: mov r1, #10
    0xe0800001,  // 108: add r0, r0, r1
    0xe2511001,  // 10c: subs r1, r1, #1
    0x1afffffc,  // 110: bne 108
    0xe3500037,  // 114: cmp r0, #55
    0x03a02001,  // 118: moveq r2, #1
    0x13a02002,  // 11c: movne r2, #2
    0xe3e03000,  // 120: mvn r3, #0
    0xe2934002,  // 124: adds r4, r3, #2
    0xe2a05000,  // 128: adc r5, r0, #0
    0xe2506038,  // 12c: subs r6, r0, #56
    0xe2c07000,  // 130: sbc r7, r0, #0
    0xe1b080a3,  // 134: movs r8, r3, lsr #1
    0xe1a09060,  // 138: mov r9, r0, rrx
    0xe3a0a003,  // 13c: mov r10, #3
    0xe1b0ba13,  // 140: movs r11, r3, lsl r10
    0xe2f0a064,  // 144: rscs r10, r0, #100
    0xe3100102,  // 148: tst r0, #0x80000000
    0xc080c000,  // 14c: addgt r12, r0, r0
    0xd280c001,  // 150: addle r12, r0, #1
    0xe3a01102,  // 154: mov r1, #0x80000000
    0xe2511001,  // 158: subs r1, r1, #1
    0x62822004,  // 15c: addvs r2, r2, #4
    0xe0030090,  // 160: mul r3, r0, r0
    0xe0341590,  // 164: mlas r4, r0, r5, r1
    0x42822008,  // 168: addmi r2, r2, #8
    0xe0865393,  // 16c: umull r5, r6, r3, r3
    0xe0f87191,  // 170: smlals r7, r8, r1, r1
    0xe03bb26b,  // 174: eors r11, r11, r11, ror #4
    0x43cbb0ff,  // 178: bicmi r11, r11, #0xff
    0xe3710001,  // 17c: cmn r1, #1
    0x638ccc01,  // 180: orrvs r12, r12, #0x100
    0xeafffffe,  // 184: b .
};

// ARM to Thumb and back with BX, a Thumb loop, BL and BX LR, PUSH and POP,
// high registers, and the ARM loads and stores: write-back, misaligned and
// signed loads, SWP, LDM and STM.
constexpr uint32_t kStates[]{
    0xe3a0d902,  // 100: mov sp, #0x8000
    0xe28f0054,  // 104: adr r0, 160
    0xe3800001,  // 108: orr r0, r0, #1
    0xe3a04007,  // 10c: mov r4, #7
    0xe92d4010,  // 110: stmfd sp!, {r4, lr}
    0xe12fff10,  // 114: bx r0
    0xe8bd0030,  // 118: ldmfd sp!, {r4, r5}
    0xe59f1038,  // 11c: ldr r1, [pc, #0x38]
    0xe52d1004,  // 120: str r1, [sp, #-4]!
    0xe5dd6001,  // 124: ldrb r6, [sp, #1]
    0xe1dd70b2,  // 128: ldrh r7, [sp, #2]
    0xe1dd80d3,  // 12c: ldrsb r8, [sp, #3]
    0xe1dd90f0,  // 130: ldrsh r9, [sp]
    0xe59da001,  // 134: ldr r10, [sp, #1]
    0xe10db094,  // 138: swp r11, r4, [sp]
    0xe49dc004,  // 13c: ldr r12, [sp], #4
    0xe54d4001,  // 140: strb r4, [sp, #-1]
    0xe88d000f,  // 144: stmia sp, {r0-r3}
    0xe9bd0060,  // 148: ldmib sp!, {r5, r6}
    0xe14d70b6,  // 14c: strh r7, [sp, #-6]
    0xe79d0084,  // 150: ldr r0, [sp, r4, lsl #1]
    0xe08f1104,  // 154: add r1, pc, r4, lsl #2
    0xeafffffe,  // 158: b .
    0xa1b2c3d4,  // 15c: .word 0xa1b2c3d4
    0x22002105,  // 160: movs r1, #5; movs r2, #0
    0x39011852,  // 164: adds r2, r2, r1; subs r1, #1
    0x0113d1fc,  // 168: bne 164; lsls r3, r2, #4
    0xf000b50c,  // 16c: push {r2, r3, lr}; bl 184 (first half)
    0xbc0af809,  // 170: bl 184 (second half); pop {r1, r3}
    0x4d04b001,  // 174: add sp, #4; ldr r5, [pc, #0x10]
    0x44c046a8,  // 178: mov r8, r5; add r8, r8
    0x435345a8,  // 17c: cmp r8, r5; muls r3, r2
    0x47304e02,  // 180: ldr r6, [pc, #8]; bx r6
    0x47703201,  // 184: adds r2, #1; bx lr
    0x12345678,  // 188: .word 0x12345678
    0x00000118,  // 18c: .word 0x118
};

// CPU modes: MSR to the control field and to the flags, MRS, banked
// registers of FIQ mode, SWI and an undefined instruction with their
// returns, LDM^ with R15 and STM^.
constexpr uint32_t kModes[]{
    0xe3a0da07,  // 100: mov sp, #0x7000
    0xe3a08008,  // 104: mov r8, #8
    0xe3a09009,  // 108: mov r9, #9
    0xe321f0d1,  // 10c: msr cpsr_c, #0xd1
    0xe3a08088,  // 110: mov r8, #0x88
    0xe3a0da06,  // 114: mov sp, #0x6000
    0xe10f0000,  // 118: mrs r0, cpsr
    0xe36ff0df,  // 11c: msr spsr_fsxc, #0xdf
    0xe321f0df,  // 120: msr cpsr_c, #0xdf
    0xe0881009,  // 124: add r1, r8, r9
    0xe3a02101,  // 128: mov r2, #0x40000000
    0xe128f002,  // 12c: msr cpsr_f, r2
    0x03a03001,  // 130: moveq r3, #1
    0xe10fa000,  // 134: mrs r10, cpsr
    0xef000012,  // 138: swi #0x12
    0xe2833002,  // 13c: add r3, r3, #2
    0xe7f000f0,  // 140: undefined
    0xe2833004,  // 144: add r3, r3, #4
    0xe92d0300,  // 148: stmfd sp!, {r8, r9}
    0xeafffffe,  // 14c: b .
};

// Thumb: a loop on conditional branches, shifts, the ALU operations with
// their flags, LDMIA and STMIA, register offsets, SP arithmetic, SWI into
// ARM state and back, and BX PC.
constexpr uint32_t kThumb[]{
    0x21012000,  // 100: movs r0, #0; movs r1, #1
    0x18422714,  // 104: movs r7, #20; adds r2, r0, r1
    0x00110008,  // 108: movs r0, r1; movs r1, r2
    0xdcfa3f01,  // 10c: subs r7, #1; bgt 106
    0x17cc08cb,  // 110: lsrs r3, r1, #3; asrs r4, r1, #31
    0x41eb2507,  // 114: movs r5, #7; rors r3, r5
    0xd800428b,  // 118: cmp r3, r1; bhi 11e
    0x4d0e2601,  // 11c: movs r6, #1; ldr r5, [pc, #0x38]
    0x3d10c50f,  // 120: stmia r5!, {r0-r3}; subs r5, #16
    0x806ecd50,  // 124: ldmia r5!, {r4, r6}; strh r6, [r5, #2]
    0xaf025fea,  // 128: ldrsh r2, [r5, r7]; add r7, sp, #8
    0x4276b084,  // 12c: sub sp, #16; negs r6, r6
    0x43a743f7,  // 130: mvns r7, r6; bics r7, r4
    0x41484208,  // 134: tst r0, r1; adcs r0, r1
    0x40884191,  // 138: sbcs r1, r2; lsls r0, r1
    0x46c0df05,  // 13c: swi #5; nop
    0x46c04778,  // 140: bx pc; nop
    0xe3a09055,  // 144: mov r9, #0x55
    0xe28fa004,  // 148: adr r10, 154
    0xe38aa001,  // 14c: orr r10, r10, #1
    0xe12fff1a,  // 150: bx r10
    0x0000e7fe,  // 154: b .
    0x00009000,  // 158: .word 0x9000
};

// Conditional stores, loads, a multiply and LDM, whose paths end with
// another SEQ than their prefetch's; registers that pending flags read,
// overwritten unconditionally and conditionally; and the patterns that v3.x
// builds translate with Zba and Zbs.
constexpr uint32_t kMerges[]{
    0xe3a00a09,  // 100: mov r0, #0x9000
    0xe3a01006,  // 104: mov r1, #6
    0xe3a05000,  // 108: mov r5, #0
    0xe3a06003,  // 10c: mov r6, #3
    0xe3a09000,  // 110: mov r9, #0
    0xe2511001,  // 114: subs r1, r1, #1
    0x14801004,  // 118: strne r1, [r0], #4
    0x15102004,  // 11c: ldrne r2, [r0, #-4]
    0x10030692,  // 120: mulne r3, r2, r6
    0x10855003,  // 124: addne r5, r5, r3
    0xe0918006,  // 128: adds r8, r1, r6
    0xe3a06003,  // 12c: mov r6, #3
    0xe3a08000,  // 130: mov r8, #0
    0x32899001,  // 134: addcc r9, r9, #1
    0xe3510002,  // 138: cmp r1, #2
    0xc2811000,  // 13c: addgt r1, r1, #0
    0xc8900c00,  // 140: ldmgtia r0, {r10, r11}
    0xe3510000,  // 144: cmp r1, #0
    0x1afffff1,  // 148: bne 114
    0xe790c101,  // 14c: ldr r12, [r0, r1, lsl #2]
    0xe38cc701,  // 150: orr r12, r12, #0x40000
    0xe3ccc102,  // 154: bic r12, r12, #0x80000000
    0xe22cca01,  // 158: eor r12, r12, #0x1000
    0xe0854109,  // 15c: add r4, r5, r9, lsl #2
    0xeafffffe,  // 160: b .
};

constexpr uint32_t kSize = 65536, kEntry = 0x100, kBudget = 400, kMaxRecords = 512;

// The RAM: every bus cycle is recorded, as on the single-step tests' bus.
class RamBus {
public:
    RamBus(uint8_t* memory, std::span<BusRecord> records) : memory_(memory), records_(records) {}
    Bus bus() { return {this, cycle}; }
    size_t size() const { return size_; }
    uint32_t cycles() const { return cycles_; }

private:
    static uint32_t cycle(void* context, uint32_t request, uint32_t address, uint32_t data) {
        auto& self = *static_cast<RamBus*>(context);
        ++self.cycles_;
        const uint32_t kind = request & 3;
        const unsigned size = 1u << ((request >> 2) & 3);
        const uint32_t at = address & (kSize - 1) & ~(size - 1);
        uint32_t value = 0;
        if (kind == kWrite) {
            std::memcpy(self.memory_ + at, &data, size);
            value = data;
        } else if (kind != kIdle) {
            std::memcpy(&value, self.memory_ + at, size);
        }
        if (self.size_ < self.records_.size()) {
            self.records_[self.size_] = {static_cast<uint8_t>(kind), static_cast<uint8_t>(kind == kIdle ? 0 : size),
                                         static_cast<uint8_t>((request & kSequential ? 1 : 0) | (request & kLock ? 8 : 0)),
                                         static_cast<uint8_t>(self.cycles_), kind == kIdle ? 0 : address, value};
        }
        ++self.size_;
        return value;
    }
    uint8_t* memory_;
    std::span<BusRecord> records_;
    size_t size_ = 0;
    uint32_t cycles_ = 0;
};

uint32_t read_code(const void* context, uint32_t address, bool thumb) {
    const auto* memory = static_cast<const uint8_t*>(context);
    uint32_t value = 0;
    std::memcpy(&value, memory + (address & (kSize - 1) & (thumb ? ~1u : ~3u)), thumb ? 2 : 4);
    return value;
}

struct Program {
    std::span<const uint32_t> code;
    uint32_t cpsr;
};

// After a refill at the entry: R13 of System, FIQ, SVC and UND set apart.
Arm7State start(const uint8_t* memory, uint32_t cpsr) {
    Arm7State state;
    state.cpsr = cpsr;
    state.r[13] = 0x7000;
    state.r13[1][0] = 0x6800;
    state.r13[2][0] = 0x5000;
    state.r13[5][0] = 0x4800;
    const bool thumb = cpsr & kFlagT;
    const uint32_t length = thumb ? 2 : 4;
    state.pipeline = {read_code(memory, kEntry, thumb), read_code(memory, kEntry + length, thumb)};
    state.r[15] = kEntry + 2 * length;
    state.sequential = true;
    return state;
}

bool same(const Arm7State& a, const Arm7State& b) {
    return a.r == b.r && a.cpsr == b.cpsr && a.pipeline == b.pipeline && a.sequential == b.sequential &&
           a.r8 == b.r8 && a.r13 == b.r13 && a.spsr == b.spsr;
}

// One backend's run of `budget` bus cycles from the program's start: an
// instruction starts while cycles remain.
struct Run {
    Arm7State state;
    uint32_t instructions = 0;
    size_t records = 0;
    bool counted = true;  // The JIT's budget agrees with the bus cycles.
};

class Fixture {
public:
    bool initialize() {
        for (auto*& memory : memory_) memory = static_cast<uint8_t*>(std::calloc(kSize, 1));
        for (auto*& records : records_) records = static_cast<BusRecord*>(std::calloc(kMaxRecords, sizeof(BusRecord)));
        image_ = static_cast<uint8_t*>(std::calloc(kSize, 1));
        jit_.reset(new (std::nothrow) Arm7Jit);
        return memory_[0] && memory_[1] && records_[0] && records_[1] && image_ && jit_ && jit_->initialize();
    }
    ~Fixture() {
        for (auto* memory : memory_) std::free(memory);
        for (auto* records : records_) std::free(records);
        std::free(image_);
    }

    // Bits 0-3: after the whole budget, the JIT's CPU state, memory, bus
    // records and instruction count equal the interpreter's; bit 4: the same
    // after every shorter budget, so at every instruction boundary of its
    // blocks; bit 5: no instruction fell back to the interpreter. Bits 6
    // and up: checks of the program's results.
    uint32_t run(const Program& program, uint32_t (*check)(const Arm7State&)) {
        std::memset(image_, 0, kSize);
        std::memcpy(image_, kVectors, sizeof(kVectors));
        std::memcpy(image_ + kEntry, program.code.data(), program.code.size_bytes());
        jit_->clear();
        const auto fallbacks = std::accumulate(jit_->fallbacks().begin(), jit_->fallbacks().end(), uint64_t{0});
        uint32_t mask = compare(program, kBudget);
        bool every = (mask & 15) == 15;
        for (uint32_t budget = 1; budget < kBudget && every; ++budget) {
            // The sweeps take seconds; let the idle task feed the watchdog.
            if (budget % 64 == 0) vTaskDelay(1);
            every = compare(program, budget) == 15;
        }
        if (every) mask |= 1u << 4;
        const auto after = std::accumulate(jit_->fallbacks().begin(), jit_->fallbacks().end(), uint64_t{0});
        if (after == fallbacks && jit_->compiled_blocks()) mask |= 1u << 5;
        // The interpreter's final state for the program's own checks.
        return mask | check(execute(false, program, kBudget).state) << 6;
    }

private:
    Run execute(bool native, const Program& program, uint32_t budget) {
        uint8_t* memory = memory_[native];
        std::memcpy(memory, image_, kSize);
        RamBus ram(memory, {records_[native], kMaxRecords});
        Run run;
        run.state = start(memory, program.cpsr);
        if (native) {
            int32_t left = static_cast<int32_t>(budget);
            run.instructions = jit_->run(run.state, ram.bus(), {memory, read_code}, left).instructions;
            run.counted = left == static_cast<int32_t>(budget - ram.cycles());
        } else {
            while (ram.cycles() < budget) {
                arm7_step(run.state, ram.bus());
                ++run.instructions;
            }
        }
        run.records = ram.size();
        return run;
    }

    uint32_t compare(const Program& program, uint32_t budget) {
        const Run interpreter = execute(false, program, budget);
        const Run native = execute(true, program, budget);
        const size_t records = std::min<size_t>(interpreter.records, kMaxRecords);
        uint32_t mask = 0;
        if (same(interpreter.state, native.state)) mask |= 1;
        if (!std::memcmp(memory_[0], memory_[1], kSize)) mask |= 2;
        if (interpreter.records == native.records &&
            !std::memcmp(records_[0], records_[1], records * sizeof(BusRecord)))
            mask |= 4;
        if (interpreter.instructions == native.instructions && native.counted) mask |= 8;
        return mask;
    }

    std::array<uint8_t*, 2> memory_{};
    std::array<BusRecord*, 2> records_{};
    uint8_t* image_ = nullptr;
    std::unique_ptr<Arm7Jit> jit_;
};

uint32_t check_flags(const Arm7State& s) {
    const auto& r = s.r;
    return (r[0] == 55 && r[2] == 13) | (r[9] == 0x8000001b && r[10] == 45 && r[11] == 0x70000007) << 1 |
           (r[12] == 0x138 && s.cpsr == 0x900000df) << 2 |
           (r[4] == 0x80000c07 && r[5] == 0x008ba0a1 && r[6] == 0 && r[7] == 55 && r[8] == 0xbffffffe) << 3;
}

uint32_t check_states(const Arm7State& s) {
    const auto& r = s.r;
    return (r[3] == 3840 && r[6] == 16) |
           (r[7] == 0xa1b2 && r[8] == 0xffffffa1 && r[9] == 0xffffc3d4 && r[10] == 0xd4a1b2c3) << 1 |
           (r[11] == 0xa1b2c3d4 && r[12] == 7 && r[13] == 0x8008 && r[5] == 0xa1b2c3d4) << 2 |
           (r[1] == 0x178 && r[0] == 0 && !(s.cpsr & kFlagT)) << 3;
}

uint32_t check_modes(const Arm7State& s) {
    const auto& r = s.r;
    return (r[1] == 17 && r[3] == 7) | (r[4] == 0x400000df && r[10] == 0x400000df && r[0] == 0xd1) << 1 |
           (r[5] == 0x400000db && s.r13[5][1] == 0x144 && s.spsr[4] == 0x400000df) << 2 |
           (s.r8[1][0] == 0x88 && s.r13[1][0] == 0x6000 && s.spsr[0] == 0xdf && r[13] == 0x6ff8 &&
            s.cpsr == 0x400000df && s.r13[2] == std::array<uint32_t, 2>{0x5000, 0x13c} &&
            s.spsr[1] == 0x400000df) << 3;
}

uint32_t check_merges(const Arm7State& s) {
    const auto& r = s.r;
    return (r[5] == 45 && r[9] == 6) | (r[12] == 0x41000 && r[4] == 69) << 1 | (r[0] == 0x9014 && r[1] == 0) << 2 |
           (s.cpsr == 0x600000df) << 3;
}

uint32_t check_thumb(const Arm7State& s) {
    const auto& r = s.r;
    return (r[7] == 0x2080 && r[6] == 0xffffd53e) | (r[1] == 0xffffffff && r[0] == 0 && r[3] == 0xb000000a) << 1 |
           (r[4] == 0x400000ff && s.spsr[1] == 0x400000ff && s.r13[2][1] == 0x13e) << 2 |
           (r[9] == 0x55 && r[13] == 0x6ff0 && r[2] == 10946 && s.cpsr == 0x400000ff) << 3;
}

}  // namespace

bool arm7_blocks(std::array<uint32_t, 5>& results) {
    Fixture fixture;
    if (!fixture.initialize()) return false;
    constexpr uint32_t kSystem = kModeSystem | kFlagI | kFlagF;
    results[0] = fixture.run({kFlags, kSystem}, check_flags);
    results[1] = fixture.run({kStates, kSystem}, check_states);
    results[2] = fixture.run({kModes, kSystem}, check_modes);
    results[3] = fixture.run({kThumb, kSystem | kFlagT}, check_thumb);
    results[4] = fixture.run({kMerges, kSystem}, check_merges);
    return true;
}

}  // namespace gbx::test
