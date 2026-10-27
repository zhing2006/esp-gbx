// Mapped blocks: the bus cycles of the code games run, on the GBA's memory
// map, with MappedBus's semantics (src/gba/mapped_bus.h).
//
// Time. kRemaining is Memory::left: an access that starts with none left
// catches up first in C++ (m_check()), as MappedBus::access() does, so
// that device events take effect at their exact time, also within an
// instruction. Cycles known at compile time are charged together and taken
// off before the next check; the slow paths take theirs in C++.
//
// Accesses. Reads and writes of IWRAM, EWRAM and VRAM run in the gateway's
// access stubs (m_load and m_store), and so do reads of the ROM while
// Memory holds the prefetch buffer stopped; a write to a 64-byte line that
// holds translated code (Memory::iwram_code and ewram_code) goes to C++,
// which retires the blocks there. Everything else goes to C++ (m_read and
// m_write), which runs MappedBus's own paths. An access whose base
// register pointed into IWRAM or EWRAM at the block's entry tries that
// RAM inline first, see m_ram_access().
//
// Code fetches. Fetches from the BIOS, IWRAM and EWRAM cost their cycles
// at compile time. Those from the ROM follow the prefetch buffer: native
// code, the gateway's fetch stubs or inline code next to a 128 KiB
// boundary, times a fetch of its head itself while the frame owns the
// buffer (kPrefetchFast, see Frame), with the rules of Memory::rom_fetch(): the
// CPU's own S access right after its previous one, otherwise the halfword
// the buffer brings, at once or when it arrives, or one it holds stopped
// full; once it is stopped and empty, an N access that starts it again.
// Static refills within the ROM block's wait states start it the same way
// (m_rom_refill()). A fetch the frame does not own, one next to a 128 KiB
// boundary and every fetch with the buffer disabled go to C++ (m_fetch),
// which hands the buffer back to the frame when it can.
//
// Exits. A block's static exits can link straight to the block of their
// target (the link stub), its dynamic ones look the target up in the
// dispatch table (the dispatch stub); both go on only while cycles are
// left, as the boundaries inside blocks do.

#include <bit>

#include "arm7_compiler.h"

