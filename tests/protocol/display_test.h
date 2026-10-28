#pragma once

#include <cstdint>

// Display pattern checks; no panel scanout, ROM or SD test data is required.
// DISPLAY_TEST (kind 13): u16 version=4, u16 reserved=0, u32 request ID (nonzero).
// DATA: u16 version, u16 result ID (0-based), u32 request ID, u32 actual.
// ID 0 is the display backend: 0 CPU, 1 PPA/DMA2D. IDs 1..8 are pass masks:
//   1: copy dispatch, 20 cases; 2: hardware copy, 20 cases (accelerated only);
//   3: fill, 20 cases; 4: nearest, 12 cases;
//   5: smooth constant images, 20 cases; 6: interpolation, 4 cases (both accelerated only);
//   7: equal/contained/overlapping damage, one fixture in each of four rotations;
//   8: SPI panel row bands, 24 cases, unrotated.
// Bits follow the fixture order in tests/display/backend.cpp. Set means PASS.
// Groups 1..3 iterate four rotations, then five rectangles; group 4 iterates
// rotations then scales 1..3; group 5 iterates rotations then five colors;
// group 8 iterates BGR555 pixels and shades, then scales 1..3, then widths 7
// and 8, then whole rows and image columns.
// Unavailable hardware groups return zero and are reported as SKIP, not PASS.
// DONE, after DATA ACKs: u16 version, u16 result count, u32 request ID.
// Missing results, ERROR or reset cannot pass. Bump the version if fixtures change.
namespace gbx::test::display_test {
inline constexpr uint16_t kVersion = 4;
inline constexpr uint16_t kResultCount = 9;
bool run(uint32_t request_id);
}  // namespace gbx::test::display_test
