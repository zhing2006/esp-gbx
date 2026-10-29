#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "gbx/gba/arm7.h"
#include "gbx/gba/bus.h"
#include "gbx/jit/cache.h"
#include "gbx/jit/store.h"
#include "gbx/platform.h"

namespace gbx::gba {

class Memory;
namespace arm7_runtime {
struct Context;
}

enum class Arm7JitExit : uint32_t { limit, control_flow, budget, fallback };
// Why an instruction ran on the interpreter instead: no code pool, an
// encoding the assembler rejected, a block that did not fit, a failed
// publication, or code in a region the JIT does not translate (VRAM,
// palette RAM, OAM, I/O, the backup's and unmapped addresses). Every
// instruction is translated, so there is no reason for an untranslated one.
enum class Arm7JitFallback : uint8_t { none, unavailable, encoding, capacity, publication, region };
inline constexpr unsigned kArm7JitFallbacks = 6;

// The code pool and the second level behind it for mapped execution, see
// Arm7Jit::initialize(Memory&). Each board sets their sizes, see
// GBX_GBA_JIT_POOL_KB and GBX_GBA_JIT_STORE_KB in components/core/Kconfig.
// On a P4 before v3 the CPU's part of the GBA's core shares the upper
// internal RAM with the heap (tools/link_core_layout.py), which then holds
// a pool of 192 KiB beside the system.
struct Arm7JitCapacity {
    size_t pool_bytes = GBX_GBA_JIT_POOL_BYTES, pool_blocks = GBX_GBA_JIT_POOL_BYTES / 192;
    size_t store_bytes = GBX_GBA_JIT_STORE_BYTES, store_records = GBX_GBA_JIT_STORE_BYTES / 256;
};

// Where the compiler reads code: the word, or in Thumb state the halfword,
// that a fetch from the address returns, read without a bus cycle.
struct Arm7CodeSource {
    const void* context;
    uint32_t (*read)(const void* context, uint32_t address, bool thumb);
};

struct Arm7JitResult {
    uint32_t instructions = 0;
    Arm7JitExit exit = Arm7JitExit::limit;
    Arm7JitFallback fallback = Arm7JitFallback::none;
};

// ARM and Thumb blocks translated to RV32, see src/gba/jit/arm7_compiler.h.
// One pinned owner task generates, publishes and runs the code. On the flat
// test bus every bus cycle, code fetches included, goes through the Bus, as
// the interpreter's do; on the GBA's memory map (Memory) blocks follow
// MappedBus's timing with their own fast paths, see run(Arm7State&,
// Memory&).
class Arm7Jit {
public:
    using Capacity = Arm7JitCapacity;
    // Out of line, so that System's code, placed with the hot code, does
    // not change with the members.
    Arm7Jit();
    ~Arm7Jit();
    // The flat test bus: the code pool and its index, with the shared code
    // and the single-block slot before them.
    bool initialize(size_t pool_bytes = 32 * 1024, size_t pool_blocks = 256);
    // The memory map: the pool, the second level, the dispatch table and
    // the tables of the runtime. The JIT watches `memory`'s IWRAM and EWRAM
    // for writes to translated code until it is destroyed or clear()ed.
    // `reserved`, a block set aside for the pool (reserved_bytes()), holds
    // the pool instead of an allocation of its own; it stays the caller's
    // and must outlive the JIT.
    bool initialize(Memory& memory, const Capacity& capacity = {},
                    const platform::ExecutableMemory* reserved = nullptr);
    // The pool's allocation with `capacity`, its shared code included,
    // rounded up.
    static size_t reserved_bytes(const Capacity& capacity) { return capacity.pool_bytes + kSharedReserve; }
    bool available() const { return gateway_bytes_ != 0; }
    void clear();

