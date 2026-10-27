#pragma once

#include <array>
#include <cstdint>

namespace gbx::test {

// SHA-256 of a manifest. Packs are not hashed on the board: the runner checks
// that the manifest equals its own, and ROM bytes are verified on loading.
// Failure leaves the digest zero.
bool read_manifest_digest(const char* path, std::array<uint8_t, 32>& manifest_digest);

}  // namespace gbx::test
