#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "gbx/jit/rv32.h"

namespace gbx::jit {

// A bounded emitter over caller-owned scratch memory. Any encoding/capacity
// error is sticky; code() returns empty rather than exposing a partial block.
// Offsets are bytes. With compression, emit() uses the 16-bit form of an
// instruction where one exists, so instructions start on any even offset.
class Assembler {
public:
    explicit Assembler(std::span<uint32_t> storage, bool compress = false)
        : storage_(storage), compress_(compress) {}
    void emit(rv32::Instruction instruction) { put(compress_ ? rv32::compress(instruction) : instruction); }
    // As given, never compressed: for code that is patched later.
    void emit_exact(rv32::Instruction instruction) { put(instruction); }
    // A word that code reads as data. It must start at a 4-byte boundary.
    void data(uint32_t word);
    // Pads with c.nop up to a 4-byte boundary.
    void align() { if (used_ & 2) put(rv32::c_nop()); }
    // Replaces an instruction of the same size.
    void patch(size_t offset, rv32::Instruction instruction);
    void li(rv32::Reg destination, uint32_t value);
    void call(uint32_t address);  // Clobbers t0 and the ABI caller-saved registers.
    // One Zbb or Zbs instruction where the build allows them (rv32::kZb),
    // otherwise two RV32I instructions.
    void zero_extend16(rv32::Reg destination, rv32::Reg source);
    void sign_extend8(rv32::Reg destination, rv32::Reg source);
    void extract_bit(rv32::Reg destination, rv32::Reg source, unsigned bit);

    // ABI prologue/epilogue: reserve 16 bytes, preserve ra/s0, keep context in s0.
    // Other s-registers must not be modified without additional save/restore.
    void begin_function();
    void end_function();  // a0 already contains the frontend-defined exit reason.

    size_t offset() const { return used_; }
    bool valid() const { return valid_; }
    // Whole words; a trailing halfword is padded with zero.
    std::span<const uint32_t> code() const {
        return valid_ ? storage_.first((used_ + 3) / 4) : std::span<const uint32_t>{};
    }

private:
    void put(rv32::Instruction instruction);
    std::span<uint32_t> storage_;
    size_t used_ = 0;
    bool compress_, valid_ = true;
};

// A 32-bit value at an even byte offset into words, little-endian, as an
// instruction that may straddle two words. Writes keep the other halves.
inline uint32_t read_at(const uint32_t* words, size_t offset) {
    if (!(offset & 2)) return words[offset / 4];
    return words[offset / 4] >> 16 | words[offset / 4 + 1] << 16;
}
template<class Word> void write_at(Word* words, size_t offset, uint32_t value) {
    if (!(offset & 2)) { words[offset / 4] = value; return; }
    words[offset / 4] = (words[offset / 4] & 0xffffu) | value << 16;
    words[offset / 4 + 1] = (words[offset / 4 + 1] & 0xffff0000u) | value >> 16;
}

}  // namespace gbx::jit