namespace gbx::gba::arm7_compiler {

bool Compiler::in_region(uint32_t address) const {
    const unsigned region = address >> 24, home = mapped_->region;
    if (home == 0) return address < kBiosBytes;
    // A ROM block stays within a pair of regions, whose wait states are
    // the same.
    if (home >= 8) return region >= 8 && region < 14 && (region >> 1) == (home >> 1);
    // A RAM block stays within the mirror of its first instruction, so
    // that its code has one range at the RAM's first mirror (RamBlock).
    const uint32_t mirror = home == 3 ? ~0x7fffu : ~0x3ffffu;
    return ((address ^ (entry_ - 2 * length_)) & mirror) == 0;
}

uint32_t Compiler::m_pc(uint32_t value) const {
    return (value & kAfterPrefetch) && thumb_ ? value | kPcThumb : value;
}

void Compiler::m_store_pc(uint32_t value) {
    li(R::t6, m_pc(value));
    emit(rv::sw(R::t6, R::sp, kFramePc));
}

// Before an access: the charged cycles come off, and with none left the
// stub catches up, in the cold stream. `stub` takes Frame::pc from the word
// after the call: m_catch_fetch_at or m_idle_at.
void Compiler::m_check(uintptr_t stub, uint32_t pc) {
    flush_charge();
    const unsigned slow = branch(rv::bge, R::zero, kRemaining);
    cold_mode_ = true;
    out().align();
    bind(slow);
    call(stub);
    data(m_pc(pc));
    const unsigned back = jump();
    cold_mode_ = false;
    bind(back);
}

void Compiler::m_call(uintptr_t stub, uint32_t pc) {
    m_store_pc(pc);
    call(stub);
}

// A block's entry with relaxed timing, which a linked exit jumps to itself
// (exit_bits::direct): with no cycles left, the boundary before its first
// instruction that the link stub would have taken. m_stop catches up, and
// the run leaves here when it must stop, with the S of the next fetch as
// Arm7State has it. C++ enters a block with cycles left (Arm7Jit::run()).
void Compiler::m_entry() {
    const unsigned stop = branch(rv::bge, R::zero, kRemaining);
    const Label resume = here();
    cold_mode_ = true;
    out().align();
    bind(stop);
    state_pointer(R::a1);
    emit(rv::lbu(R::a1, R::a1, static_cast<int32_t>(offsetof(Arm7State, sequential))));
    emit(rv::slli(R::a1, R::a1, static_cast<unsigned>(std::countr_zero(kSequential))));
    out().align();
    call(stubs_.m_stop);
    bind(jump(), resume);
    data(r15_);
    data(side_bits::sequential_request);
    cold_mode_ = false;
}

// Every instruction's prefetch. A block's first instruction runs whatever
// the cycles left, as the instruction after a boundary of
// arm7_run_mapped() does: its fetch catches up instead, or with relaxed
// timing m_entry() takes the boundary.
void Compiler::m_prefetch() {
    const uint32_t address = r15_;
#ifdef GBX_GBA_RELAXED_TIMING
    if (!index_) m_entry();
#else
    if (!index_) m_check(stubs_.m_catch_fetch_at, address);
#endif
    const unsigned region = address >> 24;
    if (region == 0 || region == 3) {
        charge(1);
        return;
    }
    if (region == 2) {
        charge(mapped_->ewram_cycles[thumb_ ? 1 : 2]);
        return;
    }
    m_rom_fetch(address);
}

void Compiler::m_rom_fetch(uint32_t address) {
    const unsigned halves = thumb_ ? 1 : 2, width = thumb_ ? 1 : 2;
#ifdef GBX_GBA_RELAXED_TIMING
    // Relaxed timing: with the buffer enabled, every fetch after a block's
    // refill takes it from the buffer in 1 cycle, as if the buffer always
    // kept up; without it, an S or N access.
    (void)address;
    (void)halves;
    charge(mapped_->prefetch ? 1 : sequential_ == 0 ? mapped_->code_n[width] : mapped_->code_s[width]);
    return;
#endif
    flush_charge();
    const auto helper = [&] {
        // a1 holds the request already after a conditional instruction
        // whose paths end with different SEQ.
        if (sequential_ != kRequested) {
            uint32_t request = kFetch | (thumb_ ? kHalf : kWord);
            if (sequential_ < 0) request |= kStateSequential;
            else if (sequential_) request |= kSequential;
            li(R::a1, request);
        }
        li(R::a2, address);
        m_call(stubs_.m_fetch, address);
    };
    // Fetches at or before a 128 KiB boundary have rules of their own.
    const uint32_t next = address + 2 * halves;
    if (!mapped_->prefetch || !(address & 0x1ffff) || !(next & 0x1ffff)) {
        helper();
        return;
    }
    const uint32_t room = (0x20000 - (address & 0x1ffff)) >> 1;
    // Where the buffer can hold 8 halfwords, the gateway's stub of the
    // state and wait states times the fetch; the request then needs no
    // register.
    if (room >= 8 && sequential_ != kRequested) {
        uint32_t request = 0;
        if (sequential_ < 0) request = kStateSequential;
        else if (sequential_) request = kSequential;
        out().align();
        call(stubs_.m_rom_fetch[(thumb_ ? 0 : 3) + (((address >> 24) - 8) >> 1)]);
        data(address | request << kRomFetchRequestShift);
        return;
    }
    const unsigned cycles = mapped_->halfword_s;
    const unsigned held = room < 8 ? room : 8;
    const auto step = static_cast<int32_t>(halves * cycles);
    // a0 = arrival - now; the buffer was full when it is at most this.
    const int32_t full = static_cast<int32_t>(halves * cycles) - static_cast<int32_t>(held * cycles);
    emit(rv::lw(R::a3, R::sp, kFramePrefetchFlags));
    // Owned, and neither stopped nor full: 1, 3, 5 or 7.
    emit(rv::addi(R::a0, R::a3, -1));
    emit(rv::sltiu(R::a0, R::a0, 7));
    const unsigned other = branch(rv::beq, R::a0, R::zero);
    emit(rv::andi(R::a0, R::a3, kPrefetchRunning));
    emit(rv::lw(R::a2, R::sp, kFramePrefetchBase));
    const unsigned running = branch(rv::bne, R::a0, R::zero);
    // Right after the CPU's own access the buffer has not begun: this fetch
    // is the CPU's own S access, and the buffer starts after it.
    emit(rv::add(R::a0, R::a2, kRemaining));
    const unsigned later = branch(rv::bne, R::a0, R::zero);
    emit(rv::addi(kRemaining, kRemaining, -static_cast<int32_t>(mapped_->code_s[width])));
    emit(rv::sub(R::a2, R::zero, kRemaining));
    emit(rv::sw(R::a2, R::sp, kFramePrefetchBase));
    const unsigned own_done = jump();
    bind(later);
    emit(rv::ori(R::a3, R::a3, kPrefetchRunning));
    bind(running);
    emit(rv::addi(R::a2, R::a2, step));
    emit(rv::add(R::a0, R::a2, kRemaining));
    emit(rv::slti(R::t6, R::a0, full + 1));
    const unsigned filled = branch(rv::bne, R::t6, R::zero);
    emit(rv::sw(R::a2, R::sp, kFramePrefetchBase));
    if (!thumb_) {
        // An ARM fetch that waits for the buffer marks it stalled.
        emit(rv::slt(R::t6, R::zero, R::a0));
        emit(rv::slli(R::t6, R::t6, 2));
        emit(rv::andi(R::a3, R::a3, ~static_cast<int32_t>(kPrefetchStalled)));
        emit(rv::or_(R::a3, R::a3, R::t6));
    }
    emit(rv::sw(R::a3, R::sp, kFramePrefetchFlags));
    const unsigned at_once = branch(rv::bge, R::zero, R::a0);
    emit(rv::sub(kRemaining, R::zero, R::a2));
    const unsigned waited = jump();
    bind(at_once);
    emit(rv::addi(kRemaining, kRemaining, -1));
    const unsigned fast_done = jump();
    cold_mode_ = true;
    // Full: the buffer stops, and this fetch takes from what it holds.
    bind(filled);
    const unsigned rest = held - halves;
    li(R::a3, rest ? kPrefetchFast | rest << kPrefetchPausedShift : kPrefetchFast | kPrefetchEmpty | kPrefetchDrained);
    emit(rv::sw(R::a3, R::sp, kFramePrefetchFlags));
    emit(rv::addi(kRemaining, kRemaining, -1));
    const unsigned filled_done = jump();
    bind(other);
    emit(rv::andi(R::a0, R::a3, kPrefetchFast));
    const unsigned not_fast = branch(rv::beq, R::a0, R::zero);
    emit(rv::andi(R::a0, R::a3, kPrefetchEmpty));
    const unsigned empty = branch(rv::bne, R::a0, R::zero);
    // Stopped full: the fetch takes from the halfwords held, and the buffer
    // stays stopped once it has none.
    emit(rv::srli(R::a0, R::a3, kPrefetchPausedShift));
    unsigned last = 0;
    if (!thumb_) {
        li(R::t6, 1);
        last = branch(rv::beq, R::a0, R::t6);
    }
    emit(rv::addi(R::a0, R::a0, -static_cast<int32_t>(halves)));
    emit(rv::addi(kRemaining, kRemaining, -1));
    const unsigned drained = branch(rv::beq, R::a0, R::zero);
    emit(rv::slli(R::a0, R::a0, kPrefetchPausedShift));
    emit(rv::ori(R::a3, R::a0, kPrefetchFast));
    emit(rv::sw(R::a3, R::sp, kFramePrefetchFlags));
    const unsigned paused_done = jump();
    if (!thumb_) {
        // An ARM fetch takes the last halfword held and waits for the
        // restarted buffer to bring the next.
        bind(last);
        emit(rv::addi(kRemaining, kRemaining, -static_cast<int32_t>(mapped_->code_n[1])));
    }
    bind(drained);
    li(R::a3, kPrefetchFast | kPrefetchEmpty | kPrefetchDrained);
    emit(rv::sw(R::a3, R::sp, kFramePrefetchFlags));
    const unsigned drained_done = jump();
    // Stopped and empty: an N fetch, after which the buffer starts.
    bind(empty);
    emit(rv::addi(kRemaining, kRemaining, -static_cast<int32_t>(mapped_->code_n[width])));
    emit(rv::sub(R::a2, R::zero, kRemaining));
    emit(rv::sw(R::a2, R::sp, kFramePrefetchBase));
    li(R::a3, kPrefetchFast);
    emit(rv::sw(R::a3, R::sp, kFramePrefetchFlags));
    const unsigned empty_done = jump();
    bind(not_fast);
    helper();
    const unsigned back = jump();
    cold_mode_ = false;
    for (const unsigned done : {own_done, waited, fast_done, filled_done, paused_done, drained_done, empty_done, back})
        bind(done);
}

// A data access or an internal cycle; a2 holds the address, a3 the data of
// a write, and a read returns in a0. a2 and a3 are kept. The gateway's
// stub of the width catches up and accesses, see m_load and m_store.
void Compiler::m_access(uint32_t request) {
    const uint32_t pc = r15_ | kAfterPrefetch;
    if ((request & 3) == kIdle) {
        m_check(stubs_.m_idle_at, pc);
        charge(1);
        return;
    }
    flush_charge();
    if ((request & 3) == kWrite) wrote_ = true;
    const unsigned region = hint_;
    hint_ = 0;
    if ((region == 2 || region == 3) && mapped_->iwram) {
        m_ram_access(request, region);
        return;
    }
    if (region == 6 && !thumb_ && (request & 3) == kWrite && ((request >> 2) & 3) && mapped_->vram) {
        m_vram_store(request);
        return;
    }
    out().align();
    const unsigned width = (request >> 2) & 3;
    call(((request & 3) == kWrite ? stubs_.m_store : stubs_.m_load)[width]);
    data(m_pc(pc) | (request & (kSequential | kLock)) << 24);
}

// regions_ after the instruction `op`: a move keeps the source's region,
// an addition or subtraction of a value below 16 MiB the pointer's, and
// whatever else writes a register makes it unknown. Only a guess for
// hint(); a wrong one costs the inline check.
void Compiler::track(uint32_t op) {
    const auto of = [&](unsigned n) -> uint8_t { return n == 15 ? static_cast<uint8_t>(r15_ >> 24) : regions_[n]; };
    // A pointer plus or minus a small value; two pointers or two small
    // values give no pointer.
    const auto combine = [](uint8_t a, uint8_t b) -> uint8_t { return b == 0 ? a : a == 0 ? b : 0; };
    const auto set = [&](unsigned n, uint8_t region) {
        if (n < regions_.size()) regions_[n] = region;
    };
    // A literal's region: the word there now.
    const auto literal = [&](uint32_t address) {
        return static_cast<uint8_t>(code_.read(code_.context, address, false) >> 24);
    };
    if (thumb_) {
        const unsigned rd = op & 7, rs = (op >> 3) & 7;
        switch (op >> 11) {
        case 0x00: set(rd, (op >> 6) & 31 ? 0 : of(rs)); break;  // LSL, MOV with no shift
        case 0x01: case 0x02: set(rd, 0); break;
        case 0x03:  // ADD and SUB, register or 3-bit immediate
            set(rd, op & (1u << 10) ? of(rs) : combine(of(rs), of((op >> 6) & 7)));
            break;
        case 0x04: set((op >> 8) & 7, 0); break;  // MOV with an 8-bit immediate
        case 0x05: break;                          // CMP
        case 0x06: case 0x07: break;               // ADD and SUB with an 8-bit immediate
        case 0x08:
            if (op & (1u << 10)) {  // high registers
                const unsigned d = rd | ((op >> 4) & 8), s = (op >> 3) & 15, kind = (op >> 8) & 3;
                if (kind == 0) set(d, combine(of(d), of(s)));
                else if (kind == 2) set(d, of(s));
            } else {
                // ADC, SBC, ORR and BIC keep a pointer; the rest of the
                // ALU operations make small values.
                const unsigned alu = (op >> 6) & 15;
                if (alu != 5 && alu != 6 && alu != 12 && alu != 14 && alu != 8 && alu != 10 && alu != 11) set(rd, 0);
            }
            break;
        case 0x09: set((op >> 8) & 7, literal((r15_ & ~2u) + (op & 0xff) * 4)); break;  // LDR PC-relative
        case 0x0a: case 0x0b:                      // register offset; LDRSB and up load
            if (((op >> 9) & 7) >= 3) set(rd, 0);
            break;
        case 0x0d: case 0x0f: case 0x11: set(rd, 0); break;  // LDR, LDRB, LDRH immediate
        case 0x13: set((op >> 8) & 7, 0); break;              // LDR SP-relative
        case 0x14: set((op >> 8) & 7, of(15)); break;          // ADD Rd, PC
        case 0x15: set((op >> 8) & 7, of(13)); break;          // ADD Rd, SP
        case 0x17: case 0x19:                                  // POP, LDMIA
            for (unsigned i = 0; i < 8; ++i) {
                if (op & (1u << i)) set(i, 0);
            }
            break;
        default: break;
        }
        return;
    }
    const unsigned rd = (op >> 12) & 15, rn = (op >> 16) & 15;
    if ((op & 0x0c000000) == 0) {
        if ((op & 0x0fc000f0) == 0x00000090) {  // MUL and MLA, Rd in bits 16-19
            set(rn, 0);
            return;
        }
        if ((op & 0x0f8000f0) == 0x00800090) {  // long multiplies
            set(rn, 0);
            set(rd, 0);
            return;
        }
        if ((op & 0x0fb00ff0) == 0x01000090) {  // SWP
            set(rd, 0);
            return;
        }
        if ((op & 0x90) == 0x90 && !(op & (1u << 25))) {  // halfword and signed transfers
            if (op & (1u << 20)) set(rd, 0);
            return;
        }
        const unsigned opcode = (op >> 21) & 15;
        if ((opcode & 0xc) == 8) {
            if ((op & 0x0fbf0fff) == 0x010f0000) set(rd, 0);  // MRS
            return;
        }
        // The second operand: an immediate's region, an unshifted register's,
        // or a shifted register's, which is small.
        uint8_t b = 0;
        if (op & (1u << 25)) b = static_cast<uint8_t>(std::rotr(op & 0xff, static_cast<int>((op >> 7) & 30)) >> 24);
        else if (!(op & 0xff0)) b = of(op & 15);
        uint8_t region = 0;
        switch (opcode) {
        case 13: region = b; break;                                      // MOV
        case 2: case 4: case 5: case 6: region = combine(of(rn), b); break;  // SUB, ADD, ADC, SBC
        case 12: case 14: case 1: region = b == 0 ? of(rn) : 0; break;      // ORR, BIC, EOR
        default: region = 0; break;
        }
        set(rd, region);
        return;
    }
    if ((op & 0x0c000000) == 0x04000000) {  // LDR and STR
        if ((op & 0x037f0003) == 0x011f0000) set(rd, literal(op & (1u << 23) ? r15_ + (op & 0xfff) : r15_ - (op & 0xfff)));
        else if (op & (1u << 20)) set(rd, 0);
        return;
    }
    if ((op & 0x0e000000) == 0x08000000 && (op & (1u << 20))) {  // LDM
        for (unsigned i = 0; i < 15; ++i) {
            if (op & (1u << i)) set(i, 0);
        }
    }
}

// An access predicted in IWRAM or EWRAM: the stub's path for that RAM
// inline, below its first mirror, with cycles left and, for a write, no
// code in the line. Any other goes to the stub from the cold stream.
void Compiler::m_ram_access(uint32_t request, unsigned region) {
    const unsigned width = (request >> 2) & 3, bits = region == 3 ? 15 : 18;
    const bool write = (request & 3) == kWrite;
    const auto split = [](const uint8_t* pointer) {
        const auto value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer));
        return std::pair{(value + 0x800u) >> 12, static_cast<int32_t>(value << 20) >> 20};
    };
    std::array<unsigned, 3> slow{};
    unsigned slow_count = 0;
    slow[slow_count++] = branch(rv::bge, R::zero, kRemaining);
    // a0 = the address within the RAM; zero above it.
    li(R::a1, region << 24);
    emit(rv::sub(R::a0, R::a2, R::a1));
    emit(rv::srli(R::a1, R::a0, static_cast<int32_t>(bits)));
    slow[slow_count++] = branch(rv::bne, R::a1, R::zero);
    if (write) {
        const auto [high, low] = split(region == 3 ? mapped_->iwram_code : mapped_->ewram_code);
        emit(rv::srli(R::a1, R::a0, static_cast<int32_t>(Memory::kCodeLineBits)));
        emit(rv::lui(R::t6, high));
        emit(rv::add(R::a1, R::a1, R::t6));
        emit(rv::lbu(R::a1, R::a1, low));
        slow[slow_count++] = branch(rv::bne, R::a1, R::zero);
    }
    if (width) emit(rv::andi(R::a0, R::a0, -(1 << width)));
    const auto [high, low] = split(region == 3 ? mapped_->iwram : mapped_->ewram);
    emit(rv::lui(R::a1, high));
    emit(rv::add(R::a0, R::a0, R::a1));
    if (write) emit(width == 2 ? rv::sw(R::a3, R::a0, low) : width == 1 ? rv::sh(R::a3, R::a0, low) : rv::sb(R::a3, R::a0, low));
    else emit(width == 2 ? rv::lw(R::a0, R::a0, low) : width == 1 ? rv::lhu(R::a0, R::a0, low) : rv::lbu(R::a0, R::a0, low));
    const auto cycles = static_cast<int32_t>(region == 3 ? 1 : mapped_->ewram_cycles[width]);
    emit(rv::addi(kRemaining, kRemaining, -cycles));
    const Label done = here();
    cold_mode_ = true;
    for (unsigned i = 0; i < slow_count; ++i) bind(slow[i]);
    out().align();
    call((write ? stubs_.m_store : stubs_.m_load)[width]);
    data(m_pc(r15_ | kAfterPrefetch) | (request & (kSequential | kLock)) << 24);
    bind(jump(), done);
    cold_mode_ = false;
}

