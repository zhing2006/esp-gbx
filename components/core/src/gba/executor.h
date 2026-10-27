#pragma once

// The ARM7TDMI interpreter as a template on the bus type, for each file that
// instantiates it: arm7_step.cpp on the flat bus of the single-step tests,
// and arm7_mapped.cpp on the memory map, so that each copy can go where its
// users need it. One file per bus type: the linker keeps a single object's
// copy of the functions and tables a template instantiates. arm7.cpp has
// the functions both copies share.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <utility>

#include "bits.h"
#include "gbx/gba/arm7.h"

namespace gbx::gba {

// Bank of R13-R14 for each mode value, an index into Arm7State::r13; the
// SPSR of a bank is spsr[bank - 1]. Values GBATEK does not list map to
// User's bank, see arm7_switch_mode().
inline constexpr unsigned kFiqBank = 1;
inline constexpr auto kBanks = [] {
    std::array<uint8_t, 32> banks{};
    banks[kModeFiq] = 1;
    banks[kModeSupervisor] = 2;
    banks[kModeAbort] = 3;
    banks[kModeIrq] = 4;
    banks[kModeUndefined] = 5;
    return banks;
}();
inline unsigned bank_of(uint32_t psr) { return kBanks[psr & kModeMask]; }

// Bit f of entry c is set when condition c passes with NZCV = f (GBATEK
// "ARM CPU Flags & Condition Field"). NV never passes.
inline constexpr auto kConditions = [] {
    std::array<uint16_t, 16> table{};
    for (unsigned f = 0; f < 16; ++f) {
        const bool n = f & 8, z = f & 4, c = f & 2, v = f & 1;
        const bool pass[16] = {z, !z, c, !c, n, !n, v, !v, c && !z, !c || z, n == v, n != v, !z && n == v,
                               z || n != v, true, false};
        for (unsigned cond = 0; cond < 16; ++cond) {
            if (pass[cond]) table[cond] |= 1u << f;
        }
    }
    return table;
}();

// The internal cycles of a multiply, m in GBATEK "ARM Opcodes: Multiply and
// Multiply-Accumulate": 1 to 4, by how many bytes of the multiplier above the
// lowest are not all zero, or, for signed multiplies, also not all one. MUL
// and MLA count as signed.
// Without branches, which the operands would make hard to predict.
inline unsigned multiply_cycles(uint32_t multiplier, bool is_signed) {
    if (is_signed) multiplier ^= static_cast<uint32_t>(static_cast<int32_t>(multiplier) >> 31);
    return 1u + (multiplier >= 1u << 8) + (multiplier >= 1u << 16) + (multiplier >= 1u << 24);
}

// C after a flag-setting multiply, see arm7.cpp. Not inlined: only
// multiplies with S compute it.
bool multiply_carry(uint32_t multiplicand, uint32_t multiplier, uint64_t accumulator, bool is_signed,
                    bool is_long, unsigned cycles);

// The handler tables: the distinct keys of the decode indices in order,
// and for each index the position of its key among them.
template<size_t N>
consteval size_t distinct_keys(const std::array<uint32_t, N>& keys) {
    auto sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    return static_cast<size_t>(std::unique(sorted.begin(), sorted.end()) - sorted.begin());
}

template<size_t M, size_t N>
consteval std::array<uint32_t, M> key_list(const std::array<uint32_t, N>& keys) {
    auto sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    std::array<uint32_t, M> list{};
    std::copy(sorted.begin(), std::unique(sorted.begin(), sorted.end()), list.begin());
    return list;
}

template<size_t M, size_t N>
consteval std::array<uint8_t, N> key_positions(const std::array<uint32_t, N>& keys,
                                               const std::array<uint32_t, M>& list) {
    static_assert(M <= 256);
    std::array<uint8_t, N> positions{};
    for (size_t i = 0; i < N; ++i)
        positions[i] = static_cast<uint8_t>(std::lower_bound(list.begin(), list.end(), keys[i]) - list.begin());
    return positions;
}

// One instruction on one bus type. Decoding is a table per state, indexed
// by the opcode bits GBATEK's "ARM Binary Opcode Format" and "THUMB Binary
// Opcode Format" tables split on: ARM bits 27-20 and 7-4, Thumb bits 15-6.
// An entry selects a handler specialized on the bits that pick the
// operation (ALU operation and S, load or store, width), the rest decoded
// at run time, so that a game's loop runs few, small handlers. Each table
// holds a byte per index into a short array of handlers, which keeps them
// small in the data cache. Rare instructions share generic handlers. Only
// the common paths are inline in the handlers; the rest are functions
// called through out(), which pass the state and the bus along. The
// mapped copy runs from internal RAM next to the JIT's code pool, which
// its size takes from. ARM and Thumb share the semantics below (ALU,
// shifter, loads, stores, block transfers, exceptions); each passes its
// own widths and offsets as constants.
//
// Bus cycles follow the ARM7TDMI TRM's cycle tables (DDI 0029G, 6.2-6.19):
// the prefetch of PC + 2L in the first cycle of every instruction, S when
// the instruction before ended with a fetch (see advance()); N data accesses
// after it, S ones within a block transfer; internal cycles; and an N then S
// fetch to refill after a branch.
template<class BusType>
class Executor {
public:
    Executor(Arm7State& state, const BusType& bus) : s_(state), bus_(bus) {}

    void step() { run_one(s_, bus_); }

    // One instruction, with the state and the bus passed along, so that a
    // run loop keeps them in registers across the handlers' calls.
    static void run_one(Arm7State& s, BusType bus) {
        if (s.cpsr & kFlagT) run_thumb(s, bus);
        else run_arm(s, bus);
    }
    [[gnu::always_inline]] static void run_thumb(Arm7State& s, BusType bus) {
        const uint32_t op = s.pipeline[0] & 0xffff;
        kThumbHandlers[kThumbPositions[op >> 6]](s, bus, op);
    }
    // A condition other than AL selects the last handler, skip(), when it
    // fails, without a branch: the flags decide, which a branch predictor
    // guesses badly.
    [[gnu::always_inline]] static void run_arm(Arm7State& s, BusType bus) {
        const uint32_t op = s.pipeline[0];
        uint32_t position = kArmPositions[((op >> 16) & 0xff0) | ((op >> 4) & 0xf)];
        if (op >> 28 != 0xe) [[unlikely]] {
            const uint32_t pass = (kPass[op >> 28] >> (s.cpsr >> 28)) & 1;
            position ^= (position ^ kArmList.size()) & (pass - 1);
        }
        kArmHandlers[position](s, bus, op);
    }

    void interrupt() {
        // The return address is the next instruction's plus 4 in both states.
        const bool thumb = s_.cpsr & kFlagT;
        if (thumb) prefetch<true>();
        else prefetch<false>();
        out<&Executor::exception>(kModeIrq, 0x18, thumb ? s_.r[15] : s_.r[15] - 4);
    }

private:
    // The condition table of this copy: kConditions is one object, which
    // the linker takes from any file that uses it.
    static constexpr auto kPass = kConditions;

    // Calls member F out of line: its copy takes the state and the bus as
    // arguments, so that the handlers keep their Executor in registers. The
    // members called this way are inlined into their copy.
    template<class T>
    struct Outline;
    template<class R, class... P>
    struct Outline<R (Executor::*)(P...)> {
        template<R (Executor::*F)(P...)>
        [[gnu::noinline]] static R call(Arm7State& s, BusType bus, P... a) {
            return (Executor(s, bus).*F)(a...);
        }
    };
    template<class R, class... P>
    struct Outline<R (Executor::*)(P...) const> {
        template<R (Executor::*F)(P...) const>
        [[gnu::noinline]] static R call(Arm7State& s, BusType bus, P... a) {
            return (Executor(s, bus).*F)(a...);
        }
    };
    template<auto F>
    static void outline(Arm7State& s, BusType bus) {
        Outline<decltype(F)>::template call<F>(s, bus);
    }
    template<auto F, class... A>
    [[gnu::always_inline]] auto out(A... a) {
        return Outline<decltype(F)>::template call<F>(s_, bus_, a...);
    }

    // Data accesses: a bus may serve those it can at once inline
    // (MappedBus: IWRAM and EWRAM), the others go out of line.
    [[gnu::always_inline]] uint32_t read(uint32_t request, uint32_t address) {
        if constexpr (requires(uint32_t& value) { bus_.read_inline(0u, 0u, value); }) {
            uint32_t value;
            if (bus_.read_inline(request, address, value)) return value;
            return out<&Executor::read_any>(request, address);
        } else {
            return bus_.access(kRead | request, address);
        }
    }
    [[gnu::always_inline]] void write(uint32_t request, uint32_t address, uint32_t value) {
        if constexpr (requires { bus_.write_inline(0u, 0u, 0u); }) {
            if (!bus_.write_inline(request, address, value)) out<&Executor::write_any>(request, address, value);
        } else {
            bus_.access(kWrite | request, address, value);
        }
    }
    [[gnu::always_inline]] uint32_t read_any(uint32_t request, uint32_t address) {
        return bus_.access(kRead | request, address);
    }
    [[gnu::always_inline]] void write_any(uint32_t request, uint32_t address, uint32_t value) {
        bus_.access(kWrite | request, address, value);
    }
    void idle() { bus_.access(kIdle, 0); }
    // `count` internal cycles: at once where the bus can.
    void idle(unsigned count) {
        if constexpr (requires { bus_.idle(count); }) {
            bus_.idle(count);
        } else {
            for (unsigned i = 0; i < count; ++i) idle();
        }
    }

