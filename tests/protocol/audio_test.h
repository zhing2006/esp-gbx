#pragma once

#include <cstdint>

// AUDIO_TEST (kind 14): u16 version=2, u16 reserved=0, u32 nonzero request ID.
// DATA: u16 version, u16 ordered result ID, u32 request ID, u32 actual.
// ID 0 is the mixer's worker: 1 on the other core, 0 where the mixer runs
// each batch as it is submitted (GBX_SINGLE_CORE), which has no pending
// sound to check. IDs 1..14 are the checks.
// DONE after DATA ACKs: u16 version, u16 result count, u32 request ID.
// Expected counts and checkpoint names are in tests/runner/run_audio.py.
// This tests mixer handoff/accounting directly, without executing guest code.
namespace gbx::test::audio_test {
inline constexpr uint16_t kVersion = 2;
inline constexpr uint16_t kCheckCount = 14;
inline constexpr uint16_t kResultCount = 1 + kCheckCount;
bool run(uint32_t request_id);
}  // namespace gbx::test::audio_test
