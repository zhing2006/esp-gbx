#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace gbx::app {

// SHA-256 (FIPS 180-4) of a buffer, for checking the GBA BIOS against the
// value tools/prepare_gba_tests.py records.
std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);

}  // namespace gbx::app
