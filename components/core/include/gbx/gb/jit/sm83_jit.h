#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include "gbx/gb/sm83.h"
#include "gbx/gb/memory.h"
#include "gbx/jit/cache.h"
#include "gbx/jit/store.h"
#include "gbx/platform.h"

namespace gbx::gb {

enum class JitExit : uint32_t { limit, control_flow, suspended, fallback, event, breakpoint, refetch };
enum class JitFallback : uint8_t { none, unavailable, encoding, capacity, publication };

#ifdef GBX_PROFILER
// Execution counts in profiling builds, shared by every Sm83Jit on the owner
// task. The reader resets them.
struct Sm83JitCounters {
    uint32_t execute, enter;   // execute() calls, gateway entries
    uint32_t exits[8];         // Native exits by JitExit
    // Limit and control-flow exits: the chain budget was spent, or the exit
    // was dynamic, linkable or neither.
    uint32_t budget, cf_dynamic, cf_linked, cf_none;
    // Cache hits, mid-block entries, installed blocks, verified blocks found
    // changed on entry, and writes of a verified block to its own code.
    uint32_t hit, mid, compiles, stale, smc;
    // Evicted or invalidated blocks run on the interpreter instead of
    // installed again, and the instructions they ran there.
    uint32_t interpreted, interpreted_instructions;
    // update_deadline() calls and the stop requests they raised; bank
    // switches that a batch ran on after.
    uint32_t deadline, stop_interrupt, stop_mapping, stop_until, stop_frame, stop_event, remap;
    // Out-of-line helper calls; group_stop counts groups that stopped.
    uint32_t group, group_stop, read, write, code_write, idle;
    // Careful reads and writes by area: below 0x8000, VRAM, cartridge RAM
    // and WRAM, OAM and the unusable area, IO, HRAM and IE. Careful writes
    // by IO register, and the writes that reset the deferral horizon.
    uint32_t read_area[6], write_area[6], io_write[128], horizon_reset;
};
extern Sm83JitCounters sm83_jit_counters;
#endif

// The code pool and the second level behind it, see Sm83Jit::initialize().
struct Sm83JitCapacity {
    size_t pool_bytes = 192 * 1024, pool_blocks = 2048;
    size_t store_bytes = 8u << 20, store_records = 32768;
};

struct JitResult {
    uint32_t instructions = 0;
    uint32_t mcycles = 0;
    JitExit exit = JitExit::limit;
    JitFallback fallback = JitFallback::none;
};

// Single pinned owner, including generation/publication/execution. Flat tests
// and mapped execution use the same resident-register compiler. Flat limit=1
// blocks are opcode-keyed and read operands at runtime. Mapped RAM opcodes are
// checked at each fetch; a mismatching block is retired outside generated code.
class Sm83Jit {
public:
    // Out of line, so that System's code, placed with the hot code, does
    // not change with the members.
    Sm83Jit();
    ~Sm83Jit();
    using Capacity = Sm83JitCapacity;
    bool initialize(std::span<const uint8_t, 65536> code);
    // `reserved`, a block set aside for the pool (reserved_bytes()), holds
    // the pool instead of an allocation of its own; it stays the caller's
    // and must outlive the JIT.
    bool initialize(Memory& memory, bool stop_on_marker = false, const Capacity& capacity = {},
                    const platform::ExecutableMemory* reserved = nullptr);
    // The pool's allocation with `capacity`, its shared code included,
    // rounded up.
    static size_t reserved_bytes(const Capacity& capacity) { return capacity.pool_bytes + kSharedReserve; }
    bool available() const { return cache_.capacity_bytes() != 0 && gateway_bytes_; }
    void clear();
    // Only together with a caller-owned CPU/memory reset, never a cache flush.
    void discard_continuation() { prefetched_count_ = 0; }
    // A fallback left an instruction whose bytes were fetched unfinished.
    bool pending() const { return prefetched_count_ != 0; }
    // After the owner replaced the memory with a saved state: retires the
    // blocks whose entry is in cartridge RAM, WRAM or HRAM. ROM blocks stay,
    // since their keys name the banks they were translated from.
    void forget_ram_code();
    void trace(bool enabled) { tracing_ = enabled; clear(); }
    uint32_t compiled_blocks() const { return compiled_blocks_; }
    // Blocks copied from the second level into the pool.
    uint32_t restored_blocks() const { return restored_blocks_; }
    // Entries from C++ into a block in the second level, and records the
    // second level dropped to make room.
    uint32_t stored_entries() const { return stored_entries_; }
    uint32_t store_drops() const { return static_cast<uint32_t>(store_.drops()); }
    uint32_t invalidations() const { return invalidations_; }
    uint32_t evictions() const { return evictions_; }
    size_t code_bytes() const { return cache_.used_bytes(); }
    const auto& fallbacks() const { return fallbacks_; }
#ifdef GBX_PROFILER
    // Native code, for attributing profiler samples.
    std::span<const uint32_t> code_pool() const { return cache_.storage(); }
    std::span<const uint32_t> gateway() const { return cache_.shared().first(gateway_bytes_ / 4); }
    // The second level, whose segments each start with a copy of the gateway.
    std::span<const uint32_t> code_store() const { return store_.storage(); }
    // The second level's contents.
    size_t store_bytes() const { return store_.used_bytes(); }
    size_t store_records() const { return store_.records(); }
    // Bytes of the compiled blocks, as installed; restores excluded.
    uint32_t compiled_bytes() const { return compiled_bytes_; }
#endif