    // Runs to the memory's target as arm7_run_mapped() does, with the same
    // results: interrupts, HALT and stops at instruction boundaries, events
    // at their exact cycles. Code the JIT does not translate runs on the
    // interpreter, counted by reason.
    void run(Arm7State& state, Memory& memory);
    // After the owner replaced the memory, as a saved state does: retires
    // the blocks in IWRAM and EWRAM. BIOS and ROM blocks stay.
    void forget_ram_code();
    // The second level, when initialize() had no size for it: a game that
    // gives it the PSRAM left once its other memory is allocated.
    bool initialize_store(size_t bytes, size_t records);
    size_t store_capacity() const { return store_.capacity_bytes(); }
    uint32_t invalidations() const { return invalidations_; }
    uint32_t evictions() const { return evictions_; }
    // Blocks copied from the second level into the pool, entries from C++
    // into a block in the second level, and records it dropped for room.
    uint32_t restored_blocks() const { return restored_blocks_; }
    uint32_t stored_entries() const { return stored_entries_; }
    uint32_t store_drops() const { return static_cast<uint32_t>(store_.drops()); }
    size_t code_bytes() const { return cache_.used_bytes(); }
#ifdef GBX_PROFILER
    // Where the generated code lives, for the profiler: the pool, the
    // gateway at its start, and the second level, whose segments each start
    // with a copy of the gateway.
    std::span<const uint32_t> code_pool() const { return cache_.storage(); }
    std::span<const uint32_t> gateway() const { return cache_.shared().first(gateway_bytes_ / 4); }
    std::span<const uint32_t> code_store() const { return store_.storage(); }
#endif
    // Lockstep diagnosis: run() calls `observer` at each turn of its loop,
    // after a boundary's catch-up and before a stop is taken, and once at
    // its end (`end`), with the instructions it ran and the stops it took.
    // Unless `linked`, blocks are then neither linked nor entered through
    // the dispatch table, nor jump back to their own start, so that each
    // entry into native code runs one block: the blocks compiled before
    // go. Null ends it.
    using Observer = void (*)(void* context, uint32_t instructions, uint32_t stops, bool end);
    void observe(Observer observer, void* context, bool linked = false) {
        const bool unlinked = observer_ && !observe_linked_;
        observer_ = observer;
        observer_context_ = context;
        observe_linked_ = linked;
        if (unlinked != (observer && !linked)) clear();
    }

    // Runs cached blocks of up to `limit` instructions from `state` while
    // `budget` bus cycles remain: an instruction starts only while it is
    // positive, inside a block too, and the bus cycles it takes are counted
    // off. On return `budget` holds what is left. An instruction whose block
    // cannot be installed runs on the interpreter, reported as a fallback.
    Arm7JitResult run(Arm7State& state, const Bus& bus, Arm7CodeSource code, int32_t& budget,
                      unsigned limit = kMaxBlock);

    // Single-step tests: the instruction at `state` compiled as a block of
    // its own, published to a slot that the next one overwrites, and run
    // once. The steps are apart so that the caller can time them. When
    // compiling or publishing fails, execute_single() runs the instruction
    // on the interpreter and reports why.
    Arm7JitFallback compile_single(const Arm7State& state, Arm7CodeSource code);
    Arm7JitFallback publish_single();
    Arm7JitResult execute_single(Arm7State& state, const Bus& bus);

    const std::array<uint64_t, kArm7JitFallbacks>& fallbacks() const { return fallbacks_; }
    uint32_t compiled_blocks() const { return compiled_blocks_; }
    // Instructions run() ran on the interpreter.
    uint64_t interpreted() const { return interpreted_; }
    // The native code and the instructions of the blocks compiled so far,
    // and the second level's use.
    uint64_t compiled_bytes() const { return compiled_bytes_; }
    uint64_t compiled_instructions() const { return compiled_instructions_; }
    size_t store_bytes() const { return store_.used_bytes(); }
    size_t store_records() const { return store_.records(); }

