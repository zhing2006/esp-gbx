// The four DMA channels (GBATEK "GBA DMA Transfers"). A channel that starts
// runs inside the bus check that found it due, see Memory's comment: the
// transfer, the events that fall due between its units and the channels of
// higher priority they start, which take over after the unit in progress.

#include <algorithm>
#include <bit>
#include <cstring>

#include "gbx/gba/memory.h"

namespace gbx::gba {
namespace {

// Cycles from a write that enables an immediate DMA, or from a blanking
// or capture trigger, until the DMA starts; the trigger's delay counts
// from the edge that raises the PPU's flag, and alyosha DMA/DMA_pause_timing
// and Interactions agree with it. A sound FIFO's request counts from the
// timer's overflow: alyosha fifo_dma/fifo_2 moves the overflow by a cycle
// at a time, and a CPU access 4 cycles after it runs before the DMA, one 5
// cycles after it waits for the DMA. Its 2 internal cycles, 4 when both addresses are in
// the gamepak (GBATEK), come half before its first access and half after
// its last, the CPU waiting for both: with this split nba-hw-test
// dma/start-delay, dma/force-nseq-access and alyosha Bus/LDRSH_misaligned
// agree, the last one showing that a CPU access started in the cycle
// before the DMA starts runs first.
constexpr Tick kImmediateDelay = 3, kTriggerDelay = 2, kFifoDelay = 5;
// GBATEK: the most significant address bits are ignored.
constexpr uint32_t kSourceMask[4] = {0x07ffffff, 0x0fffffff, 0x0fffffff, 0x0fffffff};
constexpr uint32_t kDestinationMask[4] = {0x07ffffff, 0x07ffffff, 0x07ffffff, 0x0fffffff};

unsigned base(unsigned channel) { return 0xb0 + 12 * channel; }

}  // namespace

// A write to DMAxCNT_H at clock.now; `old` is the value before.
void Memory::write_dma(unsigned channel, uint16_t value, uint16_t old) {
    auto& d = dma_[channel];
    const unsigned io = base(channel);
    // A FIFO's requests are events only while a channel waits to feed it.
    const bool fifo = (channel == 1 || channel == 2) && ((value | old) & 0x3000) == 0x3000;
    if (!(value & 0x8000)) {
        d.start = kNever;
        dma_pending_ &= static_cast<uint8_t>(~(1u << channel));
        schedule_dma();
        if (fifo && (old & 0x8000)) schedule_timers();
        return;
    }
    if (old & 0x8000) {
        // Already enabled: a new timing waits for its trigger.
        if (fifo) schedule_timers();
        return;
    }
    // Enabling copies the registers into the channel's internal ones.
    d.source = (io16(io) | uint32_t{io16(io + 2)} << 16) & kSourceMask[channel];
    d.destination = (io16(io + 4) | uint32_t{io16(io + 6)} << 16) & kDestinationMask[channel];
    const uint32_t count = io16(io + 8) & (channel == 3 ? 0xffff : 0x3fff);
    d.count = count ? count : (channel == 3 ? 0x10000 : 0x4000);
    d.gamepak_sequential = false;
    d.paused = false;
    d.eeprom = channel == 3 && (backup.eeprom_at(d.source) || backup.eeprom_at(d.destination));
    if (d.eeprom) backup.eeprom_dma(d.count, backup.eeprom_at(d.destination));
    if (((value >> 12) & 3) == 0) {
        d.start = clock.now + kImmediateDelay;
        schedule_dma();
    } else {
        // The PPU's events include the blanking edges only while a DMA waits for them.
        ppu.reschedule();
        if (fifo) schedule_timers();
    }
}

bool Memory::dma_waiting(unsigned timing) const {
    for (unsigned c = timing == 3 ? 3 : 0; c < 4; ++c) {
        const uint16_t control = io16(base(c) + 10);
        if ((control & 0x8000) && ((control >> 12) & 3) == timing) return true;
    }
    return false;
}

void Memory::stop_capture() {
    capture_ = false;
    const uint16_t control = io16(base(3) + 10);
    if ((control & 0xb000) != 0xb000) return;
    const uint16_t cleared = control & 0x7fff;
    std::memcpy(io_.data() + base(3) + 10, &cleared, 2);
    dma_[3].start = kNever;
    dma_pending_ &= static_cast<uint8_t>(~8u);
    schedule_dma();
}

// A start condition: 1 VBlank, 2 HBlank, 3 a channel's special one.
void Memory::trigger_dma(unsigned timing, unsigned channels) {
    bool any = false;
    for (unsigned c = 0; c < 4; ++c) {
        if (!(channels & (1u << c))) continue;
        const uint16_t control = io16(base(c) + 10);
        if (!(control & 0x8000) || ((control >> 12) & 3) != timing) continue;
        if (dma_[c].start != kNever || (dma_pending_ & (1u << c))) continue;
        const bool fifo = timing == 3 && c != 3;
        dma_[c].start = clock.now + (fifo ? kFifoDelay : kTriggerDelay);
        dma_[c].gamepak_sequential = false;
        // Sound FIFO DMA moves 4 words whatever the count (GBATEK).
        if (fifo) dma_[c].count = 4;
        any = true;
    }
    if (any) schedule_dma();
}

void Memory::schedule_dma() {
    Tick next = kNever;
    for (const auto& d : dma_) next = d.start < next ? d.start : next;
    // A start at clock.now is the event of this slot that the dispatch
    // under way has yet to run, as when the timer's event before it
    // triggers a FIFO's DMA: it stays, the same-time bus access waiting
    // for it.
    if (next == kNever) clock.cancel(kSlotDma);
    else if (next > clock.now) clock.schedule(kSlotDma, next, dma_event, this);
}

void Memory::dma_event(void* context) {
#ifdef GBX_PROFILER
    ++event_counts[2];
#endif
    auto& m = *static_cast<Memory*>(context);
    for (unsigned c = 0; c < 4; ++c) {
        if (m.dma_[c].start <= m.clock.now) {
            m.dma_[c].start = kNever;
            m.dma_pending_ |= static_cast<uint8_t>(1u << c);
        }
    }
    // The bus check that dispatched this event runs the transfer.
    m.schedule_dma();
}

// Runs the pending channels from clock.now, highest priority first, with
// the events that fall due meanwhile. The CPU's bus access at `access`
// waits for the end; from then on the interrupt line's delay stands still
// (alyosha Interactions/ReadMe.txt).
void Memory::run_dma(Tick access) {
    // The interrupt line stands still while the CPU waits (alyosha
    // Interactions/ReadMe.txt); in HALT (access kNever) it does not.
    irq_stall_from_ = access;
    dma_running_ = true;
    while (dma_pending_) transfer(static_cast<unsigned>(std::countr_zero(dma_pending_)));
    dma_running_ = false;
    if (access != kNever && clock.now > access) irq_stalled_ += clock.now - access;
    irq_stall_from_ = kNever;
    dma_bus_ = true;
    // The CPU's next gamepak access after a DMA is N, wherever the DMA
    // went (nba-hw-test dma/force-nseq-access).
    gamepak_next_ = 0;
}

// Transfers units of `channel` until it is done or a channel of higher
// priority becomes due.
//
// Values. Each channel has a 32-bit latch: a word read fills it, a
// halfword read fills both halves, and a halfword write takes the half of
// the destination's alignment. Reads below 2000000h leave the latch as it
// was; reads of unused memory or unreadable I/O see the open bus, which is
// the CPU's last opcode fetch before the first unit and the DMA's own
// last value after (nba-hw-test dma/latch, alyosha Bus/ReadMe.txt).
//
// Gamepak. A channel's first gamepak access of a transfer is N, its later
// ones S, other memory in between or not, but N after one at the end of a
// 128 KiB block,
// whatever the direction (alyosha DMA/readme.txt, nba-hw-test
// bus/128kb-boundary). An S access reads or writes where the cartridge's
// own address counter points, the address after its last access, not the
// channel's address (nba-hw-test dma/burst-into-tears).
void Memory::transfer(unsigned channel) {
    auto& d = dma_[channel];
    const unsigned io = base(channel);
    const uint16_t control = io16(io + 10);
    const unsigned timing = (control >> 12) & 3;
    const bool fifo = timing == 3 && (channel == 1 || channel == 2);
    const unsigned width = fifo || (control & 0x400) ? 2 : 1;
    const uint32_t size = 1u << width;
    const unsigned source_mode = (control >> 7) & 3, destination_mode = fifo ? 2 : (control >> 5) & 3;
    const bool both_gamepak = (d.source >> 24) >= 8 && (d.source >> 24) < 14 && (d.destination >> 24) >= 8 &&
                              (d.destination >> 24) < 14;
    const Tick internal = both_gamepak ? 2 : 1;
    if (!d.paused) clock.now += internal;
    const uint8_t higher = static_cast<uint8_t>((1u << channel) - 1);
    while (d.count) {
        if (!d.paused) {
            // Units between memories without side effects run together while
            // no event falls due, both addresses stay in their regions and
            // the gamepak's reads stay S at the channel's address: each takes
            // the same cycles, and nothing but the units before it changes
            // what it reads.
            const unsigned from_region = d.source >> 24, to_region = d.destination >> 24;
            const bool rom = from_region >= 8 && from_region < 14;
            const bool plain = to_region >= 2 && to_region < 8 && to_region != 4 &&
                               (rom ? d.gamepak_sequential && !d.eeprom && (source_mode == 0 || source_mode == 3) &&
                                          !prefetch_.head && gamepak_next_ == (d.source & ~(size - 1))
                                    : from_region >= 2 && from_region < 8 && from_region != 4);
            if (plain) {
                const unsigned cycles = (rom ? cycles_s : cycles_n)[from_region][width] + cycles_n[to_region][width];
                uint64_t units = clock.next > clock.now ? (clock.next - clock.now - 1) / cycles : 0;
                if (units > d.count) units = d.count;
                // Up to the end of the region, or of the gamepak's 128 KiB block.
                const auto room = [&](uint32_t address, unsigned mode, uint32_t mask) -> uint64_t {
                    if (mode == 2) return UINT64_MAX;
                    return mode == 1 ? (address & mask) / size + 1 : (mask + 1 - (address & mask)) / size;
                };
                units = std::min({units, room(d.source, source_mode, rom ? 0x1ffff : 0xffffff),
                                  room(d.destination, destination_mode, 0xffffff)});
                if (units > 1) {
                    uint32_t value = 0;
                    for (uint64_t i = 0; i < units; ++i) {
                        const uint32_t from = d.source & ~(size - 1), to = d.destination & ~(size - 1);
                        value = rom ? rom_value(from, width) : read_region(from, width, kDmaAccess);
                        write_region(to, width, width == 2 ? value : value & 0xffff);
                        if (source_mode == 1) d.source -= size;
                        else if (source_mode != 2) d.source += size;
                        if (destination_mode == 1) d.destination -= size;
                        else if (destination_mode != 2) d.destination += size;
                    }
                    d.latch = width == 2 ? value : (value & 0xffff) * 0x10001u;
                    dma_bus_ = true;
                    dma_latch_ = d.latch;
                    if (rom) {
                        gamepak_next_ = d.source & ~(size - 1);
                        d.gamepak_sequential = gamepak_next_ & 0x1ffff;
                    }
                    clock.now += units * cycles;
                    d.count -= static_cast<uint32_t>(units);
                    continue;
                }
            }
        }
        const uint32_t from = d.source & ~(size - 1), to = d.destination & ~(size - 1);
        if (!d.paused) {
            if (clock.next <= clock.now) clock.advance(clock.now);
            // The read.
            const unsigned from_region = from >> 24;
            uint32_t value;
            unsigned cycles;
            if (from_region >= 8 && from_region < 14) {
                // It waits for the halfword the prefetch is fetching.
                const unsigned wait = prefetch_wait(clock.now);
                stop_prefetch();
                const bool sequential = d.gamepak_sequential;
                const uint32_t at = sequential ? gamepak_next_ : from;
                cycles = wait + (sequential ? cycles_s : cycles_n)[from_region][width];
                value = d.eeprom && backup.eeprom_at(from) ? backup.eeprom_read(clock.now) : rom_value(at, width);
                gamepak_next_ = (at & ~(size - 1)) + size;
                d.gamepak_sequential = (from + size) & 0x1ffff;
            } else {
                cycles = from_region < 16 ? cycles_n[from_region][width] : 1;
                if (from_region < 2) {
                    value = width == 2 ? d.latch : (d.latch >> (8 * (from & 2))) & 0xffff;
                } else if (from_region < 16 && (from_region != 4 || io_dma_readable(from))) {
                    value = read_region(from, width, kDmaAccess);
                } else {
                    // The open bus: the CPU's opcodes, or what this DMA moved last.
                    value = dma_bus_ ? (width == 2 ? d.latch : d.latch & 0xffff) : open_bus(from, width, cpu->r[15]);
                }
            }
            if (from_region >= 2) d.latch = width == 2 ? value : (value & 0xffff) * 0x10001u;
            clock.now += cycles;
            dma_bus_ = true;
            dma_latch_ = width == 2 ? d.latch : (value & 0xffff) * 0x10001u;
            // A channel above that fell due takes over between the read and
            // the write (alyosha DMA/DMA_pause_timing_mid_1 and _mid_2).
            if (clock.next <= clock.now) clock.advance(clock.now);
            if (dma_pending_ & higher) {
                d.paused = true;
                return;
            }
        }
        d.paused = false;
        // The write: a halfword is the latch's half at the destination's
        // alignment.
        const unsigned to_region = to >> 24;
        const uint32_t out = width == 2 ? d.latch : (d.latch >> (8 * (to & 2))) & 0xffff;
        unsigned cycles;
        if (to_region >= 8 && to_region < 14) {
            const unsigned wait = prefetch_wait(clock.now);
            stop_prefetch();
            const bool sequential = d.gamepak_sequential;
            const uint32_t at = sequential ? gamepak_next_ : to;
            cycles = wait + (sequential ? cycles_s : cycles_n)[to_region][width];
            if (d.eeprom && backup.eeprom_at(to)) backup.eeprom_write(static_cast<uint16_t>(out), clock.now);
            gamepak_next_ = (at & ~(size - 1)) + size;
            d.gamepak_sequential = (to + size) & 0x1ffff;
        } else {
            cycles = to_region < 16 ? cycles_n[to_region][width] : 1;
            if (to_region >= 2 && to_region < 8) write_region(to, width, out);
        }
        clock.now += cycles;
        // Addresses: increment, decrement, fixed; 3 is reload for the
        // destination and treated as increment for the source.
        if (source_mode == 1) d.source -= size;
        else if (source_mode != 2) d.source += size;
        if (destination_mode == 1) d.destination -= size;
        else if (destination_mode != 2) d.destination += size;
        --d.count;
    }
    // A channel that falls due before the last internal cycle ends follows
    // at once; one due at the end waits for the CPU's access (alyosha
    // DMA/DMA_pause_timing_end_3 and _end_4).
    clock.now += internal - 1;
    if (clock.next <= clock.now) clock.advance(clock.now);
    // After a write to HALTCNT the last internal cycle waits for the CPU
    // to wake (nba-hw-test haltcnt, HALTCNT CPUSET DMA).
    if (halt_in_access_) halt_tail_ = 1;
    else clock.now += 1;
    dma_pending_ &= static_cast<uint8_t>(~(1u << channel));
    if (control & 0x4000) request_irq(static_cast<uint16_t>(irq::dma0 << channel));
    if ((control & 0x200) && timing != 0) {
        // Repeat: the count, and with reload the destination, start over
        // for the next trigger.
        const uint32_t count = io16(io + 8) & (channel == 3 ? 0xffff : 0x3fff);
        d.count = count ? count : (channel == 3 ? 0x10000 : 0x4000);
        if (destination_mode == 3)
            d.destination = (io16(io + 4) | uint32_t{io16(io + 6)} << 16) & kDestinationMask[channel];
    } else {
        const uint16_t cleared = control & 0x7fff;
        __builtin_memcpy(io_.data() + io + 10, &cleared, 2);
    }
}

}  // namespace gbx::gba
