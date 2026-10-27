#pragma once

// The interpreter's bus on the GBA's memory map: the access() member that
// Executor calls for every bus cycle, inlined into the copy of the
// interpreter in arm7_mapped.cpp. The paths of IWRAM, EWRAM, the BIOS and
// the ROM through the prefetch buffer are here; the rest goes to Memory's
// slow paths, which the JIT's helpers share.

#include <cstdint>

#include "gbx/gba/bus.h"
#include "gbx/gba/memory.h"

namespace gbx::gba {

struct MappedBus {
    Memory& m;

    [[gnu::always_inline]] uint32_t access(uint32_t request, uint32_t address, uint32_t data = 0) const {
        return access<true>(request, address, data);
    }

    // A code fetch from IWRAM with cycles left, which the interpreter makes
    // in its handlers; false for any other, which goes through access().
    template<bool Thumb>
    [[gnu::always_inline]] bool fetch_inline(uint32_t address, uint32_t& opcode) const {
        if ((address >> 24) != 0x3 || m.left <= 0) return false;
        m.left -= 1;
        opcode = Memory::load(m.iwram.data(), address & 0x7fff, Thumb ? 1 : 2);
        return true;
    }

    // A code fetch other than a refill's that takes 1 cycle and no call:
    // from IWRAM or the BIOS, and with relaxed timing from the ROM while the
    // prefetch buffer is enabled (see access()), with cycles left. False
    // for any other, which goes through access().
    template<bool Thumb>
    [[gnu::always_inline]] bool fetch_fast(uint32_t address, uint32_t& opcode) const {
        if (m.left <= 0) return false;
        const unsigned region = address >> 24;
#ifdef GBX_GBA_RELAXED_TIMING
        if (region - 8 < 6 && m.prefetch_enabled) {
            const uint8_t* page = m.rom_table[(address & (RomCache::kMaxBytes - 1)) >> RomCache::kPageBits];
            if (!page) return false;
            m.left -= 1;
            opcode = Memory::load(page, address & (RomCache::kPageBytes - 1), Thumb ? 1 : 2);
            return true;
        }
#endif
        if (region == 0x3) {
            m.left -= 1;
            opcode = Memory::load(m.iwram.data(), address & 0x7fff, Thumb ? 1 : 2);
            return true;
        }
        if (address >= kBiosBytes) return false;
        m.left -= 1;
        m.latch_bios(address);
        opcode = Memory::load(m.bios, address, Thumb ? 1 : 2);
        return true;
    }

    // Data reads and writes of IWRAM and EWRAM with cycles left, which the
    // interpreter makes in its handlers; false for any other, which goes
    // through access(), out of line.
    [[gnu::always_inline]] bool read_inline(uint32_t request, uint32_t address, uint32_t& value) const {
        const unsigned region = address >> 24, width = (request >> 2) & 3;
        if ((region != 0x2 && region != 0x3) || m.left <= 0) return false;
        if (region == 0x3) {
            m.left -= 1;
            value = Memory::load(m.iwram.data(), address & 0x7fff, width);
        } else {
            m.left -= (request & kSequential ? m.cycles_s : m.cycles_n)[2][width];
            value = Memory::load(m.ewram, address & 0x3ffff, width);
        }
        return true;
    }
    [[gnu::always_inline]] bool write_inline(uint32_t request, uint32_t address, uint32_t value) const {
        const unsigned region = address >> 24, width = (request >> 2) & 3;
        if ((region != 0x2 && region != 0x3) || m.left <= 0) return false;
        if (region == 0x3) {
            const uint32_t offset = address & 0x7fff;
            if (m.iwram_code[offset >> Memory::kCodeLineBits]) return false;
            m.left -= 1;
            Memory::store(m.iwram.data(), offset, width, value);
        } else {
            const uint32_t offset = address & 0x3ffff;
            if (m.ewram_code[offset >> Memory::kCodeLineBits]) return false;
            m.left -= (request & kSequential ? m.cycles_s : m.cycles_n)[2][width];
            Memory::store(m.ewram, offset, width, value);
        }
        return true;
    }