// An ARM halfword or word store predicted in VRAM: the stub's path for it
// inline, below the first mirror's 96 KiB and with cycles left, the page
// marked for the renderer as Memory::write_video() does. Any other goes to
// the stub from the cold stream.
void Compiler::m_vram_store(uint32_t request) {
    const unsigned width = (request >> 2) & 3;
    const auto split = [](const void* pointer) {
        const auto value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer));
        return std::pair{(value + 0x800u) >> 12, static_cast<int32_t>(value << 20) >> 20};
    };
    std::array<unsigned, 2> slow{};
    slow[0] = branch(rv::bge, R::zero, kRemaining);
    // a0 = the offset in VRAM; at least 18000h outside it.
    li(R::a1, 6u << 24);
    emit(rv::sub(R::a0, R::a2, R::a1));
    li(R::a1, 0x18000);
    slow[1] = branch(rv::bgeu, R::a0, R::a1);
    emit(rv::andi(R::a0, R::a0, -(1 << width)));
    emit(rv::srli(R::t6, R::a0, static_cast<int32_t>(Ppu::kPageBits)));
    const auto [high, low] = split(mapped_->vram);
    emit(rv::lui(R::a1, high));
    emit(rv::add(R::a0, R::a0, R::a1));
    emit(width == 2 ? rv::sw(R::a3, R::a0, low) : rv::sh(R::a3, R::a0, low));
    // The page's bit, in bytes of Memory::dirty.
    emit(rv::srli(R::a1, R::t6, 3));
    emit(rv::andi(R::t6, R::t6, 7));
    emit(rv::addi(R::a0, R::zero, 1));
    emit(rv::sll(R::t6, R::a0, R::t6));
    const auto [dirty_high, dirty_low] = split(mapped_->dirty);
    emit(rv::lui(R::a0, dirty_high));
    emit(rv::add(R::a1, R::a1, R::a0));
    emit(rv::lbu(R::a0, R::a1, dirty_low));
    emit(rv::or_(R::a0, R::a0, R::t6));
    emit(rv::sb(R::a0, R::a1, dirty_low));
    emit(rv::addi(kRemaining, kRemaining, width == 2 ? -2 : -1));
    const Label done = here();
    cold_mode_ = true;
    for (const unsigned at : slow) bind(at);
    out().align();
    call(stubs_.m_store[width]);
    data(m_pc(r15_ | kAfterPrefetch) | (request & (kSequential | kLock)) << 24);
    bind(jump(), done);
    cold_mode_ = false;
}

