#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "gbx/files.h"
#include "gbx/gba/backup.h"
#include "gbx/worker.h"

namespace gbx::gba {

// The cartridge ROM, read from the card page by page into PSRAM (product
// design, "ROM 加载"). Every reader goes through the page table: the
// interpreter's bus, the JIT's compiler and fast paths and DMA. A ROM that
// fits the budget is read whole when opened and never misses.
//
// Pages are 32 KiB: an SD read of that size costs a few milliseconds, most
// of it per read, so smaller pages would miss more often for the same
// streaming, and larger ones would hold more data a game does not use. The
// table holds 1024 entries for the 32 MiB address space of the ROM regions,
// 4 KiB of internal RAM, as every ROM access reads it.
//
// Pages past the end of the ROM read as the gamepak bus left them: the
// halfword at an address is (address / 2) & FFFFh (GBATEK "GBA
// Unpredictable Things", "Reading from GamePak ROM when no Cartridge is
// inserted"). That pattern repeats every 128 KiB, so four pattern pages
// serve the whole range, and the end of a last partial page gets it too:
// a mapped entry is never empty for want of data, only for a page not
// resident.
//
// Replacement is CLOCK without a reference bit on the access path: when a
// frame is needed, the hand unmaps the frames it passes that are still
// mapped, and takes the first it finds unmapped, a page nobody touched
// since the last sweep. Touching an unmapped resident page maps it again
// without a read. A miss reads the page and asks the reader on the other
// core for the next one, so that code and data read in order arrive
// before they are needed. Misses change only wall-clock time: the
// emulation waits for the data either way.
//
// One owner task maps pages and handles misses; the reader only fills the
// frame it was given. The emulation's results do not depend on which pages
// are resident, so two systems may share a cache as long as they do not
// run at the same time. A system that overlays page 0 sets its overlay
// again before each run.
class RomCache {
public:
    static constexpr unsigned kPageBits = 15;
    static constexpr size_t kPageBytes = size_t{1} << kPageBits;
    static constexpr size_t kMaxBytes = size_t{32} << 20;
    static constexpr unsigned kPages = kMaxBytes >> kPageBits;
    // Halfword patterns repeat every 128 KiB.
    static constexpr unsigned kPatternPages = 4;
    // The smallest cache: two frames, for a page of code and one of data.
    // Pages are read ahead only with a third.
    static constexpr size_t kMinFrames = 2;

    RomCache();
    ~RomCache();
    RomCache(const RomCache&) = delete;
    RomCache& operator=(const RomCache&) = delete;

    // Opens a ROM file and reads it from start to end once, handing each
    // piece to `observe` (such as a hash) in order. Frames take up to
    // `budget` bytes of external memory; a ROM that fits keeps every page,
    // a larger one the first pages read. False when the file cannot be read,
    // is empty or larger than 32 MiB, or memory is short.
    using Observe = void (*)(void* context, std::span<const uint8_t> bytes);
    bool open(const char* path, size_t budget, Observe observe = nullptr, void* context = nullptr);
    // A ROM already in memory, borrowed for the cache's lifetime: every page
    // is resident.
    bool attach(std::span<const uint8_t> rom);
    void close();
    // Drops every page and its frames down to `frames`, for tests that make
    // pages move in and out; 0 restores the budget given to open().
    bool set_frames(size_t frames);

    size_t size() const { return size_; }
    // The save type an ID string in the ROM names, found as it was read.
    BackupType backup_id() const { return scanner_.type(); }
    bool paged() const { return paged_; }
    // Mapped pages by (address & 1FFFFFFh) >> kPageBits; nullptr when not
    // mapped, see map().
    const uint8_t* const* table() const { return table_; }
    // Maps the page and returns its data, reading it first when it is not
    // resident.
    const uint8_t* map(unsigned page) {
        const uint8_t* data = table_[page];
        return data ? data : miss(page);
    }
    // Reads `bytes` from `offset` into `out`, which the pattern fills past
    // the end of the ROM; for the code the JIT compiles and for diagnosis.
    uint16_t halfword(uint32_t offset) {
        const auto* page = map((offset & (kMaxBytes - 1)) >> kPageBits);
        uint16_t value;
        __builtin_memcpy(&value, page + (offset & (kPageBytes - 2)), 2);
        return value;
    }
    uint32_t word(uint32_t offset) {
        const auto* page = map((offset & (kMaxBytes - 1)) >> kPageBits);
        uint32_t value;
        __builtin_memcpy(&value, page + (offset & (kPageBytes - 4)), 4);
        return value;
    }
    uint8_t byte(uint32_t offset) {
        return map((offset & (kMaxBytes - 1)) >> kPageBits)[offset & (kPageBytes - 1)];
    }
    // Page 0 reads from `data` instead, a page that the owner keeps, such
    // as a copy with the cartridge GPIO's registers; nullptr ends it. Page
    // 0's own frame may then be reused. open(), attach() and close() end it.
    void set_overlay(const uint8_t* data);
    // Ends the overlay if it is `data`, which its owner is about to free.
    void end_overlay(const uint8_t* data) {
        if (overlay_ == data) set_overlay(nullptr);
    }

    // Counters since open(): pages read on a miss and the wall time the
    // owner waited for them, pages read ahead, and remaps of resident pages.
    struct Stats {
        uint64_t misses = 0, miss_us = 0, ahead = 0, remaps = 0, read_errors = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    struct Frame {
        uint8_t* data = nullptr;
        int32_t page = -1;  // Resident page, or -1.
    };
    const uint8_t* miss(unsigned page);
    // A frame to put a page in, evicting the page it held.
    unsigned victim();
    void fill_pattern(uint8_t* data, uint32_t offset, size_t from) const;
    bool read_page(unsigned page, uint8_t* data);
    void wait_reader();
    static void reader_entry(void* self) { static_cast<RomCache*>(self)->reader_loop(); }
    void reader_loop();
    bool allocate_frames(size_t count);
    void release_frames();

    const uint8_t** table_ = nullptr;  // Internal RAM, kPages entries.
    const uint8_t* overlay_ = nullptr;  // Page 0's overlay, see set_overlay().
    size_t size_ = 0;
    unsigned rom_pages_ = 0;           // Pages holding ROM bytes.
    bool paged_ = false;
    std::span<const uint8_t> attached_;
    uint8_t* patterns_ = nullptr;       // kPatternPages pages.
    uint8_t* storage_ = nullptr;        // Frames, in one block.
    Frame* frames_ = nullptr;
    size_t frame_count_ = 0, budget_frames_ = 0;
    // Resident frame by page, -1 when not resident.
    int16_t* resident_ = nullptr;
    unsigned hand_ = 0;
    platform::files::File file_;
    Stats stats_{};
    BackupScanner scanner_;
    // The reader on the other core: one request at a time.
    platform::Worker reader_;
    platform::Notification request_, done_;
    bool reader_started_ = false;
    std::atomic<int32_t> ahead_page_{-1};   // Page being read, -1 when idle.
    unsigned ahead_frame_ = 0;
    std::atomic<bool> ahead_ok_{false}, stop_{false};
};

}  // namespace gbx::gba
