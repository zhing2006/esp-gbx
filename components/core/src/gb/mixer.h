#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include "offload.h"
#include "sound.h"

namespace gbx::gb {
// Synthesizes the four channels from the APU's batches, mixes them as NR50
// and NR51 select and resamples the result, on the worker of the other core
// or, on one core, as each batch is submitted.
//
// Each channel's digital output changes only at its own steps and at entries,
// so the mixed signal is a step function. Every change adds its difference
// to the output frames, split by where it falls in a frame's interval: this
// integrates the signal over each interval exactly (a box filter), at a
// cost per change rather than per tick, and frames need only a running sum.
// The time-to-frame map is fixed point and changes only at batch entries, so
// frames depend on the entries alone, not on how they were split into
// batches or when the worker ran.
class Mixer {
public:
    explicit Mixer(Offload& offload) : offload_(offload) {
        offload_.attach(Offload::sound, kBatches, consume, this);
    }
    // Producer side, on the CPU owner. Submit the acquired batch before
    // acquiring another; submission commits its output frames for pacing.
    sound::Batch& acquire() { return batches_[offload_.acquire(Offload::sound) % kBatches]; }
    void submit(const sound::Batch& batch);
    void finish() { offload_.finish(Offload::sound); }
    // Consumer of the frames: one task at a time, not necessarily the CPU
    // owner, so the copy can stay off the emulating core.
    size_t read(std::span<int16_t> frames);
    // Frames finished and not read yet, as read() would find them; any task.
    size_t queued() const;
    // CPU owner only. Includes frames committed by submitted batches even
    // before synthesis; conversion into ready PCM does not change the count.
    size_t buffered() const;
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    // Each channel's digital output, 0..15, at the end of the batches
    // processed; for the CPU owner while the worker is idle, see finish().
    const std::array<uint8_t, 4>& digital() const { return digital_; }

private:
    friend struct SnapshotAccess;
    // Inline, each batch is consumed as it is submitted, see Offload.
    static constexpr unsigned kBatches = Offload::kInline ? 2 : 8;
    static constexpr unsigned kOutputFrames = 8192;
    // Frames still open to changes; a power of two.
    static constexpr unsigned kPending = 1024;
    Offload& offload_;
    std::array<sound::Batch, kBatches> batches_;

    // Worker side.
    struct Square {
        Tick next = kNever;
        uint32_t spacing = 8192;
        uint8_t step = 0;
        bool silent = false;  // Outputs 0 until the step advances, after power-on.
    };
    struct Noise { Tick next = kNever; uint32_t spacing = 16; uint16_t lfsr = 0; };
    struct Envelope { uint8_t volume = 0, timer = 0; bool running = false; };
    std::array<uint8_t, 0x30> regs_{};  // FF10..FF3F as written.
    uint8_t on_ = 0;                    // Channels on, from status entries.
    std::array<Square, 2> squares_;
    WaveTimer wave_;
    uint8_t sample_ = 0;                // CH3's latest sample.
    Noise noise_;
    std::array<Envelope, 4> envelopes_;  // CH3's is unused.
    std::array<uint8_t, 4> digital_{};   // Each channel's output, 0..15.
    std::array<int32_t, 4> analog_{};    // After its DAC: 15 - 2 * digital, or 0.
    std::array<int32_t, 4> left_{}, right_{};  // Gains from NR50 and NR51.
    int32_t level_left_ = 0, level_right_ = 0;
    Tick now_ = 0;
    // Frame position, in frames << 32: base_ at base_time_, plus step_ per tick.
    uint64_t base_ = 0, step_ = 0;
    Tick base_time_ = 0;
    uint32_t rate_ = 0;
    int32_t ppm_ = 0;
    Tick chunk_ = 0;  // Ticks that fill kPending - 4 frames.
    uint32_t first_ = 0;  // The earliest open frame.
    std::array<std::array<int32_t, 2>, kPending> changes_{};  // Q16 level differences.
    int32_t sum_left_ = 0, sum_right_ = 0;  // Running Q16 levels.
    // High-pass capacitors, Q16. Each is a weighted mean of past levels, so
    // it stays within the levels' +-480 << 16 range, as does the output
    // within twice that.
    int32_t charge_left_ = 0, charge_right_ = 0;
    int32_t retain_ = 0;  // Capacitor retention per frame, Q30.
    // The capacitors stopped moving at a constant level: until the next
    // change, every frame repeats frame_, the previous one.
    bool settled_ = false;
    bool cgb_ = false;  // In padding, so the DMG's state keeps its place.
    uint32_t frame_ = 0;  // Left in the low half.
    // Frames from the worker to the reader.
    std::array<int16_t, kOutputFrames * 2> output_{};
    std::atomic<uint32_t> written_{0}, read_{0};
    std::atomic<uint32_t> reset_at_{0};  // written_ at the latest reset.
    std::atomic<uint64_t> dropped_{0};

    // Producer-side clock mirrors only rate/speed events, not synthesis.
    // scheduled_ counts complete frames across resets, as written_/read_ do.
    Tick scheduled_time_ = 0;
    uint64_t scheduled_position_ = 0, scheduled_step_ = 0;
    uint32_t scheduled_rate_ = 0, scheduled_ = 0;
    int32_t scheduled_ppm_ = 0;
    // Lifetime losses, unlike dropped_, which is reset for each run.
    std::atomic<uint32_t> lost_total_{0};

    static void consume(void* context, uint32_t index);
    void process(const sound::Batch& batch);
    void reset(Tick start);
    void apply(const sound::Entry& entry, Tick when);
    void write(uint8_t address, uint8_t value, uint8_t extra, Tick when);
    void configure(Tick when);
    void advance(Tick to);
    void run(Tick end);
    void skip(Tick end);
    void finish_frames();
    void output(unsigned channel, Tick when);
    void remix(Tick when);
    void change(Tick when, int32_t left, int32_t right);
    bool dac(unsigned channel) const;
    uint16_t step_lfsr(uint16_t lfsr) const;
    unsigned square_output(unsigned channel) const;
};
}  // namespace gbx::gb