    // An ARM instruction whose condition fails: its prefetch.
    static void skip(Arm7State& s, BusType bus, uint32_t) { Executor(s, bus).template prefetch_last<false>(); }

    // The prefetch of every instruction. A bus may serve the ARM fetches it
    // can at once inline (MappedBus: IWRAM, where games run their ARM
    // code); the others go out of line, so that the handlers stay small.
    template<bool Thumb>
    [[gnu::always_inline]] void prefetch() {
        if constexpr (!Thumb && requires(uint32_t& opcode) { bus_.template fetch_inline<Thumb>(0u, opcode); }) {
            uint32_t opcode;
            if (bus_.template fetch_inline<Thumb>(s_.r[15], opcode)) {
                s_.pipeline[0] = s_.pipeline[1];
                s_.pipeline[1] = opcode;
                return;
            }
        }
        out<&Executor::template prefetch_any<Thumb>>();
    }

    // The out-of-line prefetch: the fetches a bus can make with no call
    // (MappedBus: IWRAM, and the ROM with relaxed timing) without a stack
    // frame, the rest through a tail call.
    template<bool Thumb>
    [[gnu::always_inline]] void prefetch_any() {
        if constexpr (requires(uint32_t& opcode) { bus_.template fetch_fast<Thumb>(0u, opcode); }) {
            uint32_t opcode;
            if (bus_.template fetch_fast<Thumb>(s_.r[15], opcode)) [[likely]] {
                s_.pipeline[0] = s_.pipeline[1];
                s_.pipeline[1] = opcode;
                return;
            }
            out<&Executor::template prefetch_bus<Thumb>>();
        } else {
            prefetch_bus<Thumb>();
        }
    }
    template<bool Thumb>
    [[gnu::always_inline]] void prefetch_bus() {
        const uint32_t type = s_.sequential ? kSequential : 0;
        const uint32_t opcode = bus_.access(kFetch | (Thumb ? kHalf : kWord) | type, s_.r[15]);
        s_.pipeline[0] = s_.pipeline[1];
        s_.pipeline[1] = opcode;
    }

    // The prefetch and advance() of an instruction whose only bus cycle is
    // its prefetch, made last, after it has written its registers: the
    // slow path is then a tail call and the handler needs no stack frame.
    // The order does not show, because what the bus may run at the fetch
    // (events, DMA, open bus) reads only R15, the pipeline and CPSR's I and
    // T, which such an instruction leaves as they were until the fetch.
    template<bool Thumb>
    [[gnu::always_inline]] void prefetch_last() {
        if constexpr (!Thumb && requires(uint32_t& opcode) { bus_.template fetch_inline<Thumb>(0u, opcode); }) {
            uint32_t opcode;
            if (bus_.template fetch_inline<Thumb>(s_.r[15], opcode)) {
                s_.pipeline[0] = s_.pipeline[1];
                s_.pipeline[1] = opcode;
                advance<Thumb>();
                return;
            }
        }
        out<&Executor::template prefetch_advance<Thumb>>();
    }
    template<bool Thumb>
    [[gnu::always_inline]] void prefetch_advance() {
        if constexpr (requires(uint32_t& opcode) { bus_.template fetch_fast<Thumb>(0u, opcode); }) {
            uint32_t opcode;
            if (bus_.template fetch_fast<Thumb>(s_.r[15], opcode)) [[likely]] {
                s_.pipeline[0] = s_.pipeline[1];
                s_.pipeline[1] = opcode;
                advance<Thumb>();
                return;
            }
            out<&Executor::template prefetch_bus_advance<Thumb>>();
        } else {
            prefetch_bus_advance<Thumb>();
        }
    }
    template<bool Thumb>
    [[gnu::always_inline]] void prefetch_bus_advance() {
        prefetch_bus<Thumb>();
        advance<Thumb>();
    }

    // B, BL's second half and the like, out of line, so that the handlers
    // end in a tail call: the prefetch, the refill from `target`, and for a
    // branch back the check for a wait loop. The handler has written R14.
    template<bool Thumb>
    [[gnu::always_inline]] void jump(uint32_t target, bool back) {
        const uint32_t from = s_.r[15] - (Thumb ? 4 : 8);
        if constexpr (requires(uint32_t& opcode) { bus_.template fetch_fast<Thumb>(0u, opcode); }) {
            uint32_t opcode;
            if (bus_.template fetch_fast<Thumb>(s_.r[15], opcode)) [[likely]] {
                s_.pipeline[0] = s_.pipeline[1];
                s_.pipeline[1] = opcode;
            } else {
                out<&Executor::template prefetch_bus<Thumb>>();
            }
        } else {
            prefetch<Thumb>();
        }
        fill<Thumb>(target);
        if (back) looped<Thumb>(s_.r[15] - (Thumb ? 4 : 8), from);
    }

    // Ends an instruction that did not write R15. The next fetch is S only
    // when this instruction's last cycle was a fetch. After a store the CPU
    // drives SEQ low (TRM 6.8). After internal cycles it drives SEQ high, but
    // on the GBA that fetch is 1N, GBATEK's "Prefetch Disable Bug" ("GBA
    // GamePak Prefetch"), and the single-step tests record N. With prefetch
    // enabled, milestone 2's bus applies the buffer's rules instead; it sees
    // the internal cycles.
    template<bool Thumb>
    void advance(bool sequential = true) {
        s_.r[15] += Thumb ? 2 : 4;
        s_.sequential = sequential;
    }

    // Ends an instruction that may have written R15 where ARM's documents
    // call it unpredictable and GBATEK does not allow it: a write-back to
    // R15, or R15 as the destination of MRS, a multiply or SWP. Every write to
    // R15 refills the pipeline from the value written, as LDR does. The
    // single-step tests refill from the value + 4 after a write-back by LDR,
    // STR and their halfword and signed forms, and do not refill after MRS;
    // their README's "Known Issues" says such cases may be wrong. The cases
    // are listed in docs/plans/gba/task-1-3.md.
    template<bool Thumb>
    void finish(bool wrote_pc, bool sequential) {
        if (wrote_pc) branch(s_.r[15]);
        else advance<Thumb>(sequential);
    }

    // Refills the pipeline from a new R15, forcibly aligned: GBATEK "ARM CPU
    // Memory Alignments" says R15 always is, so that a misaligned branch does
    // not affect later instructions that read it. The single-step tests
    // fetch from the aligned address but keep the low bits in R15; those
    // cases are listed in docs/plans/gba/task-1-3.md.
    template<bool Thumb>
    [[gnu::always_inline]] void refill(uint32_t target) {
        if constexpr (requires { bus_.jumped(); }) bus_.jumped();
        fill<Thumb>(target);
    }
    // The refill alone, for jump(), which tells the bus itself.
    template<bool Thumb>
    [[gnu::always_inline]] void fill(uint32_t target) {
        constexpr uint32_t kLength = Thumb ? 2 : 4, kWidth = Thumb ? kHalf : kWord;
#ifdef GBX_GBA_RELAXED_TIMING
        // Relaxed timing marks the second fetch too: the bus times both as
        // accesses, see MappedBus.
        constexpr uint32_t kSecond = kRefill;
#else
        constexpr uint32_t kSecond = 0;
#endif
        target &= ~(kLength - 1);
        s_.pipeline[0] = bus_.access(kFetch | kWidth | kRefill, target);
        s_.pipeline[1] = bus_.access(kFetch | kWidth | kSequential | kSecond, target + kLength);
        s_.r[15] = target + 2 * kLength;
        s_.sequential = true;
    }

    // A branch back from `branch` to `target`, instructions' addresses,
    // after its refill: the bus may skip the passes of a wait loop
    // (arm7_spin.h).
    template<bool Thumb>
    void looped(uint32_t target, uint32_t branch) {
        if constexpr (requires { bus_.looped(target, branch, Thumb); }) bus_.looped(target, branch, Thumb);
    }

    // A write to R15 in the state CPSR now selects.
    void branch(uint32_t target) {
        if (s_.cpsr & kFlagT) out<&Executor::template refill<true>>(target);
        else out<&Executor::template refill<false>>(target);
    }

    // The mapped bus checks whether the interrupt line can now be taken.
    [[gnu::always_inline]] void write_cpsr(uint32_t value) {
        arm7_switch_mode(s_, s_.cpsr, value);
        s_.cpsr = value;
        bus_.psr_written(value);
    }

    // MRS and MSR on the SPSR in a mode that has none read CPSR and write
    // nothing, as the single-step tests expect; GBATEK only says that User
    // and System mode have no SPSR.
    uint32_t spsr() const {
        const unsigned bank = bank_of(s_.cpsr);
        return bank ? s_.spsr[bank - 1] : s_.cpsr;
    }

    // Data processing with S writing R15 and LDM with S loading it return
    // from an exception: CPSR = SPSR of the current mode.
    void restore_cpsr() {
        if (const unsigned bank = bank_of(s_.cpsr)) out<&Executor::write_cpsr>(s_.spsr[bank - 1]);
    }

