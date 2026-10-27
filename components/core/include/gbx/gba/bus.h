#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace gbx::gba {

// One bus cycle of the ARM7TDMI, as the CPU drives it (ARM7TDMI TRM, DDI
// 0029G, chapters 3 and 6): a code fetch, a data read or write, or an
// internal cycle with no memory request. The kind is in bits 0-1, log2 of
// the width in bytes in bits 2-3. Sequential marks an S cycle, otherwise N,
// as the GBA's memory sees it: the SEQ the CPU drove in the cycle before,
// except that a fetch after internal cycles is N, see advance() in
// src/gba/executor.h. Lock is driven through both accesses of SWP (TRM
// 6.11). Refill marks the first fetch of a pipeline refill, which the
// gamepak's prefetch buffer treats apart, see Memory::rom_fetch().
inline constexpr uint32_t kFetch = 0, kRead = 1, kWrite = 2, kIdle = 3;
inline constexpr uint32_t kByte = 0 << 2, kHalf = 1 << 2, kWord = 2 << 2;
inline constexpr uint32_t kSequential = 1 << 4, kLock = 1 << 5, kRefill = 1 << 6;

// The address is the one the CPU drives, unaligned for a misaligned LDR,
// STR, LDM and the like: the memory system ignores the low bits of halfword
// and word accesses (GBATEK "ARM CPU Memory Alignments"), and the CPU
// rotates or extends what it reads. A read returns the value of the
// access's width; a write takes it in the low bits. Internal cycles pass no
// address. Every cycle is one call, in order, internal ones too, so that a
// mapped bus can time them.
//
// One function pointer keeps the interpreter independent of the bus, as in
// gb/bus.h: the test bus below is the only one taking this path. The mapped
// bus of milestone 2 is a class with the same access() member, inlined into
// a second copy of the interpreter, so a game's accesses make no indirect
// call and carry no recording.
struct Bus {
    void* context;
    uint32_t (*cycle)(void* context, uint32_t request, uint32_t address, uint32_t data);

    uint32_t access(uint32_t request, uint32_t address, uint32_t data = 0) const {
        return cycle(context, request, address, data);
    }
    // CPSR changed; the mapped bus checks for a pending interrupt here.
    void psr_written(uint32_t) const {}
    void irq_masked_late() const {}
};

// One recorded access, with the fields of a SingleStepTests/ARM7TDMI
// transaction: kind 0 fetch, 1 read, 2 write; size in bytes; access ORs 1
// for S, 2 for a fetch and 8 for Lock; cycle counts the bus cycles of the
// instruction from 1, internal ones included.
struct BusRecord {
    uint8_t kind = 0, size = 0, access = 0, cycle = 0;
    uint32_t address = 0, data = 0;
};

// The flat bus of the single-step tests (tools/prepare_gba_tests.py), which
// have no memory map. A data read returns the data of the expected
// transaction at the same position when its kind and address match,
// otherwise of the first expected read of that address, otherwise 0. A
// fetch from the case's base address returns its opcode, any other the
// address itself, truncated to the width. Storage is supplied before
// execution and an undersized buffer is reported, not hidden.
class TestBus {
public:
    TestBus(std::span<const BusRecord> expected, uint32_t opcode, uint32_t base, std::span<BusRecord> actual)
        : expected_(expected), actual_(actual), opcode_(opcode), base_(base) {}
    Bus bus() { return {this, cycle}; }
    size_t size() const { return size_; }
    bool overflowed() const { return size_ > actual_.size(); }
    unsigned cycles() const { return cycles_; }

private:
    static uint32_t cycle(void* context, uint32_t request, uint32_t address, uint32_t data);
    std::span<const BusRecord> expected_;
    std::span<BusRecord> actual_;
    uint32_t opcode_, base_;
    size_t size_ = 0;
    unsigned cycles_ = 0;
};

}  // namespace gbx::gba
