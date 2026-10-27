#pragma once
#include <array>
#include <cstdint>

namespace gbx::test {
// ARM and Thumb programs on the ARM7TDMI JIT, compared with the interpreter
// (tests/runner/run_jit.py lists the bits of each result).
bool arm7_blocks(std::array<uint32_t, 5>& results);
}
