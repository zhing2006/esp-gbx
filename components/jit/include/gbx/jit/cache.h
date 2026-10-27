#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "gbx/executable.h"

namespace gbx::jit {

// Default callable entry: RV32 psABI integer/pointer-only function (also valid
// with IDF's ILP32F ABI). CodeCache itself never calls an entry. A frontend may
// store private entries if it always enters through its own psABI gateway.
// a0 receives context and returns an opaque frontend-defined exit reason.
// Preserve s0-s11 and sp; never change gp/tp. Keep sp 16-byte aligned. Save ra
// before calling helpers. ra, a0-a7 and t0-t6 are caller-saved; context/live
// values must survive helpers in saved registers/stack. No FP registers are
// emitted. Helpers must follow the normal ABI, including FP preservation.
// No aggregates, varargs or FP arguments/results in this initial interface.
using BlockEntry = uint32_t (*)(void* context);
using BlockKey = uint64_t;  // Opaque identity supplied by the frontend, not a hash.

enum class InstallStatus { ok, unavailable, invalid, duplicate, full, sync_failed };
struct InstallResult {
    BlockEntry entry = nullptr;
    InstallStatus status = InstallStatus::unavailable;
    uint32_t evicted = 0;  // Number of retired blocks, not whole-pool flushes.
    // Native links bypass find(); a nonzero store marks the block as used.
    // Stable storage until destruction, valid for this installation only.
    uint32_t* referenced = nullptr;
    // Holds `installation` while this block is live and zero once it is
    // retired. Stable storage until destruction. Numbers are nonzero and
    // repeat only after 2^32 - 1 installations.
    const uint32_t* live = nullptr;
    uint32_t installation = 0;
};
// A PC-relative `jal link, target` patched once the block address is known,
// so blocks can reach shared out-of-line code with one instruction. offset:
// an even byte offset into the block, where the code holds a jal that names
// the link register; the instruction may straddle two words.
struct Relocation { uint32_t offset; uintptr_t target; };
// Called for every retired block, before its storage is reused, so the
// frontend can drop native links into [begin, end).
using RetireCallback = void (*)(void* context, uintptr_t begin, uintptr_t end);

// One pinned owner task, including execution and callbacks. Allocates only at
// initialize(). install() never evicts; install_replacing() uses a CLOCK policy
// and reclaims only enough blocks for a contiguous allocation. Both replacement
// and clear() require a safe dispatch boundary, never a running block or
// helper. All entries become invalid on clear()/destruction, even if their
// addresses are reused. No cross-core execution or concurrent access is allowed.
class CodeCache {
public:
    CodeCache() = default;
    ~CodeCache();
    CodeCache(const CodeCache&) = delete;
    CodeCache& operator=(const CodeCache&) = delete;

