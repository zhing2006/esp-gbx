#pragma once

#ifdef GBX_PROFILER
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>

#include "gbx/files.h"
#include "gbx/gb/system.h"

namespace gbx::app {

// Frame-by-frame key recordings, in profiling builds only, so that both
// backends and the runs before and after a change take the same input. A
// recording starts with the ROM's load: it keeps the cartridge RAM and clock
// the game started with, then the keys the system took at each frame
// boundary. A replay starts from that state instead of the save, writes no
// save and presses the recorded keys. From the recording's mark to its end
// it times each frame run and each frame's work, then logs them with a hash
// of the machine state, which two backends and two builds must agree on.
//
// The file, little endian: "GBXKEYS2", u32 frames, u32 mark, u8 1 when the
// cartridge was in a GBC (see gb::run_mode()) or 0, u16 path bytes and the
// ROM's card path, the header bytes 0x134..0x14F, u32 cartridge RAM bytes
// and the RAM, u8 clock flag and the clock's fields, then one key byte per
// frame. "GBXKEYS1" files have no GBC byte and play in a GB.
class Replay {
public:
    ~Replay();
    // The console's task. A request applies to the next load; the caller
    // then starts the game, in a GBC when `cgb`. read_path() gives a
    // recording's ROM path and where it ran.
    void request_record(const char* file, bool cgb);
    void request_play(const char* file);
    static bool read_path(const char* file, char* path, size_t size, bool& cgb);
    void request_mark() { mark_requested_.store(true); }
    void request_stop() { stop_requested_.store(true); }

    // The emulation task, around a load: take() picks up a request. A replay
    // then reads its file, and false means the game must not start. After
    // the save is in, started() takes a recording's first state, or
    // replaces the save's with the replay's.
    bool take(std::span<const uint8_t> rom, const char* rom_path);
    bool playing() const { return mode_ == Mode::playing; }
    void started(gb::System& system);
    // Each frame: the keys to give the system instead of `live`.
    uint8_t keys(uint8_t live, gb::System& system);
    // After the frame's run: its emulated ticks, the instructions it ran
    // and how long it took.
    void ran(gb::System& system, uint64_t ticks, uint64_t instructions, uint32_t run_us);
    // Frame `id` took `us` to emulate, see Game::work_done().
    void work(uint64_t id, uint32_t us);

private:
    enum class Mode : uint8_t { off, recording, playing };
    static constexpr size_t kHeaderBytes = 0x150 - 0x134;
    bool load_file(std::span<const uint8_t> rom);
    void write_file();
    void finish(gb::System& system);
    void release();

    // Requests, under the mutex.
    std::mutex mutex_;
    Mode requested_ = Mode::off;
    bool request_cgb_ = false;
    char request_file_[platform::files::kMaxPath] = {};
    std::atomic<bool> mark_requested_{false}, stop_requested_{false};

    // The emulation task.
    Mode mode_ = Mode::off;
    bool cgb_ = false;  // a recording's cartridge is in a GBC
    char file_[platform::files::kMaxPath] = {};
    char rom_path_[platform::files::kMaxPath] = {};
    std::array<uint8_t, kHeaderBytes> header_{};
    // Buffers of kFrames entries, see replay.cpp, kept once allocated.
    uint8_t* keys_ = nullptr;
    uint32_t* run_us_ = nullptr;   // each frame's run from the mark
    uint32_t* scratch_ = nullptr;  // the frame work being reported
    uint32_t frames_ = 0, mark_ = 0, frame_ = 0;
    uint8_t* ram_ = nullptr;
    size_t ram_size_ = 0;
    bool has_rtc_ = false;
    gb::RtcState rtc_{};

    // Timing of a replay from the mark, emulation task.
    uint64_t ticks_ = 0, instructions_ = 0, start_us_ = 0, end_us_ = 0, last_id_ = 0;
    // Running counts at the mark and after the last frame. The game runs on
    // until the report, which must leave those frames out.
    struct Counts {
        uint32_t underruns = 0;
        uint32_t jit[4] = {};  // compiled, restored, evicted, invalidated
        uint64_t fallbacks = 0;
        uint32_t code = 0;     // code pool bytes in use
        gb::OffloadCounters offload{};
        gb::Sm83JitCounters counters{};
    };
    static Counts count(gb::System& system);
    Counts start_{}, end_{};
    uint64_t hash_ = 0;
    uint32_t report_in_ = 0;  // frames to run before the report, for the last frames' work
    // Frame work by frame ID, written by the PPU's worker.
    std::atomic<uint32_t>* work_us_ = nullptr;
    std::atomic<uint64_t> first_id_{0};  // 0 while no replay is timed
};

}  // namespace gbx::app
#endif
