#include "gbx/jit/cache.h"

#include <algorithm>
#include <bit>
#include <limits>

#include "gbx/jit/assembler.h"
#include "gbx/platform.h"

namespace gbx::jit {
namespace {
unsigned hole_class(size_t bytes) {
    return static_cast<unsigned>(std::min<size_t>(bytes / 64, 31));
}
}  // namespace

CodeCache::~CodeCache() {
    if (owned_) platform::free_executable(allocation_);
    platform::free_internal(slots_);
    platform::free_internal(recent_);
    platform::free_internal(index_);
    platform::free_internal(holes_);
}

bool CodeCache::initialize(size_t bytes, size_t max_blocks, size_t shared_bytes,
                           const platform::ExecutableMemory* region) {
    // The pool starts on a cache line of its own.
    shared_bytes = (shared_bytes + 63) & ~size_t{63};
    if (allocation_.words || !bytes || !max_blocks || max_blocks >= none || bytes / 4 > UINT16_MAX ||
        shared_bytes > 1024 * 1024 ||
        max_blocks > std::numeric_limits<size_t>::max() / (4 * sizeof(Slot))) return false;
    if (region && region->words) {
        if (region->size_bytes < shared_bytes + bytes) return false;
        allocation_ = *region;
        owned_ = false;
    } else {
        allocation_ = platform::allocate_executable(shared_bytes + bytes);
        if (!allocation_.words) return false;
        owned_ = true;
    }
    shared_bytes_ = shared_bytes;
    memory_ = {allocation_.words + shared_bytes / 4,
               std::min<size_t>(allocation_.size_bytes - shared_bytes, size_t{UINT16_MAX} * 4)};
    slot_count_ = max_blocks;
    index_mask_ = std::bit_ceil(max_blocks * 2) - 1;
    // Lookups and generated code read these on every block entry. Arrays
    // above the heap's internal threshold would otherwise go to external
    // memory. clear() initializes them.
    slots_ = static_cast<Slot*>(platform::allocate_internal(slot_count_ * sizeof(Slot)));
    recent_ = static_cast<uint32_t*>(platform::allocate_internal(slot_count_ * 2 * sizeof(uint32_t)));
    live_ = recent_ ? recent_ + slot_count_ : nullptr;
    index_ = static_cast<uint32_t*>(platform::allocate_internal((index_mask_ + 1) * sizeof(uint32_t)));
    set_words_ = (slot_count_ + 31) / 32;
    holes_ = static_cast<uint32_t*>(platform::allocate_internal((slot_count_ + kHoleClasses * set_words_) * sizeof(uint32_t)));
    hole_sets_ = holes_ ? holes_ + slot_count_ : nullptr;
    if (!slots_ || !recent_ || !index_ || !holes_) {
        platform::free_internal(slots_); slots_ = nullptr;
        platform::free_internal(recent_); recent_ = live_ = nullptr;
        platform::free_internal(index_); index_ = nullptr;
        platform::free_internal(holes_); holes_ = hole_sets_ = nullptr;
        slot_count_ = index_mask_ = set_words_ = 0;
        if (owned_) platform::free_executable(allocation_);
        allocation_ = memory_ = {}; shared_bytes_ = 0;
        return false;
    }
    clear();
    return true;
}

void CodeCache::retire(uint32_t index) {
    auto& slot = slots_[index];
    // A block with a successor leaves a gap; the last one returns its space
    // to the tail.
    if (slot.next != none)
        set_hole(slot.next, holes_[slot.next] + holes_[index] + slot.words * 4u);
    set_hole(static_cast<Link>(index), 0);
    if (slot.previous == none) head_ = slot.next;
    else slots_[slot.previous].next = slot.next;
    if (slot.next != none) slots_[slot.next].previous = slot.previous;
    else tail_ = slot.previous;
    used_ -= slot.words * size_t{4};
    live_[index] = 0;
    slot.next = free_;
    free_ = static_cast<Link>(index);
}
bool CodeCache::erase(BlockKey key) { return erase_index(key); }
bool CodeCache::erase_index(BlockKey key) {
    if (!index_) return false;
    for (size_t at = slot_for(key); index_[at] != vacant; at = (at + 1) & index_mask_) {
        if (slots_[index_[at]].key != key) continue;
        retire(index_[at]);
        size_t hole = at;
        for (size_t next = (at + 1) & index_mask_; index_[next] != vacant;
             next = (next + 1) & index_mask_) {
            const size_t home = slot_for(slots_[index_[next]].key);
            // Move an entry only when its probe path crosses the hole. The
            // masked distances also cover clusters wrapping around the table.
            if (((hole - home) & index_mask_) < ((next - home) & index_mask_)) {
                index_[hole] = index_[next];
                hole = next;
            }
        }
        index_[hole] = vacant;
        return true;
    }
    return false;
}
bool CodeCache::erase_containing(uintptr_t address, uintptr_t& begin, uintptr_t& end) {
    for (auto block = head_; block != none; block = slots_[block].next) {
        const auto first = reinterpret_cast<uintptr_t>(memory_.words) + start_of(block);
        const size_t bytes = slots_[block].words * size_t{4};
        if (address < first || address - first >= bytes) continue;
        begin = first; end = first + bytes;
        return erase_index(slots_[block].key);
    }
    return false;
}

void CodeCache::set_hole(Link slot, uint32_t bytes) {
    if (const auto old = holes_[slot]) {
        const unsigned size_class = hole_class(old);
        hole_sets_[size_class * set_words_ + slot / 32] &= ~(1u << slot % 32);
        if (!--hole_counts_[size_class]) hole_classes_ &= ~(1u << size_class);
    }
    holes_[slot] = bytes;
    if (bytes) {
        const unsigned size_class = hole_class(bytes);
        hole_sets_[size_class * set_words_ + slot / 32] |= 1u << slot % 32;
        ++hole_counts_[size_class];
        hole_classes_ |= 1u << size_class;
    }
}

size_t CodeCache::gap(size_t bytes, Link& previous, Link& next) {
    // Until the pool's end is reached, every block goes after the last one.
    // Then the gaps are searched by size class: the block's own class, whose
    // gaps may still be too small, then the next nonempty class, any of
    // whose gaps fits.
    const size_t end = tail_ == none ? 0 : end_of(tail_);
    previous = tail_; next = none;
    if (memory_.size_bytes - end >= bytes) return end;
    const unsigned own = hole_class(bytes);
    Link found = none;
    if (hole_classes_ >> own & 1) {
        const uint32_t* set = hole_sets_ + own * set_words_;
        for (size_t word = 0; word < set_words_ && found == none; ++word)
            for (uint32_t members = set[word]; members; members &= members - 1) {
                const auto slot = static_cast<Link>(word * 32 + std::countr_zero(members));
                if (holes_[slot] >= bytes) { found = slot; break; }
            }
    }
    const uint32_t larger = own + 1 < kHoleClasses ? hole_classes_ >> (own + 1) << (own + 1) : 0;
    if (found == none && larger) {
        const uint32_t* set = hole_sets_ + std::countr_zero(larger) * set_words_;
        size_t word = 0;
        while (!set[word]) ++word;
        found = static_cast<Link>(word * 32 + std::countr_zero(set[word]));
    }
    if (found == none) return memory_.size_bytes;
    next = found;
    previous = slots_[found].previous;
    return start_of(found) - holes_[found];
}

InstallResult CodeCache::install(BlockKey key, std::span<const uint32_t> instructions,
                                 std::span<const Relocation> relocations) {
    return insert(key, instructions, relocations, false, nullptr, nullptr);
}

InstallResult CodeCache::install_replacing(BlockKey key, std::span<const uint32_t> instructions,
                                           std::span<const Relocation> relocations,
                                           RetireCallback retired, void* context) {
    return insert(key, instructions, relocations, true, retired, context);
}

InstallResult CodeCache::insert(BlockKey key, std::span<const uint32_t> instructions,
                                std::span<const Relocation> relocations, bool replace,
                                RetireCallback retired, void* context) {
    if (!slots_) return {};
    if (instructions.empty()) return {nullptr, InstallStatus::invalid};
    if (find(key)) return {nullptr, InstallStatus::duplicate};
    const size_t bytes = instructions.size_bytes();
    if (bytes > memory_.size_bytes) return {nullptr, InstallStatus::full};
    for (const auto& relocation : relocations)
        if (relocation.offset & 1 || relocation.offset + 4 > bytes) return {nullptr, InstallStatus::invalid};
    uint32_t evicted = 0;
    Link previous, next;
    size_t offset = gap(bytes, previous, next);
    while (free_ == none || offset >= memory_.size_bytes) {
        if (!replace) return {nullptr, InstallStatus::full};
        for (;;) {
            const auto victim = static_cast<Link>(hand_);
            if (++hand_ == slot_count_) hand_ = 0;
            if (!live_[victim]) continue;
            if (recent_[victim]) { recent_[victim] = 0; continue; }
            // Remove the victim's probe-chain entry before reusing its bytes.
            // erase() also retires its storage; unrelated clusters stay intact.
            const auto& slot = slots_[victim];
            const auto begin = reinterpret_cast<uintptr_t>(memory_.words) + start_of(victim);
            const auto end = begin + slot.words * size_t{4};
            const Link before = slot.previous, after = slot.next;
            erase_index(slot.key); ++evicted;
            if (retired) retired(context, begin, end);
            // The victim's space joins the gaps beside it. Only this span is
            // checked: every other gap was already too small, and a gap found
            // while all slots were taken stays valid unless it is part of it.
            const size_t start = before == none ? 0 : end_of(before);
            const size_t stop = after == none ? memory_.size_bytes : start_of(after);
            if (stop - start >= bytes) { offset = start; previous = before; next = after; }
            break;
        }
    }
    volatile uint32_t* output = memory_.words + offset / 4;
    for (size_t i = 0; i < instructions.size(); ++i) output[i] = instructions[i];
    const auto base = reinterpret_cast<uintptr_t>(memory_.words) + offset;
    for (const auto& relocation : relocations) {
        const auto call = rv32::jal(rv32::jal_link(read_at(instructions.data(), relocation.offset)),
                                    static_cast<int32_t>(relocation.target - (base + relocation.offset)));
        if (!call.valid) return {nullptr, InstallStatus::invalid, evicted};
        write_at(output, relocation.offset, call.bits);
    }
    if (!platform::publish_executable(allocation_, shared_bytes_ + offset, bytes))
        return {nullptr, InstallStatus::sync_failed, evicted};
    const auto entry = reinterpret_cast<BlockEntry>(memory_.words + offset / 4);
    const auto slot_index = free_;
    free_ = slots_[slot_index].next;
    if (!++installations_) ++installations_;
    slots_[slot_index] = {key, static_cast<uint16_t>(offset / 4), static_cast<uint16_t>(bytes / 4), previous, next};
    recent_[slot_index] = 1;
    live_[slot_index] = installations_;
    set_hole(slot_index, static_cast<uint32_t>(offset - (previous == none ? 0 : end_of(previous))));
    if (previous == none) head_ = slot_index;
    else slots_[previous].next = slot_index;
    if (next != none) {
        slots_[next].previous = slot_index;
        set_hole(next, static_cast<uint32_t>(start_of(next) - offset - bytes));
    } else tail_ = slot_index;
    used_ += bytes;
    size_t at = slot_for(key);
    while (index_[at] != vacant) at = (at + 1) & index_mask_;
    index_[at] = slot_index;
    return {entry, InstallStatus::ok, evicted, &recent_[slot_index], &live_[slot_index], installations_};
}

void CodeCache::clear() {
    // Hand out low slots first, matching the former first-vacant scan.
    for (size_t i = 0; i < slot_count_; ++i) {
        slots_[i] = {};
        slots_[i].next = static_cast<Link>(i + 1 < slot_count_ ? i + 1 : none);
    }
    if (recent_) std::fill_n(recent_, slot_count_ * 2, 0u);
    if (index_) std::fill_n(index_, index_mask_ + 1, vacant);
    if (holes_) std::fill_n(holes_, slot_count_ + kHoleClasses * set_words_, 0u);
    hole_classes_ = 0;
    hole_counts_.fill(0);
    free_ = slot_count_ ? 0 : none;
    head_ = tail_ = none; hand_ = 0; used_ = 0;
}

}  // namespace gbx::jit
