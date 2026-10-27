#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "arm7_jit_runtime.h"
#include "gbx/jit/assembler.h"

// The block compiler of Arm7Jit, in parts:
//   arm7_compiler.cpp        the block loop, code streams, bus cycles, exits
//   arm7_compiler_flags.cpp  pending flags and conditions
//   arm7_compiler_ops.cpp    what ARM and Thumb share: the shifter, the ALU,
//                            loads, stores and block transfers
//   arm7_compiler_arm.cpp    ARM instructions and conditional execution
//   arm7_compiler_thumb.cpp  Thumb instructions
namespace gbx::gba::arm7_compiler {

using namespace arm7_runtime;
using Branch = rv::Instruction (*)(R, R, int32_t);

// Offsets into Arm7State.
inline constexpr int kStateR15 = offsetof(Arm7State, r) + 60, kStateCpsr = offsetof(Arm7State, cpsr),
                     kStatePipeline = offsetof(Arm7State, pipeline), kStateR8 = offsetof(Arm7State, r8),
                     kStateR13 = offsetof(Arm7State, r13), kStateSpsr = offsetof(Arm7State, spsr);

// An immediate of addi, xori, andi, ori and sltiu.
constexpr bool fits(uint32_t value) {
    const auto s = static_cast<int32_t>(value);
    return s >= -2048 && s <= 2047;
}
// One whose negation fits too, so that a flag source can be subtracted.
constexpr bool small(uint32_t value) {
    const auto s = static_cast<int32_t>(value);
    return s >= -2047 && s <= 2047;
}
constexpr int32_t immediate(uint32_t value) { return static_cast<int32_t>(value); }
constexpr unsigned number(R r) { return static_cast<unsigned>(r); }

// An operand of an instruction: a host register or a constant, such as R15.
struct Value {
    R reg;
    uint32_t constant;
    bool fixed;
};
constexpr Value in(R r) { return {r, 0, false}; }
constexpr Value fixed(uint32_t value) { return {R::zero, value, true}; }

// The source of a pending flag (Compiler::Flags): a register the compiler
// tracks, an immediate, or unknown.
enum : uint8_t { kUnknown, kRegister, kImmediate };
struct Operand {
    uint8_t kind;
    R reg;
    uint32_t imm;
};
constexpr Operand register_operand(R r) { return {kRegister, r, 0}; }

// The shifter's carry out for a logical operation with S: C unchanged, a
// constant, a bit of a guest register, or already in kC.
enum : uint8_t { kKeep, kCarryConstant, kCarryBit, kCarryDone };
struct Carry {
    uint8_t kind;
    R reg;
    uint8_t bit;
};
inline constexpr Carry kKeepCarry{kKeep, R::zero, 0};

// The data-processing opcodes whose C comes from the shifter.
inline constexpr uint32_t kLogical = 0xf303;

// The GBA's memory map as a mapped block sees it, fixed under its key
// (Arm7Jit::key()): the region of the block's code and the cycles of the
// accesses whose cost the code embeds.
struct MappedTarget {
    uint8_t region = 0;                // Address bits 24-31 of the code.
    std::array<uint8_t, 3> ewram_cycles{};  // By width; N and S are alike.
    std::array<uint8_t, 3> code_n{}, code_s{};  // N and S fetches from the code's region, by width.
    uint8_t halfword_s = 0;            // The prefetch buffer's cycles per halfword.
    bool prefetch = false;             // WAITCNT enables the prefetch buffer.
    bool self_loops = true;            // A static exit to the entry may jump there, see m_leave().
    bool count_instructions = true;    // Native jumps count Context::instructions, see m_leave().
    // The registers at the block's entry, which predict the region of its
    // accesses (Compiler::hint()), and IWRAM, EWRAM and their code marks,
    // which the accesses predicted there reach inline.
    const uint32_t* registers = nullptr;
    const uint8_t *iwram = nullptr, *ewram = nullptr, *iwram_code = nullptr, *ewram_code = nullptr;
    // VRAM and Memory::dirty, for ARM's stores predicted there.
    const uint8_t* vram = nullptr;
    const uint32_t* dirty = nullptr;
    uint32_t* gamepak_next = nullptr;  // Memory::gamepak_next_, see Compiler::m_literal().
};

// Translates ARM and Thumb code into RV32 blocks. A block holds up to the
// limit of instructions and ends at the first that writes R15, raises an
// exception or changes the CPU mode; its key names the entry R15, T and the
// mode, so the code depends on nothing else of the CPU state.
//
// Registers. R0-R14 and NZCV stay in host registers for the whole run (see
// arm7_jit_runtime.h); each instruction's R15 is a constant. Arm7State holds
// the rest of CPSR, the banked registers and the SPSRs, which only MSR, MRS,
// exceptions and the transfers of User mode registers touch.
//
// Flags. An instruction that sets NZCV does not compute them: it records
// where they come from (Flags), a result register for N and Z, the operands
// of an addition or subtraction for C and V, a bit of a register for the
// shifter's carry, and so on. The flags are computed when an instruction
// reads them, which often compares the recorded operands directly (CMP then
// BNE becomes bne), when a later instruction would overwrite a source, and
// at the block's exits. An instruction that sets them again drops the
// pending ones, as most flag results are. Because a block may stop at any
// instruction boundary (below), each boundary's exit describes the pending
// flags, and C++ computes them there, so that the CPSR outside a block is
// always that of executing instruction by instruction.
//
// Time. kRemaining counts the bus cycles the run may still take. Each
// instruction subtracts its own, and before each instruction after the first
// the block leaves through a side exit when none remain. Milestone 2
// replaces the count of cycles with their wait states and ends the count at
// the next device event, so that a block can stop at any instruction
// boundary for an event.
//
// Bus. On the flat test bus every bus cycle calls the bus through the
// gateway, in the interpreter's order, the prefetch of each instruction
// included, which also moves Arm7State::pipeline. That is where test code
// differs from what milestone 2 will run: code fetches there are counted
// rather than called, and RAM accesses take direct paths.
//
// Code. Blocks use the 16-bit forms of instructions where they exist (RVC);
// words read as data stay aligned: the R15 and descriptor words after the
// calls of exits. A hot stream holds the instructions, a cold stream the
// side exits. Calls into the gateway link through kLink and are relocated at
// installation.
class Compiler {
public:
    Compiler(std::span<uint32_t> hot, std::span<uint32_t> cold, const Arm7Jit::Stubs& stubs, Arm7CodeSource code,
             uint32_t r15, uint32_t cpsr, const MappedTarget* mapped = nullptr);
    // Mapped blocks: their header words before the code, see Arm7Jit.
    static constexpr unsigned kHeaderWords = Arm7Jit::kHeaderWords;
    // Instructions the block holds, once compiled.
    unsigned instructions() const { return index_; }
    // The guest code it covers ends before this address.
    uint32_t code_end() const { return last_ + length_; }
    // Instruction boundaries of a mapped block where a run may enter it,
    // see Arm7Jit::MidEntry: R15 there, the byte offset from the block's
    // start, the instructions before it and the SEQ its code takes for the
    // next fetch.
    struct Boundary {
        uint32_t r15;
        uint16_t offset;
        uint8_t completed;
        bool sequential;
    };
    std::span<const Boundary> boundaries() const { return {boundaries_.data(), boundary_count_}; }
    // The block's words, with the offsets of its calls into the gateway in
    // relocations; empty on failure.
    std::span<const uint32_t> compile(unsigned limit, std::span<uint32_t> output,
                                      std::span<jit::Relocation> relocations, size_t& count);

private:
    // No instruction starts after these many bytes of a stream, so that
    // branches between the streams stay within reach.
    static constexpr size_t kHotLimit = 2560, kColdLimit = 1024;
    // Mapped blocks keep more in the cold stream: the catch-ups and slow
    // paths of their accesses.
    static constexpr size_t kMappedHotLimit = 2048, kMappedColdLimit = 1536;
    // A position: bytes into a stream, with kCold for the cold one.
    using Label = uint32_t;
    static constexpr Label kCold = 1u << 31;
    enum class Kind : uint8_t { branch, jump };
    struct Reference {
        Label from, to;
        Branch op;
        R a, b;
        Kind kind;
        bool bound;
    };
    struct Call {
        Label at;
        uintptr_t target;
    };
    // Flags still to be computed, see the class comment and side_bits.
    struct Flags {
        uint8_t nz = kNzDone;
        R nz_reg = R::zero;
        uint8_t c = kCDone;
        R c_first = R::zero;
        uint8_t c_second = 0;  // A register number or a bit.
        bool v = false;
        uint8_t arith = kArithNone;
        Operand result{}, first{}, second{};
    };
    // What an ARM instruction does when its condition passes, for
    // conditional execution.
    enum : uint8_t { kReadsNone, kReadsCarry, kReadsAll };
    struct Shape {
        bool branch;      // Writes R15 or takes an exception.
        bool ends;        // Ends the block otherwise, by changing the mode.
        bool sets;        // Sets flags.
        uint8_t reads;    // Flags it reads.
        bool sequential;  // Its last cycle is a fetch.
        uint16_t writes;  // Guest registers it writes.
    };
    // sequential_ after a conditional instruction whose paths end with
    // different SEQ: a1 holds the request of the next prefetch.
    static constexpr int kRequested = 2;
    enum class Op : uint8_t { add, sub, and_, or_, xor_, andn, mov, mvn };
    enum class Load : uint8_t { word, half, signed_half, byte, signed_byte };

