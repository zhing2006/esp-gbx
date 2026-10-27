#include "mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gbx/gb/ppu.h"

namespace gbx::gb {
namespace {
// Duty waveforms with step 0 in bit 0, for 12.5, 25, 50 and 75 % (Pan Docs,
// NR11 waveforms: 00000001, 10000001, 10000111 and 01111110).
constexpr uint8_t kDuty[]{0b1000'0000, 0b1000'0001, 0b1110'0001, 0b0111'1110};
constexpr uint8_t kEnvelopeRegister[]{0x12, 0x17, 0x1a, 0x21};
constexpr uint8_t kPeriodLow[]{0x13, 0x18, 0x1d};
// The pulse dividers count every 8 ticks (1048576 Hz, Pan Docs NR13).
uint32_t square_ticks(unsigned period) { return (2048 - (period & 2047)) * 8; }
// LFSR clocks at 262144 / (divider * 2^shift) Hz, divider 0 counting as 0.5,
// so 16 * divider << shift T-cycles apart (Pan Docs, NR43).
uint32_t noise_ticks(uint8_t nr43) {
    const unsigned divider = nr43 & 7;
    return (divider ? divider * 32 : 16) << (nr43 >> 4);
}
}  // namespace

void Mixer::submit(const sound::Batch& batch) {
    if (batch.reset) {
        scheduled_time_ = batch.start;
        scheduled_position_ = scheduled_step_ = 0;
        scheduled_rate_ = 0;
        scheduled_ppm_ = 0;
    }
    const auto first = static_cast<uint32_t>(scheduled_position_ >> 32);
    const auto advance_to = [&](Tick when) {
        scheduled_position_ += (when - scheduled_time_) * scheduled_step_;
        scheduled_time_ = when;
    };
    for (uint32_t i = 0; i < batch.count; ++i) {
        const auto& entry = batch.entries[i];
        if (entry.event != sound::Event::rate && entry.event != sound::Event::speed) continue;
        advance_to(batch.start + entry.time);
        if (entry.event == sound::Event::rate) scheduled_rate_ = entry.value24();
        else scheduled_ppm_ = static_cast<int32_t>(entry.value24() << 8) >> 8;
        scheduled_step_ = sound::frames_per_tick(scheduled_rate_, scheduled_ppm_);
    }
    advance_to(batch.end);
    scheduled_ += static_cast<uint32_t>(scheduled_position_ >> 32) - first;
    // Account for the whole batch before the worker can publish any PCM.
    offload_.submit(Offload::sound, true);
}

void Mixer::consume(void* context, uint32_t index) {
    auto& self = *static_cast<Mixer*>(context);
    const auto& batch = self.batches_[index % kBatches];
    self.process(batch);
}
void Mixer::process(const sound::Batch& batch) {
    if (batch.reset) reset(batch.start);
    else advance(batch.start);
    // Rebase the frame position on every batch so that the product below
    // stays far from overflow however long the emulation runs.
    base_ += (batch.start - base_time_) * step_;
    base_time_ = batch.start;
    for (uint32_t i = 0; i < batch.count; ++i) {
        const auto& entry = batch.entries[i];
        const Tick when = batch.start + entry.time;
        advance(when);
        apply(entry, when);
    }
    advance(batch.end);
}
void Mixer::reset(Tick start) {
    regs_.fill(0); on_ = 0;
    squares_ = {}; wave_ = {}; sample_ = 0; noise_ = {}; envelopes_ = {};
    // The boot ROM leaves CH1 on (Pan Docs, Power Up Sequence: NR52), so it
    // has been triggered; CH2 is taken as never triggered since power-on.
    squares_[1].silent = true;
    digital_.fill(0); analog_.fill(0); left_.fill(0); right_.fill(0);
    level_left_ = level_right_ = 0;
    now_ = base_time_ = start; base_ = 0; first_ = 0;
    // The reset batch sends the rate and speed again after the registers,
    // so nothing from before the reset shapes the frames (see configure()).
    rate_ = 0; ppm_ = 0; step_ = 0; chunk_ = 0; retain_ = 0; cgb_ = false;
    for (auto& change : changes_) change = {0, 0};
    sum_left_ = sum_right_ = 0; charge_left_ = charge_right_ = 0;
    settled_ = false; frame_ = 0;
    // Frames not read yet belong to the run before.
    reset_at_.store(written_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
}
void Mixer::configure(Tick when) {
    base_ += (when - base_time_) * step_;
    base_time_ = when;
    const bool was = step_ != 0;
    // Frames per tick, << 32: rate / 2^23 adjusted by ppm.
    step_ = sound::frames_per_tick(rate_, ppm_);
    chunk_ = step_ ? (uint64_t{kPending - 4} << 32) / step_ : 0;
    // Pan Docs, Obscure Behavior: the DMG's capacitor keeps 0.999958 of its
    // charge per 4194304 Hz cycle, 0.999958^(4194304 / rate) per frame; a
    // CGB's keeps 0.998943.
    const double kept = cgb_ ? 0.998943 : 0.999958;
    retain_ = rate_ ? static_cast<int32_t>(std::lround(std::pow(kept, 4194304.0 / rate_) * (1 << 30))) : 0;
    settled_ = false;
    if (was && !step_) {
        // Frames stop: the changes still open go into the levels, which
        // then follow every change at once (see change()).
        for (auto& change : changes_) {
            sum_left_ += change[0]; sum_right_ += change[1];
            change = {0, 0};
        }
    } else if (!was && step_) {
        // Nothing followed the capacitors without frames: take them as
        // charged to the level, as after a long time at it, so the frames
        // start without a step and do not depend on what came before.
        charge_left_ = sum_left_; charge_right_ = sum_right_;
    }
}
void Mixer::apply(const sound::Entry& entry, Tick when) {
    switch (entry.event) {
    case sound::Event::write: write(entry.address, entry.value, entry.extra, when); break;
    case sound::Event::status: {
        const auto changed = on_ ^ entry.value;
        on_ = entry.value;
        for (unsigned c = 0; c < 4; ++c) {
            if (!(changed >> c & 1)) continue;
            if (!(on_ >> c & 1)) {
                if (c < 2) squares_[c].next = kNever;
                else if (c == 2) wave_.stop();
                else noise_.next = kNever;
            }
            output(c, when);
        }
        break;
    }
    case sound::Event::sweep: {
        const auto period = entry.value24();
        regs_[0x03] = static_cast<uint8_t>(period);
        regs_[0x04] = static_cast<uint8_t>((regs_[0x04] & 0xf8) | period >> 8);
        squares_[0].spacing = square_ticks(period);
        break;
    }
    case sound::Event::envelope:
        for (unsigned c : {0u, 1u, 3u}) {
            auto& e = envelopes_[c];
            const auto nrx2 = regs_[kEnvelopeRegister[c] - 0x10];
            const unsigned pace = nrx2 & 7;
            if (!pace || !e.running || --e.timer) continue;
            e.timer = static_cast<uint8_t>(pace);
            if (nrx2 & 8 ? e.volume == 15 : e.volume == 0) { e.running = false; continue; }
            e.volume = static_cast<uint8_t>(e.volume + (nrx2 & 8 ? 1 : -1));
            output(c, when);
        }
        break;
    case sound::Event::rate: rate_ = entry.value24(); configure(when); break;
    case sound::Event::model: cgb_ = entry.value24() != 0; configure(when); break;
    case sound::Event::speed:
        ppm_ = static_cast<int32_t>(entry.value24() << 8) >> 8;
        configure(when);
        break;
    }
}
void Mixer::write(uint8_t address, uint8_t value, uint8_t extra, Tick when) {
    auto& reg = regs_[address - 0x10];
    const auto old = reg;
    reg = value;
    if (extra & sound::kLoad) {
        for (unsigned c = 0; c < 4; ++c) output(c, when);
        remix(when);
        return;
    }
    const auto start_envelope = [&](unsigned c) {
        auto& e = envelopes_[c];
        const auto nrx2 = regs_[kEnvelopeRegister[c] - 0x10];
        e.volume = nrx2 >> 4;
        // A pace of 0 counts as 8. Pan Docs, Obscure Behavior: a trigger just
        // before an envelope step reloads the timer with one more.
        e.timer = static_cast<uint8_t>(((nrx2 & 7) ? nrx2 & 7 : 8) + (extra & sound::kEnvelopeNext ? 1 : 0));
        e.running = true;
    };
    switch (address) {
    case 0x12: case 0x17: case 0x21: {
        const unsigned c = address == 0x12 ? 0 : address == 0x17 ? 1 : 3;
        // Pan Docs, Obscure Behavior ("zombie" mode), as on the CGB-02 and
        // CGB-04. The DMG differs in some cases, which no test checks.
        auto& e = envelopes_[c];
        unsigned volume = e.volume;
        if (!(old & 7) && e.running) ++volume;
        else if (!(old & 8)) volume += 2;
        if ((old ^ value) & 8) volume = 16 - volume;
        e.volume = static_cast<uint8_t>(volume & 15);
        output(c, when);
        break;
    }
    case 0x11: case 0x16:
        // The output follows a new duty at once. Pan Docs (Pulse channels)
        // derives it from the duty step's bits, not a stored sample; no
        // test shows when the change takes effect.
        output(address == 0x16, when);
        break;
    case 0x13: case 0x18:
        squares_[address == 0x18].spacing = square_ticks(value | (regs_[address - 0x0f] & 7) << 8);
        break;
    case 0x14: case 0x19: {
        const unsigned c = address == 0x19;
        auto& square = squares_[c];
        square.spacing = square_ticks(regs_[kPeriodLow[c] - 0x10] | (value & 7) << 8);
        // A trigger starts only a channel whose DAC is on; the status entry
        // that follows turns it on.
        if ((value & 0x80) && dac(c)) {
            square.next = when + square.spacing;
            start_envelope(c);
            output(c, when);
        }
        break;
    }
    case 0x1a: case 0x1c: output(2, when); break;
    case 0x1d: wave_.spacing = WaveTimer::ticks(value | (regs_[0x0e] & 7) << 8); break;
    case 0x1e:
        wave_.spacing = WaveTimer::ticks(regs_[0x0d] | (value & 7) << 8);
        if ((value & 0x80) && dac(2)) wave_.trigger(when);
        break;
    case 0x22:
        noise_.spacing = noise_ticks(value);
        // Pan Docs, NR43: shifts of 14 and 15 stop the LFSR's clock, also
        // when written while CH4 plays. Nothing documents the phase of the
        // clock that resumes; it starts a full period after the write.
        if ((value >> 4) >= 14) noise_.next = kNever;
        else if ((old >> 4) >= 14 && (on_ & 8)) noise_.next = when + noise_.spacing;
        break;
    case 0x23:
        if ((value & 0x80) && dac(3)) {
            noise_.lfsr = 0;
            noise_.next = (regs_[0x12] >> 4) >= 14 ? kNever : when + noise_.spacing;
            start_envelope(3);
            output(3, when);
        }
        break;
    case 0x24: case 0x25: remix(when); break;
    case 0x26:
        if (!(value & 0x80)) {
            // Power off clears every register but NR52 and wave RAM.
            std::memset(regs_.data(), 0, 0x16);
            envelopes_ = {};
            for (unsigned c = 0; c < 4; ++c) output(c, when);
            remix(when);
        } else if (!(old & 0x80)) {
            // Power on restarts the duty steps and clears CH3's sample
            // (Pan Docs, Pulse channels and Wave channel). The first duty
            // step after the next trigger plays as 0 (Obscure Behavior).
            for (auto& square : squares_) { square.step = 0; square.silent = true; }
            sample_ = 0;
        }
        break;
    default: break;
    }
}
bool Mixer::dac(unsigned channel) const {
    return regs_[kEnvelopeRegister[channel] - 0x10] & (channel == 2 ? 0x80 : 0xf8);
}
unsigned Mixer::square_output(unsigned c) const {
    const auto& square = squares_[c];
    return !square.silent && (kDuty[regs_[c ? 0x06 : 0x01] >> 6] >> square.step & 1) ? envelopes_[c].volume : 0;
}
// Recomputes a channel's DAC output after anything it depends on changed.
// An off channel outputs digital 0; a DAC that is off outputs analog 0.
// The DAC maps digital 0..15 to analog 1..-1 (Pan Docs, DACs), here 15..-15.
void Mixer::output(unsigned c, Tick when) {
    unsigned digital = 0;
    if (on_ >> c & 1) {
        if (c < 2) digital = square_output(c);
        else if (c == 2) {
            const unsigned level = regs_[0x0c] >> 5 & 3;
            digital = level ? sample_ >> (level - 1) : 0;
        } else digital = (noise_.lfsr & 1) ? envelopes_[3].volume : 0;
    }
    digital_[c] = static_cast<uint8_t>(digital);
    const int32_t analog = dac(c) ? 15 - 2 * static_cast<int32_t>(digital) : 0;
    const int32_t difference = analog - analog_[c];
    if (!difference) return;
    analog_[c] = analog;
    const int32_t left = left_[c] * difference, right = right_[c] * difference;
    level_left_ += left; level_right_ += right;
    change(when, left, right);
}
// NR51 routes each channel to each side, and NR50 scales a side by its
// volume plus one (Pan Docs, NR50 and NR51). The range is +-480.
void Mixer::remix(Tick when) {
    const auto nr50 = regs_[0x14], nr51 = regs_[0x15];
    int32_t left = 0, right = 0;
    for (unsigned c = 0; c < 4; ++c) {
        left_[c] = (nr51 >> (c + 4) & 1) * ((nr50 >> 4 & 7) + 1);
        right_[c] = (nr51 >> c & 1) * ((nr50 & 7) + 1);
        left += left_[c] * analog_[c]; right += right_[c] * analog_[c];
    }
    const int32_t dl = left - level_left_, dr = right - level_right_;
    level_left_ = left; level_right_ = right;
    if (dl || dr) change(when, dl, dr);
}
// A level change at `when` counts in its frame for the part of the frame's
// interval after it, and fully in every later frame. Without frames, it
// goes into the levels at once.
void Mixer::change(Tick when, int32_t left, int32_t right) {
    if (!step_) { sum_left_ += left * 65536; sum_right_ += right * 65536; return; }
    const uint64_t position = base_ + (when - base_time_) * step_;
    const auto frame = static_cast<uint32_t>(position >> 32);
    const auto after = static_cast<int32_t>(position >> 16 & 0xffff);
    auto& here = changes_[frame & (kPending - 1)];
    auto& next = changes_[(frame + 1) & (kPending - 1)];
    here[0] += left * (65536 - after); next[0] += left * after;
    here[1] += right * (65536 - after); next[1] += right * after;
}
void Mixer::advance(Tick to) {
    if (to <= now_) return;
    if (!step_) { skip(to); now_ = to; return; }
    while (now_ < to) {
        const Tick end = std::min(to, now_ + chunk_);
        run(end);
        now_ = end;
        finish_frames();
    }
}
// Steps each channel through (now_, end]. Status entries stop the timers
// of channels that turn off, and only a trigger starts them.
void Mixer::run(Tick end) {
    for (unsigned c = 0; c < 2; ++c) {
        auto& square = squares_[c];
        if (square.next > end) continue;
        // Without volume, the steps change no output: skip them at once.
        if (!envelopes_[c].volume) {
            const Tick steps = (end - square.next) / square.spacing + 1;
            square.step = static_cast<uint8_t>((square.step + steps) & 7);
            square.next += steps * square.spacing;
            square.silent = false;
            continue;
        }
        for (; square.next <= end; square.next += square.spacing) {
            square.step = (square.step + 1) & 7;
            square.silent = false;
            if (square_output(c) != digital_[c]) output(c, square.next);
        }
    }
    while (wave_.next <= end) {
        const Tick when = wave_.next;
        wave_.position = (wave_.position + 1) & 31;
        wave_.last = when; wave_.next = when + wave_.spacing;
        const auto byte = regs_[0x20 + wave_.position / 2];
        sample_ = wave_.position & 1 ? byte & 15 : byte >> 4;
        output(2, when);
    }
    for (; noise_.next <= end; noise_.next += noise_.spacing) {
        noise_.lfsr = step_lfsr(noise_.lfsr);
        if (((noise_.lfsr & 1) ? envelopes_[3].volume : 0) != digital_[3]) output(3, noise_.next);
    }
}
// Pan Docs, Noise channel: bit 15, and in 7-bit mode bit 7, take the XNOR of
// bits 0 and 1 before the register shifts right.
uint16_t Mixer::step_lfsr(uint16_t lfsr) const {
    const unsigned bit = ~(lfsr ^ lfsr >> 1) & 1;
    lfsr = static_cast<uint16_t>((lfsr & 0x7fff) | bit << 15);
    if (regs_[0x12] & 8) lfsr = static_cast<uint16_t>((lfsr & ~0x80) | bit << 7);
    return lfsr >> 1;
}
// Without an output rate, channels keep their timing but make no frames. On
// a CGB the digital outputs still follow, for PCM12 and PCM34; the levels
// they change take no frames then (see change()).
void Mixer::skip(Tick end) {
    for (unsigned c = 0; c < 2; ++c) {
        auto& square = squares_[c];
        if (square.next > end) continue;
        const Tick steps = (end - square.next) / square.spacing + 1;
        square.step = static_cast<uint8_t>((square.step + steps) & 7);
        square.next += steps * square.spacing;
        square.silent = false;
        if (cgb_) output(c, end);
    }
    if (wave_.next <= end) {
        wave_.run(end);
        const auto byte = regs_[0x20 + wave_.position / 2];
        sample_ = wave_.position & 1 ? byte & 15 : byte >> 4;
        if (cgb_) output(2, end);
    }
    if (noise_.next > end) return;
    if (!cgb_) { noise_.next += ((end - noise_.next) / noise_.spacing + 1) * noise_.spacing; return; }
    // The LFSR's state depends on every step.
    for (; noise_.next <= end; noise_.next += noise_.spacing) noise_.lfsr = step_lfsr(noise_.lfsr);
    output(3, end);
}
// Frames whose interval ended by now_ take no more changes. Each one is the
// running level, through the DMG's high-pass filter (Pan Docs, Obscure
// Behavior), which also passes nothing while every DAC is off.
void Mixer::finish_frames() {
    const auto complete = static_cast<uint32_t>((base_ + (now_ - base_time_) * step_) >> 32);
    if (complete == first_) return;
    const bool dacs = dac(0) || dac(1) || dac(2) || dac(3);
    auto written = written_.load(std::memory_order_relaxed);
    // Frames from before a reset hold their slots until the reader skips
    // them: it may be copying them now.
    const auto read = read_.load(std::memory_order_acquire);
    uint64_t lost = 0;
    const auto scale = [](int32_t level) {
        // Q16 levels of +-480 become +-15360, leaving the filter headroom.
        return static_cast<uint16_t>(std::clamp<int32_t>(level >> 11, -32768, 32767));
    };
    const auto discharge = [this](int32_t level, int32_t charge) {
        return level - static_cast<int32_t>(int64_t{level - charge} * retain_ >> 30);
    };
    for (; first_ != complete; ++first_) {
        auto& change = changes_[first_ & (kPending - 1)];
        if (change[0] | change[1]) {
            sum_left_ += change[0]; sum_right_ += change[1];
            change = {0, 0};
            settled_ = false;
        }
        if (!dacs) {
            frame_ = 0;
            settled_ = false;
        } else if (!settled_) {
            const int32_t left = discharge(sum_left_, charge_left_), right = discharge(sum_right_, charge_right_);
            frame_ = scale(sum_left_ - charge_left_) | uint32_t{scale(sum_right_ - charge_right_)} << 16;
            settled_ = left == charge_left_ && right == charge_right_;
            charge_left_ = left; charge_right_ = right;
        }
        if (written - read == kOutputFrames) { ++lost; continue; }
        std::memcpy(output_.data() + (written % kOutputFrames) * 2, &frame_, 4);
        ++written;
    }
    written_.store(written, std::memory_order_release);
    if (lost) {
        dropped_.fetch_add(lost, std::memory_order_relaxed);
        lost_total_.fetch_add(static_cast<uint32_t>(lost), std::memory_order_relaxed);
    }
}
size_t Mixer::buffered() const {
    auto read = read_.load(std::memory_order_acquire);
    const auto reset_at = reset_at_.load(std::memory_order_relaxed);
    if (static_cast<int32_t>(reset_at - read) > 0) read = reset_at;
    // Mixing only moves frames from pending work to ready PCM. Only reads,
    // output overflow and reset-discarded PCM consume this reservation.
    return static_cast<uint32_t>(scheduled_ - read - lost_total_.load(std::memory_order_relaxed));
}
size_t Mixer::queued() const {
    const auto written = written_.load(std::memory_order_acquire);
    auto read = read_.load(std::memory_order_acquire);
    const auto reset_at = reset_at_.load(std::memory_order_relaxed);
    if (static_cast<int32_t>(reset_at - read) > 0) read = reset_at;
    return static_cast<size_t>(std::max<int32_t>(static_cast<int32_t>(written - read), 0));
}
size_t Mixer::read(std::span<int16_t> frames) {
    const auto written = written_.load(std::memory_order_acquire);
    auto read = read_.load(std::memory_order_relaxed);
    // Skip the frames from before a reset. Loaded after written_, reset_at_
    // is no older than those frames, but may be past them.
    const auto reset_at = reset_at_.load(std::memory_order_relaxed);
    if (static_cast<int32_t>(reset_at - read) > 0) read = reset_at;
    const auto available = static_cast<int32_t>(written - read);
    const size_t count = std::min<size_t>(std::max<int32_t>(available, 0), frames.size() / 2);
    for (size_t done = 0; done < count;) {
        const size_t at = read % kOutputFrames, part = std::min(count - done, kOutputFrames - at);
        std::memcpy(frames.data() + done * 2, output_.data() + at * 2, part * 4);
        done += part; read += static_cast<uint32_t>(part);
    }
    read_.store(read, std::memory_order_release);
    return count;
}
}  // namespace gbx::gb
