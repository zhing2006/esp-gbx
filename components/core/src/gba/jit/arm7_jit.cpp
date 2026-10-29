// Arm7Jit's code management: the code pool, the second level, compilation,
// linking and retirement, and the single-step slot. The dispatch loop is in
// arm7_jit_runtime.cpp, the gateway in arm7_gateway.cpp and the compiler in
// arm7_compiler*.cpp.

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>

#include "arm7_compiler.h"
#include "gbx/gba/memory.h"

namespace gbx::gba {

#ifdef GBX_PROFILER  // find_block()'s outcomes, for stat.
#define FIND_COUNT(n) ++arm7_find_counts[n]
#else
#define FIND_COUNT(n)
#endif

using namespace arm7_runtime;
using arm7_compiler::Compiler;
using arm7_compiler::MappedTarget;

Arm7Jit::Arm7Jit() = default;

Arm7Jit::~Arm7Jit() {
    if (!memory_) return;
    // The memory outlives the JIT: its marks and callback go with it.
    clear();
    memory_->code_written = nullptr;
    memory_->code_context = nullptr;
}

bool Arm7Jit::initialize(size_t pool_bytes, size_t pool_blocks) {
    return available() || initialize_gateway(pool_bytes, pool_blocks, nullptr);
}

namespace {
template<class T> T* external_array(size_t count) {
    return static_cast<T*>(platform::allocate_external(count * sizeof(T)));
}
}  // namespace

// Generated code reads the tags and the dispatch table on every dynamic
// exit, so they take internal RAM; the tables C++ reads on a miss go to
// external memory.
bool Arm7Jit::initialize(Memory& memory, const Capacity& capacity, const platform::ExecutableMemory* reserved) {
    if (memory_ || available()) return false;
    if (!initialize_gateway(capacity.pool_bytes, capacity.pool_blocks, &memory, reserved)) {
        platform::log(platform::LogLevel::error, "arm7-jit", "No code pool of %u bytes; largest block %u",
                      static_cast<unsigned>(capacity.pool_bytes),
                      static_cast<unsigned>(platform::largest_executable()));
        return false;
    }
    // Without memory for it, the pool alone runs the blocks.
    if (capacity.store_bytes) initialize_store(capacity.store_bytes, capacity.store_records);
    tables_.reset(static_cast<Tables*>(platform::allocate_internal(sizeof(Tables))));
    mid_.reset(external_array<MidEntry>(size_t{1} << kMidBits));
    reinstall_.reset(external_array<Reinstall>(size_t{1} << kReinstallBits));
    link_records_.reset(external_array<LinkRecord>(kLinkRecords));
    ram_blocks_.reset(external_array<RamBlock>(kRamBlocks));
    buckets_.reset(external_array<uint16_t>(kBuckets));
    code_words_.reset(external_array<uint32_t>(kCodeWords / 32));
    if (!tables_ || !mid_ || !reinstall_ || !link_records_ || !ram_blocks_ || !buckets_ || !code_words_) {
        platform::log(platform::LogLevel::error, "arm7-jit", "No memory for the runtime's tables");
        return false;
    }
    tags_ = tables_->tags.data();
    memory_ = &memory;
    memory.code_written = &code_written;
    memory.code_context = this;
    clear();
    return true;
}

bool Arm7Jit::initialize_store(size_t bytes, size_t records) {
    // Each segment of the second level starts with a copy of the gateway.
    if (!store_.initialize(bytes, records, cache_.shared().first(gateway_bytes_ / 4),
                           reinterpret_cast<uintptr_t>(cache_.shared().data())))
        return false;
#ifdef GBX_PROFILER
    // For attributing profiler samples in the copies, see the gateway's line.
    platform::log(platform::LogLevel::warn, "arm7-jit", "store %08lx %u bytes, segments of %u bytes",
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(store_.storage().data())),
                  static_cast<unsigned>(store_.storage().size_bytes()),
                  static_cast<unsigned>(jit::CodeStore::kSegmentBytes));
#endif
    return true;
}

void Arm7Jit::clear() {
    ++clears_;
    cache_.clear();
    if (!memory_) return;
    store_.clear();
    for (auto& entry : tables_->dispatch) entry.r15 = 1;
    boundary_entries_ = {};
    std::fill_n(mid_.get(), size_t{1} << kMidBits, MidEntry{});
    std::fill_n(reinstall_.get(), size_t{1} << kReinstallBits, Reinstall{});
    reset_links();
    for (unsigned i = 0; i < ram_count_; ++i) mark_lines(ram_blocks_[i].begin, ram_blocks_[i].end, -1);
    ram_count_ = 0;
    std::fill_n(buckets_.get(), kBuckets, uint16_t{0});
    std::fill_n(code_words_.get(), kCodeWords / 32, 0u);
    dead_ = false;
    dead_count_ = 0;
    update_tags();
}

