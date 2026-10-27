#include "sm83_jit_runtime.h"

#include <algorithm>

#ifdef GBX_PROFILER
namespace gbx::gb {
Sm83JitCounters sm83_jit_counters;
}
#endif

namespace gbx::gb::sm83_runtime {

// A batch may defer device events while no deferred event could be observed
// before the returned time. PPU, timer and serial events change only IO, OAM
// and VRAM permissions. No page maps those directly, so deferred runs reach
// them through careful accesses that dispatch due events first. OAM DMA
// reads and writes memory at its own events, and traced runs probe every
// access: neither defers. Other events are not deferred either. The horizon is also
// the earliest event that can request an interrupt the CPU would take, or
// that completes a frame the caller waits for. Every native exit dispatches
// the events that became due, see Sm83Jit::execute.
//
// The horizon depends on IME, IE, the STAT sources, OAM DMA, the LCD, timer
// and serial state and on events it does not defer. Until it is reached,
// only writes change those: deferred events reschedule only themselves, and
// leaving native code is the only way to set IME. Clearing IME only lets the
// horizon move later. update_deadline therefore keeps it until it is
// reached, and writes that may change those reset it, see careful_write.
static Tick lazy_horizon(const Context* c) {
    const auto& memory = *c->memory;
    if (!c->batch || c->tracing || memory.dma_busy()) return 0;
    Tick horizon = memory.deferral_limit();
    if (c->frame) horizon = std::min(horizon, memory.ppu.next_vblank());
    if (c->cpu->ime) horizon = std::min(horizon, memory.next_interrupt(memory.high[127]));
    return horizon;
}

void update_deadline(Context* c) {
    const auto& clock = *c->clock;
    auto& memory = *c->memory;
    // Device events alone do not end native execution: generated code checks
    // the window before each instruction and access. Interrupt, mapping and
    // target changes take effect at the next instruction boundary. In a batch,
    // an interrupt request matters only once it can be taken: EI and RETI
    // leave native code themselves, and DI only clears IME.
    GBX_JIT_COUNT(deadline);
    if (memory.interrupt_generation != c->interrupt_generation) {
        c->interrupt_generation = memory.interrupt_generation;
        if (!c->batch || (c->cpu->ime && memory.pending_interrupts())) {
            if (!c->stop) GBX_JIT_COUNT(stop_interrupt);
            c->stop = 1;
        }
    }
    // A bank switch in a batch leaves the code that runs alone unless it
    // moves that code, which the next fetch checks, see resume_remapped().
    const bool remapped = memory.mapping_generation != c->mapping_generation;
#ifdef GBX_PROFILER
    if (!c->stop) {
        if (remapped && (!c->batch || c->tracing)) GBX_JIT_COUNT(stop_mapping);
        else if (clock.now >= c->until) GBX_JIT_COUNT(stop_until);
        else if (c->batch ? c->frame && memory.frames != c->entry_frames : clock.generation != c->entry_generation) {
            if (c->batch) GBX_JIT_COUNT(stop_frame);
            else GBX_JIT_COUNT(stop_event);
        }
    }
#endif
    if (remapped) {
        if (c->batch && !c->tracing) c->remap = 1;
        else c->stop = 1;
    }
    // A VRAM DMA that holds the CPU passes inside the access or fetch that
    // dispatches its event, so a batch runs on after it.
    c->stop |= clock.now >= c->until ||
        (c->batch ? c->frame && memory.frames != c->entry_frames : clock.generation != c->entry_generation);
    c->next = clock.next;
    c->event_generation = clock.generation;
    // An event can change mapping between the two bytes of a CB opcode. The
    // rest of this instruction must keep using verified slow fetches.
    if (c->stop || c->remap) { c->fast_until = c->exact_until = c->budget = c->open_fast = 0; return; }
    // Direct accesses must end before the deadline. Bound the distance before
    // division so RV32 can calculate it in 32 bits; a native run is bounded
    // by its budget anyway.
    const auto until = [&](Tick deadline) {
        const auto window = deadline > clock.now ? static_cast<uint32_t>(std::min<Tick>(deadline - clock.now, 65536)) : 0;
        return c->mcycles + (window ? (window - 1) >> c->mcycle_shift : 0);
    };
    if (clock.now >= c->horizon) c->horizon = lazy_horizon(c);
    c->exact_until = until(std::min(clock.next, c->until));
    c->fast_until = c->horizon ? until(std::min(c->horizon, c->until)) : c->exact_until;
    // OAM DMA changes access permissions at its own events. Traced runs probe
    // every access. Both keep every M-cycle on the exact, out-of-line path.
    if (c->tracing || memory.dma_active()) c->fast_until = c->exact_until = c->mcycles;
}
bool resume_remapped(Context* c, uint16_t pc) {
    auto& memory = *c->memory;
    // The banks of the block that holds pc: the page it starts on is pc's
    // or the one before. Its links lead only to blocks that need no other
    // windows. Dynamic exits check the banks of their target, see
    // Memory::code_tags.
    const bool cgb = memory.mode() == Mode::cgb;
    if (c->stop || (memory.remapped & (regions(pc, cgb) | regions(static_cast<uint16_t>(pc - 128), cgb)))) {
        GBX_JIT_COUNT(stop_mapping);
        c->stop = 1;
        return false;
    }
    GBX_JIT_COUNT(remap);
    const uint32_t fast = c->open_fast, exact = c->open_exact;
    remapped(c);
    if (fast && c->mcycles < fast && c->clock->next == c->next && c->clock->generation == c->event_generation &&
        memory.interrupt_generation == c->interrupt_generation) {
        c->fast_until = fast; c->exact_until = exact;
        return true;
    }
    update_deadline(c);
    return !c->stop;
}
void remapped(Context* c) {
    c->memory->remapped = 0;
    c->mapping_generation = c->memory->mapping_generation;
    c->remap = 0;
    c->budget = c->chain;
}
#if __has_cpp_attribute(gnu::always_inline)
[[gnu::always_inline]]
#endif
static inline bool window_unchanged(const Context* c) {
    return c->mcycles < c->fast_until && c->clock->next == c->next &&
        c->clock->generation == c->event_generation && c->memory->mapping_generation == c->mapping_generation &&
        c->memory->interrupt_generation == c->interrupt_generation;
}
#ifdef GBX_PROFILER
// See Sm83JitCounters::read_area.
static unsigned area(uint32_t address) {
    return address < 0x8000 ? 0 : address < 0xa000 ? 1 : address < 0xfe00 ? 2 : address < 0xff00 ? 3 :
        address < 0xff80 ? 4 : 5;
}
#endif
// Whether a write may change what the deferral horizon depends on: IE and
// the other IO registers interrupt, timer, serial, LCD or DMA state. These
// change none of it: sound and wave RAM, as the APU schedules no events;
// SCY, BGP, OBP0 and OBP1, which the PPU only records for the row it draws,
// unlike SCX, WY and WX, which can move HBlank (see Ppu::write()); the CGB's
// VBK, palette registers, SVBK and FF72-FF75, which schedule nothing
// either; and HRAM. Cartridge control, VRAM, OAM and RAM neither: bank
// switches move memory, not events.
static bool changes_horizon(uint32_t address) {
    if (address < 0xff00) return false;
    const auto r = address & 0xff;
    return !((r >= 0x10 && r < 0x40) || r == 0x42 || (r >= 0x47 && r <= 0x49) || r == 0x4f ||
             (r >= 0x68 && r <= 0x6b) || r == 0x70 || (r >= 0x72 && r <= 0x75) || (r >= 0x80 && r != 0xff));
}
uint32_t careful_read(Context* c, uint32_t address) {
    GBX_JIT_COUNT(read);
    GBX_JIT_COUNT(read_area[area(address)]);
    sync_clock(c);
    ++c->mcycles;
    auto& memory = *c->memory;
    const auto value = c->tracing ? memory.probed_read(static_cast<uint16_t>(address)) :
        (address >> 7) == (0xff00 >> 7) ? memory.read_io_timed(static_cast<uint8_t>(address)) :
        memory.read(static_cast<uint16_t>(address));
    c->synced_cycles = c->mcycles;
    if (!window_unchanged(c)) update_deadline(c);
    return value;
}
void careful_write(Context* c, uint32_t address, uint32_t value) {
    GBX_JIT_COUNT(write);
    GBX_JIT_COUNT(write_area[area(address)]);
    if ((address >> 7) == (0xff00 >> 7)) GBX_JIT_COUNT(io_write[address & 127]);
    sync_clock(c);
    ++c->mcycles;
    if (c->tracing) c->memory->probed_write(static_cast<uint16_t>(address), static_cast<uint8_t>(value));
    else c->memory->write(static_cast<uint16_t>(address), static_cast<uint8_t>(value));
    c->synced_cycles = c->mcycles;
    const auto& memory = *c->memory;
    const auto& clock = *c->clock;
    if (changes_horizon(address)) {
        GBX_JIT_COUNT(horizon_reset);
        c->horizon = 0;
        update_deadline(c);
    } else if (c->batch && !c->tracing && !c->stop && !c->remap &&
               memory.mapping_generation != c->mapping_generation && c->mcycles < c->fast_until &&
               clock.next == c->next && clock.generation == c->event_generation &&
               memory.interrupt_generation == c->interrupt_generation) {
        // A bank switch alone, as games make many a frame: the next fetch
        // checks the code, see resume_remapped(), and then reopens these.
        c->remap = 1;
        c->open_fast = c->fast_until; c->open_exact = c->exact_until;
        c->fast_until = c->exact_until = c->budget = 0;
    } else if (!window_unchanged(c)) update_deadline(c);
}
// A write from a verified block to a page of its own code. Once it changes
// a code byte, the block must not run on: it stops at the next instruction
// boundary and is retired, see Sm83Jit::execute.
void careful_code_write(Context* c, uint32_t address, uint32_t value, const uint32_t* range) {
    GBX_JIT_COUNT(code_write);
    careful_write(c, address, value);
    const auto& memory = *c->memory;
    const auto* written = memory.backing(static_cast<uint16_t>(address));
    if (!written) return;
    for (unsigned k = 0; k < (*range >> 16); ++k) {
        if (memory.backing(static_cast<uint16_t>(*range + k)) != written) continue;
        GBX_JIT_COUNT(smc);
        c->smc = reinterpret_cast<uintptr_t>(range);
        c->stop = 1;
        c->fast_until = c->exact_until = c->budget = 0;
        return;
    }
}
void careful_idle(Context* c) {
    GBX_JIT_COUNT(idle);
    sync_clock(c);
    ++c->mcycles;
    c->memory->idle();
    c->synced_cycles = c->mcycles;
    if (!window_unchanged(c)) update_deadline(c);
}
// descriptor: the group's words in its block, see Context::active.
// counted: the cycles the inline check had already added for the group.
static uint32_t fetch_group(Context* c, uint32_t d0, uint32_t d1, const uint32_t* descriptor, unsigned counted) {
    GBX_JIT_COUNT(group);
    const auto pc = static_cast<uint16_t>(d0);
    const unsigned bytes = (d0 >> 16) & 3, idles = (d0 >> 18) & 3, first = (d1 >> 24) & 3;
    const unsigned verify = (d1 >> 26) & 7;
    const bool halt_bug = (d1 >> 29) & 1;
    c->mcycles -= counted;
    const uint32_t before = pc | ((d0 >> 20) & 31) << exit_bits::completed | ((d0 >> 25) & 1) << 24;
    // A prefetched continuation must complete its instruction.
    if (c->remap && !first) resume_remapped(c, pc);
    if (c->stop && !first) {
        GBX_JIT_COUNT(group_stop);
        c->exit = before | static_cast<uint32_t>(JitExit::event) << exit_bits::reason;
        return 1;
    }
    sync_clock(c);
    auto& memory = *c->memory;
    if (c->tracing) *c->instruction_pc = pc;
    uint32_t value = 0;
    // Prefetched bytes, operands included, keep the values read earlier.
    for (unsigned k = 0; k < first; ++k) {
        c->observed[k] = (d1 >> (k * 8)) & 255;
        if (k) value |= c->observed[k] << ((k - 1) * 8);
    }
    // Every fetch reads its page at its own M-cycle: an event such as the
    // start of OAM DMA can change page permissions within the group. The
    // deadline window is updated once, after the group.
    for (unsigned k = first; k < bytes; ++k) {
        // A HALT bug repeats the opcode address for the following byte.
        const auto address = static_cast<uint16_t>(pc + k - (halt_bug && k));
        ++c->mcycles;
        const auto byte = c->tracing ? memory.probed_read(address) : memory.read(address);
        c->observed[k] = byte;
        if (((verify >> k) & 1) && byte != ((d1 >> (k * 8)) & 255)) {
            // No semantics have run. The continuation reuses these bytes
            // instead of fetching them again at another bus time.
            c->synced_cycles = c->mcycles;
            c->consumed = k + 1;
            c->active = reinterpret_cast<uintptr_t>(descriptor);
            c->exit = before | static_cast<uint32_t>(JitExit::refetch) << exit_bits::reason;
            return 1;
        }
        if (k) value |= byte << ((k - 1) * 8);
    }
    for (unsigned i = 0; i < idles; ++i) { ++c->mcycles; memory.idle(); }
    c->value = value;
    c->synced_cycles = c->mcycles;
    if (!window_unchanged(c)) update_deadline(c);
    return 0;
}
uint32_t careful_group(Context* c, const uint32_t* descriptor) {
    const uint32_t d0 = descriptor[0], d1 = descriptor[1];
    return fetch_group(c, d0, d1, descriptor, ((d0 >> 16) & 3) - ((d1 >> 24) & 3) + ((d0 >> 18) & 3));
}
// A write that changes the mapping sets stop, and then this group only
// leaves native code. Otherwise the bank in the block's key is still mapped
// and holds the compiled instruction. Either way the cycles to take back are
// the compiled instruction's, which the descriptor keeps.
uint32_t careful_group_rom(Context* c, const uint32_t* descriptor) {
    const uint32_t d = *descriptor;
    const auto pc = static_cast<uint16_t>(d & 0x7fff);
    auto& memory = *c->memory;
    const auto opcode = memory.peek(pc);
    const unsigned length = instruction_length(opcode);
    uint32_t bytes = opcode;
    for (unsigned k = 1; k < length; ++k) bytes |= uint32_t{memory.peek(static_cast<uint16_t>(pc + k))} << (k * 8);
    return fetch_group(c, pc | length << 16 | leading_idles(opcode) << 18 | (d >> 17 & 15) << 20 | (d >> 21 & 1) << 25,
                       bytes | ((1u << length) - 1) << 26, descriptor, (d >> 15 & 3) + 1);
}

// Flat test buses: generated code reaches these through the same register-
// preserving stubs as mapped careful accesses.
uint32_t read_bus(Context* c, uint32_t address) {
    ++c->mcycles;
    return c->bus->read(static_cast<uint16_t>(address));
}
void write_bus(Context* c, uint32_t address, uint32_t value) {
    ++c->mcycles;
    c->bus->write(static_cast<uint16_t>(address), static_cast<uint8_t>(value));
}
void idle_bus(Context* c) {
    ++c->mcycles;
    c->bus->idle();
}

}  // namespace gbx::gb::sm83_runtime