    // GBATEK "ARM CPU Exceptions": R14 of the new mode is the return address,
    // its SPSR the old CPSR; then ARM state, I set, and the vector. Only
    // reset and FIQ, not emulated, also set F.
    [[gnu::always_inline]] void exception(uint32_t mode, uint32_t vector, uint32_t return_address) {
        const uint32_t old = s_.cpsr;
        out<&Executor::write_cpsr>((old & ~(kModeMask | kFlagT)) | mode | kFlagI);
        s_.spsr[bank_of(mode) - 1] = old;
        s_.r[14] = return_address;
        out<&Executor::template refill<false>>(vector);
    }

    // Undefined instructions and absent coprocessors take a prefetch and an
    // internal cycle before the trap (TRM 6.18). The return address is the
    // next instruction's.
    template<bool Thumb>
    [[gnu::always_inline]] void undefined() {
        prefetch<Thumb>();
        idle();
        out<&Executor::exception>(kModeUndefined, 0x04, s_.r[15] - (Thumb ? 2 : 4));
    }

    template<bool Thumb>
    [[gnu::always_inline]] void software_interrupt() {
        prefetch<Thumb>();
        out<&Executor::exception>(kModeSupervisor, 0x08, s_.r[15] - (Thumb ? 2 : 4));
    }

    uint32_t carry() const { return (s_.cpsr >> 29) & 1; }

    void set_nz(uint32_t result) {
        s_.cpsr = (s_.cpsr & ~(kFlagN | kFlagZ)) | (result & kFlagN) | (result ? 0 : kFlagZ);
    }

    void set_nzc(uint32_t result, uint32_t carry) {
        s_.cpsr = (s_.cpsr & ~(kFlagN | kFlagZ | kFlagC)) | (result & kFlagN) | (result ? 0 : kFlagZ) | (carry << 29);
    }

    void set_nzcv(uint32_t result, uint32_t carry, uint32_t overflow) {
        s_.cpsr = (s_.cpsr & 0x0fffffff) | (result & kFlagN) | (result ? 0 : kFlagZ) | (carry << 29) | (overflow << 28);
    }

    // a + b + carry_in. Subtraction is a + ~b + carry_in, with C = no borrow.
    uint32_t add(uint32_t a, uint32_t b, uint32_t carry_in, bool set) {
        const uint64_t wide = uint64_t(a) + b + carry_in;
        const uint32_t result = static_cast<uint32_t>(wide);
        if (set) set_nzcv(result, static_cast<uint32_t>(wide >> 32), ((a ^ result) & (b ^ result)) >> 31);
        return result;
    }
    // add() with a carry in of 0, and of 1 with b inverted, without the
    // wide sum.
    uint32_t sum(uint32_t a, uint32_t b, bool set) {
        const uint32_t result = a + b;
        if (set) set_nzcv(result, result < a, ((a ^ result) & (b ^ result)) >> 31);
        return result;
    }
    uint32_t difference(uint32_t a, uint32_t b, bool set) {
        const uint32_t result = a - b;
        if (set) set_nzcv(result, a >= b, ((a ^ b) & (a ^ result)) >> 31);
        return result;
    }

    // The ALU operations of GBATEK "ARM Opcodes: Data Processing (ALU)". The
    // logical ones set C from the shifter, the arithmetic ones from the adder.
    uint32_t alu(unsigned opcode, uint32_t a, uint32_t b, uint32_t shifter_carry, bool set) {
        uint32_t result;
        switch (opcode) {
        case 0x0: case 0x8: result = a & b; break;
        case 0x1: case 0x9: result = a ^ b; break;
        case 0x2: case 0xa: return difference(a, b, set);
        case 0x3: return difference(b, a, set);
        case 0x4: case 0xb: return sum(a, b, set);
        case 0x5: return add(a, b, carry(), set);
        case 0x6: return add(a, ~b, carry(), set);
        case 0x7: return add(b, ~a, carry(), set);
        case 0xc: result = a | b; break;
        case 0xd: result = b; break;
        case 0xe: result = a & ~b; break;
        default: result = ~b; break;
        }
        if (set) set_nzc(result, shifter_carry);
        return result;
    }

    // alu() with the operation and S fixed.
    template<unsigned Opcode, bool Set>
    [[gnu::always_inline]] uint32_t alu(uint32_t a, uint32_t b, uint32_t shifter_carry) {
        if constexpr (Opcode == 0x2 || Opcode == 0xa) return difference(a, b, Set);
        else if constexpr (Opcode == 0x3) return difference(b, a, Set);
        else if constexpr (Opcode == 0x4 || Opcode == 0xb) return sum(a, b, Set);
        else if constexpr (Opcode == 0x5) return add(a, b, carry(), Set);
        else if constexpr (Opcode == 0x6) return add(a, ~b, carry(), Set);
        else if constexpr (Opcode == 0x7) return add(b, ~a, carry(), Set);
        else {
            uint32_t result;
            if constexpr (Opcode == 0x0 || Opcode == 0x8) result = a & b;
            else if constexpr (Opcode == 0x1 || Opcode == 0x9) result = a ^ b;
            else if constexpr (Opcode == 0xc) result = a | b;
            else if constexpr (Opcode == 0xd) result = b;
            else if constexpr (Opcode == 0xe) result = a & ~b;
            else result = ~b;
            if constexpr (Set) set_nzc(result, shifter_carry);
            return result;
        }
    }

    // shift_immediate() without C, for the operations that do not set it.
    uint32_t shift_value(uint32_t type, uint32_t value, uint32_t amount) const {
        switch (type) {
        case 0: return value << amount;
        case 1: return amount ? value >> amount : 0;
        case 2: return static_cast<uint32_t>(static_cast<int32_t>(value) >> (amount ? amount : 31));
        default: return amount ? std::rotr(value, static_cast<int>(amount)) : (carry() << 31) | (value >> 1);
        }
    }

    // The barrel shifter with an immediate amount (GBATEK "ARM Opcodes: Data
    // Processing (ALU)"): amount 0 is LSL #0, which keeps C, LSR #32, ASR #32
    // or RRX.
    static uint32_t shift_immediate(uint32_t type, uint32_t value, uint32_t amount, uint32_t& carry) {
        switch (type) {
        case 0:
            if (amount == 0) return value;
            carry = (value >> (32 - amount)) & 1;
            return value << amount;
        case 1:
            if (amount == 0) { carry = value >> 31; return 0; }
            carry = (value >> (amount - 1)) & 1;
            return value >> amount;
        case 2:
            if (amount == 0) amount = 32;
            carry = (static_cast<int32_t>(value) >> (amount - 1)) & 1;
            return static_cast<uint32_t>(static_cast<int32_t>(value) >> (amount == 32 ? 31 : amount));
        default:
            if (amount == 0) {
                const uint32_t result = (carry << 31) | (value >> 1);
                carry = value & 1;
                return result;
            }
            carry = (value >> (amount - 1)) & 1;
            return std::rotr(value, static_cast<int>(amount));
        }
    }

    // With the amount in a register's low byte, 0 keeps value and C, and
    // amounts of 32 and more shift every bit out (ROR by a multiple of 32
    // keeps the value and sets C from bit 31).
    static uint32_t shift_register(uint32_t type, uint32_t value, uint32_t amount, uint32_t& carry) {
        if (amount == 0) return value;
        switch (type) {
        case 0:
            if (amount < 32) { carry = (value >> (32 - amount)) & 1; return value << amount; }
            carry = amount == 32 ? value & 1 : 0;
            return 0;
        case 1:
            if (amount < 32) { carry = (value >> (amount - 1)) & 1; return value >> amount; }
            carry = amount == 32 ? value >> 31 : 0;
            return 0;
        case 2:
            if (amount < 32) {
                carry = (value >> (amount - 1)) & 1;
                return static_cast<uint32_t>(static_cast<int32_t>(value) >> amount);
            }
            carry = value >> 31;
            return static_cast<uint32_t>(static_cast<int32_t>(value) >> 31);
        default:
            amount &= 31;
            if (amount == 0) { carry = value >> 31; return value; }
            carry = (value >> (amount - 1)) & 1;
            return std::rotr(value, static_cast<int>(amount));
        }
    }

    // Loads with the ARM7's misaligned behaviour (GBATEK "ARM CPU Memory
    // Alignments"): a word rotates by the low address bits, LDRH at an odd
    // address by 8, LDRSH at an odd address sign-extends the byte there.
    uint32_t load_word(uint32_t address, uint32_t request = 0) {
        return word_value(read(kWord | request, address), address);
    }
    uint32_t load_half(uint32_t address) { return half_value(read(kHalf, address), address); }
    uint32_t load_signed_half(uint32_t address) { return signed_half_value(read(kHalf, address), address); }
    uint32_t load_signed_byte(uint32_t address) { return signed_byte_value(read(kByte, address)); }
    static uint32_t word_value(uint32_t value, uint32_t address) {
        return std::rotr(value, static_cast<int>((address & 3) * 8));
    }
    static uint32_t half_value(uint32_t value, uint32_t address) {
        return std::rotr(value, static_cast<int>((address & 1) * 8));
    }
    static uint32_t signed_half_value(uint32_t value, uint32_t address) {
        return address & 1 ? static_cast<uint32_t>(static_cast<int8_t>(value >> 8))
                           : static_cast<uint32_t>(static_cast<int16_t>(value));
    }
    static uint32_t signed_byte_value(uint32_t value) { return static_cast<uint32_t>(static_cast<int8_t>(value)); }

