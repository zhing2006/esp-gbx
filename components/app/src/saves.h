#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>

#include "faults.h"
#include "gbx/files.h"
#include "gbx/gb/system.h"
#include "gbx/gba/system.h"
#include "gbx/platform.h"
#include "gbx/systems.h"
#include "gbx/worker.h"

namespace gbx::app {

// CRC-32 with the zlib polynomial, the check of the app's files.
uint32_t crc32(std::span<const uint8_t> data);

// The part of a ROM path that its save and state paths keep: the path below
// roms/ without the extension, or the file name for a path outside roms/.
std::string_view rom_stem(const char* rom_path);

// The card path of save file `slot`, 0 or 1, of the ROM at `rom_path`:
// roms/gb/rpg/X.gb saves as saves/gb/rpg/X.0.sav, and a path outside roms/,
// as the PC takes on its command line, as saves/X.0.sav. It is at most 12
// bytes longer, so a ROM path that fits kMaxPath has a save path that fits
// kSavePathSize.
constexpr size_t kSavePathSize = platform::files::kMaxPath + 16;
void save_path(const char* rom_path, int slot, char* out, size_t size);

// The battery-backed parts of the running game's cartridge, as Saves reads
// and writes them on the emulation task: a GB's cartridge RAM and MBC3
// clock, or a GBA's save memory, its type and the S-3511's clock. Saves
// calls it at most once a frame, so a virtual call costs nothing here.
class SaveCartridge {
public:
    static constexpr size_t kClockBytes = 20;
    // The save memory as the system holds it now. A GBA's is empty until
    // its type is known, see gba::Backup.
    virtual std::span<const uint8_t> memory() = 0;
    // The 8 KiB banks the program may have changed since the last call.
    virtual uint32_t take_written() = 0;
    virtual bool has_clock() const = 0;
    virtual bool take_clock_written() = 0;
    // The clock in the save file's layout, see saves.cpp.
    virtual void clock(std::span<uint8_t, kClockBytes> out) = 0;
    virtual void set_clock(std::span<const uint8_t, kClockBytes> in) = 0;
    // The type of a GBA's save memory, gba::BackupType; 0 on a GB.
    virtual uint8_t type() const = 0;
    // A save's contents and type.
    virtual void load(std::span<const uint8_t> data, uint8_t type) = 0;
protected:
    ~SaveCartridge() = default;
};

class GbSaveCartridge final : public SaveCartridge {
public:
    void attach(gb::System* system) { system_ = system; }
    std::span<const uint8_t> memory() override { return system_->cartridge_ram(); }
    uint32_t take_written() override { return system_->take_ram_written(); }
    bool has_clock() const override { return system_->has_rtc(); }
    bool take_clock_written() override { return system_->take_rtc_written(); }
    void clock(std::span<uint8_t, kClockBytes> out) override;
    void set_clock(std::span<const uint8_t, kClockBytes> in) override;
    uint8_t type() const override { return 0; }
    void load(std::span<const uint8_t> data, uint8_t) override { system_->load_cartridge_ram(data); }
private:
    gb::System* system_ = nullptr;
};

#if GBX_HAS_GBA
class GbaSaveCartridge final : public SaveCartridge {
public:
    void attach(gba::System* system) { system_ = system; }
    std::span<const uint8_t> memory() override { return system_->backup_data(); }
    uint32_t take_written() override { return system_->take_backup_written(); }
    bool has_clock() const override { return system_->has_rtc(); }
    bool take_clock_written() override { return system_->take_rtc_written(); }
    void clock(std::span<uint8_t, kClockBytes> out) override;
    void set_clock(std::span<const uint8_t, kClockBytes> in) override;
    uint8_t type() const override { return static_cast<uint8_t>(system_->backup_type()); }
    void load(std::span<const uint8_t> data, uint8_t type) override {
        system_->load_backup(static_cast<gba::BackupType>(type), data);
    }
private:
    gba::System* system_ = nullptr;
};
#endif

// The cartridge save of the running game: its battery-backed RAM and clock,
// in two files on the card written in turn, see saves.cpp for the format.
// The emulation task copies what the game changed into a shadow at frame
// boundaries; a writer task on the other core writes the shadow to the older
// of the two files and flushes it, so the emulation never waits for the
// card. The writer task also writes the settings file.
//
// The writer runs on core 0 at priority 2, like the library's scan.
class Saves {
public:
    // DIRTY: changes not on the card yet. WRITING: a write is under way;
    // changes made meanwhile leave it DIRTY when the write ends. ERROR: the
    // last write failed; no write is tried until retry().
    enum class State : uint8_t { none, clean, dirty, writing, error };
    // What open() found: no save, a save of this ROM, a valid save whose
    // header identifies another ROM, files that all fail their check, or a
    // file that is there but cannot be read, as on a card error.
    enum class Found : uint8_t { none, loaded, mismatch, damaged, unreadable };
    struct Opened {
        Found found = Found::none;
        char title[17] = {};  // the title of the ROM the save belongs to
        bool has_rtc = false;  // the save holds a clock
        platform::WallTime wall{};  // the board's time when the save was taken
    };
    // The system byte of the file, see saves.cpp.
    enum class System : uint8_t { gb, gba };

