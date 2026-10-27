#include "gbx/jit/store.h"

#include <algorithm>
#include <bit>
#include <limits>

#include "gbx/jit/assembler.h"
#include "gbx/platform.h"

namespace gbx::jit {
namespace {
// Record layout in words: size, never 0, which ends a segment's records
// instead; key low and high halves; instruction count (13 bits) |
// relocation count << 13 (10 bits) | data bytes << 23 (9 bits); the
// frontend's tag; then the instructions, the relocation offsets two to a
// word and the data. Every store into the executable storage is an aligned
// word.
constexpr size_t kHeaderWords = 5;
constexpr size_t kMaxInstructions = 0x1fff, kMaxRelocations = 0x3ff, kMaxData = 0x1ff;
constexpr size_t instructions_of(uint32_t word) { return word & kMaxInstructions; }
constexpr size_t relocations_of(uint32_t word) { return word >> 13 & kMaxRelocations; }
constexpr size_t data_of(uint32_t word) { return word >> 23; }
}  // namespace

CodeStore::~CodeStore() {
    platform::free_executable(memory_);
    platform::free_external(index_);
}

bool CodeStore::initialize(size_t bytes, size_t max_records, std::span<const uint32_t> shared, uintptr_t origin) {
    // Each copy of the shared code fills whole cache lines.
    const size_t shared_words = (shared.size() + 15) & ~size_t{15};
    if (words_ || shared_words >= kSegmentWords / 2 || bytes / 4 <= shared_words + kHeaderWords ||
        bytes / 4 >= std::numeric_limits<uint32_t>::max() || !max_records ||
        max_records > std::numeric_limits<uint32_t>::max() / 2) return false;
    index_mask_ = std::bit_ceil(max_records * 2) - 1;
    memory_ = platform::allocate_executable(bytes, platform::CodeMemory::external);
    index_ = static_cast<uint32_t*>(platform::allocate_external((index_mask_ + 1) * 4));
    if (!memory_.words || !index_) {
        platform::free_executable(memory_);
        platform::free_external(index_); index_ = nullptr;
        index_mask_ = 0;
        return false;
    }
    words_ = memory_.words;
    capacity_ = memory_.size_bytes / 4;
    origin_ = origin;
    shared_words_ = shared_words;
    shared_bytes_ = shared.size_bytes();
    for (size_t segment = 0; segment < capacity_; segment += kSegmentWords) {
        // A last segment without room for a record after its copy is left out.
        if (capacity_ - segment <= shared_words_ + kHeaderWords) { capacity_ = segment; break; }
        volatile uint32_t* copy = words_ + segment;
        for (size_t i = 0; i < shared.size(); ++i) copy[i] = shared[i];
        if (!platform::publish_executable(memory_, segment * 4, shared.size_bytes())) {
            platform::free_executable(memory_); words_ = nullptr;
            platform::free_external(index_); index_ = nullptr;
            capacity_ = index_mask_ = 0;
            return false;
        }
    }
    max_records_ = max_records;
    std::fill_n(index_, index_mask_ + 1, 0u);
    count_ = 0;
    clear();
    return true;
}

bool CodeStore::refresh_shared(std::span<const uint32_t> shared) {
    if (!words_) return true;
    if (shared.size_bytes() != shared_bytes_) return false;
    for (size_t segment = 0; segment < capacity_; segment += kSegmentWords) {
        volatile uint32_t* copy = words_ + segment;
        for (size_t i = 0; i < shared.size(); ++i) copy[i] = shared[i];
        if (!platform::publish_executable(memory_, segment * 4, shared.size_bytes())) return false;
    }
    return true;
}

size_t CodeStore::home(BlockKey key) const {
    uint32_t hash = static_cast<uint32_t>(key) ^ std::rotl(static_cast<uint32_t>(key >> 32), 13);
    hash ^= hash >> 16;
    hash *= 0x7feb352d;
    hash ^= hash >> 15;
    return hash & index_mask_;
}

BlockKey CodeStore::key_at(size_t offset) const {
    return words_[offset + 1] | static_cast<BlockKey>(words_[offset + 2]) << 32;
}

bool CodeStore::find(BlockKey key, Record& record) const {
    if (!count_) return false;
    for (size_t at = home(key); index_[at]; at = (at + 1) & index_mask_) {
        const auto offset = index_[at] - 1;
        if (key_at(offset) != key) continue;
        uint32_t* header = words_ + offset;
        const size_t instructions = instructions_of(header[3]), relocations = relocations_of(header[3]);
        const uint32_t* after = header + kHeaderWords + instructions;
        record = {{header + kHeaderWords, instructions},
                  {reinterpret_cast<const uint16_t*>(after), relocations},
                  {reinterpret_cast<const std::byte*>(after + (relocations + 1) / 2), data_of(header[3])},
                  header + 4};
        return true;
    }
    return false;
}

uintptr_t CodeStore::target(const Record& record, size_t relocation) const {
    const size_t offset = record.relocations[relocation];
    const auto site = reinterpret_cast<uintptr_t>(record.code.data()) + offset;
    const auto target = site + static_cast<uintptr_t>(rv32::jal_offset(read_at(record.code.data(), offset)));
    const auto start = reinterpret_cast<uintptr_t>(words_);
    const auto segment = start + (site - start) / kSegmentBytes * kSegmentBytes;
    return target - segment < shared_bytes_ ? origin_ + (target - segment) : target;
}

std::span<uint32_t> CodeStore::store(BlockKey key, std::span<const uint32_t> instructions,
                                     std::span<const Relocation> relocations, std::span<const std::byte> data,
                                     RetireCallback retired, void* context) {
    const size_t size = kHeaderWords + instructions.size() + (relocations.size() + 1) / 2 + (data.size() + 3) / 4;
    if (!words_ || instructions.empty() || instructions.size() > kMaxInstructions ||
        relocations.size() > kMaxRelocations || data.size() > kMaxData ||
        size > std::min(capacity_, kSegmentWords) - shared_words_) return {};
    for (const auto& relocation : relocations)
        if (relocation.offset & 1 || relocation.offset + 4 > instructions.size_bytes()) return {};
    // A record never crosses a segment's end; the rest of that segment stays
    // unused for this lap. Every record fits the first segment, so dropping
    // all others always makes room.
    for (;;) {
        if (count_ == max_records_) { drop(retired, context); continue; }
        const size_t at = fits(head_, size) ? head_ : next_segment(head_);
        if (!wrapped_) {
            if (fits(at, size)) {
                if (at != head_) close(head_);
                head_ = at;
                break;
            }
            close(head_);
            head_ = shared_words_;
            wrapped_ = true;
            continue;
        }
        // Free from head_ to tail_.
        if (at + size <= tail_ && fits(at, size)) {
            if (at != head_) close(head_);
            head_ = at;
            break;
        }
        drop(retired, context);
    }
    volatile uint32_t* header = words_ + head_;
    header[0] = static_cast<uint32_t>(size);
    header[1] = static_cast<uint32_t>(key);
    header[2] = static_cast<uint32_t>(key >> 32);
    header[3] = static_cast<uint32_t>(instructions.size() | relocations.size() << 13 | data.size() << 23);
    header[4] = 0;
    volatile uint32_t* code = header + kHeaderWords;
    for (size_t i = 0; i < instructions.size(); ++i) code[i] = instructions[i];
    const auto base = reinterpret_cast<uintptr_t>(code);
    const auto segment = reinterpret_cast<uintptr_t>(words_ + head_ / kSegmentWords * kSegmentWords);
    volatile uint32_t* offsets = code + instructions.size();
    for (size_t i = 0; i < relocations.size(); ++i) {
        auto target = relocations[i].target;
        if (target - origin_ < shared_bytes_) target = segment + (target - origin_);
        const auto call = rv32::jal(rv32::jal_link(read_at(instructions.data(), relocations[i].offset)),
                                    static_cast<int32_t>(target - (base + relocations[i].offset)));
        if (!call.valid) return {};
        write_at(code, relocations[i].offset, call.bits);
        if (i % 2) offsets[i / 2] = offsets[i / 2] | relocations[i].offset << 16;
        else offsets[i / 2] = relocations[i].offset;
    }
    volatile uint32_t* bytes = offsets + (relocations.size() + 1) / 2;
    for (size_t i = 0; i < data.size(); i += 4) {
        uint32_t word = 0;
        for (size_t k = 0; k < 4 && i + k < data.size(); ++k) word |= std::to_integer<uint32_t>(data[i + k]) << (k * 8);
        bytes[i / 4] = word;
    }
    if (!platform::publish_executable(memory_, (head_ + kHeaderWords) * 4, instructions.size_bytes())) return {};
    size_t at = home(key);
    while (index_[at]) at = (at + 1) & index_mask_;
    index_[at] = static_cast<uint32_t>(head_ + 1);
    const std::span<uint32_t> copy{words_ + head_ + kHeaderWords, instructions.size()};
    head_ += size;
    // A record that ends a segment leaves head_ on the next one's shared code.
    if (head_ % kSegmentWords == 0) head_ += shared_words_;
    used_ += size;
    ++count_;
    return copy;
}

void CodeStore::drop(RetireCallback retired, void* context) {
    const size_t size = words_[tail_], instructions = instructions_of(words_[tail_ + 3]);
    erase(key_at(tail_));
    if (retired) {
        const auto begin = reinterpret_cast<uintptr_t>(words_ + tail_ + kHeaderWords);
        retired(context, begin, begin + instructions * 4);
    }
    used_ -= size;
    ++drops_;
    tail_ += size;
    if (!--count_) { head_ = tail_ = shared_words_; wrapped_ = false; return; }
    // Past a segment's records: the next segment, or the first once the
    // lap ends.
    if (tail_ % kSegmentWords == 0 || tail_ >= capacity_ || !words_[tail_]) {
        tail_ = next_segment(tail_ - 1);
        if (tail_ >= capacity_) { tail_ = shared_words_; wrapped_ = false; }
    }
}

void CodeStore::erase(BlockKey key) {
    for (size_t at = home(key); index_[at]; at = (at + 1) & index_mask_) {
        if (key_at(index_[at] - 1) != key) continue;
        size_t hole = at;
        for (size_t next = (at + 1) & index_mask_; index_[next]; next = (next + 1) & index_mask_) {
            const size_t start = home(key_at(index_[next] - 1));
            // As in CodeCache::erase_index(): move an entry only when its
            // probe path crosses the hole.
            if (((hole - start) & index_mask_) < ((next - start) & index_mask_)) {
                index_[hole] = index_[next];
                hole = next;
            }
        }
        index_[hole] = 0;
        return;
    }
}

void CodeStore::clear() {
    // Every dropped record left the index, so an empty store has an empty index.
    if (count_) std::fill_n(index_, index_mask_ + 1, 0u);
    head_ = tail_ = shared_words_;
    count_ = used_ = 0;
    wrapped_ = false;
}

}  // namespace gbx::jit
