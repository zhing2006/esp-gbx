#pragma once

#include "gbx/gb/jit/sm83_jit.h"

// Private ABI shared by the code generator and its runtime helpers.
namespace gbx::gb::sm83_runtime {

// Mapped blocks run inside one gateway frame. Besides the resident guest
// registers in s2-s11, the following caller-saved registers are pinned while
// generated code runs; out-of-line stubs preserve or reload them around C++:
//   a3 = budget (block exits leave the native frame when s1 reaches it)
//   a4 = completed instructions, a5 = result whose low byte defines lazy Z
//   a6 = fast_until, a7 = pages + 2048 (MemoryPage table, signed offsets)
// t6 and ra are scratch for every access; stubs preserve a0-a2 and t0-t5.
// Calls into stubs link through ra, but those that data words follow link
// through t6 (`jal t6, stub`), and the stub returns past the words through
// it: a call through ra would leave the address of the words on the return
// address stack of the ESP32-P4 HP core (TRM 1.12.2), which predicts the
// next return through ra by fetching from it, and a word there that
// encodes a hardware loop instruction, such as 0x880150ab (esp.lp.setupi),
// corrupts the program counter (tested on a P4 v1.3).
struct Context {
    Sm83State* cpu;
    const Bus* bus;
    uint32_t instructions;
    uint32_t mcycles, synced_cycles;
    Memory* memory;
    MemoryPage* pages;
    Scheduler* clock;
    Tick until, next;
    // Ticks per M-cycle as a shift, 3 or 2 at the CGB's double speed. The
    // speed changes only at STOP, which ends the run.
    uint32_t mcycle_shift;
    // Deferral horizon, see lazy_horizon(); kept until reached or reset by
    // careful_write.
    Tick horizon;
    // chain: the M-cycle count at which this leg of a batch ends, see kLeg
    // in sm83_jit.cpp; 0 outside batches. budget is chain, or 0 to stop.
    uint32_t fast_until, budget, chain, stop;
    // IO registers change at deferred events too. Generated IO reads are
    // direct only before this M-cycle, which no event precedes.
    uint32_t exact_until;
    uint32_t lazy;          // Initial a5, for an entry with a pending Z flag.
    uint32_t mapping_generation, event_generation, interrupt_generation;
    // A batch whose mapping changed: the next fetch checks that the change
    // left the running code alone before native code goes on, see
    // resume_remapped(). Until then the windows and the budget are closed.
    uint32_t remap;
    // The windows a bank switch alone closed, which the check reopens while
    // nothing else changed; 0 once something did.
    uint32_t open_fast, open_exact;
    uint32_t tracing;
    uint32_t* dispatch;     // Sm83Jit dispatch table: {tag, entry} pairs.
    // Memory::code_tags: an entry's tag is its PC | the one for its slot.
    const uint32_t* banks;
    // A batch for a frame returns after the frame; single blocks return
    // after any event, as interpreter blocks do.
    uint32_t batch, frame, entry_generation;
    uint64_t entry_frames;
    uint32_t value;      // Immediate bytes returned by a careful fetch group.
    uint32_t exit;       // Exit descriptor of the latest native exit.
    uintptr_t link;      // Link data following that exit's descriptor.
    uintptr_t active;    // Any address inside a block that observed a refetch.
    uint32_t observed[3], consumed;
    uint32_t probe_value;
    uintptr_t smc;       // A verified block that wrote to its own code.
    // Registers preserved by out-of-line stubs: the link, kPreserved, then the
    // return addresses of the C++ call cores and of POP/PUSH, which call
    // the shared access code themselves.
    uint32_t save[14];
    uint16_t* instruction_pc;
};
static_assert(sizeof(Context) < 2048);

// Exit descriptor: one word after `jal t6, exit`, or Context::exit.
namespace exit_bits {
constexpr unsigned completed = 16;   // 5 bits: instructions to add to a4.
constexpr unsigned reason = 21;      // 3 bits: JitExit.
constexpr uint32_t lazy_zero = 1u << 24, dynamic_pc = 1u << 25;
constexpr unsigned link = 26;        // 2 bits: kLink*.
constexpr unsigned action = 28;      // 2 bits: kAction*.
// With JitExit::refetch: a verified block found its code changed on entry.
constexpr uint32_t stale = 1u << 30;
// With JitExit::refetch: a block in the second level ran out of entries
// before it ran, see Stubs::count_entry.
constexpr uint32_t spent = 1u << 31;
}
// Careful fetch group: two words after `jal t6, group`.
//   d0: PC | bytes << 16 | idles << 18 | completed << 20 | lazy Z << 25
//   d1: expected bytes (24 bits) | first unfetched byte << 24 |
//       verify mask << 26 | HALT bug << 29
// A ROM instruction without a prefetched byte or HALT bug, whose bytes the
// runtime reads again from its bank, uses one word after `jal t6, group_rom`:
//   PC (below 0x8000) | fetch cycles - 1 << 15 | completed << 17 |
//   lazy Z << 21 | halfwords back to the hot stream << 22
enum : uint32_t { kLinkNone, kLinkStatic };
// HALT with IME: the EI before HALT takes effect after HALT samples IME.
enum : uint32_t { kActionNone, kActionHalt, kActionStop, kActionHaltIme };

// Instruction bytes, including the CB opcode byte (Pan Docs, CPU Instruction Set).
inline unsigned instruction_length(uint8_t op) {
    switch (op) {
    case 0x01: case 0x08: case 0x11: case 0x21: case 0x31:
    case 0xc2: case 0xc3: case 0xc4: case 0xca: case 0xcc: case 0xcd:
    case 0xd2: case 0xd4: case 0xda: case 0xdc: case 0xea: case 0xfa: return 3;
    case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
    case 0xcb: case 0xe0: case 0xe8: case 0xf0: case 0xf8: return 2;
    default: return (op & 0xc7) == 0x06 || (op & 0xc7) == 0xc6 ? 2 : 1;
    }
}
// Idle M-cycles that always follow the fetches before any data access,
// from the M-cycle sequences in gbctr.pdf, chapter 6. Conditional idles and
// those after data accesses are emitted at their own position.
inline unsigned leading_idles(uint8_t op) {
    if (op == 0xe8) return 2;
    if (op == 0xf8 || op == 0xf9 || op == 0xc3 || op == 0x18 || op == 0xcd) return 1;
    if ((op & 0xe7) == 0xc0) return 1;  // RET cc samples its condition first.
    if ((op & 0xcf) == 0xc5 || (op & 0xc7) == 0xc7) return 1;  // PUSH, RST.
    if ((op & 0xc7) == 0x03 || (op & 0xcf) == 0x09) return 1;  // INC/DEC rr, ADD HL,rr.
    return 0;
}

// The bank windows, as Memory's window bits, that the code of a block
// starting at `pc` depends on: its page and the next (Memory::code_bank()).
// In CGB mode, VRAM and the WRAM at D000-DFFF and its echo switch banks too.
inline unsigned regions(uint16_t pc, bool cgb) {
    const auto region = [cgb](uint16_t address) -> unsigned {
        if (address < 0x4000) return Memory::kRom0;
        if (address < 0x8000) return Memory::kRom1;
        if (address >= 0xa000 && address < 0xc000) return Memory::kCartRam;
        if (!cgb) return 0;
        if (address < 0xa000) return Memory::kVram;
        return (address >= 0xd000 && address < 0xe000) || (address >= 0xf000 && address < 0xfe00) ?
            Memory::kWramBank : 0;
    };
    return region(pc) | region(static_cast<uint16_t>((pc | 127) + 1));
}
// Whether the dispatch table may hold a block starting at `pc`. A block
// key holds the banks of its first two pages, and Memory::code_tags those
// of 4 KiB slots, so the table takes only blocks whose two pages lie in one
// window: not on the last page of one, 3F80, 7F80, 9F80, BF80, CF80, DF80
// and EF80, nor from FD80 on, where slot F also holds OAM, IO and HRAM, and
// HRAM wraps around to ROM.
inline bool dispatchable(uint16_t pc) {
    return pc < 0xfd80 && !((pc & 0xf80) == 0xf80 && ((0x7a88u >> (pc >> 12)) & 1));
}

// Keep clock synchronization inline across the compiler/runtime boundary.
#if __has_cpp_attribute(gnu::always_inline)
[[gnu::always_inline]]
#endif
inline void sync_clock(Context* c) {
    // Unsynchronized cycles used direct paths proven to precede every event.
    // No device can observe this local time until a helper or run boundary.
    c->clock->now += static_cast<Tick>((c->mcycles - c->synced_cycles) << c->mcycle_shift);
    c->synced_cycles = c->mcycles;
}
#ifdef GBX_PROFILER
#define GBX_JIT_COUNT(counter) (++::gbx::gb::sm83_jit_counters.counter)
#define GBX_JIT_ADD(counter, n) (::gbx::gb::sm83_jit_counters.counter += (n))
#else
#define GBX_JIT_COUNT(counter) static_cast<void>(0)
#define GBX_JIT_ADD(counter, n) static_cast<void>(0)
#endif
void update_deadline(Context* c);
// After a bank switch in a batch, at the fetch of the instruction at pc:
// false, with stop set, when the switch moved the code that runs;
// otherwise the run goes on under the new banks.
bool resume_remapped(Context* c, uint16_t pc);
// Takes the current banks as those the code that runs next was found for.
void remapped(Context* c);
uint32_t careful_read(Context* c, uint32_t address);
void careful_write(Context* c, uint32_t address, uint32_t value);
void careful_code_write(Context* c, uint32_t address, uint32_t value, const uint32_t* range);
void careful_idle(Context* c);
uint32_t careful_group(Context* c, const uint32_t* descriptor);
uint32_t careful_group_rom(Context* c, const uint32_t* descriptor);
uint32_t read_bus(Context* c, uint32_t address);
void write_bus(Context* c, uint32_t address, uint32_t value);
void idle_bus(Context* c);

}  // namespace gbx::gb::sm83_runtime
