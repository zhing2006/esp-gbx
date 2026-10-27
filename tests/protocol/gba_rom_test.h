#pragma once
#include <array>
#include <cstdint>
#include <span>

// GBA ROM protocol v2, little endian, layered on frame.h (Kind::gba_rom_test).
// REQUEST: u16 version,u8 mode (0=paired: interpreter then JIT, each in a
// system of its own on core 1,1=interpreter only,2=JIT only,3=lockstep:
// both systems at once, see DIFF,4=concurrent: the interpreter on core 0
// and the JIT on core 1 at once, each with a ROM cache of its own),u8 end
// (1=idle loop,2=frames,3=console text),u32 id,u64 limit ticks,u32 ROM
// bytes,u16 path bytes,u16 key events (at most 64),u32 frames (end=2, else
// 0),u16 marker bytes (end=3, at most 64, else 0),u8 flags (bit0=ROM cache
// of RomCache::kMinFrames pages, dropped before each run, so that pages
// keep moving; bit1=JIT code pool of 16 KiB and 64 blocks, so that blocks
// keep moving between the JIT's two levels; bit2=capture the final frame;
// bit3=the path is under the card's own roms/, for measuring a game's
// speed; bit4=lockstep keeps the JIT's links and dispatch table, and
// compares only where native code returns),u8 zero,32-byte ROM SHA256,
// u32 audio frames per second (at most 192000, 0=no sound),u16 save points
// (at most 8, mode 0 only),u16 zero,12 bytes per key event,16 bytes per
// save point,the markers,then the UTF-8 card-relative path under
// tests/gba/, or with bit3 from the card's root.
// Runs start from System::reset() and go in quanta that end at each
// multiple of 65536 ticks, at the key events and where the end needs, the
// same on every backend; the end is checked after the quanta:
// end=1: the CPU sits at a branch to itself (System::idle_loop()), checked
// after the quanta that pass a multiple of 65536 ticks or the limit, so
// that a run resumed from a state checks at the same times.
// end=2: `frames` frames have completed (each up to the next start of
// VBlank).
// end=3: the console text (see RESULT) holds one of the markers, ASCII
// strings separated by LF, and has not changed for two frames, checked
// after each frame. A row of the text is searched as it is, spaces kept.
// A run that reaches `limit ticks` first ends there.
// Key event: u64 ticks,u16 keys (gba::key bits, set = pressed),u16 zero.
// Ticks do not decrease and stay below the limit. Keys apply from the
// first quantum boundary at or after their ticks; quanta end there.
// Save point: u64 target ticks (below the limit),u32 window ticks (at most
// 16777216),u16 conditions,u8 combinations (nonzero: bit0 INT save/INT
// load,bit1 INT/JIT,bit2 JIT/INT,bit3 JIT/JIT),u8 zero. The state is taken
// at the first instruction boundary at or after the target where every
// condition holds, and no later than target+window: bit0 a repeating
// HBlank, video or sound FIFO DMA whose source has moved from its
// register's, bit1 HALT, bit2 a register write timed within the line being
// drawn, bit3 a Direct Sound FIFO holding samples, bit4 the prefetch buffer
// holding or fetching halfwords, bit5 a Flash or EEPROM command under way,
// bit6 an RTC transfer under way (gba::Memory's probes). Each load resets
// the system first.
// The board hashes the ROM file as it reads it into the ROM cache and runs
// nothing unless it matches. A missing or wrong BIOS (HELLO) refuses all.
//
// All DATA start u16 version,u8 type,u8 backend,u32 id.
// type=0 RESULT: u8 reason (1=idle loop,2=time limit,3=frames or text
// done,4=unavailable,5=lockstep difference),u8 core,u16 flags (bit0=frame
// captured,bit1=RTC),u64 ticks,u64 wall us,CPU,
// u32 compiled,invalidated,evicted,restored,stored entries,store drops,
// code bytes (JIT counters, zero on the interpreter: blocks compiled,
// retired after a write to their code, retired for room, copied back from
// the second level, entries into blocks in the second level, records it
// dropped, and the pool's bytes in use),
// six u64 fallbacks (none,unavailable,encoding,capacity,publication,region),
// seven 32-byte SHA256s (EWRAM,IWRAM,VRAM,palette RAM,OAM,I/O registers as
// reads see them (Memory::io_snapshot()),backup memory),
// u64 ROM cache misses,miss wait us,pages read ahead,u64 HALT cycles,
// u64 PCM frames,u64 dropped frames,16-byte PCM digest (interleaved
// left/right s16 frames, all produced up to the end time; non-cryptographic,
// see PcmDigest in tests/device/gba_rom_test.cpp),
// RTC: 7 bytes year to second as the chip holds them,u8 control,u32 cycles
// since the last second (12 bytes, zero without bit1),
// console text: 600 bytes, rows 0-19 of 30 characters, the low byte of the
// BG0 map entries at 6002000h (libgba's console, nba-hw-test),
// mode 4 rows: 1920 bytes, rows 76-83 of the bitmap at 6000000h
// (alyosha-gba-tests' result text),
// when flags bit0: u64 frame ID,32-byte SHA256 of the encoded frame.
// CPU: Arm7Registers as u32s (R0-R15 of User and System, R8-R14 of FIQ,
// R13-R14 of SVC, ABT, IRQ, UND, CPSR, the five SPSRs),pipeline 2 u32,
// u8 sequential,u8 halted,u16 zero.
// type=1 DIFF (backend=2, lockstep), before the RESULTs: u64 turn (of the
// JIT's run loop, counted from power-on),u64 quantum,u32 R15,u32 CPSR,u32
// instructions,u64 ticks (the JIT's at the turn before: the block or step
// that diverged starts there),u32 interpreter and JIT instructions,u32
// interpreter and JIT stops (both counted since the quantum began),u8 kind
// (1=CPU,2=time,3=memory,4=turn: the interpreter cannot reach the JIT's
// count of instructions and stops),u8 region of the first differing byte,
// u16 zero,u32 its offset,u8 interpreter and JIT value,u16 zero,u64
// interpreter and JIT ticks,interpreter CPU,JIT CPU.
// Lockstep compares the CPUs and times at each turn, the memory regions
// (EWRAM 2,IWRAM 3,I/O 4,palette 5,VRAM 6,OAM 7) after each quantum. When
// RAM differs there, the run starts again from power-on and compares those
// regions at each turn of that quantum too. A DIFF ends the run: RESULT
// reason 5.
// type=2 PROGRESS (backend=2): u64 completed quanta of the runs going on.
// Progress refreshes transport inactivity only.
// type=3 FRAME, one part of the encoded frame: u64 frame ID,u32 encoded
// bytes,u32 offset of this part,the part's bytes, in order. Encoded frame:
// u8 format, then format 1: u16 colors n (1..256),u8 bits per pixel (1, 2,
// 4 or 8),n u16 BGR555 colors in order of first use, then the pixels'
// indices, leftmost in the low bits of each byte; format 2 (more than 256
// colors): u16 BGR555 pixels. 240x160 pixels row by row. Backend 2 when
// both results have the same frame ID and digest.
// type=4 STATE RESULT, after the RESULTs and FRAMEs, one per save point and
// requested combination, in request order, INT save before JIT save, and
// per save the load on the save backend first; backend=load backend: u8
// save point index,u8 save backend,u8 status (0=resumed,1=ended before the
// save,2=conditions not met in the window,3=capture refused,4=encoding
// failed,5=decoding refused,6=restore refused,7=no system, backend or
// buffer),u8 gba::Snapshot::Status of the decoding,u16 conditions holding
// where the save run stopped (all bits checked),u16 zero,u64 save ticks,
// u32 encoded bytes,u32 capture us,encode us,decode us,restore us,u32
// first/second frame us,u32 first/second frame compiled blocks,u32
// first/second frame blocks copied back from the JIT's second level, then a
// RESULT from its reason on. Capture and restore run on core 1, encoding
// and decoding on core 0 while core 1 waits. The frames are the first two
// spans of 280896 ticks after the restore, core 1's time only, 0 when the
// run ended within the span. The RESULT is the resumed run's: ticks count
// from power-on, wall us from the restore, and its PCM digest covers the
// frames read before the save and those after the restore. A failed save
// point's RESULT is the save run's state where it stopped.
// type=5 STATE FRAME (backend=load backend): u8 save point index,u8 save
// backend, then a FRAME part from its frame ID on; sent only when the
// resumed frame's ID or digest differs from the uninterrupted RESULT of the
// load backend, whose frame the PC already has.
// DONE after ACKs: u16 version,u16 RESULT and STATE RESULT count,u32 id.
// Speeds exclude loading, hashing (PCM included) and encoding and include
// compilation.
namespace gbx::test::gba_rom_test {
inline constexpr uint16_t kVersion = 2;
// Reads the BIOS from the card once: its SHA256 for HELLO, all zero when it
// is missing or not 16 KiB.
std::array<uint8_t, 32> load_bios();
bool run(std::span<const uint8_t> request);
}
