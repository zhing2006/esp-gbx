#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "faults.h"
#include "gbx/gb/snapshot.h"
#include "gbx/gb/system.h"
#include "gbx/gba/snapshot.h"
#include "gbx/gba/system.h"
#include "gbx/platform.h"
#include "gbx/worker.h"
#include "saves.h"

namespace gbx::app {

// The card path of instant save `slot`, 1 to 4, of the ROM at `rom_path`:
// roms/gb/rpg/X.gb has states/gb/rpg/X.1.state, and a path outside roms/,
// as the PC takes on its command line, states/X.1.state. It fits
// kSavePathSize whenever the ROM path fits kMaxPath.
void state_path(const char* rom_path, int slot, char* out, size_t size);

// The instant saves of the running game: four slots, a file each, see
// slots.cpp for the format. The emulation task only copies the state into a
// snapshot, see System::capture(). A task on core 0 encodes it, writes the
// file and flushes the card; for a load it reads the file and decodes it,
// and the emulation task restores the copy. The task also lists the slots.
//
// The task runs on core 0 at priority 2, like the cartridge save's writer.
class Slots {
public:
    static constexpr int kCount = 4;
    // What a slot holds.
    enum class Kind : uint8_t {
        empty,
        ok,
        damaged,       // fails its check, such as after a write cut off
        version,       // another format version, which is never read
        system,        // another system's state
        incompatible,  // a ROM with another cartridge RAM size
        unreadable,    // the file is there but cannot be read
        mode,          // taken in another run mode, see gb::run_mode(); it has a thumbnail
    };
    struct Info {
        Kind kind = Kind::empty;
        gb::Mode mode = gb::Mode::dmg;  // of an ok state, or of one in another mode
        bool gba = false;               // a GBA's state; mode is then dmg
        // An ok state taken on a ROM whose header identity is not this one's.
        bool other_rom = false;
        char title[17] = {};        // that ROM's title, for an ok state
        platform::WallTime wall{};  // the board's time when it was taken
    };
    // The frame a state was taken on: in GB mode shades, four a byte, the
    // leftmost pixel in bits 1:0; in the CGB's modes BGR555 colors, as the
    // game set them, 160x144; on the GBA its BGR555 colors, 240x160.
    struct Thumbnail {
        gb::Mode mode = gb::Mode::dmg;
        bool gba = false;
        std::array<uint8_t, gb::kPackedFrameBytes> shades{};
        std::array<uint16_t, gba::kFramePixels> colors{};
    };
    enum class Work : uint8_t { none, listing, writing, reading };

    // `done` is signaled each time work ends.
    bool start(Faults& faults, platform::Notification& done);
    void stop();

    // The emulation task, while it owns the system:
    // Takes the slots of the ROM at `rom_path`, which `system` has just
    // loaded, and lists them. False when there is no memory for them.
    bool open(const char* rom_path, const gb::System& system);
    bool open(const char* rom_path, gba::System& system);
    // Waits for work under way and releases the buffers.
    void close();
    // Copies the state and has the task write it to `slot`. False while
    // other work is under way, or when the state cannot be taken; that ends
    // like a failed write, see writes().
    bool save(int slot, gb::System& system);
    bool save(int slot, gba::System& system);
    // Has the task read `slot` and decode it into the snapshot.
    bool read(int slot);
    // Once read() is done: the slot's kind, ok when snapshot() holds its
    // state, and the board's time when it was taken. The snapshot stays as
    // it is until the emulation task asks for new work.
    Kind read_kind() const;
    platform::WallTime read_wall() const;
    const gb::Snapshot& snapshot() const { return snapshot_; }
    const gba::Snapshot& gba_snapshot() const { return gba_snapshot_; }

    // Any task.
    bool active() const;
    Work work() const;
    // The slot written or read, 1 to 4.
    int work_slot() const;
    // How much of a write is on the card, or of a read in memory, 0 to 100.
    int progress() const;
    // False while the slots are not listed yet. The slot being written
    // shows the state it is taking.
    bool info(int slot, Info& out) const;
    // The thumbnail of an ok slot or one of another mode, as info() lists
    // it; false for the others.
    bool thumbnail(int slot, Thumbnail& out) const;
    // Changes whenever a slot's info does.
    uint32_t generation() const { return generation_.load(std::memory_order_acquire); }
    // Why the last write failed, such as "the SD card is full."; empty after
    // a write that worked.
    void error(char* out, size_t size) const;
    // Counts the save() calls that have ended: each one once, when its write
    // worked or failed, or when save() refused it. error() is the last one's
    // once this has changed.
    uint32_t writes() const { return writes_.load(std::memory_order_acquire); }
    // The title in the header of the ROM the slots are of.
    void rom_title(char (&out)[17]) const;

private:
    static void run_entry(void* self) { static_cast<Slots*>(self)->run(); }
    void run();
    void list();
    void write_slot(int slot);
    void read_slot(int slot);
    Kind examine(int slot, Info& info, Thumbnail& thumbnail);
    void finish(Work work);
    void refuse(int slot, const char* why);
    bool reserve(int slot);
    bool submit(int slot, bool captured);
    bool begin(const char* rom_path, const gb::RomIdentity& identity, size_t file_size, size_t buffer_size);
    void release();

    Faults* faults_ = nullptr;
    platform::Notification* done_ = nullptr;
    platform::Worker task_;
    platform::Notification wake_;
    std::atomic<bool> stopping_{false};

    mutable std::mutex mutex_;
    // Under the mutex.
    bool active_ = false;
    Work work_ = Work::none;
    bool requested_ = false;  // work_ is posted to the task
    int slot_ = 0;
    char paths_[kCount][kSavePathSize] = {};
    char name_[64] = {};
    gb::RomIdentity identity_{};
    // kCount slots, then the write under way and the task's own. The
    // thumbnails, more than the task's stack, are in external memory.
    Info* infos_ = nullptr;
    Thumbnail* thumbnails_ = nullptr;
    bool listed_ = false;
    bool pending_ = false;  // infos_[kCount] stands for slot_
    char error_[48] = {};
    Kind read_kind_ = Kind::empty;
    platform::WallTime read_wall_{}, wall_{};
    std::atomic<uint32_t> generation_{0};
    std::atomic<uint32_t> writes_{0};
    std::atomic<size_t> written_{0};

    // The task's while work_ is not none, the emulation task's otherwise:
    // a GB's or, with gba_, a GBA's.
    gb::Snapshot snapshot_;
    gba::Snapshot gba_snapshot_;
    bool gba_ = false;
    // The files of this ROM in its run mode have file_size_ bytes; the
    // buffer holds one of the other mode too.
    uint8_t* file_ = nullptr;
    size_t file_size_ = 0, buffer_size_ = 0;
    gb::Mode mode_ = gb::Mode::dmg;
};

}  // namespace gbx::app