// LDM, STM, PUSH and POP from a base that pointed into IWRAM or EWRAM at
// the block's entry (regions_), and ARM's STM into VRAM, a2 holding the
// first address: when every word is in that memory below its first
// mirror's end (VRAM's 96 KiB), the last access starts with cycles left,
// so that none catches up, and a store reaches no line with code, the
// accesses run here at once, with the cycles the stub would take for each,
// VRAM's pages marked as Memory::write_video() does. The base is written back after the first access, as
// block_transfer() does, and a loaded R15 goes to a3. Otherwise the code
// that follows in the cold stream, the caller's, takes them through the
// stubs. Returns where the paths meet; the caller computed the flags that
// read a register the transfer writes. `list` is the words transferred:
// R15 alone for an empty list, whose base still moves by 40h.
Compiler::Label Compiler::m_block_inline(unsigned rn, uint32_t list, bool load, bool writeback, R moved) {
    const unsigned region = regions_[rn], bits = region == 3 ? 15 : 18;
    const bool vram = region == 6;
    const auto cycles = static_cast<int32_t>(region == 3 ? 1 : vram ? 2 : mapped_->ewram_cycles[2]);
    const auto count = static_cast<unsigned>(std::popcount(list));
    const auto last = static_cast<int32_t>(4 * (count - 1));
    const auto split = [](const void* pointer) {
        const auto value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer));
        return std::pair{(value + 0x800u) >> 12, static_cast<int32_t>(value << 20) >> 20};
    };
    flush_charge();
    std::array<unsigned, 4> slow{};
    unsigned slow_count = 0;
    li(R::a1, region << 24);
    emit(rv::sub(R::a0, R::a2, R::a1));
    if (vram) {
        // a0 = the first word's offset in VRAM; it and the last word's
        // below 18000h.
        li(R::t6, 0x18000);
        slow[slow_count++] = branch(rv::bgeu, R::a0, R::t6);
        emit(rv::addi(R::a1, R::a0, last));
        slow[slow_count++] = branch(rv::bgeu, R::a1, R::t6);
    } else {
        // a0 = the first address within the RAM; a1 nonzero when it or the
        // last is outside.
        emit(rv::addi(R::a1, R::a0, last));
        emit(rv::or_(R::a1, R::a1, R::a0));
        emit(rv::srli(R::a1, R::a1, static_cast<int32_t>(bits)));
        slow[slow_count++] = branch(rv::bne, R::a1, R::zero);
    }
    li(R::t6, static_cast<uint32_t>((count - 1) * cycles));
    slow[slow_count++] = branch(rv::bge, R::t6, kRemaining);
    if (vram) {
        wrote_ = true;
    } else if (!load) {
        // The words span two lines at most.
        const auto [high, low] = split(region == 3 ? mapped_->iwram_code : mapped_->ewram_code);
        emit(rv::lui(R::t6, high));
        emit(rv::addi(R::a3, R::a0, last));
        emit(rv::srli(R::a3, R::a3, static_cast<int32_t>(Memory::kCodeLineBits)));
        emit(rv::add(R::a3, R::a3, R::t6));
        emit(rv::lbu(R::a3, R::a3, low));
        emit(rv::srli(R::a1, R::a0, static_cast<int32_t>(Memory::kCodeLineBits)));
        emit(rv::add(R::a1, R::a1, R::t6));
        emit(rv::lbu(R::a1, R::a1, low));
        emit(rv::or_(R::a1, R::a1, R::a3));
        slow[slow_count++] = branch(rv::bne, R::a1, R::zero);
        wrote_ = true;
    }
    // a1 + offset = the first word.
    emit(rv::andi(R::a0, R::a0, -4));
    const auto [high, low] = split(region == 3 ? mapped_->iwram : vram ? mapped_->vram : mapped_->ewram);
    emit(rv::lui(R::a1, high));
    emit(rv::add(R::a1, R::a1, R::a0));
    int32_t offset = low;
    if (offset + last > 2047) {
        emit(rv::addi(R::a1, R::a1, offset));
        offset = 0;
    }
    unsigned k = 0;
    for (uint32_t rest = list; rest; rest &= rest - 1, ++k) {
        const unsigned i = static_cast<unsigned>(std::countr_zero(rest));
        const int32_t at = offset + static_cast<int32_t>(4 * k);
        const bool first_back = k == 0 && writeback;
        if (load) {
            emit(rv::lw(i == 15 ? R::a3 : first_back ? R::a0 : host(i), R::a1, at));
            if (first_back) set_guest(rn, in(moved));
            if (first_back && i != 15) set_guest(i, in(R::a0));
        } else {
            R value = i == 15 ? R::a3 : host(i);
            if (i == 15) li(R::a3, r15_ + length_);
            emit(rv::sw(value, R::a1, at));
            if (first_back) set_guest(rn, in(moved));
        }
    }
    if (vram) {
        // The pages of the first and the last word, marked for the
        // renderer as Memory::write_video() does, in bytes of Memory::dirty.
        const auto [dirty_high, dirty_low] = split(mapped_->dirty);
        for (const int32_t at : {int32_t{0}, last}) {
            if (at) emit(rv::addi(R::t6, R::a0, at));
            emit(rv::srli(R::t6, at ? R::t6 : R::a0, static_cast<int32_t>(Ppu::kPageBits)));
            emit(rv::srli(R::a1, R::t6, 3));
            emit(rv::andi(R::t6, R::t6, 7));
            emit(rv::addi(R::a3, R::zero, 1));
            emit(rv::sll(R::t6, R::a3, R::t6));
            emit(rv::lui(R::a3, dirty_high));
            emit(rv::add(R::a1, R::a1, R::a3));
            emit(rv::lbu(R::a3, R::a1, dirty_low));
            emit(rv::or_(R::a3, R::a3, R::t6));
            emit(rv::sb(R::a3, R::a1, dirty_low));
        }
    }
    emit(rv::addi(kRemaining, kRemaining, -static_cast<int32_t>(count) * cycles));
    const Label merge = here();
    cold_mode_ = true;
    for (unsigned i = 0; i < slow_count; ++i) bind(slow[i]);
    return merge;
}

