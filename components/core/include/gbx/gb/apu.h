#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include "gbx/common/scheduler.h"

namespace gbx::gb {
class Memory;
class Mixer;
class Offload;
namespace sound { struct Batch; enum class Event : uint8_t; }

// CH3 reads a sample from wave RAM each time its period divider overflows.
// The divider counts every 4 ticks (2097152 Hz) up from the 11-bit period,
// so reads are (2048 - period) * 4 ticks apart (Pan Docs, NR33). A period
// write changes the spacing after the read already counting down (Pan Docs,
// NR33 DELAY), so run() before storing it. Both cores run this timer, the
// APU for wave RAM accesses and the mixer for the samples it plays.
struct WaveTimer {
    // Ticks from a trigger to the first read, beyond one period, found with
    // blargg's dmg_sound 09-wave read while on.
    static constexpr Tick kTriggerDelay = 12;
    Tick next = kNever;        // The next read.
    Tick last = kNever;        // The latest read, kNever before the first.
    uint32_t spacing = 8192;   // Ticks between reads.
    uint8_t position = 0;      // Sample index of the latest read.
    static uint32_t ticks(unsigned period) { return (2048 - (period & 2047)) * 4; }
    void trigger(Tick now) { position = 0; last = kNever; next = now + kTriggerDelay + spacing; }
    void stop() { next = last = kNever; }
    // Reads up to and including time t.
    void run(Tick t) {
        if (t < next) return;
        const Tick reads = (t - next) / spacing + 1;
        position = static_cast<uint8_t>((position + reads) & 31);
        last = next + (reads - 1) * spacing;
        next = last + spacing;
    }
};

// The APU state the CPU can observe, kept on the CPU owner: power, the
// DIV-APU frame sequencer, length timers, CH1's sweep and CH3's wave RAM
// reads, which decide NR52's channel bits and what wave RAM accesses reach
// while CH3 plays. None of it runs cycle by cycle or needs a scheduler event:
// an access to NR52, wave RAM or another sound register first brings it up
// to the access time, as a DIV write does before it moves the sequencer.
//
// Waveforms, envelopes, mixing and resampling run on the other core. Each
// write that affects them goes into a batch with its time, followed by the
// changes the CPU side derives: channels turned off, sweep periods and
// envelope steps. The mixer works only from batches, so its PCM depends on
// the writes and their times and never feeds back into this state.
class Apu {
public:
    Apu();
    ~Apu();
    // Batches go to the worker of offload, which outlives this APU.
    bool initialize(Offload& offload);
    // Powers up as the DMG boot ROM leaves the APU, from memory.io.
    void reset(Memory& memory);
    // NR52 and wave RAM, whose values depend on the access time.
    uint8_t read(uint8_t address);
    // 0x10..0x3f: sound registers and wave RAM.
    void write(uint8_t address, uint8_t value);
    // DIV restarted at the current time; `high` when the bit that clocks
    // DIV-APU was set before.
    void restart_divider(bool high, bool double_speed) {
        sync();
        // Resetting DIV while bit 4 of DIV is set makes it fall (Pan Docs,
        // DIV-APU). DIV then counts from 0, so bit 4 falls a full step later.
        if (high) sequence(now());
        edge_ = align(now() + kStep, double_speed);
    }
    // DIV's bit stopped: DIV-APU waits for restart_divider().
    void stop_divider() { sync(); edge_ = kNever; }
    // DIV changed its rate, so that DIV-APU next falls at `edge`.
    void retime_divider(Tick edge, bool double_speed) { sync(); edge_ = align(edge, double_speed); }
    // Hands the sound up to the current time to the mixer. A run calls it
    // at its end and no later than flush_at(), which bounds a batch.
    void flush();
    Tick flush_at() const { return flush_at_; }

    // Output, on the CPU owner except read_frames() and dropped(), which one
    // other task at a time may call. Interleaved left/right 16-bit frames at
    // `rate` per second; 0 stops synthesis. A change of rate or speed takes
    // effect at the current emulated time.
    void set_rate(uint32_t rate);
    // Adjusts the frames per emulated second by parts per million.
    void set_speed(int32_t ppm);
    // Frames the mixer has finished, up to frames.size() / 2.
    size_t read_frames(std::span<int16_t> frames);
    // Frames finished and not read yet, from any task.
    size_t queued() const;
    // CPU owner only: unread frames, including submitted but unmixed work.
    size_t buffered() const;
    // Waits until the mixer has processed every flushed batch.
    void finish();
    // Frames lost because the reader left the output buffer full.
    uint64_t dropped() const;

    // The largest set_speed() adjustment, parts per million.
    static constexpr int32_t kMaxSpeed = 20000;

private:
    friend struct SnapshotAccess;
    // DIV bit 4 falls every 8192 T-cycles (Pan Docs, DIV-APU).
    static constexpr Tick kStep = 16384;
    // At double speed, DIV-APU takes DIV's falling bit at a multiple of 4
    // ticks, up to 3 ticks after it falls: AGE spsw-ch2-lc-delay-cgbBCE finds
    // its steps late by an M-cycle after some switches and on time after
    // others, as the switches move the CPU against the master clock. Normal
    // speed keeps the timing that blargg's and SameSuite's sound tests check.
    static Tick align(Tick edge, bool double_speed) { return double_speed ? (edge + 3) & ~Tick{3} : edge; }
    Memory* memory_ = nullptr;
    const Scheduler* clock_ = nullptr;
    std::unique_ptr<Mixer> mixer_;
    sound::Batch* batch_ = nullptr;  // Open batch, filled in place.
    Tick last_end_ = 0;              // End of the last batch submitted.
    uint32_t rate_ = 0;              // Output settings, sent again on reset.
    int32_t speed_ = 0;
    Tick flush_at_ = kNever;
    Tick edge_ = kNever;     // The next DIV-APU step: DIV bit 4 falls.
    uint8_t step_ = 0;       // Frame sequencer step at that edge, 0..7.
    uint8_t on_ = 0;         // Channels on, NR52 bits 0-3.
    uint8_t reported_ = 0;   // on_ as the mixer last learned it.
    std::array<uint16_t, 4> length_{};  // Steps left before a length timer expires.
    uint16_t shadow_ = 0;    // CH1 sweep period.
    uint8_t sweep_timer_ = 8;
    bool sweep_enabled_ = false, negated_ = false;
    WaveTimer wave_;
    bool powered() const;
    Tick now() const { return clock_->now; }
    void sync() { if (edge_ <= now()) run_sequencer(); }
    void run_sequencer();
    void sequence(Tick when);
    void clock_sweep(Tick when);
    unsigned sweep_target();
    void turn_off(unsigned channel);
    void report(Tick when);
    void trigger(unsigned channel, uint8_t value);
    bool wave_access();
    void append(Tick when, sound::Event event, uint8_t address = 0, uint8_t value = 0, uint8_t extra = 0);
    void append_setting(sound::Event event, uint32_t value);
    void open();
    void submit(Tick end);
};
}  // namespace gbx::gb
