#pragma once

#include <cstddef>
#include <cstdint>

// ARM7TDMI single-step requests, part of execution v7 (execution.h): the same
// version, HELLO_REPLY, DATA/ACK flow, DONE and ERROR rules. Little-endian
// integers, no native C++ struct layout. Cases come from the card's
// tests/gba/arm7tdmi/cases.bin, laid out in tools/prepare_gba_tests.py.
//
// ARM7_RUN: u16 version, u8 backends=3 (interpreter and JIT), u8 mode
// (0=device, 1=diagnostic), u32 request ID, u16 file index in cases.bin, u8
// selection (0=indices,1=range), u8 zero, u32 count; then count u32 case
// indices, ordered and unique, at most kMaxIndices, or one u32 first index of
// count consecutive cases.
//
// Each case runs on the interpreter, judged by the suite, then from the same
// initial state on the JIT, as a block of that one instruction, compared with
// the interpreter.
//
// DATA: u16 version, u8 mode, u8 zero, u32 request ID, u32 first ordinal,
// u32 case count; then one item per case, in request order, except that an
// item byte with bit 7 clear stands for (bits 0-6) + 1 cases that passed on
// both backends and carry nothing else. Otherwise the byte holds the
// interpreter's outcome in bits 0-1 (0=pass,1=fail,2=unjudged), the class in
// bits 2-3, in bit 4 that the interpreter's bus records overflowed
// kMaxRecords, in bit 5 that actuals follow, and in bit 6 that the JIT's
// actual differs from the interpreter's. A second byte holds the JIT's
// fallback reason in bits 0-2, as gba::Arm7JitFallback: 0=none,
// 1=unavailable, 2=encoding, 3=capacity, 4=publication, and in bit 3 that its
// records overflowed. A case that fell back is not a JIT pass. Device mode
// attaches the actuals of both backends to interpreter failures, to
// differences and to fallbacks; diagnostic mode attaches them to every case,
// judging none, bit 6 clear. Class 0 is judged; 1, an unpredictable R15
// write, and 2, CP14 MRC or MCR, are executed but not compared with the suite
// and count as interpreter passes, see docs/plans/gba/task-1-3.md; their JIT
// actuals are compared. The class follows from the encoding: in ARM state
// with the condition passing, a single or halfword data transfer with base
// R15 and write-back that does not load R15, MRS into R15, or MRC/MCR on
// coprocessor 14.
// Actual, the interpreter's first: u64 mask of the 40 state words of
// cases.bin, in its order, that differ from the initial state, those words
// as u32, then u8 record count and that many records <u8 kind,u8 size,u8
// access,u8 cycle,u32 address,u32 data>, gba::BusRecord. The last state word
// is the next fetch's access, 2 (N) or 3 (S), as the suite stores it.
// Judged by the suite: every state word, except R15's bit 0 in Thumb state
// and bits 1-0 in ARM state per the expected CPSR (ARM7TDMI TRM 2.6.1), and
// the records in order, by kind, size, address, data and the S bit of
// access. Lock and cycle are not judged; the suite's README says cycle is not
// recorded correctly.
// Compared between the backends: every state word and every field of every
// record, and the number of records.
//
// DONE after all DATA ACKs: u16 version, u16 zero, u32 request ID, u32 case
// count, u32 cases whose interpreter records match the suite's in number but
// not in every cycle, then microseconds: u32 the reader task spent in SD
// reads, u32 this task waited for them, u32 sending and waiting for ACKs, u32
// the whole request, u32 interpreter execution, u32 JIT compilation, u32 JIT
// publication, u32 JIT execution. The reader runs on the other core,
// alongside the rest.
namespace gbx::test::arm7_execution {
inline constexpr uint8_t kPaired = 3;
inline constexpr uint8_t kDeviceJudge = 0, kDiagnostic = 1;
inline constexpr uint8_t kPass = 0, kFail = 1, kUnjudged = 2;
inline constexpr uint8_t kJudged = 0, kR15Write = 1, kCp14 = 2;
inline constexpr size_t kRequestHeader = 16, kBatchHeader = 16, kDoneSize = 48;
inline constexpr size_t kMaxIndices = 2000, kMaxRecords = 64, kStateWords = 40;
inline constexpr size_t kMaxItem = 2 + 2 * (8 + 4 * kStateWords + 1 + 12 * kMaxRecords);
}  // namespace gbx::test::arm7_execution