// A word load from the ROM the block runs from, with relaxed timing: as
// the ROM does not change, Rd gets the word at compile time, and the load
// costs what the stub's ROM path takes, which with the buffer stopped is
// the N access after a catch-up when no cycles are left, and moves
// gamepak_next_ past the word. Not R15, nor the GPIO's registers at C4h
// in the first page or region 13, which may hold the EEPROM.
bool Compiler::m_literal(uint32_t address, unsigned rd) {
#ifdef GBX_GBA_RELAXED_TIMING
    if (!mapped_ || mapped_->region < 8 || rd == 15 || (address >> 24) > 12 || !in_region(address) ||
        (address & 0x1fffff0) == 0xc0)
        return false;
    const uint32_t value = code_.read(code_.context, address, false);
    m_check(stubs_.m_catch_at, r15_ | kAfterPrefetch);
    charge(mapped_->code_n[2]);
    const auto next = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mapped_->gamepak_next));
    li(R::a0, address + 4);
    emit(rv::lui(R::a1, (next + 0x800u) >> 12));
    emit(rv::sw(R::a0, R::a1, static_cast<int32_t>(next << 20) >> 20));
    idle();
    set_guest(rd, fixed(value));
    sequential_ = 0;
    return true;
#else
    (void)address;
    (void)rd;
    return false;