// RAM blocks verify nothing on entry: a restored state is new code in RAM.
// Their addresses are gathered in the compiler's scratch space, which is
// idle outside compilation.
void Arm7Jit::forget_ram_code() {
    if (!memory_) return;
    for (unsigned i = 0; i < ram_count_; ++i) ram_blocks_[i].dead = true;
    dead_ = ram_count_ != 0;
    dead_count_ = kDeadBlocks + 1;
    retire_dead();
}

// The tag of a region names the wait states its blocks embed: EWRAM's for
// every block, whose accesses there run inline, and for the ROM regions
// their WAITCNT field and the prefetch's enable.
void Arm7Jit::update_tags() {
    const Memory& m = *memory_;
    waits_generation_ = m.waits_generation;
    const uint16_t waitcnt = m.io16(0x204);
    const auto ewram = static_cast<uint8_t>((m.memory_control() >> 24) & 15);
    tables_->tags.fill(ewram);
    for (unsigned region = 8; region < 14; ++region) {
        const unsigned field = (waitcnt >> (2 + 3 * ((region - 8) / 2))) & 7;
        tags_[region] = static_cast<uint8_t>(ewram | (field | (m.prefetch_enabled ? 8 : 0)) << 4);
    }
    const StubCycles cycles = stub_cycles(m);
    if (cycles == stub_cycles_) return;
    stub_cycles_ = cycles;
    if (!refresh_gateway()) platform::log(platform::LogLevel::error, "arm7-jit", "Cannot regenerate the gateway");
}

Arm7Jit::StubCycles Arm7Jit::stub_cycles(const Memory& m) {
    StubCycles cycles;
    for (unsigned ws = 0; ws < 3; ++ws) {
        const unsigned region = 8 + 2 * ws;
        cycles.s16[ws] = m.cycles_s[region][1];
        cycles.s32[ws] = m.cycles_s[region][2];
        cycles.n16[ws] = m.cycles_n[region][1];
        cycles.n32[ws] = m.cycles_n[region][2];
    }
    cycles.ewram = {m.cycles_n[2][1], m.cycles_n[2][2]};
    return cycles;
}

void Arm7Jit::reset_links() {
    link_free_ = 0;
    for (uint32_t i = kLinkRecords; i; --i) {
        link_records_[i - 1].next = link_free_;
        link_free_ = i;
    }
}

void Arm7Jit::free_link(uint32_t index) {
    link_records_[index - 1].next = link_free_;
    link_free_ = index;
}

// Patches a static exit's link word and records it with the destination,
// whose code is `target`. Without a free record the exit stays unlinked.
void Arm7Jit::link(uint32_t* word, uintptr_t target) {
    if (!link_free_) return;
    auto* header = reinterpret_cast<uint32_t*>(target) - kHeaderWords;
    const auto index = link_free_;
    auto& record = link_records_[index - 1];
    link_free_ = record.next;
    record = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(word)), header[1] & 0xffff};
    header[1] = (header[1] & 0xffff0000u) | index;
    *word = static_cast<uint32_t>(target);
    direct_jump(word, target);
}

// A direct exit (exit_bits::direct) in the pool whose link word is `word`
// jumps to `target` itself, or with 0 calls the link stub again. Copies in
// the second level keep their instructions, and a target beyond a jal's
// reach is left to the stub.
void Arm7Jit::direct_jump(uint32_t* word, uintptr_t target) {
    if (!(word[-1] & exit_bits::direct) || !cache_.owns(word)) return;
    uint32_t* const call = word - 3;
    const auto site = reinterpret_cast<uintptr_t>(call);
    const auto jump = target ? rv::jal(R::zero, static_cast<int32_t>(target - site))
                             : rv::jal(kLink, static_cast<int32_t>(stubs_.link - site));
    if (!jump.valid) return;
    *call = jump.bits;
    cache_.publish_block(call, 4);
}

// A boundary inside a pool block where a run resumed, such as after an
// interrupt, goes into the dispatch table as a block's start does, so that
// the next return there through the table stays in native code. Its record
// replaces the oldest, whose entry leaves the table if still there.
void Arm7Jit::dispatch_boundary(uint32_t r15, jit::BlockKey key, uintptr_t target, unsigned skipped) {
    const auto keyhi = static_cast<uint32_t>(key >> 32);
    const auto index = static_cast<uint16_t>(((r15 >> 1) + keyhi) & (kDispatch - 1));
    const auto code = static_cast<uint32_t>(target);
    auto& entry = tables_->dispatch[index];
    if (entry.code == code && entry.r15 == r15) return;
    auto& record = boundary_entries_[boundary_next_++ % kBoundaryEntries];
    if (auto& oldest = tables_->dispatch[record.index]; record.code && oldest.code == record.code) oldest.r15 = 1;
    entry = {r15, keyhi | skipped << 24, code};
    record = {code, index};
}