    // shared_bytes: room before the pool for code that blocks reach with
    // jal, such as a frontend's out-of-line stubs, within the jal range
    // wherever the platform places the pool. `region`, when given, holds the
    // shared code and the pool instead of an allocation of their own, and
    // the pool takes all of it after the shared code, at least `bytes`; it
    // stays the caller's and must outlive the cache.
    bool initialize(size_t bytes = 64 * 1024, size_t max_blocks = 512, size_t shared_bytes = 0,
                    const platform::ExecutableMemory* region = nullptr);
    BlockEntry find(BlockKey key) const;
    InstallResult install(BlockKey key, std::span<const uint32_t> instructions,
                          std::span<const Relocation> relocations = {});
    InstallResult install_replacing(BlockKey key, std::span<const uint32_t> instructions,
                                    std::span<const Relocation> relocations = {},
                                    RetireCallback retired = nullptr, void* context = nullptr);
    void clear();
    // Retire one identity at a safe dispatch boundary, never from a helper
    // called by generated code. Returns whether a live block was removed.
    bool erase(BlockKey key);
    // Retire the live block whose storage contains address; reports its range.
    bool erase_containing(uintptr_t address, uintptr_t& begin, uintptr_t& end);
    // Visits each live block's storage as (first word, size in bytes).
    template<class Visitor> void visit(Visitor&& visitor) const {
        for (auto block = head_; block != none; block = slots_[block].next)
            visitor(memory_.words + slots_[block].offset, slots_[block].words * size_t{4});
    }
    size_t used_bytes() const { return used_; }
    size_t capacity_bytes() const { return memory_.size_bytes; }
    // The room for shared code, which the owner writes and then publishes
    // before any block that calls it runs. offset: a word-aligned start
    // within that room, for code the owner rewrites between runs.
    std::span<uint32_t> shared() const { return {allocation_.words, shared_bytes_ / 4}; }
    bool publish_shared(size_t bytes, size_t offset = 0) const {
        return offset <= shared_bytes_ && bytes <= shared_bytes_ - offset &&
               platform::publish_executable(allocation_, offset, bytes);
    }
    // Whether `word` is in the pool, and publishing words the owner
    // rewrote inside a live block, such as a jump it patched.
    bool owns(const uint32_t* word) const { return word >= memory_.words && word < memory_.words + memory_.size_bytes / 4; }
    bool publish_block(const uint32_t* words, size_t bytes) const {
        return platform::publish_executable(
            allocation_, static_cast<size_t>(reinterpret_cast<uintptr_t>(words) - reinterpret_cast<uintptr_t>(allocation_.words)),
            bytes);
    }
#ifdef GBX_PROFILER
    std::span<const uint32_t> storage() const { return {memory_.words, memory_.size_bytes / 4}; }
#endif

private:
    static constexpr uint32_t vacant = UINT32_MAX;  // Empty index entry.
    // Slot links: live blocks by address, vacant slots in the free list.
    using Link = uint16_t;
    static constexpr Link none = UINT16_MAX;
    // 16 bytes. Offsets and sizes count words: the pool is smaller than
    // 256 KiB.
    struct Slot {
        BlockKey key;
        uint16_t offset, words;
        Link previous, next;
    };
    static_assert(sizeof(Slot) == 16);
    // A slot's storage in bytes from the pool's start.
    size_t start_of(Link slot) const { return slots_[slot].offset * size_t{4}; }
    size_t end_of(Link slot) const { return (slots_[slot].offset + slots_[slot].words) * size_t{4}; }
    // Keep hashing inline in lookup and mutation paths across source files.
#if __has_cpp_attribute(gnu::always_inline)
    [[gnu::always_inline]]
#endif
    size_t slot_for(BlockKey key) const {
        // Full keys establish identity. A 32-bit hash and power-of-two index avoid
        // software 64-bit division on RV32.
        uint32_t hash = static_cast<uint32_t>(key) ^ std::rotl(static_cast<uint32_t>(key >> 32), 13);
        hash ^= hash >> 16;
        hash *= 0x7feb352d;
        hash ^= hash >> 15;
        return hash & index_mask_;
    }
    void retire(uint32_t index);
    bool erase_index(BlockKey key);
    void set_hole(Link slot, uint32_t bytes);
    size_t gap(size_t bytes, Link& previous, Link& next);
    InstallResult insert(BlockKey key, std::span<const uint32_t> instructions,
                         std::span<const Relocation> relocations, bool replace,
                         RetireCallback retired, void* context);
    platform::ExecutableMemory allocation_;  // The shared code, then the pool.
    platform::ExecutableMemory memory_;      // The pool, inside allocation_.
    bool owned_ = false;                     // allocation_ is the cache's to free.
    size_t shared_bytes_ = 0;
    Slot* slots_ = nullptr;
    // By slot: the used mark that generated code sets, and the installation
    // number, zero when vacant. Kept apart from the slots, whose layout the
    // frontend never sees.
    uint32_t* recent_ = nullptr;
    uint32_t* live_ = nullptr;
    uint32_t* index_ = nullptr;
    Link free_ = none;  // First vacant slot.
    uint32_t installations_ = 0;
    size_t slot_count_ = 0, index_mask_ = 0;
    Link head_ = none, tail_ = none;  // Live blocks in address order.
    uint32_t hand_ = 0;
    size_t used_ = 0;
    // Gaps between live blocks, the tail excluded, each recorded at the
    // block after it, and sets of those blocks by gap size class, so that a
    // search reads a few words: class c holds gaps of c * 64 to c * 64 + 63
    // bytes, the last class every larger one.
    static constexpr unsigned kHoleClasses = 32;
    uint32_t* holes_ = nullptr;      // Bytes by slot; zero for none or vacant.
    uint32_t* hole_sets_ = nullptr;  // kHoleClasses bitmaps of set_words_ words.
    size_t set_words_ = 0;
    uint32_t hole_classes_ = 0;      // Classes that hold a gap.
    std::array<uint16_t, kHoleClasses> hole_counts_{};
};

}  // namespace gbx::jit
