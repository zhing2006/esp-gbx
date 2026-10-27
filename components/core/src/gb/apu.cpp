#include "gbx/gb/apu.h"

#include <algorithm>
#include <new>

#include "gbx/gb/memory.h"
#include "mixer.h"

namespace gbx::gb {
namespace {
constexpr uint8_t kLength[]{0x11, 0x16, 0x1b, 0x20};
constexpr uint8_t kEnvelope[]{0x12, 0x17, 0x1a, 0x21};
constexpr uint8_t kControl[]{0x14, 0x19, 0x1e, 0x23};
constexpr uint16_t kLengthSteps[]{64, 64, 256, 64};
// The longest a batch waits for its run to end: 1/32 s. A frame's run is
// shorter, so the game hands its sound over once, at the end, and the
// worker mixes it while the CPU core waits for the next frame. A batch cut
// in the middle of the run held up the scanline rows behind it: mixing one
// takes longer than the JIT needs to fill the row queue.
constexpr Tick kFlushInterval = kTicksPerSecond / 32;
// The window in which a wave RAM access meets CH3's read, see wave_access().
constexpr Tick kWaveWindow = 2;
}  // namespace

Apu::Apu() = default;
Apu::~Apu() = default;
bool Apu::initialize(Offload& offload) {
    if (!mixer_) mixer_.reset(new (std::nothrow) Mixer(offload));
    return mixer_ != nullptr;
}
bool Apu::powered() const { return memory_->io[0x26] & 0x80; }

void Apu::reset(Memory& memory) {
    memory_ = &memory; clock_ = &memory.clock;
    const auto& io = memory.io;
    // Close a batch from before the reset without its entries: the mixer
    // starts over from the reset batch.
    if (batch_) { batch_->count = 0; submit(batch_->start); }
    last_end_ = now();
    edge_ = memory.next_div_edge(8192);
    step_ = 0;
    on_ = io[0x26] & 15;
    for (unsigned c = 0; c < 4; ++c)
        length_[c] = static_cast<uint16_t>(kLengthSteps[c] - (io[kLength[c]] & (c == 2 ? 255 : 63)));
    shadow_ = static_cast<uint16_t>(io[0x13] | (io[0x14] & 7) << 8);
    sweep_timer_ = 8; sweep_enabled_ = negated_ = false;
    wave_ = {};
    wave_.spacing = WaveTimer::ticks(io[0x1d] | (io[0x1e] & 7) << 8);
    reported_ = on_;
    flush_at_ = now() + kFlushInterval;
    if (!mixer_) return;
    batch_ = &mixer_->acquire();
    batch_->start = now(); batch_->count = 0; batch_->reset = true;
    for (unsigned a = 0x10; a < 0x40; ++a)
        if (a != 0x15 && a != 0x1f && (a < 0x27 || a >= 0x30))
            append(now(), sound::Event::write, static_cast<uint8_t>(a), io[a], sound::kLoad);
    append(now(), sound::Event::status, 0, on_);
    append_setting(sound::Event::model, memory.color() ? 1 : 0);
    // The batch closed above may have held a change of rate or speed. They
    // come after the registers, which the mixer then takes as settled.
    append_setting(sound::Event::rate, rate_);
    append_setting(sound::Event::speed, static_cast<uint32_t>(speed_));
}

void Apu::run_sequencer() {
    for (const Tick now = this->now(); edge_ <= now; edge_ += kStep) sequence(edge_);
}
// One DIV-APU step: length timers on even steps, the sweep on steps 2 and
// 6, envelopes on step 7 (Pan Docs, DIV-APU).
void Apu::sequence(Tick when) {
    const unsigned step = step_;
    step_ = (step_ + 1) & 7;
    if (!powered()) return;
    const auto& io = memory_->io;
    if (!(step & 1)) {
        for (unsigned c = 0; c < 4; ++c)
            if ((io[kControl[c]] & 0x40) && length_[c] && !--length_[c]) turn_off(c);
    }
    if (step == 2 || step == 6) clock_sweep(when);
    if (step == 7) append(when, sound::Event::envelope);
    report(when);
}
// Pan Docs, Pulse channel with sweep (CH1).
void Apu::clock_sweep(Tick when) {
    if (--sweep_timer_) return;
    auto& io = memory_->io;
    const unsigned pace = io[0x10] >> 4 & 7;
    sweep_timer_ = static_cast<uint8_t>(pace ? pace : 8);
    if (!sweep_enabled_ || !pace) return;
    const auto period = sweep_target();
    if (period > 2047) { turn_off(0); return; }
    if (!(io[0x10] & 7)) return;
    shadow_ = static_cast<uint16_t>(period);
    io[0x13] = static_cast<uint8_t>(period);
    io[0x14] = static_cast<uint8_t>((io[0x14] & 0xf8) | period >> 8);
    append(when, sound::Event::sweep, static_cast<uint8_t>(period >> 8), static_cast<uint8_t>(period));
    if (sweep_target() > 2047) turn_off(0);
}
unsigned Apu::sweep_target() {
    const auto nr10 = memory_->io[0x10];
    const unsigned delta = shadow_ >> (nr10 & 7);
    if (!(nr10 & 8)) return shadow_ + delta;
    negated_ = true;
    return shadow_ - delta;
}
void Apu::turn_off(unsigned channel) {
    on_ &= ~(1u << channel);
    if (channel == 2) wave_.stop();
}
// Status entries tell the mixer which channels are on, after the write or
// step that changed them.
void Apu::report(Tick when) {
    if (on_ == reported_) return;
    reported_ = on_;
    append(when, sound::Event::status, 0, on_);
}

// Pan Docs, Wave pattern RAM: while CH3 plays, the DMG reaches wave RAM only
// in the cycle in which CH3 reads it, and then the byte CH3 reads. A CGB
// always reaches the byte CH3 reads.
bool Apu::wave_access() {
    wave_.run(now());
    return memory_->color() || (wave_.last != kNever && now() - wave_.last < kWaveWindow);
}

uint8_t Apu::read(uint8_t address) {
    sync();
    const auto& io = memory_->io;
    if (address == 0x26) return static_cast<uint8_t>(0x70 | (io[0x26] & 0x80) | on_);
    if (address == 0x76 || address == 0x77) {
        // PCM12 and PCM34 read the channels' digital outputs (Pan Docs, PCM
        // registers), which only the mixer works out. It gets the sound up
        // to now and this waits for it; programs rarely read them.
        flush();
        finish();
        if (!mixer_) return 0;
        const auto digital = mixer_->digital();
        const unsigned first = address == 0x76 ? 0 : 2;
        return static_cast<uint8_t>(digital[first] | digital[first + 1] << 4);
    }
    if (!(on_ & 4)) return io[address];
    return wave_access() ? io[0x30 + wave_.position / 2] : 0xff;
}

void Apu::write(uint8_t address, uint8_t value) {
    sync();
    auto& io = memory_->io;
    const Tick now = this->now();
    if (address >= 0x30) {
        if (on_ & 4) {
            if (!wave_access()) return;
            address = static_cast<uint8_t>(0x30 + wave_.position / 2);
        }
        io[address] = value;
        append(now, sound::Event::write, address, value);
        return;
    }
    if (address == 0x26) {
        const bool was = powered();
        io[0x26] = value & 0x80;
        if (was && !(value & 0x80)) {
            // Power off clears the registers but leaves the DMG's length
            // timers (Pan Docs, NR52); a CGB's go with the registers.
            std::fill(io.begin() + 0x10, io.begin() + 0x26, uint8_t{0});
            on_ = 0; wave_.stop(); sweep_enabled_ = negated_ = false;
            if (memory_->color()) for (unsigned c = 0; c < 4; ++c) length_[c] = kLengthSteps[c];
        } else if (!was && (value & 0x80)) {
            // Power on resets the frame sequencer: its next step is step 0.
            step_ = 0;
        }
        append(now, sound::Event::write, 0x26, io[0x26]);
        report(now);
        return;
    }
    if (!powered()) {
        // The DMG's length timers stay writable while the APU is off (Pan
        // Docs, NR52); the duty bits do not. A CGB's do not either.
        if (memory_->color()) return;
        if (address == 0x11 || address == 0x16 || address == 0x20) {
            io[address] = value & 0x3f;
            length_[address == 0x11 ? 0 : address == 0x16 ? 1 : 3] = static_cast<uint16_t>(64 - (value & 63));
        } else if (address == 0x1b) {
            io[address] = value;
            length_[2] = static_cast<uint16_t>(256 - value);
        }
        return;
    }
    const auto old = io[address];
    io[address] = value;
    const auto channel = [](uint8_t a) { return a < 0x15 ? 0u : a < 0x1a ? 1u : a < 0x1f ? 2u : 3u; };
    const bool control = address == 0x14 || address == 0x19 || address == 0x1e || address == 0x23;
    append(now, sound::Event::write, address, value, control && step_ == 7 ? sound::kEnvelopeNext : 0);
    switch (address) {
    case 0x10:
        // Pan Docs, Obscure Behavior: leaving subtraction after a sweep
        // calculation used it since the trigger turns CH1 off.
        if (negated_ && !(value & 8)) turn_off(0);
        break;
    case 0x11: case 0x16: case 0x20:
        length_[channel(address)] = static_cast<uint16_t>(64 - (value & 63));
        break;
    case 0x1b: length_[2] = static_cast<uint16_t>(256 - value); break;
    case 0x12: case 0x17: case 0x21:
        if (!(value & 0xf8)) turn_off(channel(address));
        break;
    case 0x1a: if (!(value & 0x80)) turn_off(2); break;
    case 0x1d: case 0x1e:
        wave_.run(now);
        wave_.spacing = WaveTimer::ticks(io[0x1d] | (io[0x1e] & 7) << 8);
        break;
    default: break;
    }
    if (control) {
        const unsigned c = channel(address);
        // Pan Docs, Obscure Behavior: when the next step does not clock
        // length timers, enabling a timer clocks it once, and a trigger that
        // reloads an expired timer while it is enabled loads one step less.
        const bool odd = step_ & 1;
        if (odd && !(old & 0x40) && (value & 0x40) && length_[c] && !--length_[c] && !(value & 0x80)) turn_off(c);
        if (value & 0x80) trigger(c, value);
    }
    report(now);
}

void Apu::trigger(unsigned c, uint8_t value) {
    auto& io = memory_->io;
    if (c == 2 && (on_ & 4) && !memory_->color()) {
        // Pan Docs, Obscure Behavior: retriggering CH3 on the DMG as it reads
        // wave RAM copies the byte it reads over the first byte, or its
        // aligned four bytes over the first four. blargg's dmg_sound 10-wave
        // trigger while on fails unless the read is in the next 2 MHz cycle.
        wave_.run(now());
        if (wave_.next - now() <= 4) {
            const unsigned byte = ((wave_.position + 1) & 31) / 2;
            const unsigned first = byte < 4 ? byte : byte & ~3u, count = byte < 4 ? 1 : 4;
            for (unsigned i = 0; i < count; ++i) {
                io[0x30 + i] = io[0x30 + first + i];
                append(now(), sound::Event::write, static_cast<uint8_t>(0x30 + i), io[0x30 + i]);
            }
        }
    }
    const bool dac = io[kEnvelope[c]] & (c == 2 ? 0x80 : 0xf8);
    if (dac) on_ |= 1u << c;
    if (!length_[c]) length_[c] = static_cast<uint16_t>(kLengthSteps[c] - ((value & 0x40) && (step_ & 1) ? 1 : 0));
    if (c == 0) {
        shadow_ = static_cast<uint16_t>(io[0x13] | (io[0x14] & 7) << 8);
        const unsigned pace = io[0x10] >> 4 & 7;
        sweep_timer_ = static_cast<uint8_t>(pace ? pace : 8);
        sweep_enabled_ = pace || (io[0x10] & 7);
        negated_ = false;
        if ((io[0x10] & 7) && sweep_target() > 2047) turn_off(0);
    } else if (c == 2 && dac) wave_.trigger(now());
}

void Apu::append(Tick when, sound::Event event, uint8_t address, uint8_t value, uint8_t extra) {
    if (!mixer_) return;
    if (!batch_) open();
    if (batch_->count == sound::Batch::kEntries || when - batch_->start > UINT32_MAX) {
        submit(when);
        open();
    }
    batch_->entries[batch_->count++] = {static_cast<uint32_t>(when - batch_->start), event, address, value, extra};
}
void Apu::open() {
    batch_ = &mixer_->acquire();
    batch_->start = last_end_; batch_->count = 0; batch_->reset = false;
}
void Apu::submit(Tick end) {
    batch_->end = end;
    mixer_->submit(*batch_);
    last_end_ = end;
    batch_ = nullptr;
}
void Apu::flush() {
    sync();
    flush_at_ = now() + kFlushInterval;
    if (!mixer_) return;
    if (!batch_) open();
    submit(now());
}

// Both first bring in the sequencer steps already passed, which keeps the
// entries of a batch in time order.
void Apu::set_rate(uint32_t rate) {
    sync();
    rate_ = std::min<uint32_t>(rate, 0xffffff);
    append_setting(sound::Event::rate, rate_);
}
void Apu::set_speed(int32_t ppm) {
    sync();
    // A small correction for audio buffer levels, not a speed control.
    speed_ = std::clamp<int32_t>(ppm, -kMaxSpeed, kMaxSpeed);
    append_setting(sound::Event::speed, static_cast<uint32_t>(speed_));
}
// A 24-bit value, its high byte in extra.
void Apu::append_setting(sound::Event event, uint32_t value) {
    append(now(), event, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 16));
}
size_t Apu::read_frames(std::span<int16_t> frames) { return mixer_ ? mixer_->read(frames) : 0; }
size_t Apu::queued() const { return mixer_ ? mixer_->queued() : 0; }
size_t Apu::buffered() const { return mixer_ ? mixer_->buffered() : 0; }
void Apu::finish() { if (mixer_) mixer_->finish(); }
uint64_t Apu::dropped() const { return mixer_ ? mixer_->dropped() : 0; }
}  // namespace gbx::gb
