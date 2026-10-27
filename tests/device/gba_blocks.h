#pragma once
#include <array>
#include <cstdint>

namespace gbx::test {
// ARM programs on the GBA's memory map: the JIT against the interpreter,
// with interrupts inside blocks and code rewritten in IWRAM and EWRAM
// (tests/runner/run_jit.py lists the bits of each result).
bool gba_blocks(std::array<uint32_t, 2>& results);
}