    // A load or store of IWRAM or EWRAM whose instruction's prefetch
    // fetch_fast() would make, with cycles left for the prefetch, the
    // access and a load's internal cycle, so that none of them catches up:
    // all three at once, the opcode fetched in `opcode`. A store does not
    // reach a line with JIT code. False, with nothing done, for any other.
    template<bool Thumb, bool Load>
    [[gnu::always_inline]] bool transfer_inline(uint32_t pc, uint32_t address, unsigned width, uint32_t& opcode,
                                                uint32_t& value) const {
        const unsigned region = address >> 24, code_region = pc >> 24;
        if (region != 0x2 && region != 0x3) return false;
        const uint8_t* code;
        uint32_t code_offset;
        if (code_region == 0x3) {
            code = m.iwram.data();
            code_offset = pc & 0x7fff;
#ifdef GBX_GBA_RELAXED_TIMING
        } else if (code_region - 8 < 6 && m.prefetch_enabled) {
            code = m.rom_table[(pc & (RomCache::kMaxBytes - 1)) >> RomCache::kPageBits];
            if (!code) return false;
            code_offset = pc & (RomCache::kPageBytes - 1);
#endif
        } else {
            return false;
        }
        const bool iwram = region == 0x3;
        uint8_t* const data = iwram ? m.iwram.data() : m.ewram;
        const uint32_t offset = address & (iwram ? 0x7fff : 0x3ffff);
        const int32_t cycles = iwram ? 1 : m.cycles_n[2][width];
        if (m.left <= 1 + (Load ? cycles : 0)) return false;
        if (!Load && (iwram ? m.iwram_code.data() : m.ewram_code)[offset >> Memory::kCodeLineBits]) return false;
        m.left -= 1 + cycles + (Load ? 1 : 0);
        opcode = Memory::load(code, code_offset, Thumb ? 1 : 2);
        if constexpr (Load) value = Memory::load(data, offset, width);
        else Memory::store(data, offset, width, value);
        return true;
    }

    // The words of an LDM or STM from `address` up, without R15, in IWRAM
    // or EWRAM within one mirror, whose instruction's prefetch
    // fetch_fast() would make, with cycles left for the prefetch, the
    // accesses and a load's internal cycle: the prefetch and all the
    // cycles at once, the opcode in `opcode`; returns the memory, which the
    // caller reads or writes at address & mask. A store does not reach a
    // line with JIT code. Null, with nothing done, for any other.
    template<bool Thumb>
    [[gnu::always_inline]] uint8_t* block_inline(uint32_t pc, uint32_t address, unsigned count, bool load,
                                                 uint32_t& opcode, uint32_t& mask) const {
        const unsigned region = address >> 24, code_region = pc >> 24;
        if (region != 0x2 && region != 0x3) return nullptr;
        const bool iwram = region == 0x3;
        mask = iwram ? 0x7fff : 0x3ffff;
        const uint32_t first = address & mask & ~3u, last = first + 4 * (count - 1);
        if (last > mask) return nullptr;
        const uint8_t* code;
        uint32_t code_offset;
        if (code_region == 0x3) {
            code = m.iwram.data();
            code_offset = pc & 0x7fff;
#ifdef GBX_GBA_RELAXED_TIMING
        } else if (code_region - 8 < 6 && m.prefetch_enabled) {
            code = m.rom_table[(pc & (RomCache::kMaxBytes - 1)) >> RomCache::kPageBits];
            if (!code) return nullptr;
            code_offset = pc & (RomCache::kPageBytes - 1);
#endif
        } else {
            return nullptr;
        }
        const int32_t cycles = 1 + (iwram ? static_cast<int32_t>(count)
                                          : m.cycles_n[2][2] + static_cast<int32_t>(count - 1) * m.cycles_s[2][2]) +
                               (load ? 1 : 0);
        if (m.left <= cycles) return nullptr;
        if (!load) {
            const uint8_t* marks = iwram ? m.iwram_code.data() : m.ewram_code;
            for (uint32_t line = first >> Memory::kCodeLineBits; line <= last >> Memory::kCodeLineBits; ++line)
                if (marks[line]) return nullptr;
        }
        m.left -= cycles;
        opcode = Memory::load(code, code_offset, Thumb ? 1 : 2);
        return iwram ? m.iwram.data() : m.ewram;
    }

