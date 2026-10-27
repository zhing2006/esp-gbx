#include "gbx/gba/rom_cache.h"

#include <algorithm>
#include <cstring>
#include <new>

#include "gbx/platform.h"

namespace gbx::gba {

RomCache::RomCache() = default;

RomCache::~RomCache() {
    close();
    if (reader_started_) {
        stop_.store(true, std::memory_order_seq_cst);
        request_.signal();
        reader_.join();
    }
    platform::free_internal(table_);
    platform::free_external(patterns_);
}

void RomCache::fill_pattern(uint8_t* data, uint32_t offset, size_t from) const {
    for (size_t i = from & ~size_t{1}; i < kPageBytes; i += 2) {
        const auto value = static_cast<uint16_t>((offset + i) >> 1);
        if (i >= from) data[i] = static_cast<uint8_t>(value);
        data[i + 1] = static_cast<uint8_t>(value >> 8);
    }
}

void RomCache::release_frames() {
    platform::free_external(storage_);
    storage_ = nullptr;
    delete[] frames_;
    frames_ = nullptr;
    frame_count_ = 0;
}

bool RomCache::allocate_frames(size_t count) {
    release_frames();
    storage_ = static_cast<uint8_t*>(platform::allocate_external(count * kPageBytes));
    frames_ = new (std::nothrow) Frame[count];
    if (!storage_ || !frames_) {
        release_frames();
        return false;
    }
    for (size_t i = 0; i < count; ++i) frames_[i] = {storage_ + i * kPageBytes, -1};
    frame_count_ = count;
    hand_ = 0;
    return true;
}

void RomCache::set_overlay(const uint8_t* data) {
    if (data == overlay_) return;
    overlay_ = data;
    if (data) {
        table_[0] = data;
    } else if (!attached_.empty()) {
        // A borrowed ROM's page 0, or the frame of a partial one.
        table_[0] = rom_pages_ == 1 && frame_count_ ? frames_[0].data : attached_.data();
    } else {
        table_[0] = resident_[0] >= 0 ? frames_[resident_[0]].data : nullptr;
    }
}

void RomCache::close() {
    overlay_ = nullptr;
    wait_reader();
    file_.close();
    release_frames();
    delete[] resident_;
    resident_ = nullptr;
    if (table_) std::fill_n(table_, kPages, nullptr);
    size_ = 0;
    rom_pages_ = 0;
    paged_ = false;
    attached_ = {};
    stats_ = {};
}

// The table and the pattern pages, before either kind of ROM is set up.
static bool setup_common(const uint8_t**& table, uint8_t*& patterns) {
    if (!table) table = static_cast<const uint8_t**>(platform::allocate_internal(RomCache::kPages * sizeof(void*)));
    if (!patterns) patterns = static_cast<uint8_t*>(
        platform::allocate_external(RomCache::kPatternPages * RomCache::kPageBytes));
    return table && patterns;
}

bool RomCache::attach(std::span<const uint8_t> rom) {
    close();
    if (rom.empty() || rom.size() > kMaxBytes || !setup_common(table_, patterns_)) return false;
    for (unsigned j = 0; j < kPatternPages; ++j)
        fill_pattern(patterns_ + j * kPageBytes, static_cast<uint32_t>(j * kPageBytes), 0);
    size_ = rom.size();
    rom_pages_ = static_cast<unsigned>((rom.size() + kPageBytes - 1) >> kPageBits);
    // A partial last page needs the pattern after the ROM's bytes, which a
    // borrowed buffer cannot hold: that page gets a frame of its own.
    const size_t tail = rom.size() & (kPageBytes - 1);
    if (tail && !allocate_frames(1)) return false;
    attached_ = rom;
    scanner_ = {};
    scanner_.feed(rom);
    for (unsigned page = 0; page < kPages; ++page) {
        if (page >= rom_pages_) table_[page] = patterns_ + (page % kPatternPages) * kPageBytes;
        else if (tail && page == rom_pages_ - 1) {
            std::memcpy(frames_[0].data, rom.data() + (size_t{page} << kPageBits), tail);
            fill_pattern(frames_[0].data, page << kPageBits, tail);
            frames_[0].page = static_cast<int32_t>(page);
            table_[page] = frames_[0].data;
        } else {
            table_[page] = rom.data() + (size_t{page} << kPageBits);
        }
    }
    return true;
}

bool RomCache::open(const char* path, size_t budget, Observe observe, void* context) {
    close();
    if (!setup_common(table_, patterns_) || !file_.open(path)) return false;
    const int64_t size = file_.size();
    if (size <= 0 || size > static_cast<int64_t>(kMaxBytes)) {
        file_.close();
        return false;
    }
    for (unsigned j = 0; j < kPatternPages; ++j)
        fill_pattern(patterns_ + j * kPageBytes, static_cast<uint32_t>(j * kPageBytes), 0);
    size_ = static_cast<size_t>(size);
    rom_pages_ = static_cast<unsigned>((size_ + kPageBytes - 1) >> kPageBits);
    budget_frames_ = std::max(kMinFrames, std::min<size_t>(budget / kPageBytes, rom_pages_));
    resident_ = new (std::nothrow) int16_t[kPages];
    if (!resident_ || !allocate_frames(budget_frames_)) {
        close();
        return false;
    }
    std::fill_n(resident_, kPages, int16_t{-1});
    for (unsigned page = 0; page < kPages; ++page)
        table_[page] = page >= rom_pages_ ? patterns_ + (page % kPatternPages) * kPageBytes : nullptr;
    paged_ = frame_count_ < rom_pages_;
    scanner_ = {};
    // One pass over the file: the first pages stay, the rest only pass the
    // observer, a whole page at a time into a frame that the next page
    // reuses once the frames are full.
    for (unsigned page = 0; page < rom_pages_; ++page) {
        const bool keep = page < frame_count_;
        uint8_t* data = frames_[keep ? page : frame_count_ - 1].data;
        if (!keep && frames_[frame_count_ - 1].page >= 0) {
            resident_[frames_[frame_count_ - 1].page] = -1;
            table_[frames_[frame_count_ - 1].page] = nullptr;
            frames_[frame_count_ - 1].page = -1;
        }
        if (!read_page(page, data)) {
            close();
            return false;
        }
        const size_t bytes = std::min(kPageBytes, size_ - (size_t{page} << kPageBits));
        scanner_.feed({data, bytes});
        if (observe) observe(context, {data, bytes});
        if (keep) {
            frames_[page].page = static_cast<int32_t>(page);
            resident_[page] = static_cast<int16_t>(page);
            table_[page] = data;
        }
    }
    if (paged_ && !reader_started_) {
        reader_started_ = request_.initialize(1) && done_.initialize(1) &&
                          reader_.start(reader_entry, this, platform::current_core() ^ 1, "gbx-rom", 4096, 4);
    }
    return true;
}

bool RomCache::set_frames(size_t frames) {
    if (!attached_.empty() || !file_.is_open()) return false;
    wait_reader();
    const size_t count = frames ? std::max(frames, kMinFrames) : budget_frames_;
    for (unsigned page = 0; page < rom_pages_; ++page) {
        table_[page] = nullptr;
        resident_[page] = -1;
    }
    if (overlay_) table_[0] = overlay_;
    if (!allocate_frames(count)) return false;
    paged_ = frame_count_ < rom_pages_;
    if (paged_ && !reader_started_) {
        reader_started_ = request_.initialize(1) && done_.initialize(1) &&
                          reader_.start(reader_entry, this, platform::current_core() ^ 1, "gbx-rom", 4096, 4);
    }
    return true;
}

// Reads a page; the last one gets the pattern after the ROM's end.
bool RomCache::read_page(unsigned page, uint8_t* data) {
    const size_t offset = size_t{page} << kPageBits;
    const size_t bytes = std::min(kPageBytes, size_ - offset);
    if (!file_.read(offset, {data, bytes})) return false;
    if (bytes < kPageBytes) fill_pattern(data, static_cast<uint32_t>(offset), bytes);
    return true;
}

unsigned RomCache::victim() {
    for (;;) {
        const unsigned f = hand_;
        hand_ = static_cast<unsigned>((hand_ + 1) % frame_count_);
        auto& frame = frames_[f];
        // The frame the reader fills is not free yet.
        if (f == ahead_frame_ && ahead_page_.load(std::memory_order_relaxed) >= 0) continue;
        if (frame.page < 0) return f;
        // Page 0 under an overlay is not read through its frame.
        if (table_[frame.page] && !(frame.page == 0 && overlay_)) {
            table_[frame.page] = nullptr;  // A second chance.
            continue;
        }
        resident_[frame.page] = -1;
        frame.page = -1;
        return f;
    }
}

void RomCache::wait_reader() {
    const int32_t page = ahead_page_.load(std::memory_order_relaxed);
    if (page < 0) return;
    done_.wait();
    auto& frame = frames_[ahead_frame_];
    if (ahead_ok_.load(std::memory_order_acquire)) {
        frame.page = page;
        resident_[page] = static_cast<int16_t>(ahead_frame_);
        table_[page] = frame.data;
        ++stats_.ahead;
    } else {
        ++stats_.read_errors;
    }
    ahead_page_.store(-1, std::memory_order_relaxed);
}

const uint8_t* RomCache::miss(unsigned page) {
    if (resident_ && resident_[page] >= 0) {
        // Resident but unmapped by the hand: touched again.
        ++stats_.remaps;
        return table_[page] = frames_[resident_[page]].data;
    }
    if (!resident_ || page >= rom_pages_) return table_[page];  // Never empty there.
    const auto start = platform::monotonic_us();
    // The reader may be bringing this very page, and it holds the file.
    wait_reader();
    if (resident_[page] < 0) {
        const unsigned f = victim();
        if (!read_page(page, frames_[f].data)) {
            // A card error mid-game: the pattern stands in, as no data came.
            ++stats_.read_errors;
            fill_pattern(frames_[f].data, page << kPageBits, 0);
        }
        frames_[f].page = static_cast<int32_t>(page);
        resident_[page] = static_cast<int16_t>(f);
        ++stats_.misses;
        // Read the next page ahead, unless it is here or there is no frame
        // to spare beside the one just filled.
        const unsigned next = page + 1;
        if (reader_started_ && next < rom_pages_ && resident_[next] < 0 && frame_count_ > 2) {
            table_[page] = frames_[f].data;  // Keep the hand off it.
            ahead_frame_ = victim();
            ahead_ok_.store(false, std::memory_order_relaxed);
            ahead_page_.store(static_cast<int32_t>(next), std::memory_order_relaxed);
            request_.signal();
        }
    }
    stats_.miss_us += platform::monotonic_us() - start;
    return table_[page] = frames_[resident_[page]].data;
}

void RomCache::reader_loop() {
    for (;;) {
        request_.wait();
        if (stop_.load(std::memory_order_seq_cst)) return;
        const int32_t page = ahead_page_.load(std::memory_order_relaxed);
        const bool ok = read_page(static_cast<unsigned>(page), frames_[ahead_frame_].data);
        ahead_ok_.store(ok, std::memory_order_release);
        done_.signal();
    }
}

}  // namespace gbx::gba