    // The end of a single load: an internal cycle, then the register, written
    // after any write-back, or a refill when it is R15 (TRM 6.7).
    template<bool Thumb>
    void finish_load(unsigned rd, uint32_t value, bool wrote_pc = false) {
        idle();
        s_.r[rd] = value;
        finish<Thumb>(wrote_pc || rd == 15, false);
    }

    // R0-R15 of User and System mode for LDM and STM with S, whatever the
    // current mode.
    uint32_t& user_register(unsigned index) {
        const unsigned bank = bank_of(s_.cpsr);
        if (index == 15) return s_.r[15];
        if (index >= 13) return bank ? s_.r13[0][index - 13] : s_.r[index];
        if (index >= 8 && bank == kFiqBank) return s_.r8[0][index - 8];
        return s_.r[index];
    }

    // LDM and STM, and Thumb's PUSH, POP, LDMIA and STMIA (GBATEK "ARM
    // Opcodes: Memory: Block Data Transfer (LDM, STM)"). The lowest register
    // goes to the lowest address. An empty list transfers R15 and moves the
    // base by 40h. The base is written back in the cycle of the first access:
    // a load into it wins, and STM stores the old base only when it is the
    // first register. STM stores R15 as STR does, as the instruction's
    // address + 3L (GBATEK "ARM Opcodes: Memory: Single Data Transfer" for
    // ARM, the single-step tests for Thumb's empty list). With S, R15 in the
    // list of an LDM also restores CPSR from SPSR; otherwise S transfers User
    // mode's registers, and the write-back, which GBATEK says not to use
    // then, goes to User mode's base register too, as the single-step tests
    // expect.
    template<bool Thumb>
    [[gnu::always_inline]] void block_transfer(unsigned rn, uint32_t list, bool before, bool up, bool writeback, bool load, bool user) {
        const uint32_t base = s_.r[rn];
        const unsigned count = list ? count_bits(list) : 16;
        if (!list) list = 1u << 15;
        uint32_t address = up ? base + (before ? 4 : 0) : base - 4 * count + (before ? 0 : 4);
        const uint32_t moved = up ? base + 4 * count : base - 4 * count;
        if constexpr (requires(uint32_t& v) { bus_.template block_inline<Thumb>(0u, 0u, 0u, false, v, v); }) {
            // Without R15 or S, all at once when the bus can (MappedBus:
            // IWRAM or EWRAM), in the same order.
            uint32_t opcode, mask;
            uint8_t* data;
            if (!user && !(list & (1u << 15)) && !(writeback && rn == 15) &&
                (data = bus_.template block_inline<Thumb>(s_.r[15], address, count, load, opcode, mask))) {
                s_.pipeline[0] = s_.pipeline[1];
                s_.pipeline[1] = opcode;
                bool first = true;
                for (uint32_t pending = list; pending; pending &= pending - 1) {
                    const unsigned index = lowest_bit(pending);
                    uint8_t* const word = data + (address & mask & ~3u);
                    if (load) {
                        uint32_t value;
                        __builtin_memcpy(&value, word, 4);
                        if (first && writeback) s_.r[rn] = moved;
                        s_.r[index] = value;
                    } else {
                        __builtin_memcpy(word, &s_.r[index], 4);
                        if (first && writeback) s_.r[rn] = moved;
                    }
                    first = false;
                    address += 4;
                }
                advance<Thumb>(false);
                return;
            }
        }
        const bool user_bank = user && !(load && (list & (1u << 15)));
        uint32_t& written = user_bank ? user_register(rn) : s_.r[rn];
        prefetch<Thumb>();
        uint32_t request = 0;
        if (load) {
            bool first = true;
            for (uint32_t pending = list; pending; pending &= pending - 1) {
                const unsigned index = lowest_bit(pending);
                const uint32_t value = read(kWord | request, address);
                if (first && writeback) written = moved;
                first = false;
                if (user_bank) user_register(index) = value;
                else s_.r[index] = value;
                address += 4;
                request = kSequential;
            }
            idle();
            if (list & (1u << 15)) {
                if (user) restore_cpsr();
                branch(s_.r[15]);
                return;
            }
            finish<Thumb>(writeback && rn == 15, false);
            return;
        }
        for (uint32_t pending = list; pending; pending &= pending - 1) {
            const unsigned index = lowest_bit(pending);
            uint32_t value = user_bank ? user_register(index) : s_.r[index];
            // R15 is the last register, so as the base it holds the new base
            // unless it is the only one.
            if (index == 15 && !(request && writeback && rn == 15)) value += Thumb ? 2 : 4;
            write(kWord | request, address, value);
            if (!request && writeback) written = moved;
            address += 4;
            request = kSequential;
        }
        finish<Thumb>(writeback && rn == 15, false);
    }

    // The ARM handlers: the key of a decode index is the kind in bits 8 and
    // up, and the bits the kind's handler is specialized on below.
    enum ArmKind : uint32_t {
        kArmData,           // Operation and S; a register operand.
        kArmDataImmediate,  // Operation and S.
        kArmPsr,            // MRS, MSR and BX.
        kArmMultiply,       // A and S.
        kArmMultiplyLong,
        kArmSwap,
        kArmHalfword,  // L and the SH bits.
        kArmSingle,    // A register offset, B and L.
        kArmBlock,     // L.
        kArmBranch,    // L, the link.
        kArmUndefined,
        kArmCoprocessor,  // SWI, CP14's MRC and MCR, and the undefined rest.
    };

    static constexpr uint32_t arm_key(unsigned index) {
        const uint32_t op = (index >> 4) << 20 | (index & 15) << 4;
        const auto key = [](uint32_t kind, uint32_t bits) { return kind << 8 | bits; };
        const uint32_t load = (op >> 20) & 1;
        switch ((op >> 25) & 7) {
        case 0:
            if ((op & 0x90) == 0x90) {
                if (op & 0x60) return key(kArmHalfword, load << 2 | ((op >> 5) & 3));
                if (op & (1u << 24)) return key(kArmSwap, 0);
                if (op & (1u << 23)) return key(kArmMultiplyLong, 0);
                return key(kArmMultiply, (op >> 20) & 3);
            }
            if ((op & 0x01900000) == 0x01000000) return key(kArmPsr, 0);
            return key(kArmData, (op >> 20) & 0x1f);
        case 1:
            if ((op & 0x01900000) == 0x01000000) return key(kArmPsr, 0);
            return key(kArmDataImmediate, (op >> 20) & 0x1f);
        case 2: return key(kArmSingle, ((op >> 21) & 2) | load);
        case 3:
            if (op & 0x10) return key(kArmUndefined, 0);
            return key(kArmSingle, 4 | ((op >> 21) & 2) | load);
        case 4: return key(kArmBlock, load);
        case 5: return key(kArmBranch, (op >> 24) & 1);
        case 6: return key(kArmUndefined, 0);  // LDC and STC: no coprocessor answers.
        default: return key(kArmCoprocessor, 0);
        }
    }

    template<uint32_t Key>
    [[gnu::always_inline]] void arm_handler(uint32_t op) {
        constexpr uint32_t kind = Key >> 8, bits = Key & 0xff;
        if constexpr (kind == kArmData || kind == kArmDataImmediate) {
            data<(bits >> 1) & 15, bits & 1, kind == kArmDataImmediate>(op);
        } else if constexpr (kind == kArmPsr) {
            if ((op & 0x0ffffff0) == 0x012fff10) branch_exchange(op);
            else out<&Executor::psr_transfer>(op);
        } else if constexpr (kind == kArmMultiply) {
            multiply<(bits & 2) != 0, (bits & 1) != 0>(op);
        } else if constexpr (kind == kArmMultiplyLong) {
            out<&Executor::multiply_long>(op);
        } else if constexpr (kind == kArmSwap) {
            out<&Executor::swap>(op);
        } else if constexpr (kind == kArmHalfword) {
            halfword<(bits >> 2) & 1, bits & 3>(op);
        } else if constexpr (kind == kArmSingle) {
            single<(bits & 4) != 0, (bits & 2) != 0, (bits & 1) != 0>(op);
        } else if constexpr (kind == kArmBlock) {
            out<&Executor::template block_transfer<false>>((op >> 16) & 15, op & 0xffff, op & (1u << 24), op & (1u << 23), op & (1u << 21),
                                  bits, op & (1u << 22));
        } else if constexpr (kind == kArmBranch) {
            if constexpr (bits) s_.r[14] = s_.r[15] - 4;
            const auto offset = static_cast<int32_t>(op << 8) >> 6;
            out<&Executor::template jump<false>>(s_.r[15] + static_cast<uint32_t>(offset), offset < -4);
        } else if constexpr (kind == kArmUndefined) {
            out<&Executor::template undefined<false>>();
        } else {
            if (op & (1u << 24)) out<&Executor::template software_interrupt<false>>();
            else if ((op & 0x10) && ((op >> 8) & 15) == 14) out<&Executor::debug_channel>(op);
            else out<&Executor::template undefined<false>>();  // CDP, and MRC and MCR on the other coprocessors.
        }
    }

