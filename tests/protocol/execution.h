#pragma once

#include <cstddef>
#include <cstdint>

// Execution v8; little-endian integers, no native C++ struct layout.
// ROM requests/results use the companion rom_test.h schema, ARM7TDMI
// single-step requests arm7_execution.h; HELLO_REPLY below covers both.
// Only the current version is supported, including saved-report decoding.
// frame.h still supplies the same transport, sessions, CRC and flow control.
//
// RUN: u16 version, u8 backends=3, u8 model=1 (flat 64 KiB), u32 request ID,
// u16 opcode, u16 observation limit, u16 count, u8 mode (0=device,1=diagnostic),
// u8 selection (0=indices,1=range), u8 oracle (0=verify interpreter against
// suite,1=use validated reference), 3 reserved zero bytes; then count u32 case
// indices, or one u32 first index. Ordered unique indices, one opcode group.
// The host may select oracle=1 only with a matching validated baseline report.
// Both backends execute one instruction from identical initial memory/state,
// then observe HALT/STOP/lock up to the supplied observation count/limit.
//
// DATA: u16 version, u8 mode, u8 oracle, u32 request ID, u16 first ordinal,
// u16 count; each record starts with ONE byte: bits 0..1 interpreter oracle
// outcome, bits 2..3 pair outcome, bits 4..6 fallback reason, bit 7 has_actual.
// Outcomes: 0=pass,1=fail,2=limit,3=unjudged.
// Oracle=reference yields unjudged unless the interpreter exceeded its limit.
// Diagnostic mode returns unjudged outcomes (except limits) and both actuals.
// Fallback reasons match gb::JitFallback: 0=none,1=unavailable,2=encoding,
// 3=capacity,4=publication. Any fallback is NOT a native JIT pass.
// Successful device results omit actuals; failures/limits/fallbacks include
// both states. Each actual: u8 limit_exceeded, u16 register delta mask,
// changed PC/SP (u16), A/B/C/D/E/F/H/L/IME/EI/mode (u8), in bit order;
// u16 elapsed M-cycles, u8 sample count, u16 RAM count,
// RAM entries <u16 address,u8 value>, then samples <u16 address,u8 data,u8 pins>.
// Registers are relative to initial state (EI defaults to 0, mode=running).
// IME/EI normalized to 0/1; mode=running/halted/stopped/locked (0..3).
// RAM is the sorted union of final.ram and actual writes from BOTH backends.
// All its actual values and all raw bus samples are retained, not inferred
// from expected effects. This is NOT a snapshot of every byte of guest RAM.
// Pins: r=1,w=2,m=4. Pair checks ignore disconnected idle pins, not transfers.
// Suite checks retain the suite's IE/IME/null-pin exclusions and provided EI.
//
// DONE after all DATA ACKs: u16 version, u16 zero, u32 request ID, u32 case count.
// ERROR aborts a request. Missing results or DONE never constitute a pass.
//
// HELLO_REPLY: nonce, <u16 chip revision,u8 available backend bits,u8 status,
// u32 Hz,u16 payload limit,u16 window>, four u16-length strings (board,IDF,
// firmware Git version,preset), u16 execution version, 32-byte hash of
// tests/gb/manifest.tsv, 32-byte ELF hash, 32-byte SM83 identity, 32-byte
// hash of tests/gba/manifest.tsv, 32-byte ARM7TDMI identity, 32-byte hash of
// the card's bios/gba_bios.bin. A missing manifest or BIOS, or a BIOS that is
// not 16 KiB, hashes as zero.
// Backend bits: SM83 interpreter=1,SM83 JIT=2,ARM7TDMI interpreter=4,
// ARM7TDMI JIT=8. A JIT bit means the firmware has that JIT: it allocates its
// code pool at the first RUN, which fails if it cannot.
// Status bits: board=1,SD=2,SM83 pack open=4,ARM7TDMI pack open=8. The packs
// are not hashed on the board; the manifest hash identifies the data.
// Each identity excludes JIT code and the other system's core and runner, but
// covers its interpreter, common test/platform code, build configuration, SDK
// and compiler identity.
namespace gbx::test::execution {
inline constexpr uint16_t kVersion = 8;
inline constexpr uint8_t kInterpreter = 1, kJit = 2, kPaired = 3, kSm83Flat = 1;
inline constexpr uint8_t kArm7Interpreter = 4, kArm7Jit = 8;
inline constexpr size_t kMaxSamples = 64, kMaxRam = 64, kMaxSelected = 1024;
inline constexpr size_t kMaxObservedRam = kMaxRam + 2 * kMaxSamples;
inline constexpr uint8_t kDeviceJudge = 0, kDiagnostic = 1;
inline constexpr uint8_t kSuiteOracle = 0, kReferenceOracle = 1;
inline constexpr uint8_t kPass = 0, kFail = 1, kLimit = 2, kUnjudged = 3;
inline constexpr size_t kBatchHeader = 12;
inline constexpr size_t kMaxRecord = 1 + 2 * (1 + 2 + 15 + 3 + 2 + 3 * kMaxObservedRam + 4 * kMaxSamples);
}  // namespace gbx::test::execution