// The guest code of RAM blocks counts in Memory's marks of 64-byte lines;
// a count that reached 255 stays.
void Arm7Jit::mark_lines(uint32_t begin, uint32_t end, int delta) {
    Memory& m = *memory_;
    uint8_t* marks = (begin >> 24) == 3 ? m.iwram_code.data() : m.ewram_code;
    const uint32_t mask = (begin >> 24) == 3 ? 0x7fff : 0x3ffff;
    for (uint32_t line = (begin & mask) >> Memory::kCodeLineBits; line <= ((end - 1) & mask) >> Memory::kCodeLineBits;
         ++line) {
        uint8_t& mark = marks[line];
        if (delta > 0 && mark < 255) ++mark;
        else if (delta < 0 && mark && mark < 255) --mark;
    }
}

// A RAM block's record, at the first mirror, where code_written() looks; a
// copy in the second level has its size. Without a free record the oldest
// RAM block goes.
void Arm7Jit::watch(uintptr_t block, uint32_t first, uint32_t bytes, uint32_t copy_bytes) {
    if (ram_count_ == kRamBlocks && retire_ram(0)) ++evictions_;
    if (ram_count_ == kRamBlocks) return;
    const uint32_t ram_mask = (first >> 24) == 3 ? 0x7fff : 0x3ffff;
    const uint32_t begin = (first & 0xff000000) | (first & ram_mask), last = begin + bytes;
    const unsigned index = ram_count_++;
    auto& head = buckets_[bucket(begin)];
    ram_blocks_[index] = {block, begin, last, copy_bytes, 0, head, false};
    if (head) ram_blocks_[head - 1].previous = static_cast<uint16_t>(index + 1);
    head = static_cast<uint16_t>(index + 1);
    mark_lines(begin, last, 1);
    for (uint32_t word = begin & ~3u; word < last; word += 4) {
        const unsigned bit = code_word(word);
        code_words_[bit >> 5] |= 1u << (bit & 31);
    }
}

// Whether a block may move from the second level into the pool: a few a
// frame, so that the pool's CLOCK hand moves slowly enough for the marks of
// the blocks that run often to keep them.
bool Arm7Jit::may_promote() {
    const auto frame = static_cast<uint32_t>(memory_->frames);
    if (frame != promotion_frame_) {
        promotion_frame_ = frame;
        promotions_ = 0;
    }
    return promotions_ < kPromotionsPerFrame;
}

// Takes record `index` out of its bucket and moves the last record into its
// place.
void Arm7Jit::remove_ram(unsigned index) {
    const auto unlink = [&](unsigned i) {
        const auto& ram = ram_blocks_[i];
        if (ram.previous) ram_blocks_[ram.previous - 1].next = ram.next;
        else buckets_[bucket(ram.begin)] = ram.next;
        if (ram.next) ram_blocks_[ram.next - 1].previous = ram.previous;
    };
    unlink(index);
    const unsigned last = --ram_count_;
    if (index == last) return;
    auto& moved = ram_blocks_[index];
    moved = ram_blocks_[last];
    const auto number = static_cast<uint16_t>(index + 1);
    if (moved.previous) ram_blocks_[moved.previous - 1].next = number;
    else buckets_[bucket(moved.begin)] = number;
    if (moved.next) ram_blocks_[moved.next - 1].previous = number;
}

// Stops watching the copy of a RAM block in the second level at `block`,
// whose code begins at `first`, as retiring it does.
void Arm7Jit::retire_copy(uintptr_t block, uint32_t first, uint32_t copy_bytes) {
    bool found = false;
    each_ram(first, [&](unsigned i) { return found = ram_blocks_[i].block == block; });
    if (found) retired(this, block, block + copy_bytes);
}

// Retires the RAM block at `index`, whose record retired() replaces with the
// last one; false when its code was gone already, with the record removed.
bool Arm7Jit::retire_ram(unsigned index) {
    const auto& ram = ram_blocks_[index];
    if (ram.copy_bytes) {
        retired(this, ram.block, ram.block + ram.copy_bytes);
        return true;
    }
    uintptr_t begin, end;
    if (cache_.erase_containing(ram.block, begin, end)) {
        retired(this, begin, end);
        return true;
    }
    remove_ram(index);
    return false;
}

