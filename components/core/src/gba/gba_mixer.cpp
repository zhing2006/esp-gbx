// The GBA's mixer on the worker of the other core, see mixer.h.

#include "mixer.h"

#include <algorithm>
#include <cstring>

#include "gbx/gba/ppu.h"

namespace gbx::gba {
namespace {

// Duty waveforms with step 0 in bit 0, as GBATEK "GBA Sound Channel 1"
// draws them: 12.5, 25, 50 and 75 % high from the first step.
constexpr uint8_t kDuty[]{0b0000'0001, 0b0000'0011, 0b0000'1111, 0b0011'1111};
// Each channel's envelope register, as an I/O offset.
constexpr uint8_t kEnvelope[]{0x63, 0x69, 0x00, 0x79};
// GBATEK "GBA Sound Control Registers", SOUNDCNT_H bits 0-1: the PSG at 25,
// 50 or 100 %, in quarters; 3 is "prohibited", taken here as 100 %.
constexpr int32_t kPsgQuarters[]{1, 2, 4, 4};

// GBATEK "GBA Sound Channel 1": 131072 / (2048 - n) Hz for eight duty
// steps, so a step every (2048 - n) * 16 cycles; channel 3 reads a digit
// at 2097152 / (2048 - n) Hz, every (2048 - n) * 8 cycles.
uint32_t square_cycles(unsigned frequency) { return (2048 - (frequency & 2047)) * 16; }
uint32_t wave_cycles(unsigned frequency) { return (2048 - (frequency & 2047)) * 8; }
// GBATEK "GBA Sound Channel 4": 524288 Hz / r / 2^(s + 1), r = 0 taken as
// 0.5, so a shift every 64 * r << s cycles, or 32 << s.
uint32_t noise_cycles(uint8_t nr43) {
    const unsigned r = nr43 & 7;
    return (r ? r * 64 : 32) << (nr43 >> 4);
}

}  // namespace

Mixer::Mixer(gb::Offload& offload) : offload_(offload) {
    offload_.attach(gb::Offload::sound, kBatches, consume, this);
}

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
        scheduled_step_ = sound::frames_per_cycle(scheduled_rate_, scheduled_ppm_);
    }
    advance_to(batch.end);
    scheduled_ += static_cast<uint32_t>(scheduled_position_ >> 32) - first;
    // Account for the whole batch before the worker can publish any PCM.
    offload_.submit(gb::Offload::sound, true);
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
    regs_.fill(0);
    for (auto& bank : wave_ram_) bank.fill(0);
    on_ = 0;
    squares_ = {};
    wave_ = {};
    sample_ = 0;
    noise_ = {};
    envelopes_ = {};
    digital_.fill(0);
    fifo_.fill(0);
    level_left_ = level_right_ = held_left_ = held_right_ = 0;
    sample_at_ = kNever;
    now_ = base_time_ = start;
    base_ = 0;
    first_ = 0;
    // The reset batch sends the rate and speed again after the registers.
    rate_ = 0;
    ppm_ = 0;
    step_ = 0;
    chunk_ = 0;
    for (auto& change : changes_) change = {0, 0};
    sum_left_ = sum_right_ = 0;
    // Frames not read yet belong to the run before.
    reset_at_.store(written_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
}

void Mixer::configure(Tick when) {
    base_ += (when - base_time_) * step_;
    base_time_ = when;
    const bool was = step_ != 0;
    step_ = sound::frames_per_cycle(rate_, ppm_);
    chunk_ = step_ ? (uint64_t{kPending - 4} << 32) / step_ : 0;
    if (was && !step_) {
        // Frames stop: the changes still open go into the output, which
        // then follows every change at once (see change()).
        for (auto& change : changes_) {
            sum_left_ += change[0];
            sum_right_ += change[1];
            change = {0, 0};
        }
    }
}

