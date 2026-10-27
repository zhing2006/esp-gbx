#pragma once

#include <cstdint>
#include <memory>
#include <new>
#include <span>

#include "gbx/gba/arm7.h"
#include "gbx/gba/jit/arm7_jit.h"
#include "gbx/gba/memory.h"
#include "gbx/gba/snapshot.h"
#include "gbx/platform.h"

namespace gbx::gba {

enum class Backend : uint8_t { interpreter, jit };
struct FrameView {
    std::span<const uint16_t> pixels;  // BGR555; valid until the next mutating call.
    uint64_t id;
};

// The whole GBA (product design, "GBA"). One owner task makes every
// mutating call. The BIOS and the ROM's cache are borrowed and outlive the
// system; the renderer on the other core reads only the jobs it is given.
//
// Power-on skips the BIOS's start-up: reset() sets the CPU, the I/O
// registers and the devices' phases as the BIOS leaves them when it starts
// the cartridge, see Memory::reset().
class System {
public:
    // The CPU state and the memory's fast-path fields are touched by every
    // access: internal RAM while there is room.
    static void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
        return platform::allocate_internal(bytes);
    }
    static void operator delete(void* memory) noexcept { platform::free_internal(memory); }
    System();
    ~System();

    Arm7State cpu;
    Memory memory;

    // Allocates the memories and starts the renderer's worker, once.
    bool initialize();
    // A BIOS image (kBiosBytes) and a ROM opened in `rom`; then reset().
    bool load(std::span<const uint8_t, kBiosBytes> bios, RomCache& rom);
    void reset();
    // From power-on through the BIOS's own start-up, for checking the state
    // reset() sets against what the BIOS leaves.
    void reset_to_bios();
    // The JIT is created on first use, with `capacity`.
    //
    // Its code pool takes the largest block of internal RAM the system
    // needs, and on the board little more than that is free: smaller
    // allocations, anyone's, would split it once freed. A caller can set a
    // block of Arm7Jit::reserved_bytes() aside and hand it to
    // select_backend(): the pool then lives in it, and it stays the
    // caller's, to keep past the system for the next one.
    bool select_backend(Backend backend, const platform::ExecutableMemory* reserved = nullptr);
    Backend backend() const { return backend_; }
    void set_jit_capacity(const Arm7JitCapacity& capacity) { jit_capacity_ = capacity; }
    void set_buttons(uint16_t pressed) { memory.set_buttons(pressed); }

    // Runs to the first instruction boundary at or after `target`.
    Tick run_until(Tick target);
    // Runs to the next start of VBlank.
    Tick run_frame() { return run_until(memory.ppu.next_vblank(now())); }
    Tick now() const { return memory.now(); }
    // Whether the CPU sits at a branch to itself, the end of the test ROMs.
    bool idle_loop() const;

    FrameView frame() { return {memory.ppu.pixels(), memory.ppu.frame_id()}; }
    // Hands every completed frame to `sink` on the other core and tells it
    // when their sound is mixed; nullptr stops it. The sink outlives the
    // system or the next call.
    void set_frame_sink(FrameSink* sink) { memory.ppu.set_sink(sink); }
    void set_drawing(bool drawing) { memory.ppu.set_drawing(drawing); }
    // Whether the worker on the other core polls for the next line or sound
    // batch before it sleeps, on at first. Any task, after initialize().
    void set_worker_polling(bool polling) { memory.set_worker_polling(polling); }

    // Sound, see Memory: stereo frames, interleaved left and right, at
    // `rate` per emulated second; 0, the default, makes none. A run hands the
    // sound to the other core at its end, so frames trail emulation;
    // finish_audio() waits for the frames up to the current time.
    // read_audio() and audio_dropped() may run on another task than the
    // owner, one at a time. The rate and speed stay through loading and
    // reset; read_audio() skips the frames from before a reset.
    void set_audio_rate(uint32_t rate) { memory.set_audio_rate(rate); }
    // A small correction of frames per emulated second, in parts per
    // million, for a caller that paces emulation by its audio buffer.
    void set_audio_speed(int32_t ppm) { memory.set_audio_speed(ppm); }
    size_t read_audio(std::span<int16_t> frames) { return memory.read_audio(frames); }
    // Frames read_audio() would find now; any task may ask.
    size_t audio_queued() const { return memory.audio_queued(); }
    // The owner only: also the frames of work handed to the mixer.
    size_t audio_buffered() const { return memory.audio_buffered(); }
    void finish_audio() {
        memory.flush_sound();
        memory.finish_audio();
    }
    uint64_t audio_dropped() const { return memory.audio_dropped(); }

    // Takes a complete state at the current instruction boundary into an
    // allocated snapshot. The sound and lines handed to the other core are
    // finished first.
    bool capture(Snapshot& out);
    // Replaces the whole state with a valid snapshot of the same ROM, or of
    // any ROM with `other_rom`. Either backend may run on from it.
    bool restore(const Snapshot& in, bool other_rom = false);

    // The cartridge's clock, for a game that has one: its state now, a
    // state taken at another time to count on from now, and an interval
    // that passed outside emulation, such as power off.
    // The clock counts on the devices' time, clock.now.
    bool has_rtc() const { return memory.has_rtc(); }
    RtcState rtc_state() { return memory.rtc.state(memory.clock.now); }
    void set_rtc(const RtcState& state) { memory.rtc.set(state, memory.clock.now); }
    void advance_rtc(Tick elapsed) { memory.rtc.advance(elapsed, memory.clock.now); }
    bool take_rtc_written() { return memory.rtc.take_written(); }

    // The save memory: its type, from the ROM's ID string, a save file or
    // the program's first access (gba::Backup), and its contents, empty
    // until the type is known.
    BackupType backup_type() const { return memory.backup.type(); }
    std::span<uint8_t> backup_data() { return memory.backup.data(); }
    // The 8 KiB banks the program may have changed since the last call.
    uint32_t take_backup_written() { return memory.backup.take_written(); }
    // A save's type and contents, after load(); a shorter save leaves the
    // rest erased.
    void load_backup(BackupType type, std::span<const uint8_t> data) { memory.backup.load(type, data); }

    const Arm7Jit* jit() const { return jit_.get(); }
    Arm7Jit* jit() { return jit_.get(); }

private:
    std::unique_ptr<Arm7Jit> jit_;
    Arm7JitCapacity jit_capacity_{};
    Backend backend_ = Backend::interpreter;
};

// The interpreter on the memory map, until the memory's target or a stop it
// leaves to the caller (arm7_mapped.cpp).
void arm7_run_mapped(Arm7State& cpu, Memory& memory);
// One instruction, and the IRQ's entry, on the memory map: for the JIT's
// dispatcher (arm7_mapped.cpp).
void arm7_step_mapped(Arm7State& cpu, Memory& memory);
void arm7_interrupt_mapped(Arm7State& cpu, Memory& memory);

}  // namespace gbx::gba