    // Check: catch up first when no cycles are left. The JIT's helpers do
    // that in generated code and call the rest.
    template<bool Check>
    [[gnu::always_inline]] uint32_t access(uint32_t request, uint32_t address, uint32_t data = 0) const {
        const uint32_t kind = request & 3;
        if (kind == kIdle) {
            if (Check && m.left <= 0) m.idle_slow();
            --m.left;
            return 0;
        }
        const unsigned width = (request >> 2) & 3;
        const bool sequential = request & kSequential;
        if (kind == kFetch) {
            if (Check && m.left <= 0) m.catch_up_fetch();
            switch (address >> 24) {
            case 0x3:
                m.left -= 1;
                return Memory::load(m.iwram.data(), address & 0x7fff, width);
            case 0x8: case 0x9: case 0xa: case 0xb: case 0xc: case 0xd: {
#ifdef GBX_GBA_RELAXED_TIMING
                // Relaxed timing, as the JIT's (Compiler::m_rom_fetch() and
                // m_refill_static()): with the buffer enabled a fetch takes
                // 1 cycle, as if the buffer always kept up; the two fetches
                // of a refill, and every fetch without the buffer, are N or
                // S accesses. The buffer itself never runs.
                m.left -= static_cast<int32_t>(m.prefetch_enabled && !(request & kRefill)
                                                   ? 1
                                                   : m.gamepak_cycles(address, width, sequential));
#else
                m.left -= static_cast<int32_t>(m.rom_fetch(address, width, request));
#endif
                return Memory::load(m.rom_page(address), address & (RomCache::kPageBytes - 1), width);
            }
            case 0x2:
                m.left -= (sequential ? m.cycles_s : m.cycles_n)[2][width];
                return Memory::load(m.ewram, address & 0x3ffff, width);
            case 0x0:
                if (address < kBiosBytes) {
                    m.left -= 1;
                    m.latch_bios(address);
                    return Memory::load(m.bios, address, width);
                }
                break;
            default: break;
            }
            return m.fetch_slow(address, width, sequential);
        }
        if (Check && m.left <= 0) m.catch_up();
        if (kind == kRead) {
            switch (address >> 24) {
            case 0x3:
                m.left -= 1;
                return Memory::load(m.iwram.data(), address & 0x7fff, width);
            case 0x2:
                m.left -= (sequential ? m.cycles_s : m.cycles_n)[2][width];
                return Memory::load(m.ewram, address & 0x3ffff, width);
            case 0x8: case 0x9: case 0xa: case 0xb: case 0xc:
                // The ROM with the prefetch buffer stopped, as after the
                // first of a run of data reads: read_slow() without the
                // buffer's wait.
                if (!m.prefetch_.head) {
                    ++m.spin_epoch;
                    m.left -= static_cast<int32_t>(m.gamepak_cycles(address, width, sequential));
                    return Memory::load(m.rom_page(address), address & (RomCache::kPageBytes - 1), width);
                }
                break;
            default: break;
            }
            return m.read_slow(address, width, sequential, m.cpu->r[15]);
        }
        switch (address >> 24) {
        case 0x5: case 0x6: case 0x7:
            // Palette RAM, VRAM and OAM schedule no event: no rearm().
            m.left -= (sequential ? m.cycles_s : m.cycles_n)[address >> 24][width];
            m.write_video(address, width, data);
            return 0;
        case 0x3: {
            m.left -= 1;
            const uint32_t offset = address & 0x7fff;
            if (m.iwram_code[offset >> Memory::kCodeLineBits]) [[unlikely]]
                m.store_code(m.iwram.data(), offset, width, data, address);
            else Memory::store(m.iwram.data(), offset, width, data);
            return 0;
        }
        case 0x2: {
            m.left -= (sequential ? m.cycles_s : m.cycles_n)[2][width];
            const uint32_t offset = address & 0x3ffff;
            if (m.ewram_code[offset >> Memory::kCodeLineBits]) [[unlikely]]
                m.store_code(m.ewram, offset, width, data, address);
            else Memory::store(m.ewram, offset, width, data);
            return 0;
        }
        case 0x4:
            // The display registers after VCOUNT: a write changes the
            // PPU's registers and the line's timed writes, and schedules
            // no event, requests no interrupt and stops nothing, so the
            // countdown stays as write_slow()'s rearm() would leave it.
            if (address - 0x04000008u < 0x50u) {
                m.clock.now = m.now();
                m.io_write(address, width, data);
                ++m.spin_epoch;
                m.left -= (sequential ? m.cycles_s : m.cycles_n)[4][width];
                return 0;
            }
            break;
        default: break;
        }
        m.write_slow(address, width, sequential, data);
        return 0;
    }

    // An MSR or an exception return may clear CPSR's I while the interrupt
    // line is up: the run then stops at the next boundary to take it.
    // A test instruction restoring CPSR set I: the interrupt the line holds
    // is taken at the boundary all the same.
    void irq_masked_late() const {
        if (!m.irq_line) return;
        m.irq_unmasked = true;
        m.request_stop();
    }

    // Wait loops: a backward branch, and any other write to R15 but a
    // forward branch, see Memory::spin_pass().
    void looped(uint32_t target, uint32_t branch, bool thumb) const { m.spin_pass(target, branch, thumb); }
    void jumped() const { ++m.spin_epoch; }

    // Internal cycles at once when the countdown outlasts them, so that
    // none would have caught up.
    void idle(unsigned count) const {
        if (m.left >= static_cast<int32_t>(count)) {
            m.left -= static_cast<int32_t>(count);
            return;
        }
        for (unsigned i = 0; i < count; ++i) access(kIdle, 0);
    }

    void psr_written(uint32_t cpsr) const {
        if (m.irq_line && !(cpsr & kFlagI)) m.request_stop();
    }
};

}  // namespace gbx::gba
