#pragma once

#include <cstdint>

// Native JIT backend checks and fixed SM83, ARM and Thumb program checks; not
// the SD suites.
// Reuses frame.h sessions, reliable DATA, ACK and DONE. No SD suite is required.
// JIT_TEST (kind 11): u16 version=21, u16 group, u32 request ID (nonzero).
// Group 0 selects all checks; group 1 selects mapped memory, cache deletion,
// peripherals and cartridge/DMA; group 2 the Zcb, Zba, Zbb and Zbs
// instructions. IDs are consecutive within the selected group.
// DATA: u16 version, u16 check ID (0-based, ordered), u32 request ID, u32 actual.
// The runner supplies ISA/ABI-derived expectations, not emulator output.
// DONE, only after all DATA ACKs: u16 version, u16 check count, u32 request ID.
// ERROR aborts the request. A crash, missing check or missing DONE is not PASS.
// ID meanings and expectations are in tests/runner/run_jit.py; bump this version
// when changing the fixtures or their order. These checks do not certify the
// SM83 single-step suite; HELLO_REPLY independently reports available backends.
namespace gbx::test::jit_test {
inline constexpr uint16_t kVersion = 22;
inline constexpr uint16_t kCheckCount = 179;
inline constexpr uint16_t kMemoryCheckCount = 41;
inline constexpr uint16_t kExtensionCheckCount = 54;
bool run(uint32_t request_id, uint16_t group);
}  // namespace gbx::test::jit_test
