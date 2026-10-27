#pragma once

// Bit counts for the GBA's hot code. The ESP32-P4 has no instructions for
// them (no Zbb), and std::countr_zero() and std::popcount() call libgcc.

#include <cstdint>

namespace gbx::gba {

// The index of the lowest set bit of a nonzero value: a multiply by a de
// Bruijn sequence.
inline unsigned lowest_bit(uint32_t value) {
    static constexpr uint8_t kIndex[32] = {0,  1,  28, 2,  29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4,  8,
                                           31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6,  11, 5,  10, 9};
    return kIndex[((value & (0u - value)) * 0x077cb531u) >> 27];
}

inline unsigned count_bits(uint32_t value) {
    value -= (value >> 1) & 0x55555555u;
    value = (value & 0x33333333u) + ((value >> 2) & 0x33333333u);
    return (((value + (value >> 4)) & 0x0f0f0f0fu) * 0x01010101u) >> 24;
}

}  // namespace gbx::gba
