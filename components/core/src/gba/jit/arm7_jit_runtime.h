#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "gbx/gba/jit/arm7_jit.h"
#include "gbx/gba/memory.h"
#include "gbx/jit/rv32.h"

// Private ABI shared by the ARM7TDMI code generator (arm7_jit.cpp) and its
// runtime helpers (arm7_jit_runtime.cpp).
namespace gbx::gba::arm7_runtime {

namespace rv = jit::rv32;
using R = rv::Reg;

// Host registers while generated code runs. The guest's R0-R14 stay in host
// registers for the whole run, so that instructions work on them in place.
// R15 is a constant of each instruction. Guest registers fill the 12
// callee-saved registers first, so that C++ helpers keep them, the rest of
// them and the flags go in caller-saved ones, which the stubs save around
// C++. The registers of RVC's 3-bit fields, x8-x15, hold R0, R1 and the
// temporaries, which flag and address arithmetic use most. The run's
// context is the gateway's native frame (Frame), reached through sp, whose
// compressed loads and stores take any register.
inline constexpr std::array<R, 15> kGuest{
    R::s0, R::s1, R::s2, R::s3, R::s4, R::s5, R::s6, R::s7,  // R0-R7
    R::s8, R::a6, R::a7, R::t0,                              // R8-R11
    R::s9, R::s10, R::s11};                                  // R12-R14
// NZCV, kept apart so that an instruction sets each with the fewest host
// instructions: N is bit 31 of kN, Z is set when kZ is zero, C is kC (0 or
// 1) and V is bit 31 of kV. Other bits of kN and kV are undefined.
inline constexpr R kN = R::t1, kZ = R::t2, kC = R::t3, kV = R::t4;
// Bus cycles left before the run must stop, counted down by each
// instruction; an instruction starts only while it is positive.
inline constexpr R kRemaining = R::t5;
// Temporaries: a0-a3 within one instruction, where a1-a3 also pass the
// arguments of calls; a4, a5 and ra may also hold flag sources across
// instructions (Compiler::Flags).
inline constexpr std::array<R, 3> kHeld{R::a4, R::a5, R::ra};
// Calls from generated code into the gateway link through t6, `jal t6,
// stub`, and the stubs return through it, past the words that may follow
// the call. A call through ra would leave the address of those words on
// the return address stack of the ESP32-P4 HP core (TRM 1.12.2), which
// predicts the next return through ra by fetching from it; a word there
// that encodes a hardware loop instruction, such as 0x880150ab
// (esp.lp.setupi), corrupts the program counter (tested on a P4 v1.3).
// Only the gateway's calls into C++ link through ra, which it saves around
// them. t6 is scratch between calls, and the stubs jump into blocks
// through it rather than through ra, for the same reason.
inline constexpr R kLink = R::t6;

// What C++ supplies to a run and receives from it.
struct Context {
    Arm7State* cpu;
    const Bus* bus;           // Flat test bus.
    int32_t remaining;        // Bus cycles, see kRemaining.
    uint32_t instructions;    // Completed instructions, added to by exits.
    // Mapped runs only, see Arm7Jit::run(Arm7State&, Memory&).
    Memory* memory = nullptr;
    // The prefetch buffer while native code owns it, see Frame.
    int32_t prefetch_base = 0;
    uint32_t prefetch_flags = 0;
    // Dispatch table and the timing tag by address bits 24-31, see
    // Arm7Jit::key().
    const uint32_t* dispatch = nullptr;
    const uint8_t* tags = nullptr;
    // The link word of the last static exit, for Arm7Jit to link it.
    uint32_t* link = nullptr;
    // A block in the second level whose entries ran out: it left before
    // running, for Arm7Jit to move it into the pool.
    uintptr_t spent = 0;
    // A write retired translated code (Arm7Jit::code_written()): the
    // opcodes the pipeline held before it, at snap_last - length and
    // snap_last, for the interpreter to finish.
    bool smc = false, snapped = false;
    uint32_t snap_last = 0;
    std::array<uint32_t, 2> snap{};
#ifdef GBX_PROFILER
    // Linked and dispatched block entries, the access stubs' calls, the
    // dispatch stub's lookups, m_stop's calls and the cycles of the
    // dispatches that entered a block, see arm7_gateway_counts.
    std::array<uint32_t, 16> counts{};
#endif
};

// The gateway's native frame, at sp while generated code runs. Compressed
// sp-relative accesses reach its first 256 bytes.
struct Frame {
    Arm7State* cpu;
    Context* context;
    // Host registers by number, saved by the stubs around C++ and by side
    // exits, which C++ reads.
    std::array<uint32_t, 32> host;
    // The arguments of the multiply carry stubs: multiplicand, multiplier,
    // kind, accumulator low and high.
    std::array<uint32_t, 5> multiply;
    // The caller's ra and s0-s11.
    std::array<uint32_t, 13> saved;
    // Mapped runs. A block writes `pc` before it calls C++: R15 of the
    // current instruction, which its prefetch fetches; once that prefetch is
    // done | kAfterPrefetch, with kPcThumb for an instruction in Thumb
    // state, which CPSR may no longer say. Before the prefetch the
    // instruction's state is CPSR's.
    Memory* memory;
    uint32_t pc;
    // The gamepak prefetch buffer while native code owns it (kPrefetchFast):
    // base - deadline as an int32, see Memory::Prefetch, where base and
    // start are equal, and the state Memory::lend_prefetch() describes; the
    // buffer's head is the next code fetch. Without kPrefetchFast, Memory
    // holds the buffer's state.
    int32_t prefetch_base;
    uint32_t prefetch_flags;
    // A wait loop's last start (Compiler::spin()): its block's R15, 0 after
    // C++ ran or the loop left, the cycles left then, and the cycles of the
    // pass before.
    uint32_t spin_site;
    int32_t spin_remaining, spin_cycles;
    uint32_t unused;
#ifdef GBX_PROFILER
    // mcycle when the dispatch stub began, see Context::counts.
    std::array<uint32_t, 4> profile;
#endif
};
// On RV32; a 64-bit host builds the code generator but never runs its code.
static_assert(sizeof(void*) != 4 || (sizeof(Frame) % 16 == 0 && sizeof(Frame) <= 256));
inline constexpr int kFrameCpu = offsetof(Frame, cpu), kFrameContext = offsetof(Frame, context);
inline constexpr int kFrameMultiply = offsetof(Frame, multiply), kFrameSaved = offsetof(Frame, saved);
inline constexpr int kFrameMemory = offsetof(Frame, memory), kFramePc = offsetof(Frame, pc);
inline constexpr int kFramePrefetchBase = offsetof(Frame, prefetch_base),
                     kFramePrefetchFlags = offsetof(Frame, prefetch_flags);
inline constexpr int kFrameSpinSite = offsetof(Frame, spin_site), kFrameSpinRemaining = offsetof(Frame, spin_remaining),
                     kFrameSpinCycles = offsetof(Frame, spin_cycles);
inline constexpr int kContextInstructions = offsetof(Context, instructions);
inline constexpr uint32_t kAfterPrefetch = 1, kPcThumb = 1u << 31;
inline constexpr uint32_t kPrefetchFast = Memory::kLent, kPrefetchRunning = Memory::kLentRunning,
                          kPrefetchStalled = Memory::kLentStalled, kPrefetchEmpty = Memory::kLentEmpty,
                          kPrefetchDrained = Memory::kLentDrained;
inline constexpr unsigned kPrefetchPausedShift = Memory::kLentPausedShift;
constexpr int host_slot(R r) { return static_cast<int>(offsetof(Frame, host) + 4 * static_cast<unsigned>(r)); }
// A stub that calls C++ keeps its own return address in the host slot of
// gp, which nothing else uses.
inline constexpr int kFrameStubLink = host_slot(R::gp);
// The ROM refill stubs keep their target in that of tp.
inline constexpr int kFrameRefillTarget = host_slot(R::tp);

// Normal exit: `jal t6, exit` or `jal t6, exit_synced`, then the new R15
// and a word of these bits. A dynamic exit takes R15 from a0. exit_synced
// follows a helper that already wrote the guest registers and CPSR to
// Arm7State. With sequential_request, the S of the next fetch is that of
// the request in a1 (Compiler::arm()).
namespace exit_bits {
constexpr unsigned completed = 0;      // 7 bits: instructions completed in the block.
constexpr uint32_t sequential = 1u << 7;
constexpr unsigned reason = 8;         // 2 bits: Arm7JitExit.
constexpr uint32_t dynamic = 1u << 10;
constexpr unsigned sequential_request = 11;
// Mapped blocks: a link word follows, see Compiler::m_leave().
constexpr uint32_t linkable = 1u << 12;
// A linkable exit whose call of the link stub, 3 words before the link
// word, Arm7Jit::link() replaces with a jump to the target.
constexpr uint32_t direct = 1u << 13;
}

// A code fetch from the ROM through a fetch stub: `jal t6, stub`, then the
// address with the request's kSequential and kStateSequential, shifted by
// this, in bits 28-31, which ROM addresses leave clear.
inline constexpr unsigned kRomFetchRequestShift = 24;

// Side exit at an instruction boundary inside a block, when kRemaining ran
// out: `jal t6, side_exit`, then the boundary's R15, a word of these bits
// and, with an arithmetic flag source, its word and immediates. The flags
// still to be computed there (Compiler::Flags) are computed in C++.
namespace side_bits {
constexpr unsigned completed = 0;      // 7 bits.
constexpr uint32_t sequential_request = 1u << 7;  // As exit_bits.
constexpr uint32_t sequential = 1u << 8;
constexpr unsigned nz = 9;             // 2 bits: kNzDone, kNzRegister, kNzArith.
constexpr unsigned nz_register = 11;   // 5 bits.
constexpr unsigned c = 16;             // 3 bits: kC*.
constexpr unsigned c_first = 19;       // 5 bits: a register.
constexpr unsigned c_second = 24;      // 5 bits: a register or a bit number.
constexpr uint32_t v = 1u << 29;       // V comes from the arithmetic source.
constexpr unsigned arith = 30;         // 2 bits: kArith*.
}
enum : uint32_t { kNzDone, kNzRegister, kNzArith };
// C: in kC; from the arithmetic source; bit c_second of c_first; 0; 1; or
// the multiply carry of multiplicand c_first and multiplier c_second, for
// MUL without accumulation.
enum : uint32_t { kCDone, kCArith, kCBit, kCZero, kCOne, kCMultiply };
// result = first + second, or result = first - second.
enum : uint32_t { kArithNone, kArithAdd, kArithSub };
// The arithmetic word: result, first and second operand in 6 bits each, as
// a register number, kOperandUnknown or kOperandImmediate; the immediates
// follow, first before second. Two of the three are known.
inline constexpr uint32_t kOperandUnknown = 0, kOperandImmediate = 32;

// Request bit for flat_fetch(): the S of the fetch is Arm7State::sequential.
inline constexpr uint32_t kStateSequential = 1u << 6;

// Runtime helpers, called from the gateway's stubs with the frame first.
uint32_t side_exit(Frame* frame, const uint32_t* descriptor);
// MSR to CPSR fields other than the flags: CPSR = CPSR & ~mask | value &
// mask | 10h, switching banks. Arm7State already holds the registers.
uint32_t write_cpsr(Frame* frame, uint32_t value, uint32_t mask);
// CPSR = SPSR of the current mode, when it has one; `test` for TST, TEQ,
// CMP and CMN with S into R15, see MappedBus::irq_masked_late().
uint32_t restore_cpsr(Frame* frame, uint32_t test);
// Exception entry without its refill (GBATEK "ARM CPU Exceptions").
uint32_t exception(Frame* frame, uint32_t mode, uint32_t return_address);
// C of a flag-setting multiply (multiply_carry() in arm7.cpp). kind: bit 0
// signed, bit 1 long, bit 2 with the accumulator, which is read only then,
// so that MUL passes three arguments.
inline constexpr uint32_t kMultiplySigned = 1, kMultiplyLong = 2, kMultiplyAccumulate = 4;
uint32_t multiply_carry(Frame* frame, uint32_t multiplicand, uint32_t multiplier, uint32_t kind,
                        uint32_t accumulator_low, uint32_t accumulator_high);
// The flat test bus: one bus cycle, and a fetch that also moves the
// pipeline as the interpreter's prefetch does.
uint32_t flat_access(Frame* frame, uint32_t request, uint32_t address, uint32_t data);
uint32_t flat_fetch(Frame* frame, uint32_t request, uint32_t address);
// Mapped blocks, with Frame::pc set: the catch-ups before a fetch, a data
// access and an internal cycle, as MappedBus::access() starts; an access
// after its catch-up; a code fetch; and the two fetches of a refill from
// `target`, each catching up first. Arguments as Compiler::m_access()
// passes them.
uint32_t mapped_catch_fetch(Frame* frame);
uint32_t mapped_catch(Frame* frame);
uint32_t mapped_idle(Frame* frame);
uint32_t mapped_read(Frame* frame, uint32_t request, uint32_t address);
uint32_t mapped_write(Frame* frame, uint32_t request, uint32_t address, uint32_t data);
uint32_t mapped_fetch(Frame* frame, uint32_t request, uint32_t address);
uint32_t mapped_refill(Frame* frame, uint32_t length, uint32_t target);
// With the length: native code caught up before the refill's first fetch.
inline constexpr uint32_t kRefillChecked = 0x100;
// An instruction boundary with no cycles left, at Frame::pc, before the
// prefetch: as arm7_run_mapped() there, catch_up_boundary(); nonzero when
// the run must stop before the instruction. Otherwise the prefetch's own
// catch-up follows when it is still due, as the instruction would start.
uint32_t mapped_boundary(Frame* frame);
// The boundary before a linked block, or one the dispatch table found: the
// catch-up alone. The block's first instruction catches up at its fetch,
// which clocks the interrupt line's CPU stage there, as the interpreter's
// fetch does; were the boundary to do it, a block that returns to C++ at
// once, such as a copy in the second level whose entries ran out, would
// take an interrupt an instruction early.
uint32_t mapped_link_boundary(Frame* frame);
// The prefetch buffer lent to native code whose next code fetch is `next`,
// see Memory::lend_prefetch(): `base` and `flags` as in Frame, 0 when
// Memory keeps it.
void lend_prefetch(Memory& memory, uint32_t next, int32_t& base, uint32_t& flags);
// Back from native code whose next code fetch is `head`.
void return_prefetch(Memory& memory, uint32_t head, int32_t base, uint32_t flags);

}  // namespace gbx::gba::arm7_runtime