    // Data processing with the operation and S fixed, and whether the
    // second operand is an immediate (GBATEK "ARM Opcodes: Data Processing
    // (ALU)"). A shift by a register and Rd = R15 go out of line. Only the
    // logical operations with S take C from the shifter.
    template<unsigned Opcode, bool Set, bool Immediate>
    [[gnu::always_inline]] void data(uint32_t op) {
        constexpr bool kShifterCarry = Set && ((Opcode & 0xc) == 0xc || (Opcode & 0x6) == 0);
        uint32_t shifter_carry = kShifterCarry ? carry() : 0, b;
        if constexpr (Immediate) {
            const uint32_t rotate = (op >> 7) & 30;
            b = std::rotr(op & 0xff, static_cast<int>(rotate));
            if (kShifterCarry && rotate) shifter_carry = b >> 31;
        } else {
            if (op & 0x10) {
                out<&Executor::data_processing>(op);
                return;
            }
            const uint32_t value = s_.r[op & 15], amount = (op >> 7) & 31;
            if (!(op & 0x60)) {
                b = value << amount;
                if (kShifterCarry && amount) shifter_carry = (value >> (32 - amount)) & 1;
            } else if constexpr (kShifterCarry) {
                b = shift_immediate((op >> 5) & 3, value, amount, shifter_carry);
            } else {
                b = shift_value((op >> 5) & 3, value, amount);
            }
        }
        const uint32_t a = s_.r[(op >> 16) & 15];
        const unsigned rd = (op >> 12) & 15;
        if (rd == 15) {
            out<&Executor::data_to_pc_fetch>(op, a, b, shifter_carry);
            return;
        }
        // TST, TEQ, CMP and CMN without S decode as PSR transfers.
        if constexpr ((Opcode & 0xc) == 0x8) alu<Opcode, true>(a, b, shifter_carry);
        else s_.r[rd] = alu<Opcode, Set>(a, b, shifter_carry);
        prefetch_last<false>();
    }

    // MRC and MCR on CP14, the ICEbreaker's debug communications channel.
    // GBATEK "ARM CP14 ICEbreaker Debug Communications Channel": on the GBA
    // they raise no exception, and any MRC reads the opcode prefetched from
    // $+8, which alyosha's arm/Coprocessor_14 checks on hardware. MRC into
    // R15 sets only NZCV from bits 31-28 (GBATEK "ARM Opcodes: Coprocessor
    // Instructions"); MCR has no effect. The cycles are those of a ready
    // coprocessor (TRM 6.16, 6.17): the prefetch, the transfer, which
    // requests no memory and goes to the bus as an internal cycle, and for
    // MRC an internal cycle. The single-step tests take the undefined
    // instruction trap instead, see docs/plans/gba/task-1-3.md.
    [[gnu::always_inline]] void debug_channel(uint32_t op) {
        prefetch<false>();
        idle();
        if (op & (1u << 20)) {
            const uint32_t value = s_.pipeline[1];
            idle();
            const unsigned rd = (op >> 12) & 15;
            if (rd == 15) s_.cpsr = (s_.cpsr & 0x0fffffff) | (value & 0xf0000000);
            else s_.r[rd] = value;
        }
        advance<false>(false);
    }

    // Data processing with a shift by a register, which takes an internal
    // cycle. Rs is read in the first cycle, the other operands in the
    // second, after the PC has moved on: R15 reads as PC + 12 (GBATEK "ARM
    // Opcodes: Data Processing (ALU)"), and as PC + 8 for Rs, which GBATEK
    // does not allow, as the single-step tests expect.
    [[gnu::always_inline]] void data_processing(uint32_t op) {
        const unsigned opcode = (op >> 21) & 15, rd = (op >> 12) & 15;
        uint32_t shifter_carry = carry();
        const uint32_t amount = s_.r[(op >> 8) & 15] & 0xff;
        prefetch<false>();
        idle();
        s_.r[15] += 4;
        const uint32_t b = shift_register((op >> 5) & 3, s_.r[op & 15], amount, shifter_carry);
        const uint32_t a = s_.r[(op >> 16) & 15];
        s_.r[15] -= 4;
        if (rd == 15) {
            out<&Executor::data_to_pc>(op, a, b, shifter_carry, false);
            return;
        }
        // TST, TEQ, CMP and CMN without S decode as PSR transfers.
        const uint32_t result = alu(opcode, a, b, shifter_carry, op & (1u << 20));
        if ((opcode & 0xc) != 0x8) s_.r[rd] = result;
        advance<false>(false);
    }

    // The end of data processing with Rd = R15. With S, CPSR = SPSR of the
    // current mode (GBATEK "ARM Opcodes: Data Processing (ALU)"), also for
    // TST, TEQ, CMP and CMN, which do not branch; ARMv2's TEQP and the like
    // changed R15's PSR bits instead. In a mode without an SPSR, the flags
    // are set as for any other Rd. Both as the single-step tests expect;
    // GBATEK only says these are not to be used in User mode. With an
    // immediate shift the prefetch is still to come.
    [[gnu::always_inline]] void data_to_pc_fetch(uint32_t op, uint32_t a, uint32_t b, uint32_t shifter_carry) {
        prefetch<false>();
        data_to_pc(op, a, b, shifter_carry, true);
    }
    [[gnu::always_inline]] void data_to_pc(uint32_t op, uint32_t a, uint32_t b, uint32_t shifter_carry, bool sequential) {
        const unsigned opcode = (op >> 21) & 15;
        const bool set = op & (1u << 20);
        const bool restore = set && bank_of(s_.cpsr);
        if ((opcode & 0xc) == 0x8) {
            alu(opcode, a, b, shifter_carry, !restore);
            if (restore) {
                // An interrupt the line already holds still enters after
                // this instruction when it sets I (alyosha psr/readme.txt).
                const bool unmasked = !(s_.cpsr & kFlagI);
                restore_cpsr();
                if (unmasked) bus_.irq_masked_late();
            }
            advance<false>(sequential);
            return;
        }
        const uint32_t result = alu(opcode, a, b, shifter_carry, set && !restore);
        if (restore) restore_cpsr();
        branch(result);
    }

    // MRS and MSR (GBATEK "ARM Opcodes: PSR Transfer (MRS, MSR)"). MSR writes
    // the fields selected by bits 16-19; in User mode only the flags of
    // CPSR. Mode bit 4 always reads 1, and the T bit and the reserved bits
    // are written as the single-step tests expect, though GBATEK says
    // programs must not change them.
    [[gnu::always_inline]] void psr_transfer(uint32_t op) {
        const bool saved = op & (1u << 22);
        if (!(op & (1u << 21))) {
            prefetch<false>();
            const unsigned rd = (op >> 12) & 15;
            s_.r[rd] = saved ? spsr() : s_.cpsr;
            finish<false>(rd == 15, true);
            return;
        }
        const uint32_t value =
            (op & (1u << 25)) ? std::rotr(op & 0xff, static_cast<int>((op >> 7) & 30)) : s_.r[op & 15];
        prefetch<false>();
        uint32_t mask = 0;
        if (op & (1u << 16)) mask |= 0x000000ff;
        if (op & (1u << 17)) mask |= 0x0000ff00;
        if (op & (1u << 18)) mask |= 0x00ff0000;
        if (op & (1u << 19)) mask |= 0xff000000;
        if (saved) {
            if (const unsigned bank = bank_of(s_.cpsr))
                s_.spsr[bank - 1] = (s_.spsr[bank - 1] & ~mask) | (value & mask);
        } else {
            if ((s_.cpsr & kModeMask) == kModeUser) mask &= 0xff000000;
            out<&Executor::write_cpsr>((s_.cpsr & ~mask) | (value & mask) | 0x10);
        }
        advance<false>();
    }

    void branch_exchange(uint32_t op) {
        const uint32_t target = s_.r[op & 15];
        prefetch<false>();
        if (target & 1) {
            s_.cpsr |= kFlagT;
            out<&Executor::template refill<true>>(target);
        } else {
            out<&Executor::template refill<false>>(target);
        }
    }

    // A register read after the first cycle, when R15 reads as PC + 12.
    uint32_t late_register(unsigned index) const { return s_.r[index] + (index == 15 ? 4 : 0); }

    // The multiplies read their operands after the prefetch, as the
    // single-step tests expect: R15, which GBATEK does not allow, reads as
    // PC + 12.
    template<bool Accumulate, bool Set>
    [[gnu::always_inline]] void multiply(uint32_t op) {
        const unsigned rd = (op >> 16) & 15;
        constexpr bool accumulate = Accumulate;
        prefetch<false>();
        const uint32_t multiplicand = late_register(op & 15), multiplier = late_register((op >> 8) & 15);
        const uint32_t addend = accumulate ? late_register((op >> 12) & 15) : 0;
        const unsigned cycles = multiply_cycles(multiplier, true);
        idle(cycles + accumulate);
        const uint32_t result = multiplicand * multiplier + addend;
        if constexpr (Set) set_nzc(result, multiply_carry(multiplicand, multiplier, addend, true, false, cycles));
        s_.r[rd] = result;
        finish<false>(rd == 15, false);
    }