void Mixer::apply(const sound::Entry& entry, Tick when) {
    switch (entry.event) {
    case sound::Event::write:
        if (entry.extra & sound::kLoad) {
            regs_[entry.address - 0x60] = entry.value;
            squares_[0].spacing = square_cycles(reg(0x64) | (reg(0x65) & 7) << 8);
            squares_[1].spacing = square_cycles(reg(0x6c) | (reg(0x6d) & 7) << 8);
            wave_.spacing = wave_cycles(reg(0x74) | (reg(0x75) & 7) << 8);
            noise_.spacing = noise_cycles(reg(0x7c));
            for (unsigned c = 0; c < 4; ++c) output(c, when);
            remix(when);
        } else {
            write(entry.address, entry.value, when);
        }
        break;
    case sound::Event::wave:
        wave_ram_[(entry.address >> 4) & 1][entry.address & 15] = entry.value;
        break;
    case sound::Event::status: {
        const auto changed = on_ ^ entry.value;
        on_ = entry.value;
        for (unsigned c = 0; c < 4; ++c) {
            if (!(changed >> c & 1) || (on_ >> c & 1)) continue;
            // A channel that turns off stops its timer; a trigger starts it.
            if (c < 2) squares_[c].next = kNever;
            else if (c == 2) wave_.next = kNever;
            else noise_.next = kNever;
        }
        remix(when);
        break;
    }
    case sound::Event::sweep: {
        const auto frequency = entry.value24();
        regs_[0x64 - 0x60] = static_cast<uint8_t>(frequency);
        regs_[0x65 - 0x60] = static_cast<uint8_t>((reg(0x65) & 0xf8) | frequency >> 8);
        squares_[0].spacing = square_cycles(frequency);
        break;
    }
    case sound::Event::envelope:
        for (unsigned c : {0u, 1u, 3u}) {
            auto& e = envelopes_[c];
            const auto nrx2 = reg(kEnvelope[c]);
            const unsigned pace = nrx2 & 7;
            if (!pace || !e.running || --e.timer) continue;
            e.timer = static_cast<uint8_t>(pace);
            if (nrx2 & 8 ? e.volume == 15 : e.volume == 0) {
                e.running = false;
                continue;
            }
            e.volume = static_cast<uint8_t>(e.volume + (nrx2 & 8 ? 1 : -1));
            output(c, when);
        }
        break;
    case sound::Event::sample:
        fifo_[entry.address & 1] = static_cast<int8_t>(entry.value);
        remix(when);
        break;
    case sound::Event::rate:
        rate_ = entry.value24();
        configure(when);
        break;
    case sound::Event::speed:
        ppm_ = static_cast<int32_t>(entry.value24() << 8) >> 8;
        configure(when);
        break;
    }
}

// A byte written to a sound register. The CPU's core keeps the channels'
// on flags, length timers and sweep; triggers start the timers here.
void Mixer::write(unsigned offset, uint8_t value, Tick when) {
    const uint8_t old = reg(offset);
    regs_[offset - 0x60] = value;
    const auto start_envelope = [&](unsigned c) {
        auto& e = envelopes_[c];
        const auto nrx2 = reg(kEnvelope[c]);
        e.volume = nrx2 >> 4;
        e.timer = static_cast<uint8_t>((nrx2 & 7) ? nrx2 & 7 : 8);
        e.running = true;
    };
    switch (offset) {
    case 0x62: output(0, when); break;
    case 0x68: output(1, when); break;
    case 0x64: case 0x65: case 0x6c: case 0x6d: {
        const unsigned c = offset >= 0x6c;
        const unsigned low = c ? 0x6c : 0x64;
        auto& square = squares_[c];
        square.spacing = square_cycles(reg(low) | (reg(low + 1) & 7) << 8);
        // A trigger starts only a channel whose DAC is on; the status entry
        // that follows turns it on.
        if (offset == low + 1 && (value & 0x80) && (reg(kEnvelope[c]) & 0xf8)) {
            square.next = when + square.spacing;
            start_envelope(c);
            output(c, when);
        }
        break;
    }
    case 0x70: case 0x73: output(2, when); break;
    case 0x74: case 0x75:
        wave_.spacing = wave_cycles(reg(0x74) | (reg(0x75) & 7) << 8);
        if (offset == 0x75 && (value & 0x80) && (reg(0x70) & 0x80)) trigger_wave(when);
        break;
    case 0x7c:
        noise_.spacing = noise_cycles(value);
        break;
    case 0x7d:
        // GBATEK "GBA Sound Channel 4": a restart sets the shift register to
        // 40h in 7-bit mode and to 4000h in 15-bit mode.
        if ((value & 0x80) && (reg(0x79) & 0xf8)) {
            noise_.lfsr = reg(0x7c) & 8 ? 0x40 : 0x4000;
            noise_.high = false;
            noise_.next = when + noise_.spacing;
            start_envelope(3);
            output(3, when);
        }
        break;
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x88: case 0x89: remix(when); break;
    case 0x84:
        if (!(value & 0x80)) {
            // Off: the PSG's registers read zero and its output stops
            // (GBATEK, SOUNDCNT_X); wave RAM and 82h, 88h stay.
            std::memset(regs_.data(), 0, 0x22);
            envelopes_ = {};
            for (unsigned c = 0; c < 4; ++c) output(c, when);
        } else if (!(old & 0x80)) {
            for (auto& square : squares_) square.step = 0;
            sample_ = 0;
        }
        remix(when);
        break;
    default: break;
    }
}

