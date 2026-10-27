#pragma once
#include <cstdint>
#include <span>

// ROM protocol v10, little endian, layered on frame.h (Kind::rom_test).
// REQUEST: u16 version,u8 mode (0=paired,1=INT-only,2=JIT-only,3=lockstep,4=paired sequential),
// u8 criterion (1=register LD B,B,2=frame LD B,B,3=frame duration),u32 id,u64 limit_ticks,u32 ROM bytes,
// u16 path bytes,u16 script events (at most 128),u16 save points (at most 8, mode 4 only),
// u8 machine (0=DMG-C in GB mode,1=CPU CGB E in CGB mode,2=CPU CGB E in compatibility mode),
// u8 frame rows (0=all 144, else the top rows the final frame keeps, at most 144),
// 32-byte ROM SHA256,16-byte setup,12 bytes per script event,16 bytes per save point,
// then UTF-8 card-relative path.
// Setup: u32 audio frames per second (0=no sound),u8 flags (bit0=set RTC,
// bit1=JIT code pool of 16 KiB and 64 blocks, so that blocks keep moving
// between the JIT's two levels),
// RTC: u8 S,M,H,u16 days,u8 halt,carry,u32 subsecond ticks (below 8388608).
// Without bit0 the RTC fields are zero; a cartridge without RTC ignores them.
// Script event: u64 ticks,u8 kind,u8 value,u16 zero. Ticks do not decrease and
// stay below limit_ticks. An event applies at the first instruction boundary
// at or after its ticks, which is the same time on both backends.
// kind=0: buttons held from then on, value bits 0..3 Right,Left,Up,Down and
// 4..7 A,B,Select,Start. kind=1: take the RTC state and write it back.
// Save point: u64 target ticks (below limit_ticks),u32 window ticks (at most
// 8388608),u8 conditions bits 0..7,u8 combinations,u8 conditions bits 8..15,
// u8 zero.
// The state is taken at the first instruction boundary at or after the target
// where every condition bit holds, and no later than target+window:
// bit0 OAM DMA busy, bit1 STAT mode 3 with a PPU change pending in the row
// in flight, bit2 HALT, bit3 EI pending, bit4 a sound channel on (NR52 bits
// 0..3), bit5 RTC subsecond ticks nonzero, bit6 cartridge RAM differs from
// its contents after loading, bit7 the CPU at the CGB's double speed, bit8 an
// HBlank DMA under way. Combinations, nonzero: bit0 INT save/INT load,
// bit1 INT/JIT, bit2 JIT/INT, bit3 JIT/JIT. Each load resets the system
// first.
// Only supported ROMs may be submitted; the PC index controls dependencies.
// The board hashes the loaded ROM, not just the manifest file.
//
// All DATA start u16 version,u8 type,u8 backend,u32 id.
// type=0 RESULT: u8 reason (1=breakpoint,2=time-limit,3=suspended,4=unavailable),
// u8 owner core,u16 flags(bit0=serial truncated,bit1=frame captured,bit2=RTC),
// u64 ticks,u64 wall us,
// u64 instructions,CPU,u32 compiled/invalidated/evicted/code-bytes,
// where invalidated counts blocks retired after an opcode mismatch, and
// evicted counts blocks retired for capacity; neither counts whole-cache flushes,
// five u64 fallback counters (none/unavailable/encoding/capacity/publication),
// six 32-byte SHA256s (WRAM,VRAM,OAM,HRAM,IO+IE,all cartridge RAM; on a CGB,
// WRAM in bank order 0..7 and VRAM bank 0 then bank 1),
// u64 PCM frames,u64 dropped frames,16-byte PCM digest (interleaved
// left/right s16 frames, all produced up to the end time; non-cryptographic,
// see PcmDigest in tests/device/rom_test.cpp),
// final RTC: u8 S,M,H,u16 days,u8 halt,carry,5 latched registers,u32
// subsecond ticks (16 bytes, zero without bit2),
// When bit1 is set: u64 frame ID,32-byte SHA256 of the encoded frame.
// u16 serial bytes,serial.
// CPU: u16 PC,SP;u8 A,F,B,C,D,E,H,L,IME,EI,mode,halt_bug (16 bytes).
// type=1 DIFF (backend=2):u64 block ordinal,u16 entry,last instruction PC,
// u32 instruction count,u8 difference kind (1=CPU,2=time,3=access,4=memory,
// 5=trace overflow),u32 first differing access index (FFFFFFFF if not access),
// u16 memory address,u8 INT value,JIT value,CPU INT,CPU JIT,u64 INT/JIT ticks,
// u16 INT/JIT trace counts,then all INT records then JIT records.
// Trace record: u64 ticks,u16 instruction PC,address,u8 data,operation,
// u16 zero. Operation=0 read (including fetch),1 write. Overflow invalidates
// diagnosis. Block partition/register allocation/lazy flags do not change.
// type=2 PROGRESS (backend=2): u64 completed work units; no judgment implied.
// In lockstep, units are compared blocks. In normal execution, units are the
// sum of completed run-until quanta across the requested workers, and in a
// save point's run those of that run. Progress refreshes transport
// inactivity only; it never extends the ROM wall deadline.
// type=3 FRAME, one part of the encoded frame: u64 frame ID,u32 encoded
// bytes,u32 offset of this part,the part's bytes. Parts come in order and
// together hold the whole encoding. Encoded frame: u8 format, then
// format 0 (GB mode): 40 bytes a row, four 2-bit shades per byte, leftmost
// pixel in bits 1:0; shade 0=white,3=black.
// format 1 (CGB): u16 colors n (1..256),u8 bits per pixel (1, 2, 4 or 8, the
// fewest that index n colors),n u16 BGR555 colors in order of first use,
// then the pixels' indices, leftmost in the low bits of each byte.
// format 2 (CGB, more than 256 colors): u16 BGR555 pixels.
// Pixels go row by row, the rows the request keeps. The digest covers those
// rows only. Backend 2 applies to both results with equal frame IDs/digests.
// Rendering and sound synthesis finish before timing stops and
// before capture. Both run on owner core XOR 1.
// type=4 STATE RESULT, one per save point and requested combination, in
// request order, INT save before JIT save, and per save the load on the save
// backend first; backend=load backend: u8 save point index,u8 save backend,
// u8 status (0=resumed,1=ended before the save,2=conditions not met in the
// window,3=capture refused,4=encoding failed,5=decoding refused,6=restore
// refused,7=no system, backend or buffer),u8 conditions bits 0..7 holding
// where the save run stopped (all bits checked),u8 Snapshot::Status of the
// decoding,u8 conditions bits 8..15 holding,u16 zero,u64 save ticks,u64
// instructions before the
// save,u32 encoded bytes,u32 capture us,encode us,decode us,restore us,
// u32 first/second frame us,u32 first/second frame compiled blocks,u32
// first/second frame blocks installed from the code store, then a RESULT
// from its reason on. Capture and restore run on the owner core, encoding
// and decoding on the other core while the owner waits. The frames are the
// first two 140448-tick spans after the restore, owner time only, 0 when the
// run ended within the span. The RESULT is the resumed run's: ticks and
// instructions count from power-on, wall us from the restore, and its PCM
// digest covers the frames read before the save and those after the restore.
// A failed save point's RESULT is the save run's state where it stopped.
// type=5 STATE FRAME (backend=load backend): u8 save point index,u8 save
// backend, then a FRAME part from its frame ID on; sent only when the resumed
// frame's ID or digest differs from the uninterrupted RESULT of the load
// backend, whose frame the PC already has.
// DONE after ACKs: u16 version,u16 RESULT and STATE RESULT count,u32 id.
// No missing RESULT or DONE may pass. Speeds exclude loading, hashing (PCM
// included) and encoding and include compilation.
namespace gbx::test::rom_test {
inline constexpr uint16_t kVersion = 10;
bool run(std::span<const uint8_t> request);
}