    static constexpr unsigned kMaxBlock = 64;
    // Entries of the dispatch table, see Stubs::dispatch.
    static constexpr unsigned kDispatch = 1024;
    // Mapped block header: the entry R15, the head of the list of links
    // into the block (LinkRecord index + 1), and the code pool's CLOCK
    // mark word, or in the second level the entries left. Code starts after
    // it.
    static constexpr unsigned kHeaderWords = 3;
    // A block's key: R15, then CPSR's T and mode and the timing tag of the
    // code's region (Memory's wait states that its code embeds).
    static jit::BlockKey key(uint32_t r15, uint32_t keyhi) { return r15 | uint64_t{keyhi} << 32; }

    // Shared code reached from blocks with jal, see initialize_gateway().
    // The m_ stubs serve mapped blocks, see arm7_compiler_mapped.cpp.
    struct Stubs {
        uintptr_t enter, exit, exit_synced, side_exit;
        uintptr_t access, fetch, sync_out, read_cpsr;
        uintptr_t write_cpsr, restore_cpsr, exception, carry_mul, carry;
        uintptr_t m_catch_fetch, m_catch, m_idle, m_read, m_write, m_fetch, m_refill, m_boundary, m_stop;
        uintptr_t m_catch_fetch_at, m_catch_at, m_idle_at;
        uintptr_t link, link_requested, dispatch, link_synced, dispatch_synced, count_entry;
        // ROM fetches and refills with the prefetch buffer: Thumb from WS0,
        // WS1, WS2, then ARM, see Compiler::m_rom_fetch(), m_rom_refill() and
        // m_refill_dynamic().
        std::array<uintptr_t, 6> m_rom_fetch, m_rom_refill, m_rom_refill_dynamic;
        // Dynamic refills from IWRAM and EWRAM blocks, into Thumb and ARM
        // state, see Compiler::m_refill_dynamic().
        std::array<uintptr_t, 2> m_ram_refill;
        // Data reads and writes by width, see Compiler::m_access().
        std::array<uintptr_t, 3> m_load, m_store;
    };

private:
    friend struct Arm7JitAccess;
    // The wait states the gateway's stubs embed, from Memory's cycles: the
    // S and N fetches of a halfword and of a word by ROM region pair, and
    // EWRAM accesses of a byte or halfword and of a word. New ones
    // regenerate the gateway, see refresh_gateway().
    struct StubCycles {
        std::array<uint8_t, 3> s16{}, s32{}, n16{}, n32{};
        std::array<uint8_t, 2> ewram{};
        bool operator==(const StubCycles&) const = default;
    };
    static StubCycles stub_cycles(const Memory& memory);
    // The access stubs embed the addresses of `memory`'s RAM; without it
    // they are never called.
    bool initialize_gateway(size_t pool_bytes, size_t pool_blocks, const Memory* memory,
                            const platform::ExecutableMemory* reserved = nullptr);
    // Above the gateway and the single-step slot, about 11 KiB.
    static constexpr size_t kSharedReserve = 16 * 1024;
    // Assembles the gateway into output_ with the stubs' offsets.
    bool generate_gateway(const Memory* memory, const StubCycles& cycles, Stubs& stubs, size_t& bytes);
    bool refresh_gateway();
    // Compiles the block at `state` into output_; returns its size in words,
    // 0 on failure, with the reason.
    size_t compile(const Arm7State& state, Arm7CodeSource code, unsigned limit, Arm7JitFallback& failure);
    uint32_t enter(void* context, uintptr_t entry) const;
    Arm7JitResult fallback(Arm7State& state, const Bus& bus, Arm7JitFallback reason);

    jit::CodeCache cache_;
    Stubs stubs_{};
    size_t gateway_bytes_ = 0;
    // The single-block slot follows the gateway in the shared code.
    static constexpr size_t kSingleBytes = 4096;
    size_t single_words_ = 0;
    Arm7JitFallback single_failure_ = Arm7JitFallback::unavailable;
    std::array<uint64_t, kArm7JitFallbacks> fallbacks_{};
    uint32_t compiled_blocks_ = 0;
    uint64_t compiled_bytes_ = 0, compiled_instructions_ = 0, interpreted_ = 0;
#ifdef GBX_PROFILER
public:
    std::array<uint32_t, 2> compile_reasons_{};
private:
#endif