    bool start(Faults& faults);
    // Writes what is due and ends the task. Close the session first.
    void stop();

    // The emulation task, while it owns the system behind `cartridge`:
    // Reads the saves of the ROM at `rom_path` with `identity` (see
    // gb::rom_identity() and gba::rom_identity()) for a system that has just
    // loaded it, whose save memory can grow to `capacity` bytes: a GBA's
    // type may be decided while it runs. False when there is no memory for
    // them. After Found::unreadable the game must not start: its writes
    // would go over a save nobody has seen.
    bool open(const char* rom_path, std::span<const uint8_t, 24> identity, System system, size_t capacity,
              SaveCartridge& cartridge, Opened& out);
    // Starts the session: with the save open() found, or with the system's
    // empty RAM. Starting empty over a save keeps the old files as .old.
    void use(SaveCartridge& cartridge, bool load_save);
    // At a frame boundary: copies the RAM banks and clock the game changed
    // to the shadow. It never waits; a busy shadow waits for the next frame.
    void snapshot(SaveCartridge& cartridge);
    // Copies every change, waiting for the shadow. `final` also takes the
    // clock and asks for an immediate write: the game is quitting.
    void snapshot_all(SaveCartridge& cartridge, bool final);
    // Ends the session; changes not written are lost. Waits for a write
    // under way to end.
    void close();

    // Any task.
    State state() const;
    // The write under way, 0 to 100.
    int progress() const;
    // Writes changes now instead of after the usual delay.
    void commit();
    // After an error: tries the write again, mounting the card again first
    // when the card failed.
    void retry();
    // Why the last write failed, such as "the SD card is full."
    const char* error_text() const;
    // Writes the settings file a second after the last call.
    void write_settings(std::string_view text);
    // Writes a pending settings file now.
    void flush_settings();

private:
    static void run_entry(void* self) { static_cast<Saves*>(self)->run(); }
    void run();
    uint32_t wait_ms();
    void write_save();
    void write_settings_file();
    void copy_changes(SaveCartridge& cartridge, uint32_t banks, bool rtc);
    platform::files::Error back_up();
    void release();

    Faults* faults_ = nullptr;
    platform::Worker task_;
    platform::Notification wake_;
    std::atomic<bool> stopping_{false};

    mutable std::mutex mutex_;
    // The session, under the mutex.
    bool active_ = false;
    char paths_[2][kSavePathSize] = {};
    char name_[64] = {};  // the ROM's file name, for messages
    std::array<uint8_t, 24> identity_{};
    System system_ = System::gb;
    bool has_rtc_ = false;
    size_t ram_size_ = 0;        // what the shadow holds, up to capacity_
    size_t capacity_ = 0;
    uint8_t type_ = 0;           // SaveCartridge::type() of the shadow
    uint8_t* shadow_ = nullptr;  // capacity_ bytes
    uint8_t* file_ = nullptr;    // the file being written, or read by open(); for capacity_
    size_t file_size_ = 0;
    std::array<uint8_t, SaveCartridge::kClockBytes> clock_{};  // with the shadow
    bool saved_rtc_ = false;     // the save open() found holds a clock
    bool saved_system_ = true;   // and is of this system
    size_t saved_size_ = 0;      // its save memory's bytes, and type
    uint8_t saved_type_ = 0;
    platform::WallTime wall_{};  // when the shadow was taken
    Found found_ = Found::none;
    uint32_t generation_ = 0;    // counts shadow changes
    uint32_t committed_ = 0;     // the generation on the card
    uint32_t writing_generation_ = 0;
    uint32_t sequence_ = 0;      // of the newest file on the card
    int next_slot_ = 0;
    bool backup_ = false;        // copy the files to .old before the first write
    bool writing_ = false;
    bool error_ = false;
    platform::files::Error last_error_ = platform::files::Error::none;
    bool commit_requested_ = false;
    bool retry_requested_ = false;
    uint64_t changed_us_ = 0;      // the latest change
    uint64_t dirty_since_us_ = 0;  // the first change not being written
    std::atomic<size_t> written_{0};

    // The emulation task's marks not yet compared.
    bool session_ = false;
    uint32_t pending_banks_ = 0;
    bool pending_rtc_ = false;

    // The settings file, under the mutex.
    char settings_[1024] = {};
    size_t settings_size_ = 0;
    bool settings_due_ = false;
    uint64_t settings_at_us_ = 0;
};

}  // namespace gbx::app
