#pragma once

#include <memory>
#include <new>
#include <span>

#include "gbx/gb/jit/sm83_jit.h"
#include "gbx/gb/snapshot.h"
#include "gbx/platform.h"

namespace gbx::gb {
#ifdef GBX_PROFILER
// In profiling builds, the CPU owner's waits for the worker on the other
// core, by queue (rows, sound): how often a full queue or a finish() waited
// and for how long. The reader takes differences.
struct OffloadCounters { uint32_t waits[2]; uint64_t wait_us[2]; };
extern OffloadCounters offload_counters;
#endif
enum class Backend : uint8_t { interpreter, jit };
enum class RunEnd : uint8_t { target, frame, breakpoint, suspended, unavailable };
struct RunResult { RunEnd end; uint64_t instructions; Tick time; };
struct FrameView {
    std::span<const uint16_t> pixels;  // Native BGR555; valid until the next mutating call.
    uint64_t id;
};

// The owner task performs every mutating call, including selecting a backend.
// ROM bytes are borrowed read-only and must outlive the system. No render task
// may read cpu/memory directly: scanline jobs carry copied input state.
class System {
public:
    static constexpr unsigned kBlockInstructions = 16;
    // Every emulated access touches the state, so it lives in internal RAM
    // while there is room, see platform::allocate_internal().
    static void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
        return platform::allocate_internal(bytes);
    }
    static void operator delete(void* memory) noexcept { platform::free_internal(memory); }
    Sm83State cpu;
    Memory memory;
    // Starts the cartridge in `mode`, which stays until the next load; see
    // Mode for how a CGB picks its mode.
    bool load_rom(std::span<const uint8_t> rom, bool stop_on_marker = false, Mode mode = Mode::dmg);
    void reset();
    Mode mode() const { return memory.mode(); }
    // The JIT is created on first use. Its code pool takes the largest
    // block of internal RAM the system needs, and on the board little more
    // than that is free: smaller allocations, anyone's, would split it once
    // freed. A caller can set a block of Sm83Jit::reserved_bytes() aside
    // and hand it to select_backend(): the pool then lives in it, and it
    // stays the caller's, to keep past the system for the next one.
    bool select_backend(Backend backend, const platform::ExecutableMemory* reserved = nullptr);
    Backend backend() const { return backend_; }
    // The JIT's code pool and second level, taken when select_backend()
    // creates the JIT; tests shrink them to make blocks move between levels.
    void set_jit_capacity(const Sm83Jit::Capacity& capacity) { jit_capacity_ = capacity; }
    void set_buttons(uint8_t buttons) { memory.set_buttons(buttons); }
    void set_trace(TraceBuffer* sink);
    // Stops at the first instruction boundary at/after target. Events and IO
    // within that instruction still use exact timestamps. Never pads or drops
    // a partial instruction to manufacture an exact boundary timestamp.
    RunResult run_until(Tick target);
    RunResult run_frame();
    JitResult step_block(Tick until, unsigned instruction_limit = kBlockInstructions);
    // In GB mode, the frame's shades in the gray scale; a CGB's frames are in
    // color already.
    FrameView frame() { memory.ppu.finish(); return {memory.ppu.pixels(), memory.ppu.frame_id()}; }
    // Hands every completed frame to `sink` on the other core and tells it
    // when their sound is mixed, see FrameSink; nullptr stops it. The sink
    // outlives the system or the next call.
    void set_frame_sink(FrameSink* sink) { memory.ppu.set_sink(sink); }
    // GB mode only.
    std::span<const uint8_t> shades() { memory.ppu.finish(); return memory.ppu.shades(); }
    // Colors of the GB mode frames the sink receives, see Ppu::set_palette().
    // Any task.
    void set_palette(const std::array<uint16_t, 4>& palette) { memory.ppu.set_palette(palette); }
    // Whether the next frames are drawn, see Ppu::set_drawing(). Between
    // frames; skipping one changes no emulated result.
    void set_drawing(bool drawing) { memory.ppu.set_drawing(drawing); }
    // Whether a frame is under way, see Ppu::in_frame(). A run can end
    // inside one, at a STOP or after the LCD turns on.
    bool in_frame() const { return memory.ppu.in_frame(); }
    // Whether the worker on the other core polls for the next row or sound
    // batch before it sleeps, on at first. Polling spares the CPU core the
    // wakeups; sleeping leaves the other core's time to its other tasks.
    // Any task, once the system has a ROM; it stays through loading.
    void set_worker_polling(bool polling) { memory.set_worker_polling(polling); }
    std::span<const uint8_t> serial() const { return memory.serial(); }
    bool serial_truncated() const { return memory.serial_truncated(); }

