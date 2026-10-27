#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "gbx/executable.h"
#include "gbx/jit/cache.h"

namespace gbx::jit {

// The second level behind a CodeCache: copies of compiled blocks in PSRAM
// (platform::CodeMemory::external), each relocated for its place and
// published, so that a block the pool evicted runs where it is stored. Only
// blocks whose code cannot change under their key belong here. The storage
// is divided into 1 MiB segments, each starting with a copy of the shared
// code that blocks call with jal, the same code the frontend placed before
// the pool, so that every call stays within jal range. Records are dropped
// oldest first to make room, which retires their code. Allocates only at
// initialize(); one owner task, and retirement only at a safe dispatch
// boundary, as for CodeCache.
class CodeStore {
public:
    struct Record {
        // The executable copy, as relocated for its segment. The frontend may
        // update the words it keeps as inline data, never the instructions.
        std::span<uint32_t> code;
        std::span<const uint16_t> relocations;  // Byte offsets of the calls.
        std::span<const std::byte> data;        // Frontend data, as stored.
        uint32_t* tag;                          // Frontend word, zero when stored.
    };
    static constexpr size_t kSegmentBytes = size_t{1} << 20;
    CodeStore() = default;
    ~CodeStore();
    CodeStore(const CodeStore&) = delete;
    CodeStore& operator=(const CodeStore&) = delete;

    // shared: the code that relocations into [origin, origin + its size)
    // call; the segments' copies take their place.
    bool initialize(size_t bytes, size_t max_records, std::span<const uint32_t> shared, uintptr_t origin);
    // New shared code of the same size for the segments' copies. A store
    // that initialize() gave no storage, as on targets where the platform
    // allocates no external code memory, has none to update.
    bool refresh_shared(std::span<const uint32_t> shared);
    // Stores and publishes a block as compiled; relocations are the calls
    // as for CodeCache::install(). A key already stored must not be stored
    // again. Returns the copy, or empty when it does not fit or a call does
    // not reach. Calls retired for each record dropped to make room.
    std::span<uint32_t> store(BlockKey key, std::span<const uint32_t> instructions,
                              std::span<const Relocation> relocations, std::span<const std::byte> data,
                              RetireCallback retired, void* context);
    // The record stays valid until the next store() or clear().
    bool find(BlockKey key, Record& record) const;
    // A call's target as the frontend gave it to store().
    uintptr_t target(const Record& record, size_t relocation) const;
    // Forgets every record without retiring any: the frontend has dropped
    // every reference to them.
    void clear();
    size_t used_bytes() const { return used_ * 4; }
    size_t records() const { return count_; }
    size_t drops() const { return drops_; }
#ifdef GBX_PROFILER
    std::span<const uint32_t> storage() const { return {memory_.words, memory_.size_bytes / 4}; }
#endif

private:
    static constexpr size_t kSegmentWords = kSegmentBytes / 4;
    size_t home(BlockKey key) const;
    BlockKey key_at(size_t offset) const;
    // The first record position of the segment after the one holding at.
    size_t next_segment(size_t at) const { return (at / kSegmentWords + 1) * kSegmentWords + shared_words_; }
    bool fits(size_t at, size_t size) const {
        return at + size <= std::min((at / kSegmentWords + 1) * kSegmentWords, capacity_);
    }
    // Ends the records of at's segment at at, where a record would start.
    void close(size_t at) { if (at % kSegmentWords && at < capacity_) words_[at] = 0; }
    void drop(RetireCallback retired, void* context);
    void erase(BlockKey key);
    platform::ExecutableMemory memory_;
    uint32_t* words_ = nullptr;
    uint32_t* index_ = nullptr;  // Record offset + 1, or 0 when vacant.
    uintptr_t origin_ = 0;
    size_t shared_words_ = 0, shared_bytes_ = 0;
    size_t capacity_ = 0, index_mask_ = 0, max_records_ = 0;
    // Records lie in [tail_, head_), or once wrapped from tail_ to the end
    // of the lap and from the first segment to head_. A segment's records
    // end at a zero size word or at the segment's end.
    size_t head_ = 0, tail_ = 0, count_ = 0, used_ = 0, drops_ = 0;
    bool wrapped_ = false;
};

}  // namespace gbx::jit