#endif
}

// a3 internal cycles, at least one: each catches up when none are left,
// which no cycle does when a3 are left.
void Compiler::m_idle_loop() {
    flush_charge();
    const unsigned slow = branch(rv::blt, kRemaining, R::a3);
    emit(rv::sub(kRemaining, kRemaining, R::a3));
    const unsigned done = jump();
    cold_mode_ = true;
    bind(slow);
    const Label loop = here();
    const unsigned ready = branch(rv::blt, R::zero, kRemaining);
    m_call(stubs_.m_idle, r15_ | kAfterPrefetch);
    bind(ready);
    emit(rv::addi(kRemaining, kRemaining, -1));
    emit(rv::addi(R::a3, R::a3, -1));
    bind(branch(rv::bne, R::a3, R::zero), loop);
    const unsigned back = jump();
    cold_mode_ = false;
    bind(done);
    bind(back);
}

// A static refill native code times itself: into IWRAM or EWRAM from a
// block there, or into the BIOS from anywhere but the ROM. A ROM block's
// refills go through the prefetch buffer, see m_rom_refill(); refills out
// of the BIOS set its latch in C++.
bool Compiler::m_inline_refill(uint32_t target) const {
    const unsigned to = target >> 24, from = mapped_->region;
#ifdef GBX_GBA_RELAXED_TIMING
    // Relaxed timing: ROM blocks time their refills into IWRAM, EWRAM and
    // their own ROM regions natively too, see m_refill_static().
    if (from >= 8) return to == 2 || to == 3 || in_region(target);
#endif
    if (from >= 8) return false;
    if (to == 0) return target + 8 <= kBiosBytes;
    return (to == 2 || to == 3) && from != 0;
}