// A retired block's links: those into it return to C++, those out of it
// leave their destinations' lists. Its dispatch entry and, in RAM, its
// record and marks go too.
void Arm7Jit::retired(void* self, uintptr_t begin, uintptr_t end) {
    auto& jit = *static_cast<Arm7Jit*>(self);
    auto* block = reinterpret_cast<uint32_t*>(begin);
    for (auto index = block[1] & 0xffff; index;) {
        const auto& record = jit.link_records_[index - 1];
        auto* const word = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(record.word));
        *word = 0;
        jit.direct_jump(word, 0);
        const auto next = record.next;
        jit.free_link(index);
        index = next;
    }
    block[1] &= 0xffff0000u;
    const size_t size = (end - begin) / 4, count = block[size - 1];
    for (size_t i = 0; i < count; ++i) {
        uint32_t* word = block + block[size - 1 - count + i];
        if (!*word) continue;
        const uintptr_t destination = *word;
        // A copy in the second level may run again, unlinked.
        *word = 0;
        auto* header = reinterpret_cast<uint32_t*>(destination) - kHeaderWords;
        const auto address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(word));
        const auto head = header[1] & 0xffff;
        if (jit.link_records_[head - 1].word == address) {
            header[1] = (header[1] & 0xffff0000u) | jit.link_records_[head - 1].next;
            jit.free_link(head);
            continue;
        }
        for (auto previous = head; previous;) {
            const auto next = jit.link_records_[previous - 1].next;
            if (next && jit.link_records_[next - 1].word == address) {
                jit.link_records_[previous - 1].next = jit.link_records_[next - 1].next;
                jit.free_link(next);
                break;
            }
            previous = next;
        }
    }
    const uint32_t r15 = block[0], keyhi = block[1] >> 16;
    auto& entry = jit.tables_->dispatch[((r15 >> 1) + keyhi) & (kDispatch - 1)];
    if (entry.code == begin + kHeaderWords * 4) entry.r15 = 1;
    for (auto& boundary : jit.boundary_entries_) {
        if (boundary.code < begin || boundary.code >= end) continue;
        auto& inside = jit.tables_->dispatch[boundary.index];
        if (inside.code == boundary.code) inside.r15 = 1;
        boundary = {};
    }
    const unsigned region = r15 >> 24;
    if (region != 2 && region != 3) return;
    // The code begins 8 bytes before R15 in ARM state, 4 in Thumb state.
    unsigned found = kRamBlocks;
    for (const uint32_t first : {r15 - 4, r15 - 8}) {
        jit.each_ram(first, [&](unsigned i) {
            if (jit.ram_blocks_[i].block != begin) return false;
            found = i;
            return true;
        });
        if (found != kRamBlocks) break;
    }
    if (found == kRamBlocks) return;
    const auto& ram = jit.ram_blocks_[found];
    // A copy in the second level is no longer watched.
    if (ram.copy_bytes) block[kHeaderWords - 1] = 0;
    jit.mark_lines(ram.begin, ram.end, -1);
    jit.remove_ram(found);
}

size_t Arm7Jit::compile_mapped(const Arm7State& state, unsigned limit) {
    static_assert(sizeof(Compiler) <= sizeof(state_) && alignof(Compiler) <= 8);
    Memory& m = *memory_;
    const uint32_t r15 = state.r[15];
    const unsigned region = r15 >> 24;
    MappedTarget target;
    target.region = static_cast<uint8_t>(region);
    target.ewram_cycles = m.cycles_n[2];
    target.code_n = m.cycles_n[region & 15];
    target.code_s = m.cycles_s[region & 15];
    target.halfword_s = m.cycles_s[region & 15][1];
    target.prefetch = m.prefetch_enabled;
    target.self_loops = !observer_ || observe_linked_;
#ifndef GBX_PROFILER
    // Only the profiler's counts and an observer read them.
    target.count_instructions = observer_ != nullptr;
#endif
    target.registers = state.r.data();
    target.iwram = m.iwram.data();
    target.ewram = m.ewram;
    target.iwram_code = m.iwram_code.data();
    target.ewram_code = m.ewram_code;
    target.vram = m.vram;
    target.dirty = m.dirty.data();
    target.gamepak_next = &m.gamepak_next_;
    const Arm7CodeSource code{&m, [](const void* context, uint32_t address, bool thumb) {
        return const_cast<Memory*>(static_cast<const Memory*>(context))->peek(address, thumb ? 1 : 2);
    }};
    for (;;) {
        auto* compiler = new (state_.data()) Compiler(hot_, cold_, stubs_, code, r15, state.cpsr, &target);
        size_t count = 0;
        const size_t words = compiler->compile(limit, output_, relocations_, count).size();
        if (words) {
            relocation_count_ = count;
            compiled_.instructions = compiler->instructions();
            compiled_.end = compiler->code_end();
            const auto boundaries = compiler->boundaries();
            compiled_.boundaries = static_cast<unsigned>(boundaries.size());
            for (size_t i = 0; i < boundaries.size(); ++i)
                compiled_.boundary[i] = boundaries[i].offset | uint32_t{boundaries[i].completed} << 16 |
                                        uint32_t{boundaries[i].sequential} << 24;
        }
        compiler->~Compiler();
        if (words) return words;
        if (limit == 1) return 0;
        limit = (limit + 1) / 2;
    }
}

