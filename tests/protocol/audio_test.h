#pragma once

#include <cstdint>

// AUDIO_TEST (kind 14): u16 version=1, u16 reserved=0, u32 nonzero request ID.
// DATA: u16 version, u16 ordered check ID, u32 request ID, u32 actual.
// DONE after DATA ACKs: u16 version, u16 check count, u32 request ID.
// Expected counts and checkpoint names are in tests/runner/run_audio.py.
// This tests mixer handoff/accounting directly, without executing guest code.
namespace gbx::test::audio_test {
inline constexpr uint16_t kVersion = 1;
inline constexpr uint16_t kCheckCount = 14;
bool run(uint32_t request_id);
}  // namespace gbx::test::audio_test