// A static refill from a ROM block to the ROM of its wait states, timed
// by the gateway's m_rom_refill with the rules of Memory::rom_fetch() while
// the frame owns the buffer: the target's N fetch after the halfword the
// buffer is fetching, then the CPU's own S fetch, after which the buffer
// starts. Refills to the halfword the buffer holds next, those next to a
// 128 KiB boundary and an ARM one to the word after the next go to C++,
// which also takes the second fetch when it has to catch up first.
bool Compiler::m_rom_refill(uint32_t target, bool thumb) {
    const uint32_t length = thumb ? 2 : 4, head = r15_ + length;
    if (mapped_->region < 8 || !mapped_->prefetch || thumb != thumb_ || !in_region(target) || target == head ||
        (!thumb && target == head + 4) || !((target + length) & 0x1ffff) || !((target + 2 * length) & 0x1ffff) ||
        (head & 0x1ffff) > 0x1fff0)
        return false;
    flush_charge();
    out().align();
    call(stubs_.m_rom_refill[(thumb ? 0 : 3) + ((mapped_->region - 8u) >> 1)]);
    data(target);
    data(m_pc(r15_ | kAfterPrefetch));
    return true;
}

void Compiler::m_refill_static(uint32_t target, bool thumb, bool synced) {
    const uint32_t length = thumb ? 2 : 4;
    target &= ~(length - 1);
    exchanged_ = thumb != thumb_;
    if (m_inline_refill(target)) {
        const unsigned width = thumb ? 1 : 2, to = target >> 24;
        const unsigned cycles = to == 2 ? mapped_->ewram_cycles[width] : 1;
        for (unsigned i = 0; i < 2; ++i) {
            m_check(stubs_.m_catch_fetch_at, r15_ | kAfterPrefetch);
            // A ROM target, with relaxed timing: an N then an S access.
            charge(to < 8 ? cycles : i ? mapped_->code_s[width] : mapped_->code_n[width]);
        }
    } else if (!m_rom_refill(target, thumb)) {
        flush_charge();
        li(R::a1, length);
        li(R::a2, target);
        m_call(stubs_.m_refill, r15_ | kAfterPrefetch);
    }
    leave(target + 2 * length, false, true, Arm7JitExit::control_flow, synced);
}

// The target's region is known only now: from a ROM block in the same
// state, the gateway's stub times a refill within the block's region pair
// as m_rom_refill() does; from an IWRAM or EWRAM block, its stub times one
// into either as m_refill_static() does. They leave the rest to C++, which
// times every other.
void Compiler::m_refill_dynamic(R target, bool thumb, bool synced) {
    const uint32_t length = thumb ? 2 : 4, head = r15_ + length;
    flush_charge();
    emit(rv::andi(R::a2, target, -static_cast<int32_t>(length)));
#ifdef GBX_GBA_RELAXED_TIMING
    // Relaxed timing: a ROM block times a refill into its own ROM regions
    // natively, an N then an S access, and leaves the rest to C++.
    if (mapped_->region >= 8) {
        const unsigned width = thumb ? 1 : 2;
        emit(rv::srli(R::a0, R::a2, 25));
        li(R::a1, mapped_->region >> 1);
        const unsigned other = branch(rv::bne, R::a0, R::a1);
        emit(rv::addi(kRemaining, kRemaining, -static_cast<int32_t>(mapped_->code_n[width] + mapped_->code_s[width])));
        const Label timed = here();
        cold_mode_ = true;
        bind(other);
        li(R::a1, length);
        m_call(stubs_.m_refill, r15_ | kAfterPrefetch);
        bind(jump(), timed);
        cold_mode_ = false;
        emit(rv::addi(R::a0, R::a2, static_cast<int32_t>(2 * length)));
        leave(0, true, true, Arm7JitExit::control_flow, synced);
        return;
    }
#endif
    if (mapped_->region >= 8 && mapped_->prefetch && thumb == thumb_ && (head & 0x1ffff) <= 0x1fff0) {
        out().align();
        call(stubs_.m_rom_refill_dynamic[(thumb ? 0 : 3) + ((mapped_->region - 8u) >> 1)]);
        data(head);
        data(m_pc(r15_ | kAfterPrefetch));
    } else if (mapped_->region == 2 || mapped_->region == 3) {
        out().align();
        call(stubs_.m_ram_refill[thumb ? 0 : 1]);
        data(m_pc(r15_ | kAfterPrefetch));
    } else {
        li(R::a1, length);
        m_call(stubs_.m_refill, r15_ | kAfterPrefetch);
    }
    emit(rv::addi(R::a0, R::a2, static_cast<int32_t>(2 * length)));
    leave(0, true, true, Arm7JitExit::control_flow, synced);
}