    [[gnu::always_inline]] void multiply_long(uint32_t op) {
        const unsigned high = (op >> 16) & 15, low = (op >> 12) & 15;
        const bool is_signed = op & (1u << 22), accumulate = op & (1u << 21);
        prefetch<false>();
        const uint32_t multiplicand = late_register(op & 15), multiplier = late_register((op >> 8) & 15);
        const uint64_t addend = accumulate ? (uint64_t(late_register(high)) << 32) | late_register(low) : 0;
        const unsigned cycles = multiply_cycles(multiplier, is_signed);
        idle(cycles + 1 + accumulate);
        const uint64_t product = is_signed ? static_cast<uint64_t>(int64_t(static_cast<int32_t>(multiplicand)) *
                                                                   static_cast<int32_t>(multiplier))
                                           : uint64_t(multiplicand) * multiplier;
        const uint64_t result = product + addend;
        if (op & (1u << 20)) {
            const uint32_t c = multiply_carry(multiplicand, multiplier, addend, is_signed, true, cycles);
            s_.cpsr = (s_.cpsr & ~(kFlagN | kFlagZ | kFlagC)) | (static_cast<uint32_t>(result >> 32) & kFlagN) |
                      (result ? 0 : kFlagZ) | (c << 29);
        }
        s_.r[low] = static_cast<uint32_t>(result);
        s_.r[high] = static_cast<uint32_t>(result >> 32);
        finish<false>(low == 15 || high == 15, false);
    }

    // SWP reads, then writes, with LOCK through both (TRM 6.11); the word
    // read rotates like LDR's, the write does not. Its registers are read
    // after the prefetch, R15 as PC + 12, as the single-step tests expect.
    [[gnu::always_inline]] void swap(uint32_t op) {
        prefetch<false>();
        const uint32_t address = late_register((op >> 16) & 15), source = late_register(op & 15);
        uint32_t value;
        if (op & (1u << 22)) {
            value = read(kByte | kLock, address);
            write(kByte | kLock, address, source & 0xff);
        } else {
            value = load_word(address, kLock);
            write(kWord | kLock, address, source);
        }
        idle();
        const unsigned rd = (op >> 12) & 15;
        s_.r[rd] = value;
        finish<false>(rd == 15, false);
    }

    // LDR, STR, LDRB and STRB, with a register offset, B and L fixed.
    // Post-indexing always writes back. A load into the base wins over the
    // write-back, a store of R15 stores PC + 12. T (LDRT and the like) only
    // changes nTRANS, which the GBA does not use.
    template<bool Register, bool Byte, bool Load>
    [[gnu::always_inline]] void single(uint32_t op) {
        if constexpr (requires(uint32_t& v) { bus_.template transfer_inline<false, Load>(0u, 0u, 0u, v, v); }) {
            const unsigned rn = (op >> 16) & 15, rd = (op >> 12) & 15;
            uint32_t offset;
            if constexpr (Register) offset = shift_value((op >> 5) & 3, s_.r[op & 15], (op >> 7) & 31);
            else offset = op & 0xfff;
            const uint32_t base = s_.r[rn];
            const uint32_t moved = op & (1u << 23) ? base + offset : base - offset;
            const uint32_t address = op & (1u << 24) ? moved : base;
            const bool writeback = !(op & (1u << 24)) || (op & (1u << 21));
            uint32_t opcode, value = Load ? 0 : s_.r[rd] & (Byte ? 0xff : ~0u);
            if (rd != 15 && !(writeback && rn == 15) &&
                bus_.template transfer_inline<false, Load>(s_.r[15], address, Byte ? 0 : 2, opcode, value)) {
                s_.pipeline[0] = s_.pipeline[1];
                s_.pipeline[1] = opcode;
                if (writeback) s_.r[rn] = moved;
                if constexpr (Load) s_.r[rd] = Byte ? value : word_value(value, address);
                advance<false>(false);
                return;
            }
            out<&Executor::template single_any<Register, Byte, Load>>(op);
        } else {
            single_any<Register, Byte, Load>(op);
        }
    }
    template<bool Register, bool Byte, bool Load>
    [[gnu::always_inline]] void single_any(uint32_t op) {
        const unsigned rn = (op >> 16) & 15, rd = (op >> 12) & 15;
        uint32_t offset;
        if constexpr (Register) {
            uint32_t ignored = carry();
            offset = shift_immediate((op >> 5) & 3, s_.r[op & 15], (op >> 7) & 31, ignored);
        } else {
            offset = op & 0xfff;
        }
        const uint32_t base = s_.r[rn];
        const uint32_t moved = op & (1u << 23) ? base + offset : base - offset;
        const uint32_t address = op & (1u << 24) ? moved : base;
        const bool writeback = !(op & (1u << 24)) || (op & (1u << 21));
        prefetch<false>();
        if constexpr (Load) {
            const uint32_t value = Byte ? read(kByte, address) : load_word(address);
            if (writeback) s_.r[rn] = moved;
            finish_load<false>(rd, value, writeback && rn == 15);
        } else {
            const uint32_t value = s_.r[rd] + (rd == 15 ? 4 : 0);
            if constexpr (Byte) write(kByte, address, value & 0xff);
            else write(kWord, address, value);
            if (writeback) s_.r[rn] = moved;
            finish<false>(writeback && rn == 15, false);
        }
    }

    // LDRH, STRH, LDRSB and LDRSH (GBATEK "ARM Opcodes: Memory: Halfword,
    // Doubleword, and Signed Data Transfer"), with L and the SH bits fixed.
    // The stores with S set are ARMv5's LDRD and STRD; no source gives what
    // an ARM7TDMI does with them, and here they store a halfword like STRH.
    template<bool Load, unsigned Type>
    [[gnu::always_inline]] void halfword(uint32_t op) {
        if constexpr (requires(uint32_t& v) { bus_.template transfer_inline<false, Load>(0u, 0u, 0u, v, v); }) {
            const unsigned rn = (op >> 16) & 15, rd = (op >> 12) & 15;
            const uint32_t offset = op & (1u << 22) ? ((op >> 4) & 0xf0) | (op & 0xf) : s_.r[op & 15];
            const uint32_t base = s_.r[rn];
            const uint32_t moved = op & (1u << 23) ? base + offset : base - offset;
            const uint32_t address = op & (1u << 24) ? moved : base;
            const bool writeback = !(op & (1u << 24)) || (op & (1u << 21));
            uint32_t opcode, value = Load ? 0 : s_.r[rd] & 0xffff;
            if (rd != 15 && !(writeback && rn == 15) &&
                bus_.template transfer_inline<false, Load>(s_.r[15], address, Load && Type == 2 ? 0 : 1, opcode, value)) {
                s_.pipeline[0] = s_.pipeline[1];
                s_.pipeline[1] = opcode;
                if (writeback) s_.r[rn] = moved;
                if constexpr (Load) {
                    if constexpr (Type == 1) s_.r[rd] = half_value(value, address);
                    else if constexpr (Type == 2) s_.r[rd] = signed_byte_value(value);
                    else s_.r[rd] = signed_half_value(value, address);
                }
                advance<false>(false);
                return;
            }
            out<&Executor::template halfword_any<Load, Type>>(op);
        } else {
            halfword_any<Load, Type>(op);
        }
    }
    template<bool Load, unsigned Type>
    [[gnu::always_inline]] void halfword_any(uint32_t op) {
        const unsigned rn = (op >> 16) & 15, rd = (op >> 12) & 15;
        const uint32_t offset = op & (1u << 22) ? ((op >> 4) & 0xf0) | (op & 0xf) : s_.r[op & 15];
        const uint32_t base = s_.r[rn];
        const uint32_t moved = op & (1u << 23) ? base + offset : base - offset;
        const uint32_t address = op & (1u << 24) ? moved : base;
        const bool writeback = !(op & (1u << 24)) || (op & (1u << 21));
        prefetch<false>();
        if constexpr (Load) {
            uint32_t value;
            if constexpr (Type == 1) value = load_half(address);
            else if constexpr (Type == 2) value = load_signed_byte(address);
            else value = load_signed_half(address);
            if (writeback) s_.r[rn] = moved;
            finish_load<false>(rd, value, writeback && rn == 15);
        } else {
            write(kHalf, address, (s_.r[rd] + (rd == 15 ? 4 : 0)) & 0xffff);
            if (writeback) s_.r[rn] = moved;
            finish<false>(writeback && rn == 15, false);
        }
    }

    // The Thumb handlers, keyed as the ARM ones, by format.
    enum ThumbKind : uint32_t {
        kThumbShift,         // THUMB.1, the shift.
        kThumbAddSubtract,   // THUMB.2, the immediate and subtract bits.
        kThumbImmediate,     // THUMB.3, the operation.
        kThumbAlu,           // THUMB.4, the operation.
        kThumbHigh,          // THUMB.5, the operation.
        kThumbPcLoad,        // THUMB.6.
        kThumbRegister,      // THUMB.7 and THUMB.8, the operation.
        kThumbOffset,        // THUMB.9, B and L.
        kThumbHalfword,      // THUMB.10, L.
        kThumbStack,         // THUMB.11, L.
        kThumbAddress,       // THUMB.12, SP.
        kThumbAddSp,         // THUMB.13.
        kThumbPush,          // THUMB.14.
        kThumbPop,
        kThumbMultiple,      // THUMB.15, L.
        kThumbConditional,   // THUMB.16.
        kThumbSwi,           // THUMB.17.
        kThumbBranch,        // THUMB.18.
        kThumbLinkHigh,      // THUMB.19.
        kThumbLinkLow,
        kThumbUndefined,
    };