    // arm7_compiler.cpp ------------------------------------------------------
    void instruction();
    // Streams, labels and relocations.
    jit::Assembler& out() { return cold_mode_ ? cold_ : hot_; }
    Label here() { return static_cast<Label>(out().offset()) | (cold_mode_ ? kCold : 0); }
    void emit(rv::Instruction instruction) { out().emit(instruction); }
    void data(uint32_t word) { out().data(word); }
    void li(R d, uint32_t value) { out().li(d, value); }
    void mv(R d, R s) { if (d != s) emit(rv::addi(d, s, 0)); }
    unsigned reference(Kind kind, Branch op, R a, R b);
    unsigned branch(Branch op, R a, R b) { return reference(Kind::branch, op, a, b); }
    unsigned jump() { return reference(Kind::jump, nullptr, R::zero, R::zero); }
    void bind(unsigned index) {
        references_[index].to = here();
        references_[index].bound = true;
    }
    void bind(unsigned index, Label to) {
        references_[index].to = to;
        references_[index].bound = true;
    }
    void call(uintptr_t target);
    std::span<const uint32_t> finish(std::span<uint32_t> output, std::span<jit::Relocation> relocations,
                                     size_t& count);
    // Time and the bus. Bus cycles are counted at compile time and taken off
    // kRemaining where paths part and before exits.
    void charge(unsigned cycles = 1) { charge_ += cycles; }
    void flush_charge();
    void bus(uint32_t request);
    void read(uint32_t request) { bus(kRead | request); }
    void write(uint32_t request) { bus(kWrite | request); }
    void idle() { bus(kIdle); }
    void fetch(uint32_t request);
    void prefetch();
    void multiply_cycles(Value multiplier, bool is_signed, unsigned extra);
    void idle_loop();
    // Guest registers.
    static R host(unsigned n) { return kGuest[n]; }
    // Guest register n, with `pc` as R15's value.
    static Value guest(unsigned n, uint32_t pc) { return n == 15 ? fixed(pc) : in(host(n)); }
    R reg(Value value, R scratch);
    void load_value(R d, Value value);
    void set_guest(unsigned n, Value value);
    void state_pointer(R d) { emit(rv::lw(d, R::sp, kFrameCpu)); }
    void binary(Op op, R d, Value a, Value b);
    // Exits and control flow. sequential: 0, 1 or kRequested.
    void leave(uint32_t r15, bool dynamic, int sequential, Arm7JitExit reason, bool synced = false);
    void boundary();
    void refill_static(uint32_t target, bool thumb, bool synced = false);
    void refill_dynamic(R target, bool thumb, bool synced = false);
    void branch_to(Value target);
    void refill_by_state(R target);
    void sync();
    void return_from_exception(R target);
    void exception(uint32_t mode, uint32_t vector, bool undefined);
    void set_thumb(bool on);
    void exchange(Value target);

