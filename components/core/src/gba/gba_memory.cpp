// The GBA's memory map, wait states, prefetch buffer and the slow paths of
// the CPU's bus. The I/O registers are in gba_io.cpp, the timers, DMA and
// PPU in their own files.

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

#include "gbx/gba/memory.h"
#include "gbx/platform.h"
#include "../gb/offload.h"
#include "mixer.h"
#include "sound.h"

namespace gbx::gba {
namespace {

constexpr size_t kEwramBytes = 256 * 1024, kVramBytes = 96 * 1024;
// GBATEK "GBA System Control", WAITCNT: wait states by field value.
constexpr uint8_t kRomN[4] = {4, 3, 2, 8};
constexpr uint8_t kRomS[3][2] = {{2, 1}, {4, 1}, {8, 1}};

}  // namespace

Memory::Memory() = default;

Memory::~Memory() {
    // The worker may still be drawing from this memory's PPU, which, a
    // member, outlives this body and the worker, or mixing.
    ppu.detach();
    if (mixer_) mixer_->finish();
    offload_.reset();
    mixer_.reset();
    platform::free_external(ewram);
    platform::free_external(vram);
    platform::free_internal(ewram_code);
    // The ROM's cache outlives the memory, and another system may use it.
    if (rom && gpio_page_) rom->end_overlay(gpio_page_);
    platform::free_external(gpio_page_);
}

bool Memory::initialize() {
    if (!ewram) ewram = static_cast<uint8_t*>(platform::allocate_external(kEwramBytes));
    if (!vram) vram = static_cast<uint8_t*>(platform::allocate_external(kVramBytes));
    if (!ewram_code) ewram_code = static_cast<uint8_t*>(platform::allocate_internal(kEwramBytes >> kCodeLineBits));
    if (!ewram || !vram || !ewram_code || !backup.allocate()) return false;
    if (!offload_) {
        offload_.reset(new (std::nothrow) gb::Offload);
        // The GBA's renderer can take most of the other core. Below the
        // present task, LVGL, the ROM cache's reader and the app's tasks,
        // and level with the main task's input and the console, it leaves
        // them the time they need; its queue holds more than a frame, so
        // the CPU core does not wait while they run.
        if (offload_) offload_->set_priority(1);
    }
    if (offload_ && !mixer_) mixer_.reset(new (std::nothrow) Mixer(*offload_));
    return offload_ && mixer_ && offload_->start() && ppu.initialize(*offload_);
}

bool Memory::load(std::span<const uint8_t, kBiosBytes> bios_image, RomCache& cartridge) {
    bios = bios_image.data();
    rom = &cartridge;
    rom_table = cartridge.table();
    backup.configure(cartridge.backup_id(), cartridge.size());
    return load_gpio();
}

void Memory::update_waits() {
    const uint16_t waitcnt = io16(0x204);
    // Regions without wait states: the BIOS, IWRAM, I/O and OAM; palette RAM
    // and VRAM are 16 bits wide (GBATEK "GBA Memory Map").
    for (unsigned region = 0; region < 16; ++region) {
        cycles_n[region] = cycles_s[region] = {1, 1, 1};
    }
    cycles_n[5][2] = cycles_s[5][2] = cycles_n[6][2] = cycles_s[6][2] = 2;
    // EWRAM: 16 bits wide, 15 - the control's field wait states.
    const unsigned ewram_wait = 15 - ((memory_control_ >> 24) & 15);
    cycles_n[2] = cycles_s[2] = {static_cast<uint8_t>(1 + ewram_wait), static_cast<uint8_t>(1 + ewram_wait),
                                 static_cast<uint8_t>(2 + 2 * ewram_wait)};
    // ROM: 16 bits wide; a word's second half is S.
    for (unsigned ws = 0; ws < 3; ++ws) {
        const unsigned n = 1 + kRomN[(waitcnt >> (2 + 3 * ws)) & 3];
        const unsigned s = 1 + kRomS[ws][(waitcnt >> (4 + 3 * ws)) & 1];
        for (unsigned region : {8 + 2 * ws, 9 + 2 * ws}) {
            cycles_n[region] = {static_cast<uint8_t>(n), static_cast<uint8_t>(n), static_cast<uint8_t>(n + s)};
            cycles_s[region] = {static_cast<uint8_t>(s), static_cast<uint8_t>(s), static_cast<uint8_t>(2 * s)};
        }
    }
    // SRAM: 8 bits wide; every width is one access.
    const auto sram = static_cast<uint8_t>(1 + kRomN[waitcnt & 3]);
    cycles_n[14] = cycles_s[14] = cycles_n[15] = cycles_s[15] = {sram, sram, sram};
    prefetch_enabled = waitcnt & 0x4000;
    if (!prefetch_enabled) prefetch_.head = 0;
    ++waits_generation;
    request_stop();
}

// --- Time -------------------------------------------------------------------

void Memory::rearm() {
    ++spin_epoch;
    if (clock.now >= target) stop = true;
    Tick limit = std::min(clock.next, target);
    // The interrupt line's time stands still in a DMA, and runs behind the
    // clock by irq_stalled_ otherwise.
    if (irq_watch_)
        limit = std::min(limit, irq_stall_from_ == kNever ? std::max(clock.now, irq_next_ + irq_stalled_) : clock.now);
    if (stop || dma_bus_ || dma_pending_ || dma_deferred_ != kNever) limit = clock.now;
    // The countdown is 32 bits; a far deadline is refreshed on the way.
    limit = std::min<Tick>(limit, clock.now + 0x40000000);
    deadline = limit;
    left = static_cast<int32_t>(limit - clock.now);
    if (static_cast<int32_t>(static_cast<uint32_t>(clock.now) - prefetch_.base) > (1 << 28)) [[unlikely]]
        age_prefetch();
}

void Memory::catch_up() {
#ifdef GBX_PROFILER
    ++catch_ups;
#endif
    const Tick time = now();
    if (dma_deferred_ != kNever) {
        // A DMA found at an internal cycle starts where it was found; this
        // access waits for it if it is still running.
        dma_deferred_ = kNever;
        // The access begins a CPU cycle before it waits; the events before
        // it come with the DMA.
        const Tick edge = irq_time(time);
        run_dma(time);
        if (clock.now < time) clock.advance(time);
        if (irq_watch_) irq_edge(edge);
    } else {
        clock.advance(time);
        if (irq_watch_) irq_edge(irq_time(time));
        if (dma_pending_) run_dma(time);
    }
    if (halt_in_access_) {
        halt_in_access_ = false;
        wait_halt(false);
        clock.advance(clock.now + halt_tail_);
        halt_tail_ = 0;
    }
    rearm();
}

// A DMA found due at an internal cycle starts there, but runs at the CPU's
// next bus access, which waits for it if it has not ended by then: the
// internal cycles in between change nothing it reads or writes. The clock
// stays at the DMA's start meanwhile, and each internal cycle checks again.
void Memory::idle_slow() {
    const Tick time = now();
    if (dma_deferred_ == kNever) {
        clock.advance(time);
        if (irq_watch_) irq_edge(irq_time(time));
        if (!dma_pending_) {
            rearm();
            return;
        }
        dma_deferred_ = time;
    } else if (irq_watch_) {
        // The events since the DMA's start wait for the next bus access.
        irq_edge(irq_time(time));
    }
    deadline = time;
    left = 0;
}

// --- The prefetch buffer --------------------------------------------------------

// The prefetch fetches the halfwords after the CPU's opcode fetches from
// the ROM while the gamepak bus is free, each in S cycles, and an opcode
// fetch it holds takes 1 cycle; one it is fetching takes the cycles left
// (GBATEK "GBA GamePak Prefetch"). It stops when it holds 8 halfwords or
// would fetch across a 128 KiB boundary, and starts again, with an N
// access, once the CPU has taken them all (alyosha prefetcher/readme.txt).
// A gamepak access by the CPU's data or a DMA stops it and drops what it
// holds; an opcode fetch elsewhere in the ROM does that and starts it
// after the fetch.
//
// The buffer is computed from the time when the CPU fetches: halfword i
// after the head arrives at base + (i + 1) * cycles while it runs, so
// nothing is done for the cycles in between. rom_fetch() has the common
// cases inline.
unsigned Memory::rom_fetch_slow(uint32_t address, unsigned width, uint32_t request) {
    auto& p = prefetch_;
    const bool sequential = request & kSequential;
    const uint32_t time = static_cast<uint32_t>(now());
    const unsigned halves = width == 2 ? 2 : 1;
    // Right after the CPU's own access the prefetch has yet to begin, and a
    // fetch of the head then is the CPU's own access too: S, or N for a
    // branch (alyosha prefetcher/readme.txt); the prefetch starts after it.
    // Once it runs, it fetches the head however the CPU asks for it.
    if (p.head == address && (p.running || p.paused || static_cast<int32_t>(time - p.start) > 0)) {
        const unsigned region = address >> 24;
        if (!p.paused) {
            const uint32_t room = (0x20000 - (address & 0x1ffff)) >> 1;
            const unsigned held = room < 8 ? room : 8;
            if (static_cast<int32_t>(time - p.base) >= static_cast<int32_t>(held * p.cycles))
                p.paused = static_cast<uint8_t>(held);
        }
        unsigned cost;
        p.stalled = false;
        if (p.paused) {
            if (halves <= p.paused) {
                p.paused = static_cast<uint8_t>(p.paused - halves);
                cost = 1;
            } else {
                // An ARM fetch takes the last halfword held and waits for
                // the restarted prefetch to bring the next.
                p.paused = 0;
                cost = cycles_n[region][1];
            }
            p.head = address + 2 * halves;
            if (!p.paused) {
                // Empty: it stays stopped, and the CPU's next fetch is N.
                p.head = 0;
                gamepak_next_ = 0;
            }
            return cost;
        }
        const uint32_t arrival = p.base + halves * p.cycles;
        const auto wait = static_cast<int32_t>(arrival - time);
        cost = wait > 0 ? static_cast<unsigned>(wait) : 1;
        p.running = true;
        p.stalled = wait > 0 && halves == 2;
        p.base = p.start = arrival;
        p.head = address + 2 * halves;
        if (!(p.head & 0x1ffff)) {
            // It stopped before the boundary and starts there with N.
            p.start = time + cost;
            p.base = p.start + cycles_n[region][1] - p.cycles;
        }
        return cost;
    }
    // A fetch elsewhere waits for the halfword the prefetch is fetching.
    // After an ARM fetch that waited for it, the prefetch goes straight on
    // with the next word, and a branch to the word after that one waits
    // for it (alyosha prefetcher_branch_thumb_arm_3; a branch elsewhere
    // does not, irq/IRQ_sub).
    const bool next_word = (request & kRefill) && p.head && p.stalled && time == p.start && address == p.head + 4;
    const unsigned wait = next_word ? 2 * p.cycles : prefetch_wait(time);
    const unsigned cost = wait + gamepak_cycles(address, width, sequential);
    if (prefetch_enabled) {
        const unsigned region = address >> 24;
        p.head = address + 2 * halves;
        p.cycles = cycles_s[region][1];
        p.paused = 0;
        p.running = p.stalled = false;
        p.start = p.base = time + cost;
        if (!(p.head & 0x1ffff)) p.base += cycles_n[region][1] - p.cycles;
    }
    return cost;
}

void Memory::stop_prefetch() { prefetch_.head = 0; }

// The halfwords the buffer brings follow from base alone while it runs
// unpaused: native code times the fetches of its head with the rules of
// rom_fetch() above, and leaves the rest here. Before the buffer starts,
// the CPU's own fetch of the head is S, so the gamepak's address counter
// must be at the head.
uint32_t Memory::lend_prefetch(uint32_t next, int32_t& offset) {
    auto& p = prefetch_;
#ifdef GBX_GBA_RELAXED_TIMING
    // Relaxed timing: the JIT times ROM fetches without the buffer
    // (Compiler::m_rom_fetch()), which stays stopped while native code runs.
    (void)next;
    (void)offset;
    p.head = 0;
    return 0;
#endif
    const unsigned region = next >> 24;
    if (!prefetch_enabled || region < 8 || region > 13 || ((next + 0x10) & 0x1ffff) <= 0x10) return 0;
    uint32_t state;
    if (!p.head) {
        if (gamepak_next_ == next) return 0;
        state = kLent | kLentEmpty;
    } else {
        if (p.head != next || p.start != p.base || p.cycles != cycles_s[region][1]) return 0;
        if (p.paused) state = kLent | uint32_t{p.paused} << kLentPausedShift;
        else if (p.running) state = kLent | kLentRunning | (p.stalled ? kLentStalled : 0);
        else if (gamepak_next_ == next) state = kLent;
        else return 0;
    }
    const auto distance = static_cast<int32_t>(p.base - static_cast<uint32_t>(deadline));
    if (distance < -0x40000000 || distance > 0x40000000) return 0;
    offset = distance;
    p.head = 0;
    return state;
}

void Memory::return_prefetch(uint32_t head, int32_t offset, uint32_t state) {
    auto& p = prefetch_;
    if (state & kLentEmpty) {
        p.head = 0;
        if (state & kLentDrained) gamepak_next_ = 0;
        return;
    }
    p.head = head;
    p.base = p.start = static_cast<uint32_t>(deadline) + static_cast<uint32_t>(offset);
    // Native code starts an empty buffer at the rate of the current wait
    // states, which a lent buffer already had.
    p.cycles = cycles_s[(head >> 24) & 15][1];
    p.paused = static_cast<uint8_t>(state >> kLentPausedShift);
    p.running = state & kLentRunning;
    p.stalled = state & kLentStalled;
    // Until it starts, the CPU's own fetches move the address counter.
    if (!p.running && !p.paused) gamepak_next_ = head;
}

// Cycles until the halfword the prefetch is fetching at `time` arrives; 0
// when it is not in the middle of one. Stopped full it fetches nothing, but
// stopped before a 128 KiB boundary it keeps the bus in its S slots
// (alyosha prefetcher_boundary_1 to _4).
unsigned Memory::prefetch_wait(Tick time) const {
    const auto& p = prefetch_;
    const auto now32 = static_cast<uint32_t>(time);
    if (!p.head || static_cast<int32_t>(now32 - p.start) <= 0) return 0;
    const uint32_t room = (0x20000 - (p.head & 0x1ffff)) >> 1;
    const auto since = static_cast<int32_t>(now32 - p.base);
    if (room >= 8) {
        if (p.paused || since >= 8 * static_cast<int32_t>(p.cycles)) return 0;
    }
    if (since < static_cast<int32_t>(p.cycles)) return static_cast<unsigned>(static_cast<int32_t>(p.cycles) - since);
    const unsigned into = static_cast<unsigned>(since) % p.cycles;
    return into ? p.cycles - into : 0;
}

// The buffer's 32-bit times stay within 2^28 cycles of now(): older ones
// move on by whole halfwords, which changes none of the comparisons above.
void Memory::age_prefetch() {
    auto& p = prefetch_;
    const uint32_t age = static_cast<uint32_t>(clock.now) - p.base - (1u << 27);
    const uint32_t step = p.cycles ? age - age % p.cycles : age;
    p.base += step;
    p.start += step;
}

// --- Slow paths -----------------------------------------------------------------

uint32_t Memory::fetch_slow(uint32_t address, unsigned width, bool sequential) {
    // Opcodes from VRAM, palette RAM, OAM, I/O or unmapped memory.
    ++spin_epoch;
    clock.now = now();
    const unsigned region = address >> 24;
    const uint32_t value = region < 16 ? read_region(address, width, address) : open_bus(address, width, address);
    left -= region < 16 ? (sequential ? cycles_s : cycles_n)[region][width] : 1;
    return value;
}

uint32_t Memory::read_slow(uint32_t address, unsigned width, bool sequential, uint32_t r15) {
    // I/O reads may see the time, which no event marks.
    ++spin_epoch;
    clock.now = now();
    const unsigned region = address >> 24;
    unsigned cycles;
    if (region >= 8 && region < 16) {
        // It waits for the halfword the prefetch is fetching.
        const unsigned wait = prefetch_wait(clock.now);
        stop_prefetch();
        cycles = wait + (region < 14 ? gamepak_cycles(address, width, sequential) : cycles_n[region][width]);
    } else {
        cycles = region < 16 ? (sequential ? cycles_s : cycles_n)[region][width] : 1;
    }
    const uint32_t value = region < 16 ? read_region(address, width, r15) : open_bus(address, width, r15);
    left -= static_cast<int32_t>(cycles);
    return value;
}

void Memory::write_slow(uint32_t address, unsigned width, bool sequential, uint32_t value) {
    clock.now = now();
    const unsigned region = address >> 24;
    unsigned cycles;
    if (region >= 8 && region < 16) {
        // It waits for the halfword the prefetch is fetching.
        const unsigned wait = prefetch_wait(clock.now);
        stop_prefetch();
        cycles = wait + (region < 14 ? gamepak_cycles(address, width, sequential) : cycles_n[region][width]);
    } else {
        cycles = region < 16 ? (sequential ? cycles_s : cycles_n)[region][width] : 1;
    }
    if (region < 16) write_region(address, width, value);
    // A write may schedule events, request an interrupt or stop the run.
    rearm();
    left -= static_cast<int32_t>(cycles);
}

uint32_t Memory::read_region(uint32_t address, unsigned width, uint32_t r15) {
    switch (address >> 24) {
    case 0x0:
        if (address >= kBiosBytes) break;
        // The BIOS reads only while the CPU runs in it.
        if (r15 < kBiosBytes) return load(bios, address, width);
        return width == 2 ? bios_latch : (bios_latch >> (8 * (address & (width ? 2 : 3)))) & (width ? 0xffff : 0xff);
    case 0x2: return load(ewram, address & 0x3ffff, width);
    case 0x3: return load(iwram.data(), address & 0x7fff, width);
    case 0x4: return io_read(address, width, r15);
    case 0x5: return load(palette.data(), address & 0x3ff, width);
    case 0x6: {
        const uint32_t offset = vram_offset(address);
        return offset < 0x18000 ? load(vram, offset, width) : 0;
    }
    case 0x7: return load(oam.data(), address & 0x3ff, width);
    case 0x8: case 0x9: case 0xa: case 0xb: case 0xc:
        return rom_value(address, width);
    case 0xd:
        if (backup.eeprom_at(address)) return backup.eeprom_read(clock.now);
        return rom_value(address, width);
    case 0xe: case 0xf: {
        // 8 bits wide: the byte repeats in wider reads (GBATEK "GBA
        // Unpredictable Things").
        const uint32_t byte = backup.read(address);
        return width == 2 ? byte * 0x01010101u : width == 1 ? byte * 0x0101u : byte;
    }
    default: break;
    }
    return open_bus(address, width, r15);
}

uint32_t Memory::rom_value(uint32_t address, unsigned width) {
    return load(rom_page(address), address & (RomCache::kPageBytes - 1), width);
}

void Memory::write_region(uint32_t address, unsigned width, uint32_t value) {
    switch (address >> 24) {
    case 0x2: {
        const uint32_t offset = address & 0x3ffff;
        if (ewram_code[offset >> kCodeLineBits]) store_code(ewram, offset, width, value, address);
        else store(ewram, offset, width, value);
        return;
    }
    case 0x3: {
        const uint32_t offset = address & 0x7fff;
        if (iwram_code[offset >> kCodeLineBits]) store_code(iwram.data(), offset, width, value, address);
        else store(iwram.data(), offset, width, value);
        return;
    }
    case 0x4: io_write(address, width, value); return;
    case 0x5: case 0x6: case 0x7: write_video(address, width, value); return;
    case 0x8: case 0xa: case 0xc:
        if (gpio_page_ && (address & 0x1fffff0) == 0xc0) write_gpio(address, width, value);
        return;
    case 0xd:
        if (backup.eeprom_at(address)) backup.eeprom_write(static_cast<uint16_t>(value), clock.now);
        return;
    case 0xe: case 0xf:
        // The byte of the value that this address's lane carries.
        backup.write(address, static_cast<uint8_t>(value >> (8 * (address & (width == 2 ? 3 : width == 1 ? 1 : 0)))));
        return;
    default: return;
    }
}

// Palette RAM, VRAM and OAM. A byte write to palette RAM or BG VRAM fills
// its halfword; to OBJ VRAM or OAM it is ignored (GBATEK "GBA Unpredictable
// Things"). Each write marks its page for the renderer.
void Memory::write_video(uint32_t address, unsigned width, uint32_t value) {
    const unsigned region = address >> 24;
    uint8_t* base;
    uint32_t offset;
    unsigned page;
    if (region == 6) {
        offset = vram_offset(address);
        if (offset >= 0x18000 || (width == 0 && offset >= vram_obj_start)) return;
        base = vram;
        page = offset >> Ppu::kPageBits;
    } else {
        offset = address & 0x3ff;
        if (width == 0 && region == 7) return;
        base = region == 5 ? palette.data() : oam.data();
        page = region == 5 ? Ppu::kPalettePage + (offset >> Ppu::kColorPageBits)
                           : Ppu::kOamPage + (offset >> Ppu::kPageBits);
    }
    if (width == 0) {
        width = 1;
        value = (value & 0xff) * 0x0101u;
    }
    store(base, offset, width, value);
    dirty[page >> 5] |= 1u << (page & 31);
}

// GBATEK "GBA Unpredictable Things": the CPU reads the opcodes it fetched
// last, [$+8] in ARM state. In Thumb state it depends on where they are:
// [$+4] twice from EWRAM, palette RAM, VRAM and the ROM; [$+4] with
// [$+6] or [$+2] with [$+4] from the BIOS and OAM, by the alignment; [$+4]
// with the IWRAM bus's other half, [$+2], from IWRAM. A DMA that moved a
// value since the last opcode fetch left that on the bus instead (alyosha
// Bus/ReadMe.txt).
uint32_t Memory::open_bus(uint32_t address, unsigned width, uint32_t r15) {
    if (lazy_pipeline) {
        const bool thumb = cpu->cpsr & kFlagT;
        const uint32_t last = lazy_pipeline & ~1u, length = thumb ? 2 : 4;
        cpu->pipeline = {peek(last - length, thumb ? 1 : 2), peek(last, thumb ? 1 : 2)};
    }
    uint32_t word;
    if (dma_bus_) {
        word = dma_latch_;
    } else if (!(cpu->cpsr & kFlagT)) {
        word = cpu->pipeline[1];
    } else {
        const uint32_t next = cpu->pipeline[1] & 0xffff, current = cpu->pipeline[0] & 0xffff;
        switch (r15 >> 24) {
        case 0x0: case 0x7:
            word = r15 & 2 ? current | next << 16 : next | peek(r15 + 2, 1) << 16;
            break;
        case 0x3:
            word = r15 & 2 ? current | next << 16 : next | current << 16;
            break;
        default:
            word = next * 0x10001u;
            break;
        }
    }
    if (width == 2) return word;
    const unsigned shift = 8 * (address & (width ? 2 : 3));
    return (word >> shift) & (width ? 0xffff : 0xff);
}

uint32_t Memory::peek(uint32_t address, unsigned width) {
    switch (address >> 24) {
    case 0x0: return address < kBiosBytes ? load(bios, address, width) : 0;
    case 0x2: return load(ewram, address & 0x3ffff, width);
    case 0x3: return load(iwram.data(), address & 0x7fff, width);
    case 0x5: return load(palette.data(), address & 0x3ff, width);
    case 0x6: {
        const uint32_t offset = vram_offset(address);
        return offset < 0x18000 ? load(vram, offset, width) : 0;
    }
    case 0x7: return load(oam.data(), address & 0x3ff, width);
    case 0x8: case 0x9: case 0xa: case 0xb: case 0xc: case 0xd: return rom_value(address, width);
    default: return 0;
    }
}

// code_written() runs before the store: native code in the middle of a
// block keeps the pipeline it fetched, which code_written() reads from the
// memory as it was (a DMA's write; the CPU's own write snapped it before).
void Memory::store_code(uint8_t* base, uint32_t offset, unsigned width, uint32_t value, uint32_t address) {
    const uint32_t mask = width == 2 ? 0xffffffffu : width == 1 ? 0xffffu : 0xffu;
    if (code_written && load(base, offset, width) != (value & mask)) code_written(code_context, address);
    store(base, offset, width, value);
}

// --- Power-on ------------------------------------------------------------------

namespace {
// What the BIOS's start-up leaves when it starts the cartridge, found by
// running it from power-on (System::reset_to_bios()) and checked with the
// tests that depend on it: alyosha ppu/start_up (the PPU's position),
// timer/timer (the prescaler's phase), timer/timer_reset (timer 0, which
// the start-up sound left stopped), jsmolka bios (the BIOS opcode last
// fetched, E129F000h). The start-up takes 75998008 cycles.
constexpr unsigned kBootLine = 126, kBootCycle = 842;
constexpr Tick kBootPrescaler = 858;
constexpr uint16_t kBootTimer0 = 0xff8b;
constexpr uint32_t kBootLatch = 0xe129f000;
// The I/O registers it leaves that are not zero: DISPCNT's forced blank,
// BG2 and BG3's PA and PD at 1.0, SOUNDCNT_H, SOUNDBIAS, SIOCNT and RCNT
// from its multiboot check, and POSTFLG.
constexpr std::pair<uint16_t, uint16_t> kBootIo[] = {
    {0x000, 0x0080}, {0x020, 0x0100}, {0x026, 0x0100}, {0x030, 0x0100}, {0x036, 0x0100}, {0x082, 0x000e},
    {0x088, 0x0200}, {0x128, 0x0080}, {0x134, 0x8000}, {0x300, 0x0001}};
}  // namespace

void Memory::reset(bool power_on) {
    ppu.finish();
    init_io_reads();
    // The clock runs on its battery; the port's outputs drop, which ends a
    // transfer.
    const RtcState clock_state = rtc.state(clock.now);
    clock.reset();
    rtc.reset(clock_state, 0);
    gpio_ = {};
    deadline = 0;
    left = 0;
    stop = irq_line = irq_unmasked = halted = false;
    spin_ = {};
    ++spin_epoch;
    std::memset(ewram, 0, kEwramBytes);
    std::memset(vram, 0, kVramBytes);
    iwram.fill(0);
    palette.fill(0);
    oam.fill(0);
    io_.fill(0);
    iwram_code.fill(0);
    std::memset(ewram_code, 0, kEwramBytes >> kCodeLineBits);
    reset_irq();
    timer_irq_ = 0;
    timer_irq_time_ = kNever;
    memory_control_ = 0x0d000020;
    timers_ = {};
    fifo_quiet_until_ = 0;
    dma_ = {};
    dma_latch_ = 0;
    dma_pending_ = 0;
    dma_running_ = false;
    dma_deferred_ = kNever;
    dma_bus_ = false;
    fifo_ = {};
    capture_ = false;
    prefetch_ = {};
    gamepak_next_ = 0;
    frames = 0;
    halt_cycles = 0;
    backup.reset_state();
    if (power_on) {
        prescaler_phase_ = 0;
        bios_latch = 0;
        update_waits();
        vram_obj_start = 0x10000;
        ppu.reset(*this, 0, 0, 0);
    } else {
        for (const auto& [offset, value] : kBootIo) std::memcpy(io_.data() + offset, &value, 2);
        timers_[0].counter = kBootTimer0;
        prescaler_phase_ = kBootPrescaler;
        bios_latch = kBootLatch;
        update_waits();
        vram_obj_start = 0x10000;
        // The BIOS's last two fetches from the cartridge, at 8000000h and
        // 8000004h, leave the gamepak sequential.
        gamepak_next_ = 0x08000008;
        ppu.reset(*this, 0, kBootLine, kBootCycle);
    }
    reset_sound();
    if (gpio_page_) update_gpio();
    rearm();
}

void Memory::catch_up_boundary() {
    const Tick time = now();
    if (dma_deferred_ != kNever) {
        // A DMA found at an internal cycle keeps the clock at its start
        // until the next access runs it.
        deadline = time;
        left = 0;
        return;
    }
    clock.advance(time);
    rearm();
}

void Memory::begin_run(Tick until) {
    // Another system may have run on the same ROM cache.
    if (gpio_page_) update_gpio();
    target = until;
    stop = halted || (irq_line && !(cpu->cpsr & kFlagI));
    catch_up_boundary();
}

void Memory::set_buttons(uint16_t pressed) {
    buttons_ = static_cast<uint16_t>(~pressed & 0x3ff);
    check_keypad();
}

}  // namespace gbx::gba