    static constexpr uint32_t thumb_key(unsigned index) {
        const uint32_t op = index << 6;
        const auto key = [](uint32_t kind, uint32_t bits) { return kind << 8 | bits; };
        switch (op >> 11) {
        case 0x00: case 0x01: case 0x02: return key(kThumbShift, op >> 11);
        case 0x03: return key(kThumbAddSubtract, (op >> 9) & 3);
        case 0x04: case 0x05: case 0x06: case 0x07: return key(kThumbImmediate, (op >> 11) & 3);
        case 0x08:
            if (op & (1u << 10)) return key(kThumbHigh, (op >> 8) & 3);
            return key(kThumbAlu, (op >> 6) & 15);
        case 0x09: return key(kThumbPcLoad, 0);
        case 0x0a: case 0x0b: return key(kThumbRegister, (op >> 9) & 7);
        case 0x0c: case 0x0d: case 0x0e: case 0x0f: return key(kThumbOffset, (op >> 11) & 3);
        case 0x10: case 0x11: return key(kThumbHalfword, (op >> 11) & 1);
        case 0x12: case 0x13: return key(kThumbStack, (op >> 11) & 1);
        case 0x14: case 0x15: return key(kThumbAddress, (op >> 11) & 1);
        case 0x16: case 0x17:
            switch ((op >> 8) & 0xf) {
            case 0x0: return key(kThumbAddSp, 0);
            case 0x4: case 0x5: return key(kThumbPush, 0);
            case 0xc: case 0xd: return key(kThumbPop, 0);
            // Encodings GBATEK lists as reserved or as later ARMs' (BKPT
            // and others) are undefined on the ARMv4T.
            default: return key(kThumbUndefined, 0);
            }
        case 0x18: case 0x19: return key(kThumbMultiple, (op >> 11) & 1);
        case 0x1a: case 0x1b:
            if (((op >> 8) & 0xf) == 0xf) return key(kThumbSwi, 0);
            return key(kThumbConditional, 0);
        case 0x1c: return key(kThumbBranch, 0);
        case 0x1d: return key(kThumbUndefined, 0);  // ARMv5's BLX suffix.
        case 0x1e: return key(kThumbLinkHigh, 0);
        default: return key(kThumbLinkLow, 0);
        }
    }

    template<uint32_t Key>
    [[gnu::always_inline]] void thumb_handler(uint32_t op) {
        constexpr uint32_t kind = Key >> 8, bits = Key & 0xff;
        if constexpr (kind == kThumbShift) {  // LSL, LSR, ASR by an immediate.
            uint32_t shifter_carry = carry();
            const uint32_t result = shift_immediate(bits, s_.r[(op >> 3) & 7], (op >> 6) & 31, shifter_carry);
            s_.r[op & 7] = result;
            set_nzc(result, shifter_carry);
            prefetch_last<true>();
        } else if constexpr (kind == kThumbAddSubtract) {  // A register or a 3-bit immediate.
            const uint32_t a = s_.r[(op >> 3) & 7];
            const uint32_t b = bits & 2 ? (op >> 6) & 7 : s_.r[(op >> 6) & 7];
            s_.r[op & 7] = bits & 1 ? difference(a, b, true) : sum(a, b, true);
            prefetch_last<true>();
        } else if constexpr (kind == kThumbImmediate) {  // MOV, CMP, ADD and SUB with an 8-bit immediate.
            const unsigned rd = (op >> 8) & 7;
            const uint32_t imm = op & 0xff;
            if constexpr (bits == 0) {
                s_.r[rd] = imm;
                set_nz(imm);
            } else if constexpr (bits == 1) {
                difference(s_.r[rd], imm, true);
            } else if constexpr (bits == 2) {
                s_.r[rd] = sum(s_.r[rd], imm, true);
            } else {
                s_.r[rd] = difference(s_.r[rd], imm, true);
            }
            prefetch_last<true>();
        } else if constexpr (kind == kThumbAlu) {
            thumb_alu<bits>(op);
        } else if constexpr (kind == kThumbHigh) {
            high_register<bits>(op);
        } else if constexpr (kind == kThumbPcLoad || kind == kThumbRegister || kind == kThumbOffset ||
                             kind == kThumbHalfword || kind == kThumbStack) {
            if constexpr (requires(uint32_t& v) { bus_.template transfer_inline<true, true>(0u, 0u, 0u, v, v); }) {
                if (thumb_transfer_fast<Key>(op)) return;
                out<&Executor::template thumb_transfer<Key>>(op);
            } else {
                thumb_transfer<Key>(op);
            }
        } else if constexpr (kind == kThumbAddress) {  // ADD Rd, PC or SP, the PC word-aligned.
            const uint32_t base = bits ? s_.r[13] : s_.r[15] & ~2u;
            s_.r[(op >> 8) & 7] = base + (op & 0xff) * 4;
            prefetch_last<true>();
        } else if constexpr (kind == kThumbAddSp) {  // ADD SP, signed 9-bit offset.
            const uint32_t offset = (op & 0x7f) * 4;
            s_.r[13] = op & 0x80 ? s_.r[13] - offset : s_.r[13] + offset;
            prefetch_last<true>();
        } else if constexpr (kind == kThumbPush) {  // PUSH, with LR when bit 8 is set.
            out<&Executor::template block_transfer<true>>(13, (op & 0xff) | ((op & 0x100) << 6), true, false, true, false, false);
        } else if constexpr (kind == kThumbPop) {  // POP, with PC when bit 8 is set.
            out<&Executor::template block_transfer<true>>(13, (op & 0xff) | ((op & 0x100) << 7), false, true, true, true, false);
        } else if constexpr (kind == kThumbMultiple) {  // STMIA and LDMIA, writing back unless a load includes the base.
            const unsigned rb = (op >> 8) & 7;
            out<&Executor::template block_transfer<true>>(rb, op & 0xff, false, true, !(bits && (op & (1u << rb))), bits, false);
        } else if constexpr (kind == kThumbConditional) {
            // Condition 1110 is "Undefined, should not be used" in GBATEK
            // and branches always on the single-step tests, which the user
            // chose to follow (task gba 1-3).
            const unsigned condition = (op >> 8) & 0xf;
            if (condition == 0xe || ((kPass[condition] >> (s_.cpsr >> 28)) & 1)) {
                const auto offset = static_cast<int8_t>(op & 0xff);
                out<&Executor::template jump<true>>(s_.r[15] + static_cast<uint32_t>(offset * 2), offset < -1);
            } else {
                prefetch_last<true>();
            }
        } else if constexpr (kind == kThumbSwi) {
            out<&Executor::template software_interrupt<true>>();
        } else if constexpr (kind == kThumbBranch) {
            const auto offset = static_cast<int32_t>(op << 21) >> 20;
            out<&Executor::template jump<true>>(s_.r[15] + static_cast<uint32_t>(offset), offset < -2);
        } else if constexpr (kind == kThumbLinkHigh) {  // BL, first half: LR = PC + offset << 12.
            s_.r[14] = s_.r[15] + (static_cast<uint32_t>(static_cast<int32_t>(op << 21) >> 9));
            prefetch_last<true>();
        } else if constexpr (kind == kThumbLinkLow) {
            // BL, second half: PC = LR + offset << 1, LR = next instruction | 1 (TRM 6.3).
            const uint32_t target = s_.r[14] + (op & 0x7ff) * 2;
            s_.r[14] = (s_.r[15] - 2) | 1;
            out<&Executor::template jump<true>>(target, target < s_.r[15]);
        } else {
            out<&Executor::template undefined<true>>();
        }
    }

    // THUMB.6 to THUMB.11, the single loads and stores.
    template<uint32_t Key>
    [[gnu::always_inline]] void thumb_transfer(uint32_t op) {
        constexpr uint32_t kind = Key >> 8, bits = Key & 0xff;
        if constexpr (kind == kThumbPcLoad) {  // LDR PC-relative, from the word-aligned PC.
            const uint32_t address = (s_.r[15] & ~2u) + (op & 0xff) * 4;
            prefetch<true>();
            finish_load<true>((op >> 8) & 7, load_word(address));
        } else if constexpr (kind == kThumbRegister) {  // A register offset.
            const unsigned rd = op & 7;
            const uint32_t address = s_.r[(op >> 3) & 7] + s_.r[(op >> 6) & 7];
            prefetch<true>();
            if constexpr (bits == 0) {
                write(kWord, address, s_.r[rd]);
                advance<true>(false);
            } else if constexpr (bits == 1) {
                write(kHalf, address, s_.r[rd] & 0xffff);
                advance<true>(false);
            } else if constexpr (bits == 2) {
                write(kByte, address, s_.r[rd] & 0xff);
                advance<true>(false);
            } else if constexpr (bits == 3) {
                finish_load<true>(rd, load_signed_byte(address));
            } else if constexpr (bits == 4) {
                finish_load<true>(rd, load_word(address));
            } else if constexpr (bits == 5) {
                finish_load<true>(rd, load_half(address));
            } else if constexpr (bits == 6) {
                finish_load<true>(rd, read(kByte, address));
            } else {
                finish_load<true>(rd, load_signed_half(address));
            }
        } else if constexpr (kind == kThumbOffset) {  // An immediate offset, words and bytes.
            constexpr bool byte = bits & 2;
            const unsigned rd = op & 7;
            const uint32_t address = s_.r[(op >> 3) & 7] + ((op >> 6) & 31) * (byte ? 1 : 4);
            prefetch<true>();
            if constexpr (bits & 1) {
                finish_load<true>(rd, byte ? read(kByte, address) : load_word(address));
            } else {
                if constexpr (byte) write(kByte, address, s_.r[rd] & 0xff);
                else write(kWord, address, s_.r[rd]);
                advance<true>(false);
            }
        } else if constexpr (kind == kThumbHalfword) {  // LDRH and STRH with an immediate offset.
            const unsigned rd = op & 7;
            const uint32_t address = s_.r[(op >> 3) & 7] + ((op >> 6) & 31) * 2;
            prefetch<true>();
            if constexpr (bits) {
                finish_load<true>(rd, load_half(address));
            } else {
                write(kHalf, address, s_.r[rd] & 0xffff);
                advance<true>(false);
            }
        } else if constexpr (kind == kThumbStack) {  // SP-relative.
            const unsigned rd = (op >> 8) & 7;
            const uint32_t address = s_.r[13] + (op & 0xff) * 4;
            prefetch<true>();
            if constexpr (bits) {
                finish_load<true>(rd, load_word(address));
            } else {
                write(kWord, address, s_.r[rd]);
                advance<true>(false);
            }
        }
    }