    // Sound, see Apu: stereo frames, interleaved left and right, at `rate`
    // per emulated second; 0, the default, makes none. A run hands the sound
    // to the other core at its end and at least every 1/32 s of emulated
    // time, so frames trail emulation; finish_audio() waits for the frames
    // up to the current time. read_audio() and audio_dropped() may run on
    // another task than the owner, one at a time. The rate and speed stay
    // through loading and reset. Once the mixer reaches a reset, which
    // finish_audio() waits for, read_audio() skips the frames from before
    // it and the dropped count starts over.
    void set_audio_rate(uint32_t rate) { memory.apu.set_rate(rate); }
    // A small correction of frames per emulated second, in parts per
    // million, for a caller that paces emulation by its audio buffer.
    void set_audio_speed(int32_t ppm) { memory.apu.set_speed(ppm); }
    size_t read_audio(std::span<int16_t> frames) { return memory.apu.read_frames(frames); }
    // Frames read_audio() would find now; any task may ask.
    size_t audio_queued() const { return memory.apu.queued(); }
    // CPU owner only. For pacing, also count submitted mixer work so the
    // same audio deficit is not filled again while synthesis is pending.
    size_t audio_buffered() const { return memory.apu.buffered(); }
    void finish_audio() { memory.apu.flush(); memory.apu.finish(); }
    uint64_t audio_dropped() const { return memory.apu.dropped(); }

    // The MBC3 clock, counted in emulated time. The state is exact to the
    // tick: writing back a state taken earlier makes the clock continue from
    // it, neither gaining nor losing time.
    bool has_rtc() const { return memory.cartridge.has_rtc(); }
    RtcState rtc() { return memory.cartridge.rtc().state(memory.clock.now); }
    void set_rtc(const RtcState& state) { memory.cartridge.rtc().set(state, memory.clock.now); }
    // Counts time that passed while the system did not run.
    void advance_rtc(Tick elapsed) { memory.cartridge.rtc().advance(elapsed, memory.clock.now); }
    // Battery-backed cartridge RAM for the save file, see Cartridge.
    bool has_battery() const { return memory.cartridge.battery(); }
    std::span<const uint8_t> cartridge_ram() const { return memory.cartridge.ram(); }
    void load_cartridge_ram(std::span<const uint8_t> bytes) { memory.cartridge.load_ram(bytes); }
    uint32_t take_ram_written() { return memory.cartridge.take_ram_written(); }
    bool take_rtc_written() { return memory.cartridge.take_rtc_written(); }
    const Sm83Jit* jit() const { return jit_.get(); }

    // Instant saves, see Snapshot. Between runs, which end at instruction
    // boundaries: waits for the worker to finish the rows and sound handed
    // to it, then copies the whole state into `out`. False when `out` was
    // allocated for another cartridge RAM size, or when a JIT fallback left
    // an instruction half done.
    bool capture(Snapshot& out);
    // Replaces the whole state with `in`, which holds a state with this
    // cartridge's RAM size; a state whose ROM identity is not this ROM's
    // needs `other_rom`. False leaves the state unchanged. The backend stays;
    // translated RAM code is dropped, ROM code kept. The output rate and
    // speed stay the caller's, and sound not read yet is dropped, as on reset.
    bool restore(const Snapshot& in, bool other_rom = false);
private:
    std::unique_ptr<Sm83Jit> jit_;
    Sm83Jit::Capacity jit_capacity_{};
    Backend backend_ = Backend::interpreter;
    bool marker_ = false;
    TraceBuffer* trace_ = nullptr;
    JitResult boundary(Tick until);
    RunResult run(Tick until, bool frame);
    JitResult halt_cgb(Tick until);
};
}  // namespace gbx::gb
