#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

#include "gbx/worker.h"
#include "rom_info.h"

namespace gbx::app {

// The folders and ROMs under roms/ on the card. A task of its own scans the
// card folder by folder, breadth first, and adds what it finds in batches,
// so the ROM list fills in while the scan goes on. It reads each ROM's
// header for the badge and the footer, and lists saves/ once per folder to
// mark the ROMs that have a save. Entries live in fixed arrays in external
// memory, sized by the board, see components/app/Kconfig; a card with more
// stops the scan at kMaxEntries.
//
// The scan task runs on core 0 at priority 2, below the audio (7), the PPU
// and APU worker (5), the present task (4) and LVGL (3), and away from the
// emulation on core 1.
class Library {
public:
    static constexpr int kMaxEntries = GBX_LIBRARY_ENTRIES;
    static constexpr size_t kNamePool = GBX_LIBRARY_NAME_BYTES;
    static constexpr int kRoot = -1;  // roms/ itself

    struct Entry {
        int32_t parent = kRoot;
        uint32_t name = 0;  // in the name pool
        uint16_t name_size = 0;
        bool directory = false;
        bool save = false;  // a ROM with a save on the card
        uint32_t roms = 0;  // a directory: the ROMs in it and below
        RomInfo info;       // a ROM
    };
    enum class State : uint8_t { idle, scanning, no_card, done };

    bool start();
    void stop();
    // Scans the card, mounting it first, unless a scan has found it already.
    // Any task.
    void scan();

    State state() const { return state_.load(std::memory_order_acquire); }
    bool full() const { return full_.load(std::memory_order_relaxed); }
    uint32_t roms() const;
    // Changes whenever entries are added or change.
    uint32_t generation() const { return generation_.load(std::memory_order_acquire); }

    // The entries of `folder`, folders first, each group in natural order:
    // case is ignored and digit runs compare as numbers. Returns how many.
    int list(int folder, std::span<int32_t> out) const;
    // A copy; an empty entry for an index out of range.
    Entry entry(int index) const;
    void name(int index, char* out, size_t size) const;
    // The card path, such as "roms/gb/Tetris.gb"; roms/ itself is "roms".
    void path(int index, char* out, size_t size) const;
    // The entry at a card path, or -1 while the scan has not found it.
    int find(const char* path) const;
    void set_save(int index, bool save);

private:
    static void run_entry(void* self) { static_cast<Library*>(self)->run(); }
    void run();
    void scan_all();
    void scan_folder(int folder);
    void publish(int end);
    void path_locked(int index, char* out, size_t size) const;

    platform::Worker task_;
    platform::Notification wake_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> requested_{false};
    std::atomic<State> state_{State::idle};
    std::atomic<bool> full_{false};
    std::atomic<uint32_t> generation_{0};

    // Entries below count_ are published: their names and places do not
    // change, and the mutex guards their counts and save marks. The scan task
    // fills the entries from count_ on before it publishes them.
    mutable std::mutex mutex_;
    Entry* entries_ = nullptr;
    char* names_ = nullptr;
    int count_ = 0;
    uint32_t roms_ = 0;
    size_t names_used_ = 0;  // scan task
};

}  // namespace gbx::app
