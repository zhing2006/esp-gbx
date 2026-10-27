#include "executor.h"

namespace gbx::gba {

// C after a flag-setting multiply, which ARM's manuals call meaningless. It is
// a bit of the multiplier array's carry-save state, found on hardware by
// zayd's "Solving the Mystery of ARM7TDMI Multiply Carry Flag" (bmchtech,
// 2024, https://bmchtech.github.io/post/multiply/). In that model each of the
// m cycles feeds 4 radix-4 Booth addends of the multiplicand, extended to 34
// bits (signed for MUL, MLA, SMULL and SMLAL), through 4 carry-save adders
// into a 33-bit partial sum and carry; the accumulator starts in the partial
// sum and its bits above 33 enter 2 per adder. The adders move bits only
// towards bit 0, so C, taken from bit 22, 14 or 6 of the final carry after 1,
// 2 or 3 cycles and from bit 30 after 4 cycles of a long multiply, depends on
// the registers' bits 1-32 alone: here a 32-bit word holds those bits. After
// 4 cycles of MUL or MLA it is the negation flag of the last Booth digit.
bool multiply_carry(uint32_t multiplicand, uint32_t multiplier, uint64_t accumulator, bool is_signed,
                    bool is_long, unsigned cycles) {
    const uint32_t sign = is_signed ? multiplicand >> 31 : 0;  // Bits 32 and 33 of the extended multiplicand.
    const uint32_t single = (multiplicand >> 1) | (sign << 31);  // Bits 1-32 of +1 times it, +2 times is multiplicand.
    uint32_t sum = static_cast<uint32_t>(accumulator >> 2);
    // The carry starts with -multiplicand when bit 0 of the multiplier is set,
    // its +1 entering at the final adder.
    uint32_t carry = (multiplier & 1) ? (~multiplicand >> 2) | ((sign ? 0u : 3u) << 30) : 0;
    uint32_t high = static_cast<uint32_t>(accumulator >> 34);
    int32_t remaining = static_cast<int32_t>(multiplier);
    uint32_t negative = 0;
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        for (unsigned adder = 0; adder < 4; ++adder) {
            // Booth digit of 3 multiplier bits: 0, +1, +1, +2, -2, -1, -1, 0
            // as magnitude in bits 0-1 and negation in bit 2.
            const unsigned digit = (05562110u >> (3 * ((remaining >> (2 * adder)) & 7))) & 7;
            negative = digit >> 2;
            const uint32_t addend = (digit & 3) == 0 ? 0 : (digit & 3) == 1 ? single : multiplicand;
            const uint32_t addend_sign = (digit & 3) == 0 ? 0 : sign;
            const uint32_t a = addend ^ (0u - negative);
            const uint32_t extension = (high & 1) + (~carry >> 31) + (addend_sign ^ negative ^ 1);
            const uint32_t majority = (sum & a) | (carry & (sum | a));
            sum = ((sum ^ a ^ carry) >> 2) | (extension << 30);
            carry = (majority >> 1) | ((~high & 2) << 30);
            high >>= 2;
        }
        remaining = is_signed ? remaining >> 8 : static_cast<int32_t>(static_cast<uint32_t>(remaining) >> 8);
    }
    if (cycles == 4 && !is_long) return negative;
    constexpr uint8_t kBit[4] = {21, 13, 5, 29};
    return (carry >> kBit[cycles - 1]) & 1;
}

void arm7_switch_mode(Arm7State& state, uint32_t from, uint32_t to) {
    const unsigned old_bank = bank_of(from), new_bank = bank_of(to);
    if (old_bank == new_bank) return;
    state.r13[old_bank] = {state.r[13], state.r[14]};
    state.r[13] = state.r13[new_bank][0];
    state.r[14] = state.r13[new_bank][1];
    if ((old_bank == kFiqBank) != (new_bank == kFiqBank)) {
        auto& saved = state.r8[old_bank == kFiqBank];
        const auto& loaded = state.r8[new_bank == kFiqBank];
        for (unsigned i = 0; i < 5; ++i) {
            saved[i] = state.r[8 + i];
            state.r[8 + i] = loaded[i];
        }
    }
}

void arm7_set_registers(Arm7State& state, const Arm7Registers& registers) {
    for (unsigned i = 0; i < 16; ++i) state.r[i] = registers.r[i];
    for (unsigned i = 0; i < 5; ++i) state.r8[1][i] = registers.fiq[i];
    state.r13[1] = {registers.fiq[5], registers.fiq[6]};
    state.r13[2] = registers.svc;
    state.r13[3] = registers.abt;
    state.r13[4] = registers.irq;
    state.r13[5] = registers.und;
    state.spsr = registers.spsr;
    state.cpsr = registers.cpsr;
    arm7_switch_mode(state, kModeUser, registers.cpsr);
}

Arm7Registers arm7_registers(const Arm7State& state) {
    Arm7State user = state;
    arm7_switch_mode(user, state.cpsr, kModeUser);
    Arm7Registers registers;
    registers.r = user.r;
    for (unsigned i = 0; i < 5; ++i) registers.fiq[i] = user.r8[1][i];
    registers.fiq[5] = user.r13[1][0];
    registers.fiq[6] = user.r13[1][1];
    registers.svc = user.r13[2];
    registers.abt = user.r13[3];
    registers.irq = user.r13[4];
    registers.und = user.r13[5];
    registers.cpsr = state.cpsr;
    registers.spsr = state.spsr;
    return registers;
}

}  // namespace gbx::gba