    // --- Mapped execution, see arm7_jit_runtime.cpp -------------------------
    struct Release { void operator()(void* table) const { platform::free_external(table); } };
    struct ReleaseInternal { void operator()(void* table) const { platform::free_internal(table); } };
    // Where to run the code at the state's R15 when no block starts there
    // in the pool: the code to enter, with `start` when a block starts
    // there and `skipped` instructions of the block before it; or 0 with
    // `interpret` instructions to run on the interpreter instead, or with
    // the reason when no block could be installed. With `resume`, a run
    // may enter a block at an inner boundary.
    struct Found {
        uintptr_t target = 0;
        bool start = false;
        // A boundary inside a pool block that the dispatch table may hold,
        // see dispatch_boundary().
        bool dispatchable = false;
        uint8_t skipped = 0;
        unsigned interpret = 0;
        Arm7JitFallback failure = Arm7JitFallback::none;
    };
    Found find_block(const Arm7State& state, jit::BlockKey key, bool resume);
    // Compiles the mapped block at `state` into output_: its size in words,
    // with its instructions and boundaries (compiled_), or 0.
    size_t compile_mapped(const Arm7State& state, unsigned limit);
    // The interpreter for up to `count` instructions from `state`, or while
    // its code stays in regions the JIT does not translate (count 0),
    // stopping early at a boundary with no cycles left. Returns the
    // instructions it ran.
    unsigned interpret(Arm7State& state, unsigned count);
    void update_tags();
    uint32_t keyhi(uint32_t r15, uint32_t cpsr) const { return (cpsr & 0x3f) | uint32_t{tags_[r15 >> 24]} << 6; }
    static void retired(void* self, uintptr_t begin, uintptr_t end);
    static void code_written(void* self, uint32_t address);
    void link(uint32_t* word, uintptr_t target);
    void direct_jump(uint32_t* word, uintptr_t target);
    void dispatch_boundary(uint32_t r15, jit::BlockKey key, uintptr_t target, unsigned skipped);
    void free_link(uint32_t index);
    void reset_links();
    void retire_dead();
    void mark_lines(uint32_t begin, uint32_t end, int delta);
    void watch(uintptr_t block, uint32_t first, uint32_t bytes, uint32_t copy_bytes);
    bool retire_ram(unsigned index);
    bool may_promote();
    void retire_copy(uintptr_t block, uint32_t first, uint32_t copy_bytes);
    static constexpr unsigned kPromotionsPerFrame = 16;
    uint32_t promotion_frame_ = 0;
    unsigned promotions_ = 0;
    Memory* memory_ = nullptr;
    arm7_runtime::Context* active_ = nullptr;  // The run in progress.
    // Timing tags by address bits 24-31, followed by the dispatch table:
    // {R15, key high word, code}, by (R15 / 2 + key high word) modulo
    // kDispatch. Internal RAM: generated code reads both. The entries of
    // boundaries inside blocks (dispatch_boundary()) carry the instructions
    // the run skips in bits 24-31 of the key word, and the latest
    // kBoundaryEntries are on record, so that retired() finds those inside
    // a block.
    struct Dispatch { uint32_t r15, keyhi, code; };
    struct Tables {
        std::array<uint8_t, 256> tags;
        std::array<Dispatch, kDispatch> dispatch;
    };
    std::unique_ptr<Tables, ReleaseInternal> tables_;
    static constexpr unsigned kBoundaryEntries = 16;
    struct BoundaryEntry { uint32_t code; uint16_t index; };
    std::array<BoundaryEntry, kBoundaryEntries> boundary_entries_{};
    unsigned boundary_next_ = 0;
    uint8_t* tags_ = nullptr;
    uint32_t waits_generation_ = 0;
    StubCycles stub_cycles_{};
    // The second level: an executable copy of every BIOS and ROM block the
    // pool accepted, in PSRAM. An evicted block runs there until its
    // entries, counted down in its header, run out; it then moves into the
    // pool again, and the next count is twice as long.
    static constexpr uint32_t kStoredEntries = 64;
    static constexpr unsigned kMaxStoredLevel = 7;
    jit::CodeStore store_;
    // RAM blocks also keep copies there, up to kVersions under each key,
    // with the code they were compiled from: games that swap routines in
    // and out of IWRAM or patch them run the same code again and again,
    // and a copy whose code is in RAM again moves back into the pool. The
    // copies never run in the second level, where writes are not watched.
    static constexpr unsigned kVersions = 4;
    static jit::BlockKey version_key(jit::BlockKey key, unsigned slot) { return key | uint64_t{slot + 1} << 61; }
    bool same_code(const jit::CodeStore::Record& record, uint32_t first) const;
    // Instruction boundaries inside live blocks, by the key a block starting
    // there would have, where a run resumes after a stop inside the block,
    // such as an interrupt. Only boundaries without pending flags qualify.
    // `offset` is in bytes from the block's start, `completed` the
    // instructions before it and `remaining` those from it to the block's
    // end. The code there takes the next fetch's SEQ from the instruction
    // before (`sequential`); a run that comes with another, as a return
    // from an interrupt does, enters after one instruction on the
    // interpreter. `owner` is the block's key: once the pool evicts a BIOS
    // or ROM block, the same offset into its copy in the second level
    // resumes there.
    struct MidEntry {
        jit::BlockKey key, owner;
        uintptr_t block;
        const uint32_t* live;
        uint32_t installation;
        uint16_t offset;
        uint8_t completed : 7, sequential : 1;
        uint8_t remaining;
    };
    static constexpr unsigned kMidBits = 13;
    static unsigned mid_slot(jit::BlockKey key) {
        const auto hash = (static_cast<uint32_t>(key) ^ static_cast<uint32_t>(key >> 32) * 0x9e3779b1u) * 0x85ebca6bu;
        return hash >> (32 - kMidBits);
    }
    std::unique_ptr<MidEntry[], Release> mid_;
    // BIOS and ROM blocks installed before, by key: one that misses in both
    // levels runs on the interpreter until it has missed (1 << level) - 1
    // more times, and each installation raises the level, see find_block().
    struct Reinstall {
        uint16_t tag;
        uint8_t misses;
        uint8_t level;
        uint8_t count;  // The block's instructions, 0 when vacant.
    };
    static constexpr unsigned kReinstallBits = 12, kMaxLevel = 7;
    static uint32_t reinstall_hash(jit::BlockKey key) {
        return (static_cast<uint32_t>(key) * 0x9e3779b1u ^ static_cast<uint32_t>(key >> 32)) * 0x85ebca6bu;
    }
    std::unique_ptr<Reinstall[], Release> reinstall_;
    uint32_t installation_ = 0;  // Latest; a smaller one means numbers wrapped.
    // Native links into each block, so that retiring it resets only those.
    struct LinkRecord { uint32_t word, next; };  // next and list heads: index + 1
    static constexpr unsigned kLinkRecords = 8192;
    std::unique_ptr<LinkRecord[], Release> link_records_;
    uint32_t link_free_ = 0;
    // Blocks in IWRAM and EWRAM: their storage and the guest code they
    // cover, [begin, end), at the RAM's first mirror, as code_written()
    // compares the writes there, which retire them (dead). A block stays
    // within one mirror (Compiler::in_region()). Their 64-byte lines are
    // counted in Memory's marks.
    struct RamBlock {
        uintptr_t block;
        uint32_t begin, end, copy_bytes;
        uint16_t previous, next;  // in the bucket's list, index + 1
        bool dead;
    };
    // Enough that a block is rarely retired for want of a record, which
    // brings it back from the second level the next time it runs.
    static constexpr unsigned kRamBlocks = 4096;
    std::unique_ptr<RamBlock[], Release> ram_blocks_;
    // The records by the 256 bytes of RAM their code begins in, IWRAM's
    // and then EWRAM's, as lists (heads: index + 1): a block covers at most
    // kMaxBlock words, so that a write and a retirement look only at the
    // records of the bucket they fall in and the one before.
    static constexpr unsigned kBucketBits = 8, kBuckets = (0x8000 + 0x40000) >> kBucketBits;
    static_assert(kMaxBlock * 4 <= 1u << kBucketBits);
    std::unique_ptr<uint16_t[], Release> buckets_;
    static unsigned bucket(uint32_t address) {
        return ((address >> 24) == 3 ? address & 0x7fff : 0x8000 + (address & 0x3ffff)) >> kBucketBits;
    }
    // Calls f(index) for each record of the buckets of `address` and the one
    // before, while f returns false; the records may not change meanwhile.
    template<class F> void each_ram(uint32_t address, F f) const {
        const unsigned last = bucket(address);
        // The first bucket of IWRAM or of EWRAM has none before it.
        const bool start = !(address & ((address >> 24) == 3 ? 0x7fff : 0x3ffff) & ~((1u << kBucketBits) - 1));
        for (unsigned b = start ? last : last - 1; b <= last; ++b) {
            for (unsigned i = buckets_[b]; i;) {
                const unsigned index = i - 1;
                i = ram_blocks_[index].next;
                if (f(index)) return;
            }
        }
    }
    void remove_ram(unsigned index);
    // Records code_written() marked dead since retire_dead(), by their
    // block, or more than these.
    static constexpr unsigned kDeadBlocks = 16;
    std::array<std::pair<uintptr_t, uint32_t>, kDeadBlocks> dead_blocks_{};
    unsigned dead_count_ = 0;
    // The words of IWRAM, then EWRAM, that a block's code covered, a bit
    // each: a write to a marked line elsewhere, such as data next to code,
    // skips the search of the blocks. A bit stays when its blocks retire
    // and goes at the next write that finds none.
    static constexpr unsigned kIwramWords = 0x8000 / 4, kCodeWords = kIwramWords + 0x40000 / 4;
    std::unique_ptr<uint32_t[], Release> code_words_;
    static unsigned code_word(uint32_t address) {
        return (address >> 24) == 3 ? (address & 0x7fff) >> 2 : kIwramWords + ((address & 0x3ffff) >> 2);
    }
    unsigned ram_count_ = 0;
    bool dead_ = false;
    Observer observer_ = nullptr;
    void* observer_context_ = nullptr;
    bool observe_linked_ = false;
    // What compile_mapped() found: the block's instructions, the guest code
    // it covers and its inner boundaries, as offset | completed << 16 |
    // sequential << 24.
    struct Compiled {
        unsigned instructions = 0, boundaries = 0;
        uint32_t end = 0;
        std::array<uint32_t, kMaxBlock + 1> boundary{};
    } compiled_;
    uint32_t invalidations_ = 0, evictions_ = 0, restored_blocks_ = 0, stored_entries_ = 0;
    // Each clear() too retires every block, run() included, as when an
    // observer changes the mode from its call.
    uint32_t clears_ = 0;
    // Moves whenever blocks are retired, so that run() links no exit of a
    // block that may be gone.
    uint32_t retired() const { return evictions_ + invalidations_ + clears_ + static_cast<uint32_t>(store_.drops()); }
    // Compilation streams, output, relocations and compiler state, kept off
    // the owner task's stack. A block that does not fit is compiled shorter.
    static constexpr size_t kHotWords = 2048, kColdWords = 1024, kOutputWords = 3072, kRelocations = 512;
    // The compiler object, whose pointers are larger on 64-bit hosts.
    static constexpr size_t kStateWords = sizeof(void*) == 4 ? 2688 : 4352;
    std::array<uint32_t, kHotWords> hot_;
    std::array<uint32_t, kColdWords> cold_;
    std::array<uint32_t, kOutputWords> output_;
    std::array<jit::Relocation, kRelocations> relocations_;
    size_t relocation_count_ = 0;
    alignas(8) std::array<uint32_t, kStateWords> state_;
};

}  // namespace gbx::gba
