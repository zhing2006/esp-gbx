#pragma once
#include <array>
#include <cstdint>

namespace gbx::test {
// Paired interpreter/native program checks beyond the single-step corpus.
// Results are condition bitmasks with independent ISA-derived expectations.
bool sm83_blocks(std::array<uint32_t, 8>& results);
}