// A RAM block's copy in the second level: its data holds the boundaries'
// count, the boundaries, the instruction count, the code's size in bytes
// and the code it was compiled from, see find_block().
bool Arm7Jit::same_code(const jit::CodeStore::Record& record, uint32_t first) const {
    const auto* data = reinterpret_cast<const uint32_t*>(record.data.data());
    const size_t words = record.data.size() / 4;
    if (!words || words < 3 + data[0]) return false;
    const uint32_t count = data[0], bytes = data[2 + count];
    if ((3 + count) * 4 + bytes > record.data.size()) return false;
    const uint8_t* now = (first >> 24) == 3 ? memory_->iwram.data() + (first & 0x7fff) : memory_->ewram + (first & 0x3ffff);
    return std::memcmp(data + 3 + count, now, bytes) == 0;
}

// A miss in the pool: an inner boundary of a live block, the block's copy
// in the second level, or a new block. BIOS and ROM blocks are fixed code:
// only they run in the second level and go to the reinstall counts; RAM
// blocks come back from there only while their code is the same, see
// kVersions.
Arm7Jit::Found Arm7Jit::find_block(const Arm7State& state, jit::BlockKey key, bool resume) {
    Found found;
    const uint32_t r15 = state.r[15];
    const unsigned region = r15 >> 24;
    const bool fixed = region == 0 || region >= 8;
    const uint32_t keyhi = static_cast<uint32_t>(key >> 32);
    const unsigned length = state.cpsr & kFlagT ? 2 : 4;
    const uint32_t ram_mask = region == 3 ? 0x7fff : 0x3ffff;
    // A RAM block's first instruction.
    const uint32_t first = (r15 & ~ram_mask) | ((r15 - 2 * length) & ram_mask);
    const MidEntry* mid = nullptr;
    if (resume) {
        const auto& candidate = mid_[mid_slot(key)];
        if (candidate.key == key && candidate.block) {
            // Only the gamepak's fetches differ between N and S: in the BIOS,
            // EWRAM and IWRAM the boundary's code fits either.
            if (candidate.sequential != state.sequential && region >= 8) {
                found.interpret = 1;
                FIND_COUNT(0);
                return found;
            }
            mid = &candidate;
        }
    }
    const auto resume_at = [&](uintptr_t block) {
        found.target = block + mid->offset;
        found.skipped = mid->completed;
        FIND_COUNT(1);
        return found;
    };
    if (mid && *mid->live == mid->installation) {
        FIND_COUNT(2);
        // The dispatch table's entries fetch next with S, as the boundary's
        // code may only expect in the gamepak.
        found.dispatchable = region < 8 || mid->sequential;
        return resume_at(mid->block);
    }
    jit::CodeStore::Record copy{}, owner{};
    bool promote = false;
    if (fixed && store_.find(key, copy)) {
        // The pool evicted the block. It runs in the second level until its
        // entries there run out (Stubs::count_entry), then moves into the
        // pool while this frame's promotions last.
        auto& count = copy.code[kHeaderWords - 1];
        if (static_cast<int32_t>(count) > 1 || !may_promote()) {
            if (static_cast<int32_t>(count) <= 1) count = kStoredEntries << *copy.tag;
            found.target = reinterpret_cast<uintptr_t>(copy.code.data()) + kHeaderWords * 4;
            found.start = true;
            ++stored_entries_;
            FIND_COUNT(3);
            return found;
        }
        promote = true;
    } else if (mid && fixed && store_.find(mid->owner, owner)) {
        // The same boundary in the evicted block's copy.
        FIND_COUNT(4);
        return resume_at(reinterpret_cast<uintptr_t>(owner.code.data()));
    } else if (mid && fixed) {
        // The block left both levels. A block starting here would cover the
        // rest of its code a second time; finish it on the interpreter.
        found.interpret = mid->remaining;
        FIND_COUNT(5);
        return found;
    }
    bool version = false;
    unsigned free_slot = kVersions;
    if (!fixed) {
        for (unsigned slot = 0; slot < kVersions; ++slot) {
            jit::CodeStore::Record record{};
            if (!store_.find(version_key(key, slot), record)) {
                if (free_slot == kVersions) free_slot = slot;
            } else if (auto& count = record.code[kHeaderWords - 1]; count || same_code(record, first)) {
                // The copy runs where it is, watched as a pool block is
                // while its count is not 0, until its entries run out as a
                // BIOS or ROM block's do (count 1).
                const auto copy_bytes = static_cast<uint32_t>(record.code.size() * 4);
                if (count && static_cast<int32_t>(count) <= 1 && may_promote()) {
                    retire_copy(reinterpret_cast<uintptr_t>(record.code.data()), first, copy_bytes);
                    copy = record;
                    promote = version = true;
                    break;
                }
                if (!count) {
                    const auto* data = reinterpret_cast<const uint32_t*>(record.data.data());
                    watch(reinterpret_cast<uintptr_t>(record.code.data()), first, data[2 + data[0]], copy_bytes);
                }
                if (static_cast<int32_t>(count) <= 1) count = kStoredEntries << *record.tag;
                found.target = reinterpret_cast<uintptr_t>(record.code.data()) + kHeaderWords * 4;
                found.start = true;
                ++stored_entries_;
                FIND_COUNT(6);
                return found;
            }
        }
    }
    Reinstall* reinstall = nullptr;
    uint16_t reinstall_tag = 0;
    if (!promote && fixed) {
        const auto hash = reinstall_hash(key);
        reinstall = &reinstall_[hash >> (32 - kReinstallBits)];
        reinstall_tag = static_cast<uint16_t>(hash);
        auto& r = *reinstall;
        if (r.count && r.tag == reinstall_tag) {
            if (r.misses < (1u << r.level) - 1) {
                ++r.misses;
                found.interpret = r.count;
                FIND_COUNT(7);
                return found;
            }
            r.misses = 0;
            if (r.level < kMaxLevel) ++r.level;
        }
    }
    std::span<const uint32_t> words;
    std::span<const jit::Relocation> relocations;
    if (promote) {
        // No compilation, only relocation, and the CLOCK mark back in place
        // of the call to count_entry.
        std::copy(copy.code.begin(), copy.code.end(), output_.begin());
        output_[kHeaderWords] = rv::auipc(R::ra, 0).bits;
        words = std::span(output_).first(copy.code.size());
        // The copy's links have records for the copy's words only: the
        // block starts unlinked, see the link table at its end.
        const size_t size = words.size(), links = output_[size - 1];
        for (size_t i = 0; i < links; ++i) output_[output_[size - 1 - links + i]] = 0;
        relocation_count_ = 0;
        for (size_t i = 0; i < copy.relocations.size(); ++i) {
            const auto call = store_.target(copy, i);
            if (call != stubs_.count_entry) relocations_[relocation_count_++] = {copy.relocations[i], call};
        }
        ++promotions_;
        // The next eviction gets a count twice as long, so that a block
        // the pool keeps evicting moves less often.
        *copy.tag = static_cast<uint8_t>(std::min<uint32_t>(*copy.tag + 1, kMaxStoredLevel));
        if (version) {
            const auto* data = reinterpret_cast<const uint32_t*>(copy.data.data());
            const uint32_t count = data[0];
            std::memcpy(compiled_.boundary.data(), data + 1, count * 4);
            compiled_.boundaries = count;
            compiled_.instructions = data[1 + count];
            compiled_.end = r15 - 2 * length + data[2 + count];
        } else {
            const size_t count = copy.data.size() / 4 - 1;
            std::memcpy(compiled_.boundary.data(), copy.data.data(), copy.data.size());
            compiled_.boundaries = static_cast<unsigned>(count);
            compiled_.instructions = compiled_.boundary[count];
            copy.code[kHeaderWords - 1] = kStoredEntries << *copy.tag;
        }
    } else {
#ifdef GBX_PROFILER
        // Why blocks are compiled: after a stop, and inside a RAM block.
#endif
#ifdef GBX_PROFILER
        const uint32_t compile_start = CycleSpan::now();
#endif
        const size_t size = compile_mapped(state, kMaxBlock);
#ifdef GBX_PROFILER
        arm7_cycle_counts[20] += CycleSpan::now() - compile_start;
#endif
        if (!size) {
            found.failure = Arm7JitFallback::encoding;
            FIND_COUNT(8);
            return found;
        }
        words = std::span(output_).first(size);
    }
    relocations = std::span(relocations_).first(relocation_count_);
#ifdef GBX_PROFILER
    const uint32_t install_start = CycleSpan::now();
#endif
    const auto result = cache_.install_replacing(key, words, relocations, &retired, this);
#ifdef GBX_PROFILER
    arm7_cycle_counts[21] += CycleSpan::now() - install_start;
#endif
    evictions_ += result.evicted;
#ifdef GBX_PROFILER
    if (!fixed) compile_reasons_[1] += result.evicted;
#endif
    if (!result.entry) {
        if (promote && !version) {
            // The pool did not take it: run the copy where it is.
            found.target = reinterpret_cast<uintptr_t>(copy.code.data()) + kHeaderWords * 4;
            found.start = true;
            ++stored_entries_;
            FIND_COUNT(9);
            return found;
        }
        found.failure = result.status == jit::InstallStatus::sync_failed ? Arm7JitFallback::publication
                                                                         : Arm7JitFallback::capacity;
        FIND_COUNT(10);
        return found;
    }
    auto* block = reinterpret_cast<uint32_t*>(result.entry);
    const auto start = reinterpret_cast<uintptr_t>(block);
    found.target = start + kHeaderWords * 4;
    found.start = true;
    if (promote) ++restored_blocks_;
    else {
        ++compiled_blocks_;
        compiled_bytes_ += words.size() * 4;
        compiled_instructions_ += compiled_.instructions;
    }
    // Inline data, never executed: no republication.
    block[0] = r15;
    block[1] = keyhi << 16;
    block[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(result.referenced));
    // A repeated installation number could match an old mid-block entry.
    if (result.installation < installation_) std::fill_n(mid_.get(), size_t{1} << kMidBits, MidEntry{});
    installation_ = result.installation;
    for (unsigned i = 0; i < compiled_.boundaries; ++i) {
        const uint32_t boundary = compiled_.boundary[i], completed = (boundary >> 16) & 0xff;
        const jit::BlockKey mid_key = Arm7Jit::key(r15 + completed * length, keyhi);
        mid_[mid_slot(mid_key)] = {mid_key,
                                   key,
                                   start,
                                   result.live,
                                   result.installation,
                                   static_cast<uint16_t>(boundary),
                                   static_cast<uint8_t>(completed),
                                   static_cast<uint8_t>(boundary >> 24),
                                   static_cast<uint8_t>(compiled_.instructions - completed)};
    }
    if (promote && !version) {
        // Links into the copy in the second level now enter this one; their
        // records move along.
        auto* second = copy.code.data();
        for (auto index = second[1] & 0xffff; index; index = link_records_[index - 1].next) {
            auto* const word = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(link_records_[index - 1].word));
            *word = static_cast<uint32_t>(found.target);
            direct_jump(word, found.target);
        }
        block[1] |= second[1] & 0xffff;
        second[1] &= 0xffff0000u;
        FIND_COUNT(11);
        return found;
    }
    if (fixed) {
        // Only blocks that the pool accepted are stored. The copy enters
        // through count_entry instead of the CLOCK mark, see Stubs.
        const unsigned count = compiled_.boundaries;
        compiled_.boundary[count] = compiled_.instructions;
        if (relocation_count_ < relocations_.size()) {
            relocations_[relocation_count_] = {kHeaderWords * 4, stubs_.count_entry};
            output_[kHeaderWords] = rv::jal(kLink, 0).bits;
            GBX_CYCLE_SPAN(22);
            const auto second = store_.store(key, words, std::span(relocations_).first(relocation_count_ + 1),
                                             std::as_bytes(std::span(compiled_.boundary).first(count + 1)), &retired,
                                             this);
            if (!second.empty()) {
                second[0] = r15;
                second[1] = keyhi << 16;
                second[kHeaderWords - 1] = kStoredEntries;
            }
        }
        if (reinstall) {
            auto& r = *reinstall;
            if (!r.count || r.tag != reinstall_tag) r = {reinstall_tag, 0, 0, 0};
            r.count = static_cast<uint8_t>(compiled_.instructions);
        }
        FIND_COUNT(10);
        return found;
    }
    // A RAM block: its code lines are watched for writes.
    const uint32_t bytes = compiled_.end - (r15 - 2 * length);
    if (!version && free_slot < kVersions && (first & ram_mask) + bytes <= ram_mask + 1) {
        // Its copy, with the code it was compiled from.
        std::array<uint32_t, 3 + kMaxBlock + 1 + kMaxBlock> data{};
        const uint32_t count = compiled_.boundaries;
        data[0] = count;
        std::copy_n(compiled_.boundary.begin(), count, data.begin() + 1);
        data[1 + count] = compiled_.instructions;
        data[2 + count] = bytes;
        const uint8_t* code = region == 3 ? memory_->iwram.data() + (first & ram_mask) : memory_->ewram + (first & ram_mask);
        std::memcpy(data.data() + 3 + count, code, bytes);
        // It enters through count_entry, as a BIOS or ROM block's copy
        // does, with a count of 0 while it is not watched.
        if (relocation_count_ < relocations_.size()) {
            relocations_[relocation_count_] = {kHeaderWords * 4, stubs_.count_entry};
            output_[kHeaderWords] = rv::jal(kLink, 0).bits;
            const auto second = store_.store(version_key(key, free_slot), words,
                                             std::span(relocations_).first(relocation_count_ + 1),
                                             std::as_bytes(std::span(data).first(3 + count + (bytes + 3) / 4)),
                                             &retired, this);
            if (!second.empty()) {
                second[0] = r15;
                second[1] = keyhi << 16;
                second[kHeaderWords - 1] = 0;
            }
        }
    }
    watch(start, first, bytes, 0);
    FIND_COUNT(10);
    return found;
}