    // Wait loops: a block that branches back to its start and whose every
    // pass does the same, see spin().
    unsigned spin_loop() const;
    void spin(unsigned instructions);

    // arm7_compiler_mapped.cpp -----------------------------------------------
    bool in_region(uint32_t address) const;
    // Frame::pc for `value`: R15, | kAfterPrefetch once the prefetch is done.
    uint32_t m_pc(uint32_t value) const;
    void m_store_pc(uint32_t value);
    void m_check(uintptr_t stub, uint32_t pc);
    void m_call(uintptr_t stub, uint32_t pc);
    void m_prefetch();
    void m_rom_fetch(uint32_t address);
    void m_access(uint32_t request);
    void m_ram_access(uint32_t request, unsigned region);
    void m_vram_store(uint32_t request);
    Label m_block_inline(unsigned rn, uint32_t list, bool load, bool writeback, R moved);
    bool m_literal(uint32_t address, unsigned rd);
    // The next access is likely in the region guest register n points
    // into, see regions_.
    void hint(unsigned n) { hint_ = n == 15 ? static_cast<uint8_t>(r15_ >> 24) : regions_[n]; }
    void track(uint32_t op);
    void m_idle_loop();
    bool m_inline_refill(uint32_t target) const;
    bool m_rom_refill(uint32_t target, bool thumb);
    void m_refill_static(uint32_t target, bool thumb, bool synced);
    void m_refill_dynamic(R target, bool thumb, bool synced);
    void m_leave(uint32_t r15, bool dynamic, int sequential, Arm7JitExit reason, bool synced);
    void m_entry();
    std::span<const uint32_t> m_finish(std::span<uint32_t> output, std::span<jit::Relocation> relocations,
                                       size_t& count);

