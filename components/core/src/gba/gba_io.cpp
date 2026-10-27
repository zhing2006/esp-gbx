// The I/O registers (GBATEK "GBA I/O Map"), interrupts, HALT, the keypad
// and the serial port without a partner. The sound registers and the
// Direct Sound FIFOs are in gba_sound.cpp.

#include <algorithm>
#include <cstring>

#include "gbx/gba/memory.h"

namespace gbx::gba {
namespace {

// Cycles from the write that starts a serial transfer until its first bit
// (alyosha Serial).
constexpr Tick kSerialStart = 6;

// What reads of the I/O area see, by halfword: the bits that read back, and
// whether a 32-bit fragment reads as open bus. GBATEK "GBA Unpredictable
// Things": a fragment whose halves are both unused or write-only reads as
// open bus; otherwise an unreadable half reads 0. Bits are from each
// register's section in GBATEK; 4Eh reads 0 by its example.
struct IoTable {
    std::array<uint16_t, 0x200> mask{};
    std::array<bool, 0x200> readable{};
    std::array<bool, 0x100> open{};
};
constexpr IoTable kIo = [] {
    IoTable t;
    auto r = [&](unsigned offset, uint16_t mask) {
        t.mask[offset >> 1] = mask;
        t.readable[offset >> 1] = true;
    };
    r(0x00, 0xffff); r(0x02, 0xffff); r(0x04, 0xffff); r(0x06, 0x00ff);
    r(0x08, 0xdfff); r(0x0a, 0xdfff); r(0x0c, 0xffff); r(0x0e, 0xffff);
    r(0x48, 0x3f3f); r(0x4a, 0x3f3f); r(0x4e, 0);
    r(0x50, 0x3fff); r(0x52, 0x1f1f);
    // Sound: lengths and frequencies are write-only.
    r(0x60, 0x007f); r(0x62, 0xffc0); r(0x64, 0x4000); r(0x66, 0);
    r(0x68, 0xffc0); r(0x6a, 0); r(0x6c, 0x4000); r(0x6e, 0);
    r(0x70, 0x00e0); r(0x72, 0xe000); r(0x74, 0x4000); r(0x76, 0);
    r(0x78, 0xff00); r(0x7a, 0); r(0x7c, 0x40ff); r(0x7e, 0);
    r(0x80, 0xff77); r(0x82, 0x770f); r(0x84, 0x008f); r(0x86, 0);
    r(0x88, 0xc3fe); r(0x8a, 0);
    for (unsigned o = 0x90; o < 0xa0; o += 2) r(o, 0xffff);
    // DMA: only the control reads back.
    for (unsigned c = 0; c < 4; ++c) {
        r(0xb8 + 12 * c, 0);
        r(0xba + 12 * c, c == 3 ? 0xffe0 : 0xf7e0);
    }
    for (unsigned i = 0; i < 4; ++i) { r(0x100 + 4 * i, 0xffff); r(0x102 + 4 * i, 0x00c7); }
    r(0x120, 0xffff); r(0x122, 0xffff); r(0x124, 0xffff); r(0x126, 0xffff);
    r(0x128, 0x7fff); r(0x12a, 0xffff);
    r(0x130, 0x03ff); r(0x132, 0xc3ff); r(0x134, 0xc1ff); r(0x136, 0);
    r(0x140, 0x0047); r(0x142, 0);
    r(0x150, 0xffff); r(0x152, 0xffff); r(0x154, 0xffff); r(0x156, 0xffff); r(0x158, 0x003a); r(0x15a, 0);
    r(0x200, 0x3fff); r(0x202, 0x3fff); r(0x204, 0x5fff); r(0x206, 0);
    r(0x208, 0x0001); r(0x20a, 0);
    r(0x300, 0x0001); r(0x302, 0);
    for (unsigned f = 0; f < 0x100; ++f) t.open[f] = !t.readable[2 * f] && !t.readable[2 * f + 1];
    return t;
}();

bool timer_register(uint32_t offset) { return offset >= 0x100 && offset < 0x110; }

}  // namespace

// --- Reads ------------------------------------------------------------------

uint16_t Memory::io_read16(uint32_t offset) {
    switch (offset) {
    case 0x004: return ppu.dispstat(clock.now);
    case 0x006:
        ppu.mirror_vcount(clock.now);
        return io16(6);
    case 0x100: case 0x104: case 0x108: case 0x10c:
        // A read sees the counter of the cycle before, see gba_timer.cpp.
        return timer_counter((offset - 0x100) >> 2, clock.now - 1);
    case 0x084: return sound_status();
    case 0x130: return buttons_;
    case 0x202: return irq_flags_ & ~(irq_recent_time_ == clock.now ? irq_recent_ : 0);
    default: return io16(offset) & kIo.mask[offset >> 1];
    }
}

// The registers that read back what was written, see io_reads. IF's copy
// already has the bits requested in the current cycle, which a read sees
// only from the next one (io_read16()): only the relaxed timing reads IF
// from its copy, a cycle early for those bits.
void Memory::init_io_reads() {
    io_reads.fill(0);
    for (const unsigned offset : {0x00u, 0x02u, 0x08u, 0x0au, 0x0cu, 0x0eu, 0x48u, 0x4au, 0x50u, 0x52u, 0xbau, 0xc6u,
                                  0xd2u, 0xdeu, 0x102u, 0x106u, 0x10au, 0x10eu, 0x200u, 0x204u, 0x208u})
        io_reads[offset >> 1] = kIo.mask[offset >> 1];
#ifdef GBX_GBA_RELAXED_TIMING
    io_reads[0x202 >> 1] = kIo.mask[0x202 >> 1];
#endif
}

bool Memory::io_dma_readable(uint32_t address) const {
    const uint32_t offset = address & 0xffffff;
    if (offset >= 0x400) return (offset & 0xfffc) == 0x800;
    return !kIo.open[offset >> 2];
}

uint32_t Memory::io_read(uint32_t address, unsigned width, uint32_t r15) {
    const uint32_t offset = address & 0xffffff;
    if (offset >= 0x400 || kIo.open[offset >> 2]) {
        if ((offset & 0xfffc) == 0x800) {
            // 4000800h, repeated every 64 KiB (GBATEK "GBA System Control").
            const uint32_t word = memory_control_;
            return width == 2 ? word : (word >> (8 * (offset & (width ? 2 : 3)))) & (width ? 0xffff : 0xff);
        }
        return open_bus(address, width, r15);
    }
    if (width == 2) return io_read16(offset & ~3u) | uint32_t{io_read16((offset & ~3u) + 2)} << 16;
    const uint16_t half = io_read16(offset & ~1u);
    return width == 1 ? half : (half >> (8 * (offset & 1))) & 0xff;
}

void Memory::io_snapshot(std::span<uint8_t, 0x400> out) {
    clock.now = std::max(clock.now, now());
    for (uint32_t offset = 0; offset < 0x400; offset += 2) {
        const uint16_t value = kIo.readable[offset >> 1] ? io_read16(offset) : 0;
        out[offset] = static_cast<uint8_t>(value);
        out[offset + 1] = static_cast<uint8_t>(value >> 8);
    }
}

// --- Writes -----------------------------------------------------------------

void Memory::io_write(uint32_t address, unsigned width, uint32_t value) {
    const uint32_t offset = address & 0xffffff;
    if (offset >= 0x400) {
        if ((offset & 0xfffc) == 0x800) {
            const unsigned shift = 8 * (offset & (width == 2 ? 0 : width == 1 ? 2 : 3));
            const uint32_t mask = (width == 2 ? 0xffffffffu : width == 1 ? 0xffffu : 0xffu) << shift;
            write_memory_control((memory_control_ & ~mask) | ((value << shift) & mask));
        }
        return;
    }
    if (width == 2) {
        io_write16(offset & ~3u, static_cast<uint16_t>(value), 0xffff);
        io_write16((offset & ~3u) + 2, static_cast<uint16_t>(value >> 16), 0xffff);
    } else if (width == 1) {
        io_write16(offset & ~1u, static_cast<uint16_t>(value), 0xffff);
    } else {
        const unsigned shift = 8 * (offset & 1);
        io_write16(offset & ~1u, static_cast<uint16_t>((value & 0xff) << shift), static_cast<uint16_t>(0xff << shift));
    }
}

void Memory::write_memory_control(uint32_t value) {
    // Bit 4 and bits 6-23 are unused and read 0 (GBATEK).
    memory_control_ = value & 0xff00002f;
    update_waits();
}

void Memory::io_write16(uint32_t offset, uint16_t value, uint16_t mask) {
    const uint16_t old = io16(offset);
    uint16_t merged = static_cast<uint16_t>((old & ~mask) | (value & mask));
    auto store16 = [&](uint16_t v) { __builtin_memcpy(io_.data() + offset, &v, 2); };
    if (offset < 0x58) {
        if (offset == 0x06) return;  // VCOUNT
        if (offset == 0x00) merged = static_cast<uint16_t>((merged & ~8) | (old & 8));  // CGB mode: BIOS only.
        if (offset == 0x04) merged = static_cast<uint16_t>((merged & 0xff38) | (old & 7));
        // A write to a reference point reloads the internal one even with
        // the value it holds (GBATEK "LCD I/O BG Rotation/Scaling").
        const bool reference = (offset >= 0x28 && offset < 0x30) || (offset >= 0x38 && offset < 0x40);
        if (merged == old && !reference) return;
        ppu.write_register(clock.now, offset, merged);
        store16(merged);
        if (offset == 0x00) vram_obj_start = (merged & 7) >= 3 ? 0x14000 : 0x10000;
        if (offset == 0x04) ppu.reschedule();
        return;
    }
    if (offset < 0x60) return;  // 58h-5Fh: not used (GBATEK "GBA I/O Map").
    if (offset < 0xa0) {
        write_sound(offset, merged, mask);
        return;
    }
    if (offset < 0xa8) {
        write_fifo((offset - 0xa0) >> 2, mask == 0xffff ? 2 : 1, mask == 0xff00 ? value >> 8 : value);
        return;
    }
    if (offset < 0xb0) return;
    if (offset < 0xe0) {
        // Whether DMA1 or DMA2 waits to feed a FIFO changes at the write:
        // the samples before it ask the channels as they were.
        if (offset == 0xc6 || offset == 0xd2) catch_up_sound(clock.now);
        store16(merged);
        const unsigned channel = (offset - 0xb0) / 12;
        if ((offset - 0xb0) % 12 == 10) write_dma(channel, merged, old);
        return;
    }
    if (timer_register(offset)) {
        const unsigned index = (offset - 0x100) >> 2;
        if (offset & 2) {
            merged &= 0x00c7;
            store16(merged);
            write_timer(index, io16(offset - 2), merged, true);
        } else {
            // The counter reads in its place; the written value is the reload.
            store16(merged);
            write_timer(index, merged, io16(offset + 2), false);
        }
        return;
    }
    switch (offset) {
    case 0x128:
        store16(static_cast<uint16_t>((merged & 0x7f7f) | (old & 0x80) | (merged & 0x80)));
        write_serial_control(old);
        return;
    case 0x130: return;  // KEYINPUT
    case 0x132:
        store16(merged & 0xc3ff);
        check_keypad();
        return;
    case 0x200:
        store16(merged & 0x3fff);
        update_irq();
        return;
    case 0x202:
        // Writing 1 acknowledges.
        irq_flags_ &= static_cast<uint16_t>(~(value & mask));
        mirror_if();
        check_keypad();
        update_irq();
        return;
    case 0x204:
        store16(merged & 0x5fff);
        update_waits();
        return;
    case 0x208:
        store16(merged & 1);
        update_irq();
        return;
    case 0x300:
        // POSTFLG and HALTCNT take writes only while the CPU runs in the
        // BIOS, by its own stores or a DMA it started; POSTFLG cannot be
        // cleared (nba-hw-test haltcnt).
        if (cpu->r[15] >= kBiosBytes) return;
        if (mask & 0x00ff) store16(static_cast<uint16_t>(old | (merged & 1)));
        if (mask & 0xff00) {
            // HALTCNT: the CPU stops after this write, until IE & IF. STOP
            // (bit 7) is run as HALT: no source tells which events wake it.
            // Written by a DMA, it stops the CPU in the access that waits
            // for the DMA (nba-hw-test haltcnt, HALTCNT CPUSET DMA).
            if (dma_running_) {
                halt_in_access_ = true;
            } else {
                halted = true;
                request_stop();
            }
        }
        return;
    default:
        store16(merged);
        return;
    }
}

// --- Interrupts ----------------------------------------------------------------

void Memory::check_keypad() {
    const uint16_t control = io16(0x132);
    if (!(control & 0x4000)) return;
    const uint16_t pressed = static_cast<uint16_t>(~buttons_ & control & 0x3ff);
    const uint16_t selected = control & 0x3ff;
    const bool met = control & 0x8000 ? (selected && pressed == selected) : pressed != 0;
    if (met && !(irq_flags_ & irq::keypad)) request_irq(irq::keypad);
}

// --- Serial ------------------------------------------------------------------------

// Normal mode with the internal clock and no partner: the transfer takes its
// bits at 256 KiHz or 2 MiHz, then reads all ones (alyosha Serial).
void Memory::write_serial_control(uint16_t old) {
    const uint16_t control = io16(0x128);
    const bool normal = !(io16(0x134) & 0x8000) && !(control & 0x2000);
    if (!(control & 0x80)) {
        clock.cancel(kSlotSerial);
        return;
    }
    // A transfer the BIOS left waiting for an external clock starts when
    // the clock becomes internal.
    if (clock.deadline(kSlotSerial) != kNever || !normal || !(control & 1)) return;
    (void)old;
    const Tick bits = control & 0x1000 ? 32 : 8;
    clock.schedule(kSlotSerial, clock.now + kSerialStart + bits * (control & 2 ? 8 : 64), serial_event, this);
}

// The end of a transfer clears the start bit and requests its interrupt
// (alyosha Serial/serial_time and serial_time_start_bit).
void Memory::serial_event(void* context) {
    auto& m = *static_cast<Memory*>(context);
    const uint16_t control = m.io16(0x128);
    const uint16_t done = control & ~0x80;
    __builtin_memcpy(m.io_.data() + 0x128, &done, 2);
    if (control & 0x1000) std::memset(m.io_.data() + 0x120, 0xff, 4);
    else m.io_[0x12a] = 0xff;
    if (control & 0x4000) m.request_irq(irq::serial);
}

}  // namespace gbx::gba