    // Up to 16 guest instructions, stopping at control flow or suspension.
    // Flat tests request one instruction and replace only the bus. Mapped
    // tracing sends every access through the exact out-of-line path and adds
    // probes, without changing block partition, resident registers or flags.
    // An unavailable/failed translation may execute one interpreter instruction
    // and reports the reason; callers must not count it as native coverage.
    // An unfinished prefetched instruction stays pending instead of refetching.
    JitResult run_block(Sm83State& state, const Bus& bus, unsigned limit = 16);
    JitResult run_block(Sm83State& state, Tick until, unsigned limit = 16);
    // Reuse the mapped execution context across blocks until an interrupt,
    // CPU boundary, the target or, with frame set, a completed frame requires
    // returning to System. Diagnostic stepping retains the single-block
    // interface above.
    JitResult run(Sm83State& state, Tick until, bool frame);

    // Entries of the dispatch table used by generated dynamic exits.
    static constexpr unsigned kDispatchBits = 10;
    // Mapped block header: entry PC | incoming link list << 16, then the
    // CLOCK reference word. Code starts after it.
    static constexpr unsigned kHeaderWords = 2;

    // Shared out-of-line code reached from generated blocks. Register
    // indices follow the SM83 encoding, B C D E H L - A; 6 names a0 for loads
    // and a2 for stores. Addresses: a1, BC, DE, HL. light: no temporary is
    // live across the access, see Compiler::access_stub().
    struct Stubs {
        uintptr_t enter, exit, unlinked, group, read, write, flat_read, flat_write, flat_idle, code_write;
        uintptr_t read_light, write_light;
        uintptr_t group_rom, idle_fast, zero_flag, count_entry;
        std::array<uintptr_t, 17> link, dispatch;        // By instructions completed.
        uintptr_t load[2][4][8], store[2][4][8];         // [light][address][register]
        uintptr_t load_step[2][2], store_step[2][2];     // A via (HL+) or (HL-): [light][decrement]
        uintptr_t inc_dec[2][8];                         // [decrement][register], 6: a0 to a2
        uintptr_t alu[8], rotate[2][8], bit[8];          // Operand a0; rotate[zero flag][operation]
        uintptr_t step_pair[2][3], add_hl[4], daa;       // INC/DEC rr [decrement][BC, DE, HL]
        uintptr_t pop[5], push[5];                       // BC, DE, HL, AF, then a0 or t2
    };

private:
    bool initialize_gateway(size_t pool_bytes, size_t pool_blocks,
                            const platform::ExecutableMemory* reserved = nullptr);
    // Above the gateway.
    static constexpr size_t kSharedReserve = 16 * 1024;
    static void retired(void* self, uintptr_t begin, uintptr_t end);
    void link(uint32_t* word, uintptr_t target);
    void free_link(uint32_t index);
    void reset_links();
    JitResult execute(Sm83State& state, const Bus* bus, Tick until, unsigned limit, bool batch = false,
                      bool frame = false);
    const uint8_t* code_ = nullptr;
    Memory* memory_ = nullptr;
    bool stop_on_marker_ = false;
    bool tracing_ = false;
    unsigned prefetched_count_ = 0;
    std::array<uint8_t, 3> prefetched_{};
    uint32_t invalidations_ = 0, evictions_ = 0;
    std::array<uint64_t, 5> fallbacks_{};
    // The gateway's shared code lies in the room before the pool, so that
    // blocks reach it with jal wherever the platform places the pool.
    jit::CodeCache cache_;
    size_t gateway_bytes_ = 0;
    // The second level: an executable copy of every ROM block the pool
    // accepted, in PSRAM. A block the pool evicted runs there until its
    // entries, counted down in its header, run out; it is then copied into
    // the pool again, and the next count is twice as long. Optional:
    // without it, evicted blocks are compiled again. Records average about
    // 355 bytes in V-Rally, so the bytes run out before the records do.
    // 8 MiB plus a 256 KiB index still leave room beside an 8 MiB ROM in
    // 32 MB of PSRAM.
    static constexpr uint32_t kStoredEntries = 64;
    static constexpr unsigned kMaxStoredLevel = 7;
    jit::CodeStore store_;
    Stubs stubs_{};
    // Batch block entries by PC: {PC | bank << 16, entry}, the bank of the
    // window the block lies in, so that an entry serves again whenever that
    // bank is back. Retired blocks are removed. Mapped only. See
    // Memory::code_tags.
    using Dispatch = std::array<std::array<uint32_t, 2>, 1u << kDispatchBits>;
    std::unique_ptr<Dispatch> dispatch_;
    // Instruction starts inside live blocks, by the key a block starting
    // there would have. Execution resumes there after a mid-block exit. The
    // cache's installation number, kept outside the code pool, proves that
    // the block is still live.
    // `offset` is in bytes from the block's start, `code` in words, and
    // `remaining` counts the block's instructions from there to its end.
    // `owner` is the block's key: once the pool evicts the block, the same
    // offset into its copy in the second level resumes there.
    struct MidEntry {
        jit::BlockKey key, owner; uintptr_t block; const uint32_t* live; uint32_t installation;
        uint16_t offset, code; uint8_t completed, remaining; bool lazy_zero;
    };
    // Large, in external memory: a missing entry makes a new block start
    // there, which costs pool space and evictions.
    static constexpr unsigned kMidBits = 13;
    using MidEntries = std::array<MidEntry, 1u << kMidBits>;
    static unsigned mid_slot(jit::BlockKey key) {
        const auto hash = (static_cast<uint32_t>(key) ^ static_cast<uint32_t>(key >> 32) * 0x9e3779b1u) * 0x85ebca6bu;
        return hash >> (32 - kMidBits);
    }
    std::unique_ptr<MidEntries> mid_;
    // Code changes seen per entry PC. Beyond a few, RAM blocks there verify
    // each opcode at its fetch and read operands at run time instead.
    static constexpr unsigned kUnstableBits = 10;
    static constexpr uint8_t kUnstable = 3;
    // Read only while compiling, so it stays out of internal RAM.
    struct Release { void operator()(void* table) const { platform::free_external(table); } };
    std::unique_ptr<uint8_t[], Release> unstable_;
    // ROM blocks installed before, by key: a block that misses in both
    // levels was evicted from the pool and dropped from the second level,
    // or never stored there. While that keeps happening, installing one
    // again costs as much as interpreting it dozens of times, so it runs on
    // the interpreter until it has missed (1 << level) - 1 more times, and
    // each installation raises the level. Collisions cost only speed. A
    // batch that resumes inside such a block also finishes it on the
    // interpreter, see execute(). Read only on a miss, so it stays out of
    // internal RAM too.
    struct Reinstall {
        uint16_t tag;
        uint8_t misses;
        uint8_t level : 3, count : 5;  // count: the block's instructions, 0 when vacant.
    };
    static constexpr unsigned kReinstallBits = 12, kMaxLevel = 7;
    static uint32_t reinstall_hash(jit::BlockKey key) {
        return (static_cast<uint32_t>(key) * 0x9e3779b1u ^ static_cast<uint32_t>(key >> 32)) * 0x85ebca6bu;
    }
    std::unique_ptr<Reinstall[], Release> reinstall_;
    void unstable(uint16_t pc, uint8_t count);
    bool code_matches(const uint32_t* copy) const;
    uint32_t installation_ = 0;  // Latest; a smaller one means numbers wrapped.
    // Native links into each block, so that retiring it resets only those.
    struct LinkRecord { uint32_t word, next; };  // next and list heads: index + 1
    static constexpr unsigned kLinkRecords = 8192;
    std::unique_ptr<LinkRecord[]> link_records_;
    uint32_t link_free_ = 0;
    // Installations, compiled or from the store, and entries into the
    // store; not reset by clear().
    uint32_t compiled_blocks_ = 0, restored_blocks_ = 0, stored_entries_ = 0;
#ifdef GBX_PROFILER
    uint32_t compiled_bytes_ = 0;
#endif
    // Compilation streams, output, relocations and compiler state; kept off
    // the owner task's stack. A block that does not fit is compiled shorter.
    static constexpr size_t kStreamWords = 2048, kOutputWords = 4096, kStateWords = 3072;
    alignas(8) std::array<uint32_t, 2 * kStreamWords + kOutputWords + kStateWords> scratch_{};
};

}  // namespace gbx::gb