    // arm7_compiler_flags.cpp ------------------------------------------------
    static bool tracked(R r);
    bool refers(R r) const;
    void settle();
    void kill(bool nz, bool c, bool v);
    bool constant_arith() const { return flags_.first.kind == kImmediate && flags_.second.kind == kImmediate; }
    void constant_flags(uint32_t& result, uint32_t& carry, uint32_t& overflow) const;
    void emit_add(R d, const Operand& a, const Operand& b);
    void emit_sub(R d, const Operand& a, const Operand& b);
    void emit_xor(R d, const Operand& a, const Operand& b);
    void emit_sltu(R d, const Operand& a, const Operand& b);
    bool may_materialize();
    void materialize_nz();
    void materialize_c();
    void materialize_v();
    void materialize_all() {
        materialize_nz();
        materialize_v();
        materialize_c();
    }
    void clobber(R r);
    bool relocate(R r);
    void preserve(uint16_t writes);
    unsigned free_held() const;
    R hold();
    static Operand pending(Value value, R dest);
    void set_carry(const Carry& carry);
    void flags_from(Value value);
    unsigned when(unsigned cond, std::array<unsigned, 2>& refs);
    uint8_t live_flags(uint32_t address) const;

    // arm7_compiler_ops.cpp --------------------------------------------------
    Value shift_immediate(unsigned type, Value m, unsigned amount, bool want, Carry& carry, R target);
    Value shift_register(unsigned type, Value m, Value s, bool want, R target);
    void alu(unsigned opcode, Value a, Value b, Carry carry, bool set, R dest);
    void logical(unsigned opcode, Value a, Value b, Carry carry, bool set, R dest);
    void carry_arith(unsigned opcode, Value a, Value b, bool set, R dest);
    void rotate_loaded(int32_t mask);
    void load(Load kind);
    void store(uint32_t width, Value value);
    void finish_load(unsigned rd);
    int user_slot(unsigned i) const;
    void block_transfer(unsigned rn, uint32_t list, bool before, bool up, bool writeback, bool load, bool user);