// Starts channel 3 at the first digit of the selected bank: GBATEK "GBA
// Sound Channel 3", with two banks "output will start by replaying the
// currently selected bank".
void Mixer::trigger_wave(Tick when) {
    wave_.index = 0;
    wave_.next = when + wave_.spacing;
}

// Digit `index` of what channel 3 plays: the selected bank's 32, or with
// the dimension bit set 64 from the selected bank on (GBATEK, NR30). A
// byte holds two digits, the high nibble first.
uint8_t Mixer::wave_digit(unsigned index) const {
    const unsigned bank = ((reg(0x70) >> 6) + (index >> 5)) & 1, digit = index & 31;
    const uint8_t byte = wave_ram_[bank][digit >> 1];
    return digit & 1 ? byte & 15 : byte >> 4;
}

unsigned Mixer::square_output(unsigned c) const {
    const auto& square = squares_[c];
    return (kDuty[reg(c ? 0x68 : 0x62) >> 6] >> square.step & 1) ? envelopes_[c].volume : 0;
}

// Recomputes a channel's digital output after something it depends on
// changed. Channel 3's volume: GBATEK NR32, 100, 50 or 25 %, or 75 % with
// the force bit.
void Mixer::output(unsigned c, Tick when) {
    unsigned digital;
    if (c < 2) {
        digital = square_output(c);
    } else if (c == 2) {
        const uint8_t nr32 = reg(0x73);
        const unsigned shift = (nr32 >> 5) & 3;
        digital = nr32 & 0x80 ? (sample_ * 3u) >> 2 : shift ? sample_ >> (shift - 1) : 0;
    } else {
        digital = noise_.high ? envelopes_[3].volume : 0;
    }
    if (digital == digital_[c]) return;
    digital_[c] = static_cast<uint8_t>(digital);
    remix(when);
}

// The sum changed at `when`: the next sampling point takes it, see
// mixer.h. Sampling points are multiples of 512 cycles at 32768 Hz, and
// shorter at the higher rates of SOUNDBIAS bits 14-15.
void Mixer::remix(Tick when) {
    if (sample_at_ != kNever) return;
#ifdef GBX_GBA_RELAXED_TIMING
    // Relaxed: 32768 Hz, the default, at every rate; the amplitude
    // resolution still follows SOUNDBIAS (held()).
    const unsigned shift = 9;
#else
    const unsigned shift = 9 - (reg(0x89) >> 6);
#endif
    sample_at_ = ((when >> shift) + 1) << shift;
}

// The PWM stage's output for a sum: GBATEK "Max Output Levels", the bias
// added and the result clipped to 0-3FFh, then "Amplitude Resolution": 9
// bits at 32768 Hz down to 6 bits at 262144 Hz. Taken against the bias.
int32_t Mixer::held(int32_t level) const {
    const auto soundbias = static_cast<uint16_t>(reg(0x88) | reg(0x89) << 8);
    const int32_t bias = soundbias & 0x3fe;
    const int32_t value = std::clamp<int32_t>((level >> 2) + bias, 0, 0x3ff) & ~((2 << (soundbias >> 14)) - 1);
    return value - bias;
}