// The compiler lives in state_ rather than on the owner task's stack. A block
// too long for its streams or branches is compiled shorter; an instruction
// that fails alone is reported.
size_t Arm7Jit::compile(const Arm7State& state, Arm7CodeSource code, unsigned limit, Arm7JitFallback& failure) {
    static_assert(sizeof(Compiler) <= sizeof(state_) && alignof(Compiler) <= 8);
    for (;;) {
        auto* compiler = new (state_.data()) Compiler(hot_, cold_, stubs_, code, state.r[15], state.cpsr);
        size_t count = 0;
        const size_t words = compiler->compile(limit, output_, relocations_, count).size();
        compiler->~Compiler();
        if (words) {
            relocation_count_ = count;
            return words;
        }
        if (limit == 1) {
            failure = Arm7JitFallback::encoding;
            return 0;
        }
        limit = (limit + 1) / 2;
    }
}

Arm7JitResult Arm7Jit::fallback(Arm7State& state, const Bus& bus, Arm7JitFallback reason) {
    ++fallbacks_[static_cast<unsigned>(reason)];
    arm7_step(state, bus);
    return {1, Arm7JitExit::fallback, reason};
}

Arm7JitFallback Arm7Jit::compile_single(const Arm7State& state, Arm7CodeSource code) {
    single_words_ = 0;
    if (!available()) return single_failure_ = Arm7JitFallback::unavailable;
    single_failure_ = Arm7JitFallback::none;
    single_words_ = compile(state, code, 1, single_failure_);
    if (single_words_ * 4 > kSingleBytes) single_failure_ = Arm7JitFallback::capacity;
    return single_failure_;
}