    // arm7_compiler_arm.cpp --------------------------------------------------
    void arm(uint32_t op);
    Shape arm_shape(uint32_t op) const;
    void arm_execute(uint32_t op);
    void data_processing(uint32_t op);
    uint32_t msr_mask(uint32_t op) const;
    void psr_transfer(uint32_t op);
    void debug_channel(uint32_t op);
    void multiply(uint32_t op);
    void multiply_long(uint32_t op);
    void store_multiply(Value multiplicand, Value multiplier, uint32_t kind, Value low, Value high);
    void swap(uint32_t op);
    void transfer(unsigned rn, unsigned rd, Value offset, bool up, bool pre, bool writeback, bool load, Load kind,
                  uint32_t width, unsigned scale = 0);
    void single_transfer(uint32_t op);
    void halfword_transfer(uint32_t op);

    // arm7_compiler_thumb.cpp ------------------------------------------------
    void thumb(uint32_t op);
    void thumb_alu(uint32_t op);
    void thumb_multiply(unsigned rd, unsigned rs);
    void high_register(uint32_t op);

    jit::Assembler hot_, cold_;
    bool cold_mode_ = false;
    const Arm7Jit::Stubs& stubs_;
    Arm7CodeSource code_;
    const MappedTarget* mapped_;
    // The entry's R15, and the value LR got from the first half of a Thumb
    // BL just before, for a second half whose target is then constant.
    uint32_t entry_, last_ = 0;
    bool bl_pending_ = false;
    uint32_t bl_value_ = 0;
    // Link words of static exits, as byte offsets into the hot stream.
    unsigned link_count_ = 0;
    std::array<uint16_t, 32> links_{};
    // Boundaries for mid-block entries, with offsets into the hot stream
    // until m_finish().
    unsigned boundary_count_ = 0;
    std::array<Boundary, 64> boundaries_{};
    uint32_t r15_;  // R15 while the current instruction executes.
    bool thumb_;
    uint32_t mode_;
    unsigned bank_;
    unsigned length_;
    unsigned index_ = 0, completed_ = 0;
    // The S of the next code fetch: 0, 1, kRequested, or -1 for
    // Arm7State::sequential.
    int sequential_ = -1;
    bool end_ = false, failed_ = false, merging_ = false, spin_ = false;
    // The refill being compiled goes to the other state, whose code the
    // exit after it does not read for live_flags().
    bool exchanged_ = false;
    // The instruction wrote memory, which may stop the run (HALTCNT, an
    // interrupt, code it overwrote): with relaxed timing, the boundaries
    // after such instructions are the only ones, see instruction().
    bool wrote_ = false;
    unsigned charge_ = 0;
    uint8_t hint_ = 0;  // see hint()
    // The region (address bits 24-31) each guest register likely points
    // into, from its value at the block's entry, carried through moves and
    // additions of small values; 0 when unknown.
    std::array<uint8_t, 15> regions_{};
    uint8_t busy_ = 0;  // Held registers the current instruction uses.
    Flags flags_;
    unsigned reference_count_ = 0, call_count_ = 0;
    std::array<Reference, 384> references_;
    std::array<Call, 384> calls_;
};

}  // namespace gbx::gba::arm7_compiler
