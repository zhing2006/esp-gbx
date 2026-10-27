#pragma once

// Wait loops, which both of the GBA's backends run fast: code that branches
// back to its start, and of which every pass does the same as the one
// before (arm7_spin_loop()). The JIT skips whole passes in its blocks
// (Compiler::spin()), the interpreter at its backward branches
// (arm7_mapped.cpp).

#include <array>
#include <cstdint>

namespace gbx::gba {

namespace spin {

// What an instruction of a wait loop reads and writes: guest registers by
// bit, and flags as NZCV in bits 3-0.
struct Effect {
    bool ok = false;
    uint16_t reads = 0, writes = 0;
    uint8_t flags_read = 0, flags_set = 0;
};
inline constexpr uint8_t kN = 8, kZ = 4, kC = 2, kV = 1;

// The flags a condition reads.
constexpr uint8_t condition_flags(unsigned cond) {
    constexpr uint8_t kFlags[15]{kZ, kZ, kC, kC, kN, kN, kV, kV, kC | kZ, kC | kZ, kN | kV, kN | kV,
                                 kZ | kN | kV, kZ | kN | kV, 0};
    return cond < 15 ? kFlags[cond] : 0;
}

constexpr uint16_t bit(unsigned n) { return static_cast<uint16_t>(1u << n); }

// A Thumb instruction that neither writes memory nor branches.
inline Effect thumb_effect(uint32_t op) {
    Effect e;
    const unsigned rd = op & 7, rs = (op >> 3) & 7;
    switch (op >> 11) {
    case 0x00: case 0x01: case 0x02: {  // LSL, LSR, ASR by an immediate.
        const bool keeps_c = (op >> 11) == 0 && ((op >> 6) & 31) == 0;
        e = {true, bit(rs), bit(rd), 0, static_cast<uint8_t>(kN | kZ | (keeps_c ? 0 : kC))};
        break;
    }
    case 0x03: {  // ADD and SUB, a register or a 3-bit immediate.
        const bool immediate = op & (1u << 10);
        e = {true, static_cast<uint16_t>(bit(rs) | (immediate ? 0 : bit((op >> 6) & 7))), bit(rd), 0,
             kN | kZ | kC | kV};
        break;
    }
    case 0x04: e = {true, 0, bit((op >> 8) & 7), 0, kN | kZ}; break;                        // MOV
    case 0x05: e = {true, bit((op >> 8) & 7), 0, 0, kN | kZ | kC | kV}; break;              // CMP
    case 0x06: case 0x07:                                                                    // ADD, SUB
        e = {true, bit((op >> 8) & 7), bit((op >> 8) & 7), 0, kN | kZ | kC | kV};
        break;
    case 0x08:
        if (!(op & (1u << 10))) {  // ALU operations.
            const unsigned operation = (op >> 6) & 15;
            const uint16_t both = static_cast<uint16_t>(bit(rd) | bit(rs));
            switch (operation) {
            case 0x2: case 0x3: case 0x4: case 0x7: e = {true, both, bit(rd), 0, kN | kZ}; break;  // Shifts by Rs.
            case 0x5: case 0x6: e = {true, both, bit(rd), kC, kN | kZ | kC | kV}; break;           // ADC, SBC
            case 0x8: e = {true, both, 0, 0, kN | kZ}; break;                                      // TST
            case 0x9: e = {true, bit(rs), bit(rd), 0, kN | kZ | kC | kV}; break;                   // NEG
            case 0xa: case 0xb: e = {true, both, 0, 0, kN | kZ | kC | kV}; break;                  // CMP, CMN
            case 0xd: e = {true, both, bit(rd), 0, kN | kZ | kC}; break;                           // MUL
            case 0xf: e = {true, bit(rs), bit(rd), 0, kN | kZ}; break;                             // MVN
            default: e = {true, both, bit(rd), 0, kN | kZ}; break;                                 // AND, EOR, ORR, BIC
            }
            break;
        }
        {  // ADD, CMP and MOV on any registers; BX and writes to R15 branch.
            const unsigned d = (op & 7) | ((op >> 4) & 8), m = (op >> 3) & 15, operation = (op >> 8) & 3;
            if (operation == 3 || (operation != 1 && d == 15)) break;
            if (operation == 0) e = {true, static_cast<uint16_t>(bit(d) | bit(m)), bit(d), 0, 0};
            else if (operation == 1) e = {true, static_cast<uint16_t>(bit(d) | bit(m)), 0, 0, kN | kZ | kC | kV};
            else e = {true, bit(m), bit(d), 0, 0};
        }
        break;
    case 0x09: e = {true, 0, bit((op >> 8) & 7), 0, 0}; break;  // LDR Rd, [PC, #imm]
    case 0x0a: case 0x0b:  // Register offsets: stores 0-2, loads 3-7.
        if (((op >> 9) & 7) >= 3) e = {true, static_cast<uint16_t>(bit(rs) | bit((op >> 6) & 7)), bit(rd), 0, 0};
        break;
    case 0x0d: case 0x0f: case 0x11:  // LDR, LDRB and LDRH with an immediate offset.
        e = {true, bit(rs), bit(rd), 0, 0};
        break;
    case 0x13: e = {true, bit(13), bit((op >> 8) & 7), 0, 0}; break;            // LDR Rd, [SP, #imm]
    case 0x14: e = {true, 0, bit((op >> 8) & 7), 0, 0}; break;                  // ADD Rd, PC, #imm
    case 0x15: e = {true, bit(13), bit((op >> 8) & 7), 0, 0}; break;            // ADD Rd, SP, #imm
    default: break;
    }
    return e;
}

// An unconditional ARM instruction that neither writes memory nor branches.
inline Effect arm_effect(uint32_t op) {
    Effect e;
    if ((op >> 28) != 0xe) return e;
    const unsigned rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
    if (rd == 15) return e;
    // The second operand of data processing and of a register offset:
    // its registers and whether it reads C (RRX).
    const auto shifted = [&](uint16_t& reads, uint8_t& flags_read) {
        reads |= bit(rm);
        if (op & (1u << 4)) reads |= bit((op >> 8) & 15);
        else if (((op >> 5) & 3) == 3 && ((op >> 7) & 31) == 0) flags_read |= kC;
    };
    if (((op >> 26) & 3) == 1) {  // LDR and LDRB with an offset and no writeback.
        if (!(op & (1u << 20)) || !(op & (1u << 24)) || (op & (1u << 21))) return e;
        e = {true, bit(rn), bit(rd), 0, 0};
        if (op & (1u << 25)) {
            if (op & (1u << 4)) return {};
            shifted(e.reads, e.flags_read);
        }
        return e;
    }
    if (((op >> 26) & 3) != 0) return e;
    if ((op & 0x0e000090u) == 0x00000090u) {
        // Multiplies, swaps and halfword transfers: LDRH, LDRSB and LDRSH
        // with an offset and no writeback only.
        if ((op & 0x60) == 0 || !(op & (1u << 20)) || !(op & (1u << 24)) || (op & (1u << 21))) return e;
        e = {true, bit(rn), bit(rd), 0, 0};
        if (!(op & (1u << 22))) e.reads |= bit(rm);
        return e;
    }
    const unsigned opcode = (op >> 21) & 15;
    const bool set = op & (1u << 20);
    if (opcode >= 8 && opcode <= 11 && !set) return e;  // MRS, MSR and BX.
    if (opcode >= 8 && opcode <= 11 && rd != 0) return e;
    e.ok = true;
    if (opcode != 13 && opcode != 15) e.reads |= bit(rn);
    if (opcode < 8 || opcode > 11) e.writes = bit(rd);
    bool c_set;
    if (op & (1u << 25)) {
        c_set = ((op >> 8) & 15) != 0;
    } else {
        shifted(e.reads, e.flags_read);
        const unsigned type = (op >> 5) & 3, amount = (op >> 7) & 31;
        c_set = !(op & (1u << 4)) && (type != 0 || amount != 0);
    }
    if (opcode == 5 || opcode == 6 || opcode == 7) e.flags_read |= kC;  // ADC, SBC, RSC
    if (set) {
        const bool logical = (0xf303u >> opcode) & 1;  // AND, EOR, TST, TEQ, ORR, MOV, BIC, MVN
        e.flags_set = static_cast<uint8_t>(kN | kZ | (logical ? (c_set ? kC : 0) : (kC | kV)));
    }
    return e;
}

}  // namespace spin

// The instructions of the wait loop at `start` with the branch back, or 0:
// up to a conditional branch back to `start`, instructions that only read
// memory, of which each reads only registers and flags that an instruction
// before it in the same pass wrote, or that no instruction of the loop
// writes. Every pass then does the same as the one before, and the memory
// it reads changes only through what C++ does: events, DMA and interrupts.
// `read(address)` gives the opcode there, ~0 where the loop may not go.
template<class Read>
unsigned arm7_spin_loop(uint32_t start, bool thumb, Read read) {
    using namespace spin;
    constexpr unsigned kMaxInstructions = 16;
    const uint32_t length = thumb ? 2 : 4;
    std::array<Effect, kMaxInstructions> effects{};
    unsigned count = 0;
    uint8_t branch_flags = 0;
    for (uint32_t address = start;; address += length) {
        if (count == kMaxInstructions) return 0;
        const uint32_t op = read(address);
        if (op == ~0u) return 0;
        const uint32_t pc = address + 2 * length;
        // The branch back: conditional, or B.
        if (thumb && ((op >> 12) & 15) == 0xd && ((op >> 8) & 15) < 14) {
            if (pc + static_cast<uint32_t>(static_cast<int8_t>(op & 0xff) * 2) != start) return 0;
            branch_flags = condition_flags((op >> 8) & 15);
            break;
        }
        if (thumb && ((op >> 11) & 31) == 0x1c) {
            if (pc + static_cast<uint32_t>(static_cast<int32_t>(op << 21) >> 20) != start) return 0;
            break;
        }
        if (!thumb && ((op >> 25) & 7) == 5 && !(op & (1u << 24)) && (op >> 28) < 15) {
            if (pc + static_cast<uint32_t>(static_cast<int32_t>(op << 8) >> 6) != start) return 0;
            branch_flags = condition_flags(op >> 28);
            break;
        }
        effects[count] = thumb ? thumb_effect(op & 0xffff) : arm_effect(op);
        if (!effects[count].ok) return 0;
        ++count;
    }
    uint16_t written = 0, written_before = 0;
    for (unsigned i = 0; i < count; ++i) written |= effects[i].writes;
    uint8_t flags = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto& e = effects[i];
        if (e.reads & written & ~written_before) return 0;
        if (e.flags_read & ~flags) return 0;
        written_before |= e.writes;
        flags |= e.flags_set;
    }
    if (branch_flags & ~flags) return 0;
    return count + 1;
}

}  // namespace gbx::gba