// Exits: as leave() describes, through the link stub for a static R15
// with a link word after the exit's words, through the dispatch stub for a
// dynamic one. Exits after a helper that changed the mode or CPSR's other
// bits reload the registers from Arm7State first. A RAM or BIOS block's
// static exit to its own entry jumps straight back to its start while
// cycles are left, doing what the link stub would: the instructions
// counted, Arm7State::sequential set. Not while an observer sees each
// block, see Arm7Jit::observe().
void Compiler::m_leave(uint32_t r15, bool dynamic, int sequential, Arm7JitExit reason, bool synced) {
    flush_charge();
#ifdef GBX_GBA_RELAXED_TIMING
    // Relaxed timing: only the flags the code after a static exit may read
    // before it sets them, when that code is fixed. Those it sets first are
    // never read in the state left at the exit: an interrupt taken there
    // keeps them in its SPSR and restores them, and the code sets them
    // before it reads them all the same.
    if (!synced) {
        const uint8_t live = dynamic || exchanged_ ? 15 : live_flags(r15 - 2 * length_);
        if (live & 12) materialize_nz();
        if (live & 1) materialize_v();
        if (live & 2) materialize_c();
    }
#else
    if (!synced) materialize_all();
#endif
    exchanged_ = false;
    if (!dynamic && !synced && r15 == entry_ && sequential != kRequested && mapped_->region < 8 &&
        mapped_->self_loops) {
        const unsigned none_left = branch(rv::bge, R::zero, kRemaining);
        emit(rv::lw(R::a1, R::sp, kFrameContext));
        emit(rv::lw(R::a0, R::a1, kContextInstructions));
        emit(rv::addi(R::a0, R::a0, static_cast<int32_t>(completed_)));
        emit(rv::sw(R::a0, R::a1, kContextInstructions));
        state_pointer(R::a1);
        emit(rv::addi(R::a0, R::zero, sequential == 1 ? 1 : 0));
        emit(rv::sb(R::a0, R::a1, static_cast<int32_t>(offsetof(Arm7State, sequential))));
        bind(jump(), 0);
        bind(none_left);
    }
    const bool linkable = !dynamic && !cold_mode_ && (sequential != kRequested || !synced) &&
                          link_count_ < links_.size();
    // With relaxed timing, an exit that needs nothing of the link stub but
    // its jump: once linked, link() turns the call of the stub into a jump
    // to the target, whose first instruction checks the cycles (m_entry()).
    // The S of the next fetch, which the stub stores, is not read there;
    // the instructions are counted here where something reads them, and
    // the stub then counts none.
#ifdef GBX_GBA_RELAXED_TIMING
    const bool direct = linkable && !synced && sequential != kRequested;
#else
    const bool direct = false;
#endif
    uint32_t completed = completed_;
    if (direct && mapped_->count_instructions) {
        emit(rv::lw(R::a1, R::sp, kFrameContext));
        emit(rv::lw(R::a0, R::a1, kContextInstructions));
        emit(rv::addi(R::a0, R::a0, static_cast<int32_t>(completed_)));
        emit(rv::sw(R::a0, R::a1, kContextInstructions));
        completed = 0;
    }
    out().align();
    if (synced) call(dynamic ? stubs_.dispatch_synced : linkable ? stubs_.link_synced : stubs_.exit_synced);
    else call(dynamic          ? stubs_.dispatch
              : !linkable      ? stubs_.exit
              : sequential == kRequested ? stubs_.link_requested
                                         : stubs_.link);
    data(dynamic ? 0 : r15);
    data(completed << exit_bits::completed | (sequential == 1 ? exit_bits::sequential : 0) |
         (sequential == kRequested ? 1u << exit_bits::sequential_request : 0) |
         static_cast<uint32_t>(reason) << exit_bits::reason | (dynamic ? exit_bits::dynamic : 0) |
         (linkable ? exit_bits::linkable : 0) | (direct ? exit_bits::direct : 0));
    if (linkable) {
        links_[link_count_++] = static_cast<uint16_t>(here());
        data(0);
    }
    end_ = true;
}

// Header words, the prologue that marks the block used for the code pool's
// CLOCK, the streams, then the link table: the word offsets of the link
// words and their count. Every entry jumps to the prologue, which finds
// its own address; the second header word holds the pool's mark word.
std::span<const uint32_t> Compiler::m_finish(std::span<uint32_t> output, std::span<jit::Relocation> relocations,
                                             size_t& count) {
    hot_.align();
    cold_.align();
    if (failed_ || !hot_.valid() || !cold_.valid()) return {};
    std::array<uint32_t, 8> prologue_words{};
    jit::Assembler prologue(prologue_words, true);
    for (unsigned i = 0; i < kHeaderWords; ++i) prologue.data(0);
    prologue.emit(rv::auipc(R::ra, 0));
    prologue.emit(rv::lw(R::ra, R::ra, -4));
    prologue.emit(rv::sw(R::ra, R::ra, 0));
    prologue.align();
    if (!prologue.valid()) return {};
    const size_t head = prologue.offset(), hot = hot_.offset(), cold = cold_.offset();
    const size_t code = (head + hot + cold) / 4, total = code + link_count_ + 1;
    if (total > output.size() || call_count_ > relocations.size()) return {};
    for (unsigned i = 0; i < boundary_count_; ++i) boundaries_[i].offset = static_cast<uint16_t>(head + boundaries_[i].offset);
    std::copy_n(prologue.code().begin(), head / 4, output.begin());
    std::copy_n(hot_.code().begin(), hot / 4, output.begin() + head / 4);
    std::copy_n(cold_.code().begin(), cold / 4, output.begin() + (head + hot) / 4);
    const auto offset = [&](Label label) { return head + (label & kCold ? hot : 0) + (label & ~kCold); };
    for (unsigned i = 0; i < reference_count_; ++i) {
        const auto& r = references_[i];
        if (!r.bound) return {};
        const auto at = offset(r.from);
        const auto distance = static_cast<int32_t>(static_cast<int64_t>(offset(r.to)) - static_cast<int64_t>(at));
        const auto word = r.kind == Kind::branch ? r.op(r.a, r.b, distance) : rv::jal(R::zero, distance);
        if (!word.valid) return {};
        jit::write_at(output.data(), at, word.bits);
    }
    for (unsigned i = 0; i < call_count_; ++i)
        relocations[i] = {static_cast<uint32_t>(offset(calls_[i].at)), calls_[i].target};
    count = call_count_;
    for (unsigned i = 0; i < link_count_; ++i) output[code + i] = static_cast<uint32_t>((head + links_[i]) / 4);
    output[code + link_count_] = link_count_;
    return output.first(total);
}

}  // namespace gbx::gba::arm7_compiler