    // thumb_transfer() at once when the bus can make the prefetch, the
    // access and a load's internal cycle with no call, see
    // MappedBus::transfer_inline(); false, with nothing done, otherwise.
    template<uint32_t Key>
    [[gnu::always_inline]] bool thumb_transfer_fast(uint32_t op) {
        constexpr uint32_t kind = Key >> 8, bits = Key & 0xff;
        // Load, then the width and how a load's value is extended: 0 a
        // word, 1 a halfword, 2 a byte, 3 a signed byte, 4 a signed halfword.
        constexpr bool kLoad = kind == kThumbPcLoad || (kind == kThumbRegister && bits >= 3) ||
                               ((kind == kThumbOffset || kind == kThumbHalfword || kind == kThumbStack) && (bits & 1));
        constexpr unsigned kType = kind == kThumbRegister ? (bits == 1 || bits == 5   ? 1
                                                             : bits == 2 || bits == 6 ? 2
                                                             : bits == 3              ? 3
                                                             : bits == 7              ? 4
                                                                                      : 0)
                                   : kind == kThumbOffset   ? (bits & 2 ? 2 : 0)
                                   : kind == kThumbHalfword ? 1
                                                            : 0;
        constexpr unsigned kWidth = kType == 0 ? 2 : kType == 1 || kType == 4 ? 1 : 0;
        unsigned rd;
        uint32_t address;
        if constexpr (kind == kThumbPcLoad) {
            rd = (op >> 8) & 7;
            address = (s_.r[15] & ~2u) + (op & 0xff) * 4;
        } else if constexpr (kind == kThumbRegister) {
            rd = op & 7;
            address = s_.r[(op >> 3) & 7] + s_.r[(op >> 6) & 7];
        } else if constexpr (kind == kThumbOffset) {
            rd = op & 7;
            address = s_.r[(op >> 3) & 7] + ((op >> 6) & 31) * (bits & 2 ? 1 : 4);
        } else if constexpr (kind == kThumbHalfword) {
            rd = op & 7;
            address = s_.r[(op >> 3) & 7] + ((op >> 6) & 31) * 2;
        } else {
            rd = (op >> 8) & 7;
            address = s_.r[13] + (op & 0xff) * 4;
        }
        uint32_t opcode, value = kLoad ? 0 : s_.r[rd] & (kWidth == 2 ? ~0u : kWidth == 1 ? 0xffff : 0xff);
        if (!bus_.template transfer_inline<true, kLoad>(s_.r[15], address, kWidth, opcode, value)) return false;
        s_.pipeline[0] = s_.pipeline[1];
        s_.pipeline[1] = opcode;
        if constexpr (kLoad) {
            if constexpr (kType == 0) s_.r[rd] = word_value(value, address);
            else if constexpr (kType == 1) s_.r[rd] = half_value(value, address);
            else if constexpr (kType == 2) s_.r[rd] = value;
            else if constexpr (kType == 3) s_.r[rd] = signed_byte_value(value);
            else s_.r[rd] = signed_half_value(value, address);
        }
        advance<true>(false);
        return true;
    }

    // THUMB.4: the ALU operations, each that of an ARM data processing
    // instruction with S. Shifts by a register take an internal cycle, MUL m
    // of them, by Rd's value (MULS Rd, Rs, Rd).
    template<unsigned Operation>
    [[gnu::always_inline]] void thumb_alu(uint32_t op) {
        const unsigned rd = op & 7;
        const uint32_t d = s_.r[rd], m = s_.r[(op >> 3) & 7];
        const uint32_t shifter_carry = carry();
        if constexpr (Operation == 0x2 || Operation == 0x3 || Operation == 0x4 || Operation == 0x7) {
            constexpr uint32_t kType = Operation == 0x2 ? 0 : Operation == 0x3 ? 1 : Operation == 0x4 ? 2 : 3;
            prefetch<true>();
            idle();
            uint32_t carry_out = shifter_carry;
            const uint32_t result = shift_register(kType, d, m & 0xff, carry_out);
            s_.r[rd] = result;
            set_nzc(result, carry_out);
            advance<true>(false);
            return;
        } else if constexpr (Operation == 0xd) {
            prefetch<true>();
            const unsigned cycles = multiply_cycles(d, true);
            idle(cycles);
            const uint32_t result = m * d;
            s_.r[rd] = result;
            set_nzc(result, multiply_carry(m, d, 0, true, false, cycles));
            advance<true>(false);
            return;
        } else if constexpr (Operation == 0x5) {
            s_.r[rd] = add(d, m, carry(), true);
        } else if constexpr (Operation == 0x6) {
            s_.r[rd] = add(d, ~m, carry(), true);
        } else if constexpr (Operation == 0x8) {
            alu<0x8, true>(d, m, shifter_carry);
        } else if constexpr (Operation == 0x9) {
            s_.r[rd] = difference(0, m, true);
        } else if constexpr (Operation == 0xa) {
            difference(d, m, true);
        } else if constexpr (Operation == 0xb) {
            sum(d, m, true);
        } else {
            s_.r[rd] = alu<Operation, true>(d, m, shifter_carry);
        }
        prefetch_last<true>();
    }

    // THUMB.5: ADD, CMP and MOV on any registers, and BX. Only CMP sets
    // flags; ADD and MOV to R15 branch in Thumb state, BX by bit 0 of Rs.
    // BX R15 continues in ARM state at the word-aligned PC.
    template<unsigned Operation>
    [[gnu::always_inline]] void high_register(uint32_t op) {
        const unsigned rd = (op & 7) | ((op >> 4) & 8);
        const uint32_t source = s_.r[(op >> 3) & 15];
        if constexpr (Operation == 3) {
            prefetch<true>();
            if (source & 1) {
                out<&Executor::template refill<true>>(source);
            } else {
                s_.cpsr &= ~kFlagT;
                out<&Executor::template refill<false>>(source);
            }
        } else if constexpr (Operation == 1) {
            difference(s_.r[rd], source, true);
            prefetch_last<true>();
        } else {
            const uint32_t result = Operation == 0 ? s_.r[rd] + source : source;
            if (rd == 15) {
                out<&Executor::template jump<true>>(result, result < s_.r[15]);
                return;
            }
            s_.r[rd] = result;
            prefetch_last<true>();
        }
    }

    // The tables, see the class comment.
    using Handler = void (*)(Arm7State&, BusType, uint32_t);
    template<uint32_t Key>
    static void arm_entry(Arm7State& s, BusType bus, uint32_t op) {
        Executor(s, bus).template arm_handler<Key>(op);
    }
    template<uint32_t Key>
    static void thumb_entry(Arm7State& s, BusType bus, uint32_t op) {
        Executor(s, bus).template thumb_handler<Key>(op);
    }

    static constexpr auto kArmKeys = [] {
        std::array<uint32_t, 4096> keys{};
        for (unsigned i = 0; i < keys.size(); ++i) keys[i] = arm_key(i);
        return keys;
    }();
    static constexpr auto kArmList = key_list<distinct_keys(kArmKeys)>(kArmKeys);
    static constexpr auto kArmPositions = key_positions(kArmKeys, kArmList);
    static_assert(kArmList.size() < 256);
    static constexpr auto kArmHandlers = []<size_t... I>(std::index_sequence<I...>) {
        return std::array<Handler, sizeof...(I) + 1>{&arm_entry<kArmList[I]>..., &skip};
    }(std::make_index_sequence<kArmList.size()>{});

    static constexpr auto kThumbKeys = [] {
        std::array<uint32_t, 1024> keys{};
        for (unsigned i = 0; i < keys.size(); ++i) keys[i] = thumb_key(i);
        return keys;
    }();
    static constexpr auto kThumbList = key_list<distinct_keys(kThumbKeys)>(kThumbKeys);
    static constexpr auto kThumbPositions = key_positions(kThumbKeys, kThumbList);
    static constexpr auto kThumbHandlers = []<size_t... I>(std::index_sequence<I...>) {
        return std::array<Handler, sizeof...(I)>{&thumb_entry<kThumbList[I]>...};
    }(std::make_index_sequence<kThumbList.size()>{});

    Arm7State& s_;
    // A copy: the mapped bus is a reference to Memory, one load away.
    const BusType bus_;
};

}  // namespace gbx::gba
