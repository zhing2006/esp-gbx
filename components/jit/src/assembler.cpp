#include "gbx/jit/assembler.h"

namespace gbx::jit {

void Assembler::put(rv32::Instruction instruction) {
    const size_t size = instruction.compressed ? 2 : 4;
    if (!valid_ || !instruction.valid || storage_.size() * 4 - used_ < size) {
        valid_ = false;
        return;
    }
    auto& word = storage_[used_ / 4];
    if (used_ & 2) {
        word = (word & 0xffffu) | instruction.bits << 16;
        if (size == 4) storage_[used_ / 4 + 1] = instruction.bits >> 16;
    } else word = instruction.bits;
    used_ += size;
}

void Assembler::data(uint32_t word) {
    if (used_ & 2) valid_ = false;
    put({word, true});
}

void Assembler::patch(size_t offset, rv32::Instruction instruction) {
    const size_t size = instruction.compressed ? 2 : 4;
    // The low two bits of an instruction's first halfword give its size.
    if (!valid_ || !instruction.valid || offset & 1 || offset + size > used_ ||
        ((storage_[offset / 4] >> (offset & 2) * 8 & 3) == 3) != (size == 4)) {
        valid_ = false;
        return;
    }
    if (size == 4) write_at(storage_.data(), offset, instruction.bits);
    else {
        auto& word = storage_[offset / 4];
        const unsigned shift = (offset & 2) * 8;
        word = (word & ~(0xffffu << shift)) | instruction.bits << shift;
    }
}

void Assembler::li(rv32::Reg destination, uint32_t value) {
    using namespace rv32;
    const int32_t low = static_cast<int32_t>(value & 0xfff) - ((value & 0x800) ? 4096 : 0);
    const uint32_t high = (value + 0x800u) >> 12;
    if (high) emit(lui(destination, high));
    emit(addi(destination, high ? destination : Reg::zero, low));
}

void Assembler::call(uint32_t address) {
    li(rv32::Reg::t0, address);
    emit(rv32::jalr(rv32::Reg::ra, rv32::Reg::t0, 0));
}

void Assembler::zero_extend16(rv32::Reg destination, rv32::Reg source) {
    using namespace rv32;
    if (kZb) { emit(zext_h(destination, source)); return; }
    emit(slli(destination, source, 16)); emit(srli(destination, destination, 16));
}

void Assembler::sign_extend8(rv32::Reg destination, rv32::Reg source) {
    using namespace rv32;
    if (kZb) { emit(sext_b(destination, source)); return; }
    emit(slli(destination, source, 24)); emit(srai(destination, destination, 24));
}

void Assembler::extract_bit(rv32::Reg destination, rv32::Reg source, unsigned bit) {
    using namespace rv32;
    if (kZb) { emit(bexti(destination, source, bit)); return; }
    emit(srli(destination, source, bit)); emit(andi(destination, destination, 1));
}

void Assembler::begin_function() {
    using namespace rv32;
    emit(addi(Reg::sp, Reg::sp, -16));
    emit(sw(Reg::ra, Reg::sp, 12));
    emit(sw(Reg::s0, Reg::sp, 8));
    emit(addi(Reg::s0, Reg::a0, 0));
}

void Assembler::end_function() {
    using namespace rv32;
    emit(lw(Reg::s0, Reg::sp, 8));
    emit(lw(Reg::ra, Reg::sp, 12));
    emit(addi(Reg::sp, Reg::sp, 16));
    emit(ret());
}

}  // namespace gbx::jit