// The slot follows the gateway, so that its calls need no stub of their own.
Arm7JitFallback Arm7Jit::publish_single() {
    if (single_failure_ != Arm7JitFallback::none) return single_failure_;
    uint32_t* slot = cache_.shared().data() + gateway_bytes_ / 4;
    const auto address = reinterpret_cast<uintptr_t>(slot);
    for (size_t i = 0; i < relocation_count_; ++i) {
        const auto& relocation = relocations_[i];
        const auto instruction = rv::jal(rv::jal_link(jit::read_at(output_.data(), relocation.offset)),
                                         static_cast<int32_t>(static_cast<int64_t>(relocation.target) -
                                                              static_cast<int64_t>(address + relocation.offset)));
        if (!instruction.valid) return single_failure_ = Arm7JitFallback::capacity;
        jit::write_at(output_.data(), relocation.offset, instruction.bits);
    }
    volatile uint32_t* destination = slot;
    for (size_t i = 0; i < single_words_; ++i) destination[i] = output_[i];
    if (!cache_.publish_shared(single_words_ * 4, gateway_bytes_)) single_failure_ = Arm7JitFallback::publication;
    return single_failure_;
}

Arm7JitResult Arm7Jit::execute_single(Arm7State& state, const Bus& bus) {
    if (single_failure_ != Arm7JitFallback::none) return fallback(state, bus, single_failure_);
    arm7_runtime::Context context{&state, &bus, INT32_MAX, 0};
    const auto entry = reinterpret_cast<uintptr_t>(cache_.shared().data()) + gateway_bytes_;
    const auto exit = static_cast<Arm7JitExit>(enter(&context, entry));
    return {context.instructions, exit, Arm7JitFallback::none};
}

}  // namespace gbx::gba