// A sampling point: the sum now, through the PWM stage.
void Mixer::settle(Tick when) {
    sample_at_ = kNever;
    int32_t left = 0, right = 0;
    if (reg(0x84) & 0x80) {
        const uint8_t volumes = reg(0x80), routes = reg(0x81), control = reg(0x82), fifos = reg(0x83);
        const int32_t psg = kPsgQuarters[control & 3];
        const int32_t right_volume = (volumes & 7) + 1, left_volume = ((volumes >> 4) & 7) + 1;
        for (unsigned c = 0; c < 4; ++c) {
            if (!(on_ >> c & 1)) continue;
            const int32_t a = (2 * digital_[c] - 15) * psg;
            if (routes >> c & 1) right += a * right_volume;
            if (routes >> (c + 4) & 1) left += a * left_volume;
        }
        // GBATEK SOUNDCNT_H: each FIFO at 50 or 100 %, 100 % spanning +-200h.
        const int32_t a = fifo_[0] * (control & 4 ? 16 : 8), b = fifo_[1] * (control & 8 ? 16 : 8);
        if (fifos & 0x01) right += a;
        if (fifos & 0x02) left += a;
        if (fifos & 0x10) right += b;
        if (fifos & 0x20) left += b;
    }
    level_left_ = left;
    level_right_ = right;
    const int32_t l = held(left), r = held(right);
    if (l == held_left_ && r == held_right_) return;
    change(when, l - held_left_, r - held_right_);
    held_left_ = l;
    held_right_ = r;
}

// An output change at `when` counts in its frame for the part of the
// frame's interval after it, and fully in every later frame. Without
// frames, it goes into the output at once.
void Mixer::change(Tick when, int32_t left, int32_t right) {
    if (!step_) {
        sum_left_ += left * 65536;
        sum_right_ += right * 65536;
        return;
    }
    const uint64_t position = base_ + (when - base_time_) * step_;
    const auto frame = static_cast<uint32_t>(position >> 32);
    const auto after = static_cast<int32_t>(position >> 16 & 0xffff);
    auto& here = changes_[frame & (kPending - 1)];
    auto& next = changes_[(frame + 1) & (kPending - 1)];
    here[0] += left * (65536 - after);
    next[0] += left * after;
    here[1] += right * (65536 - after);
    next[1] += right * after;
}

void Mixer::advance(Tick to) {
    while (now_ < to) {
        const Tick end = step_ ? std::min(to, now_ + chunk_) : to;
        run(end);
        now_ = end;
        if (step_) finish_frames();
    }
}

