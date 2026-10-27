#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include "../gb/offload.h"
#include "sound.h"

namespace gbx::gba {

// Synthesizes the four PSG channels, takes the Direct Sound FIFOs' samples,
// mixes them as SOUNDCNT_L, SOUNDCNT_H and SOUNDBIAS set, and resamples the
// result, on the worker of the other core (GBATEK "GBA Sound Controller").
// It works only from the batches of the CPU's core, so its PCM depends on
// the writes, the samples and their times alone, not on how they were
// split into batches or when the worker ran.
//
// The GB's mixer (gb/mixer.h) does the same for the GB's channels. The
// GBA's has its own: its registers and wave RAM differ, it adds the FIFOs
// and the PWM stage, and the GB's PCM and speed stay as they are.
//
// Output. The hardware adds the channels, the bias and clips them to 10
// bits, then samples the sum at the rate SOUNDBIAS selects, 32768 Hz by
// default, at 9 bits ("Resampling to 32.768kHz / 9bit"). The sample holds
// until the next one, so the output is a step function that changes only
// at those sampling points: a change of the sum takes effect at the next
// one, where the latest sum is taken. Each change of the output adds its
// difference to the PCM frames, split by where it falls in a frame's
// interval, as the GB's mixer does: an exact box filter, at a cost per
// change. The output is taken against the bias, as the coupling capacitors
// after the PWM stage do, so the bias itself is not heard.
//
// Levels. GBATEK, "Max Output Levels": each FIFO spans +-200h at 100 %, a
// PSG channel +-80h with the master volume at 7 and 100 %. A channel's
// digital output 0-15 counts as 2 * d - 15 while it is on, times its side's
// master volume plus one, which reaches +-120; GBATEK gives no finer rule.
// Sums are kept in quarters, so the PSG's 25 % is exact.
class Mixer {
public:
    explicit Mixer(gb::Offload& offload);
    // Producer side, on the CPU's core. Submit the acquired batch before
    // acquiring another; submission commits its output frames for pacing.
    sound::Batch& acquire() { return batches_[offload_.acquire(gb::Offload::sound) % kBatches]; }
    void submit(const sound::Batch& batch);
    void finish() { offload_.finish(gb::Offload::sound); }
    // Consumer of the frames: one task at a time, not necessarily the CPU's.
    size_t read(std::span<int16_t> frames);
    // Frames finished and not read yet, as read() would find them; any task.
    size_t queued() const;
    // CPU's core only: also the frames of submitted batches not mixed yet.
    size_t buffered() const;
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    friend struct SnapshotAccess;
    static constexpr unsigned kBatches = 8;
    static constexpr unsigned kOutputFrames = 8192;
    // Frames still open to changes; a power of two.
    static constexpr unsigned kPending = 1024;
    gb::Offload& offload_;
    std::array<sound::Batch, kBatches> batches_;

    // Worker side.
    struct Square { Tick next = kNever; uint32_t spacing = 0; uint8_t step = 0; };
    struct Wave { Tick next = kNever; uint32_t spacing = 0; uint8_t index = 0; };
    struct Noise { Tick next = kNever; uint32_t spacing = 0; uint16_t lfsr = 0; bool high = false; };
    struct Envelope { uint8_t volume = 0, timer = 0; bool running = false; };
    std::array<uint8_t, 0x30> regs_{};  // 60h-8Fh as written.
    std::array<std::array<uint8_t, 16>, 2> wave_ram_{};
    uint8_t on_ = 0;                    // Channels on, from status entries.
    std::array<Square, 2> squares_;
    Wave wave_;
    uint8_t sample_ = 0;                // Channel 3's latest digit.
    Noise noise_;
    std::array<Envelope, 4> envelopes_;  // Channel 3's is unused.
    std::array<uint8_t, 4> digital_{};   // Each channel's output, 0-15.
    std::array<int8_t, 2> fifo_{};       // Each FIFO's latest sample.
    // The sum of the channels on each side at the last sampling point, in
    // quarters, and the output the PWM stage holds: the clipped, quantized
    // sum less the bias.
    int32_t level_left_ = 0, level_right_ = 0;
    int32_t held_left_ = 0, held_right_ = 0;
    // The next sampling point, which takes the sum, once something changed
    // it since the last one; kNever otherwise.
    Tick sample_at_ = kNever;
    Tick now_ = 0;
    // Frame position, in frames << 32: base_ at base_time_, plus step_ per cycle.
    uint64_t base_ = 0, step_ = 0;
    Tick base_time_ = 0;
    uint32_t rate_ = 0;
    int32_t ppm_ = 0;
    Tick chunk_ = 0;      // Cycles that fill kPending - 4 frames.
    uint32_t first_ = 0;  // The earliest open frame.
    std::array<std::array<int32_t, 2>, kPending> changes_{};  // Q16 output differences.
    int32_t sum_left_ = 0, sum_right_ = 0;                    // Running Q16 output.
    // Frames from the worker to the reader.
    std::array<int16_t, kOutputFrames * 2> output_{};
    std::atomic<uint32_t> written_{0}, read_{0};
    std::atomic<uint32_t> reset_at_{0};  // written_ at the latest reset.
    std::atomic<uint64_t> dropped_{0};

    // Producer side: the rate and speed events alone, for buffered().
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
    void write(unsigned offset, uint8_t value, Tick when);
    void configure(Tick when);
    void advance(Tick to);
    void run(Tick end);
    void finish_frames();
    void output(unsigned channel, Tick when);
    unsigned square_output(unsigned channel) const;
    uint8_t wave_digit(unsigned index) const;
    void trigger_wave(Tick when);
    // Something that the sum depends on changed at `when`: the next
    // sampling point takes it.
    void remix(Tick when);
    // A sampling point.
    void settle(Tick when);
    void change(Tick when, int32_t left, int32_t right);
    int32_t held(int32_t level) const;
    uint8_t reg(unsigned offset) const { return regs_[offset - 0x60]; }
};

}  // namespace gbx::gba