// Steps the channels through (now_, end] and takes the sampling points in
// between, in time order: a point takes the sum as the steps before it
// left it. A channel that cannot change the sum until the next entry, off
// or silent or routed nowhere, steps at once without sampling.
void Mixer::run(Tick end) {
    const bool master = reg(0x84) & 0x80;
    const uint8_t routes = reg(0x81);
    const auto routed = [&](unsigned c) { return master && (on_ >> c & 1) && ((routes >> c | routes >> (c + 4)) & 1); };
    uint8_t live = 0;
    for (unsigned c = 0; c < 2; ++c) {
        auto& square = squares_[c];
        if (square.next > end) continue;
        if (routed(c) && envelopes_[c].volume) {
            live |= 1u << c;
            continue;
        }
        const Tick steps = (end - square.next) / square.spacing + 1;
        square.step = static_cast<uint8_t>((square.step + steps) & 7);
        square.next += steps * square.spacing;
        digital_[c] = static_cast<uint8_t>(square_output(c));
    }
    const unsigned digits = reg(0x70) & 0x20 ? 64 : 32;
    if (wave_.next <= end) {
        if (routed(2) && (reg(0x73) & 0xe0)) {
            live |= 4;
        } else {
            const Tick reads = (end - wave_.next) / wave_.spacing + 1;
            sample_ = wave_digit(static_cast<unsigned>((wave_.index + reads - 1) % digits));
            wave_.index = static_cast<uint8_t>((wave_.index + reads) % digits);
            wave_.next += reads * wave_.spacing;
            const uint8_t nr32 = reg(0x73);
            const unsigned shift = (nr32 >> 5) & 3;
            digital_[2] = static_cast<uint8_t>(nr32 & 0x80 ? (sample_ * 3u) >> 2 : shift ? sample_ >> (shift - 1) : 0);
        }
    }
    const uint16_t taps = reg(0x7c) & 8 ? 0x60 : 0x6000;
    if (noise_.next <= end) {
        if (routed(3) && envelopes_[3].volume) {
            live |= 8;
        } else {
            // The shift register's state carries on to when it is heard.
            for (; noise_.next <= end; noise_.next += noise_.spacing) {
                noise_.high = noise_.lfsr & 1;
                noise_.lfsr = static_cast<uint16_t>((noise_.lfsr >> 1) ^ (noise_.high ? taps : 0));
            }
            digital_[3] = noise_.high ? envelopes_[3].volume : 0;
        }
    }
    for (;;) {
        // The next step of channels 1-3, then of the noise, which comes
        // after them at the same time.
        Tick others = kNever;
        unsigned which = 4;
        for (unsigned c = 0, m = live & 7; m; ++c, m >>= 1) {
            if (!(m & 1)) continue;
            const Tick at = c < 2 ? squares_[c].next : wave_.next;
            if (at < others) {
                others = at;
                which = c;
            }
        }
        Tick next = others;
        if ((live & 8) && noise_.next < next) {
            next = noise_.next;
            which = 3;
        }
        if (sample_at_ <= end && sample_at_ <= next) {
            settle(sample_at_);
            continue;
        }
        if (next > end) break;
        if (which < 2) {
            auto& square = squares_[which];
            square.step = (square.step + 1) & 7;
            square.next += square.spacing;
            output(which, next);
        } else if (which == 2) {
            sample_ = wave_digit(wave_.index);
            wave_.index = static_cast<uint8_t>((wave_.index + 1) % digits);
            wave_.next += wave_.spacing;
            output(2, next);
        } else {
            // The noise's shifts before the next step of another channel,
            // the sampling point and past the end, at once, as output()
            // would take them one by one: a change moves the sampling
            // point (remix()), which ends the run of shifts.
            const uint8_t volume = envelopes_[3].volume;
            Tick stop = std::min({others, sample_at_, end + 1});
            do {
                noise_.high = noise_.lfsr & 1;
                noise_.lfsr = static_cast<uint16_t>((noise_.lfsr >> 1) ^ (noise_.high ? taps : 0));
                const uint8_t digital = noise_.high ? volume : 0;
                if (digital != digital_[3]) {
                    digital_[3] = digital;
                    remix(noise_.next);
                    stop = std::min(stop, sample_at_);
                }
                noise_.next += noise_.spacing;
            } while (noise_.next < stop);
        }
    }
}

// Frames whose interval ended by now_ take no more changes.
void Mixer::finish_frames() {
    const auto complete = static_cast<uint32_t>((base_ + (now_ - base_time_) * step_) >> 32);
    if (complete == first_) return;
    auto written = written_.load(std::memory_order_relaxed);
    // Frames from before a reset hold their slots until the reader skips
    // them: it may be copying them now.
    const auto read = read_.load(std::memory_order_acquire);
    uint64_t lost = 0;
    // Q16 outputs of +-3FFh: 64 per step of the PWM stage's 10 bits.
    const auto scale = [](int32_t sum) { return static_cast<uint16_t>(std::clamp<int32_t>(sum >> 10, -32768, 32767)); };
    for (; first_ != complete; ++first_) {
        auto& change = changes_[first_ & (kPending - 1)];
        sum_left_ += change[0];
        sum_right_ += change[1];
        change = {0, 0};
        if (written - read == kOutputFrames) {
            ++lost;
            continue;
        }
        const uint32_t frame = scale(sum_left_) | uint32_t{scale(sum_right_)} << 16;
        std::memcpy(output_.data() + (written % kOutputFrames) * 2, &frame, 4);
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
        done += part;
        read += static_cast<uint32_t>(part);
    }
    read_.store(read, std::memory_order_release);
    return count;
}

}  // namespace gbx::gba
