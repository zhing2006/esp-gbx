// The sound registers and the Direct Sound FIFOs on the CPU's core (GBATEK
// "GBA Sound Controller"): what reads observe, and the batches for the mixer
// on the other core (gba_mixer.cpp).

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>

#include "gbx/gba/memory.h"
#include "../gb/offload.h"
#include "mixer.h"
#include "sound.h"

namespace gbx::gba {
namespace {

// The PSG's sequencer steps at 512 Hz (GBATEK: lengths in 1/256 s, the
// sweep in 1/128 s, envelopes in 1/64 s). GBATEK does not give its phase.
// Here it counts from power-on, as the timers' prescaler does, and the
// BIOS's start-up, which reset() skips, takes 75998008 cycles (see
// gba_memory.cpp): 9016 into a step.
constexpr Tick kStep = 32768;
constexpr Tick kStepPhase = 75998008 % kStep;
constexpr unsigned kLengthSteps[]{64, 64, 256, 64};
// Each channel's envelope (channel 3: its DAC) and control registers, as
// I/O offsets.
constexpr uint8_t kEnvelope[]{0x63, 0x69, 0x70, 0x79};
constexpr uint8_t kControl[]{0x65, 0x6d, 0x75, 0x7d};

Tick next_step(Tick after) { return ((after + kStepPhase) / kStep + 1) * kStep - kStepPhase; }

}  // namespace

void Memory::set_worker_polling(bool polling) {
    if (offload_) offload_->set_polling(polling);
}

// --- Power-on ---------------------------------------------------------------------

void Memory::reset_sound() {
    // A batch from before the reset closes without its entries: the mixer
    // starts over from the reset batch.
    if (batch_) {
        batch_->count = 0;
        submit_batch(batch_->start);
    }
    sound_end_ = clock.now;
    psg_ = {};
    psg_.edge = next_step(clock.now);
    for (unsigned c = 0; c < 4; ++c) psg_.length[c] = static_cast<uint16_t>(kLengthSteps[c]);
    wave_played_.fill(0);
    if (!mixer_) return;
    open_batch();
    batch_->reset = true;
    for (unsigned offset = 0x60; offset < 0x8a; ++offset)
        append(clock.now, sound::Event::write, static_cast<uint8_t>(offset), io_[offset], sound::kLoad);
    const unsigned visible = (~io_[0x70] >> 6) & 1;
    for (unsigned i = 0; i < 16; ++i) {
        append(clock.now, sound::Event::wave, static_cast<uint8_t>(visible * 16 + i), io_[0x90 + i]);
        append(clock.now, sound::Event::wave, static_cast<uint8_t>((visible ^ 1) * 16 + i), wave_played_[i]);
    }
    append(clock.now, sound::Event::status, 0, 0);
    append_setting(sound::Event::rate, audio_rate_);
    append_setting(sound::Event::speed, static_cast<uint32_t>(audio_speed_));
}

// --- The sequencer and the FIFOs --------------------------------------------------

void Memory::catch_up_sound(Tick to) {
    while (psg_.edge <= to) {
        catch_up_fifos(psg_.edge);
        sequence(psg_.edge);
        psg_.edge += kStep;
    }
    catch_up_fifos(to);
}

// One step: length timers on even steps, the sweep on steps 2 and 6,
// envelopes on step 7, as on the CGB (Pan Docs, DIV-APU), whose units
// GBATEK keeps.
void Memory::sequence(Tick when) {
    const unsigned step = psg_.step;
    psg_.step = (psg_.step + 1) & 7;
    if (!sound_on()) return;
    if (!(step & 1)) {
        for (unsigned c = 0; c < 4; ++c)
            if ((io_[kControl[c]] & 0x40) && psg_.length[c] && !--psg_.length[c]) turn_off(c);
    }
    if (step == 2 || step == 6) clock_sweep(when);
    if (step == 7) append(when, sound::Event::envelope);
    report(when);
}

// GBATEK "GBA Sound Channel 1": X(t) = X(t-1) +/- X(t-1) / 2^n each sweep
// time; a frequency past 2047 stops the channel.
void Memory::clock_sweep(Tick when) {
    if (--psg_.sweep_timer) return;
    const unsigned pace = io_[0x60] >> 4 & 7;
    psg_.sweep_timer = static_cast<uint8_t>(pace ? pace : 8);
    if (!psg_.sweep_enabled || !pace) return;
    const unsigned frequency = sweep_target();
    if (frequency > 2047) {
        turn_off(0);
        return;
    }
    if (!(io_[0x60] & 7)) return;
    psg_.shadow = static_cast<uint16_t>(frequency);
    io_[0x64] = static_cast<uint8_t>(frequency);
    io_[0x65] = static_cast<uint8_t>((io_[0x65] & 0xf8) | frequency >> 8);
    append(when, sound::Event::sweep, static_cast<uint8_t>(frequency >> 8), static_cast<uint8_t>(frequency));
    if (sweep_target() > 2047) turn_off(0);
}

unsigned Memory::sweep_target() {
    const uint8_t nr10 = io_[0x60];
    const unsigned delta = psg_.shadow >> (nr10 & 7);
    if (!(nr10 & 8)) return psg_.shadow + delta;
    psg_.negated = true;
    return psg_.shadow - delta;
}

void Memory::turn_off(unsigned channel) { psg_.on &= static_cast<uint8_t>(~(1u << channel)); }

void Memory::report(Tick when) {
    if (psg_.on == psg_.reported) return;
    psg_.reported = psg_.on;
    append(when, sound::Event::status, 0, psg_.on);
}

// GBATEK SOUNDCNT_X: bits 0-3 set when a channel starts and clear when its
// length expires.
uint16_t Memory::sound_status() {
    catch_up_sound(clock.now);
    return static_cast<uint16_t>((io_[0x84] & 0x80) | psg_.on);
}

void Memory::write_fifo(unsigned which, unsigned bytes, uint32_t value) {
    // With the sound off a write loads nothing (alyosha fifo_dma/fifo).
    if (!sound_on()) return;
    catch_up_sound(clock.now);
    auto& f = fifo_[which];
    // A halfword write puts two samples in, a byte write one; the low
    // address goes first. The count wraps at 32: after 8 words the first
    // overflow asks for a DMA as if the FIFO were empty (alyosha
    // fifo_dma/fifo_5).
    for (unsigned i = 0; i < bytes; ++i) {
        f.data[(f.read + f.size) & 31] = static_cast<uint8_t>(value >> (8 * i));
        f.size = static_cast<uint8_t>((f.size + 1) & 31);
    }
}

// With the sound on, overflows of timer 0 or 1 take a sample from each FIFO
// that uses the timer; a FIFO that held fewer than 16 bytes before the
// sample left asks its DMA for more. GBATEK "GBA Sound Channel A and B" puts
// the line at 16 bytes; with 4 words written, the first overflow asks for
// nothing and the second does (alyosha fifo_dma/fifo_4). This happens
// whether the FIFO goes to the speakers or not (alyosha fifo_dma/fifo). An
// empty FIFO takes nothing, and its output stays at the last sample. The
// samples go to the mixer in time order, which cannot go back.
void Memory::fifo_overflow(unsigned timer, uint64_t count, Tick first, Tick period) {
    const unsigned fifos = fifos_of(timer);
    unsigned holding = fifos_holding(fifos);
    for (uint64_t i = 0; i < count && holding; ++i) holding = fifo_samples(holding, first + i * period);
    fifo_requests(fifos);
}

unsigned Memory::fifos_of(unsigned timer) const {
    const uint16_t sound = io16(0x82);
    return static_cast<unsigned>(((sound >> 10) & 1) == timer) | static_cast<unsigned>(((sound >> 14) & 1) == timer) << 1;
}

unsigned Memory::fifos_holding(unsigned fifos) const {
    return fifos & (static_cast<unsigned>(fifo_[0].size != 0) | static_cast<unsigned>(fifo_[1].size != 0) << 1);
}

unsigned Memory::fifo_samples(unsigned fifos, Tick when) {
    for (unsigned rest = fifos; rest; rest &= rest - 1) {
        const unsigned which = static_cast<unsigned>(std::countr_zero(rest));
        auto& f = fifo_[which];
        const auto sample = static_cast<int8_t>(f.data[f.read]);
        f.read = static_cast<uint8_t>((f.read + 1) & 31);
        if (!--f.size) fifos &= ~(1u << which);
        if (sample == f.sample) continue;
        f.sample = sample;
        // Without an output rate (2X) the mixer makes no sound and hears of
        // no sample: set_audio_rate() tells it the current ones.
        if (audio_rate_) append(when, sound::Event::sample, static_cast<uint8_t>(which), static_cast<uint8_t>(sample));
    }
    return fifos;
}

void Memory::fifo_requests(unsigned fifos) {
    for (unsigned which = 0; which < 2; ++which) {
        // Fewer than 16 before the last sample left; 0 when empty.
        if (!(fifos >> which & 1) || fifo_[which].size >= 15) continue;
        const uint32_t address = which ? 0x040000a4 : 0x040000a0;
        unsigned channels = 0;
        for (unsigned c = 1; c <= 2; ++c)
            if (dma_[c].destination == address) channels |= 1u << c;
        if (channels) trigger_dma(3, channels);
    }
}

// --- Registers --------------------------------------------------------------------

// GBATEK "GBA Sound Control Registers": with bit 7 of SOUNDCNT_X clear the
// PSG registers 60h-81h are zero and ignore writes; SOUNDCNT_H, SOUNDBIAS
// and the wave RAM stay writable. A halfword is written a byte at a time,
// the low one first, so that a byte write to a register's low half does not
// repeat the trigger bit of its high half.
void Memory::write_sound(uint32_t offset, uint16_t value, uint16_t mask) {
    catch_up_sound(clock.now);
    for (unsigned i = 0; i < 2; ++i) {
        if (!((mask >> (8 * i)) & 0xff)) continue;
        const unsigned at = offset + i;
        const auto byte = static_cast<uint8_t>(value >> (8 * i));
        if (at >= 0x90) {
            // The bank channel 3 does not play (GBATEK, NR30).
            io_[at] = byte;
            const unsigned bank = (~io_[0x70] >> 6) & 1;
            append(clock.now, sound::Event::wave, static_cast<uint8_t>(bank * 16 + at - 0x90), byte);
        } else {
            write_psg(at, byte);
        }
    }
    report(clock.now);
}

void Memory::write_psg(unsigned offset, uint8_t value) {
    const Tick now = clock.now;
    switch (offset) {
    case 0x82:
        io_[0x82] = value & 0x0f;
        append(now, sound::Event::write, 0x82, io_[0x82]);
        fifo_quiet_until_ = 0;
        schedule_timers();
        return;
    case 0x83:
        // Bits 11 and 15 reset FIFO A and B and read 0.
        if (value & 0x08) fifo_[0].read = fifo_[0].size = 0;
        if (value & 0x80) fifo_[1].read = fifo_[1].size = 0;
        io_[0x83] = value & 0x77;
        append(now, sound::Event::write, 0x83, io_[0x83]);
        fifo_quiet_until_ = 0;
        schedule_timers();
        return;
    case 0x84: {
        const bool was = sound_on();
        if (!(value & 0x80)) {
            // NR30 clears with the rest: bank 0 plays, and the CPU reaches 1.
            if (io_[0x70] & 0x40) {
                uint8_t swapped[16];
                std::memcpy(swapped, io_.data() + 0x90, 16);
                std::memcpy(io_.data() + 0x90, wave_played_.data(), 16);
                std::memcpy(wave_played_.data(), swapped, 16);
            }
            std::memset(io_.data() + 0x60, 0, 0x22);
            // The FIFOs empty too (alyosha fifo_dma/fifo).
            fifo_[0].read = fifo_[0].size = 0;
            fifo_[1].read = fifo_[1].size = 0;
            psg_.on = 0;
            psg_.sweep_enabled = psg_.negated = false;
            for (unsigned c = 0; c < 4; ++c) psg_.length[c] = static_cast<uint16_t>(kLengthSteps[c]);
        } else if (!was) {
            // Power on: the sequencer's next step is step 0, as on the CGB.
            psg_.step = 0;
        }
        io_[0x84] = value & 0x80;
        append(now, sound::Event::write, 0x84, io_[0x84]);
        fifo_quiet_until_ = 0;
        schedule_timers();
        return;
    }
    case 0x88: case 0x89:
        io_[offset] = value;
        append(now, sound::Event::write, static_cast<uint8_t>(offset), value);
        return;
    case 0x85: case 0x86: case 0x87: case 0x8a: case 0x8b: case 0x8c: case 0x8d: case 0x8e: case 0x8f:
        io_[offset] = value;
        return;
    default: break;
    }
    // 60h-81h: the PSG, off with the sound.
    if (!sound_on()) return;
    switch (offset) {
    case 0x61: case 0x66: case 0x67: case 0x6a: case 0x6b: case 0x6e: case 0x6f: case 0x71: case 0x76: case 0x77:
    case 0x7a: case 0x7b: case 0x7e: case 0x7f:
        io_[offset] = value;
        return;
    default: break;
    }
    const uint8_t old = io_[offset];
    if (offset == 0x70 && ((old ^ value) & 0x40)) {
        // The other bank becomes the one the CPU reaches.
        uint8_t swapped[16];
        std::memcpy(swapped, io_.data() + 0x90, 16);
        std::memcpy(io_.data() + 0x90, wave_played_.data(), 16);
        std::memcpy(wave_played_.data(), swapped, 16);
    }
    io_[offset] = value;
    append(now, sound::Event::write, static_cast<uint8_t>(offset), value);
    const auto channel = [](unsigned o) { return o < 0x68 ? 0u : o < 0x70 ? 1u : o < 0x78 ? 2u : 3u; };
    switch (offset) {
    case 0x60:
        // Leaving subtraction after a sweep calculation used it since the
        // trigger turns channel 1 off, as on the CGB (Pan Docs, Obscure
        // Behavior).
        if (psg_.negated && !(value & 8)) turn_off(0);
        break;
    case 0x62: case 0x68: case 0x78:
        psg_.length[channel(offset)] = static_cast<uint16_t>(64 - (value & 63));
        break;
    case 0x72: psg_.length[2] = static_cast<uint16_t>(256 - value); break;
    case 0x63: case 0x69: case 0x79:
        // The DAC is off without a volume and an increasing envelope.
        if (!(value & 0xf8)) turn_off(channel(offset));
        break;
    case 0x70:
        if (!(value & 0x80)) turn_off(2);
        break;
    case 0x65: case 0x6d: case 0x75: case 0x7d:
        if (value & 0x80) trigger(channel(offset));
        break;
    default: break;
    }
}

// A restart (GBATEK, the Initial bit): the channel turns on if its DAC is,
// an expired length timer starts over, and channel 1's sweep reloads.
void Memory::trigger(unsigned c) {
    const bool dac = io_[kEnvelope[c]] & (c == 2 ? 0x80 : 0xf8);
    if (dac) psg_.on |= static_cast<uint8_t>(1u << c);
    if (!psg_.length[c]) psg_.length[c] = static_cast<uint16_t>(kLengthSteps[c]);
    if (c != 0) return;
    psg_.shadow = static_cast<uint16_t>(io_[0x64] | (io_[0x65] & 7) << 8);
    const unsigned pace = io_[0x60] >> 4 & 7;
    psg_.sweep_timer = static_cast<uint8_t>(pace ? pace : 8);
    psg_.sweep_enabled = pace || (io_[0x60] & 7);
    psg_.negated = false;
    if ((io_[0x60] & 7) && sweep_target() > 2047) turn_off(0);
}

// --- Batches ----------------------------------------------------------------------

void Memory::append(Tick when, sound::Event event, uint8_t address, uint8_t value, uint8_t extra) {
    if (!mixer_) return;
    if (!batch_) open_batch();
    if (batch_->count == sound::Batch::kEntries || when - batch_->start > UINT32_MAX) {
        submit_batch(when);
        open_batch();
    }
    batch_->entries[batch_->count++] = {static_cast<uint32_t>(when - batch_->start), event, address, value, extra};
}

// A 24-bit value, its high byte in extra.
void Memory::append_setting(sound::Event event, uint32_t value) {
    append(clock.now, event, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value),
           static_cast<uint8_t>(value >> 16));
}

void Memory::open_batch() {
    batch_ = &mixer_->acquire();
    batch_->start = sound_end_;
    batch_->count = 0;
    batch_->reset = false;
}

void Memory::submit_batch(Tick end) {
    batch_->end = end;
    mixer_->submit(*batch_);
    sound_end_ = end;
    batch_ = nullptr;
}

void Memory::flush_sound() {
    catch_up_sound(clock.now);
    if (!mixer_) return;
    if (!batch_) open_batch();
    submit_batch(clock.now);
}

// --- Output ---------------------------------------------------------------------------

void Memory::set_audio_rate(uint32_t rate) {
    catch_up_sound(clock.now);
    const bool resumed = !audio_rate_ && rate;
    audio_rate_ = std::min<uint32_t>(rate, 0xffffff);
    append_setting(sound::Event::rate, audio_rate_);
    if (resumed) fifo_resume();
}

// The FIFOs' samples the mixer did not hear of while it had no output rate.
void Memory::fifo_resume() {
    for (unsigned which = 0; which < 2; ++which)
        append(clock.now, sound::Event::sample, static_cast<uint8_t>(which), static_cast<uint8_t>(fifo_[which].sample));
}

void Memory::set_audio_speed(int32_t ppm) {
    catch_up_sound(clock.now);
    // A small correction for audio buffer levels, not a speed control.
    audio_speed_ = std::clamp<int32_t>(ppm, -kMaxAudioSpeed, kMaxAudioSpeed);
    append_setting(sound::Event::speed, static_cast<uint32_t>(audio_speed_));
}

size_t Memory::read_audio(std::span<int16_t> frames_out) { return mixer_ ? mixer_->read(frames_out) : 0; }
size_t Memory::audio_queued() const { return mixer_ ? mixer_->queued() : 0; }
size_t Memory::audio_buffered() const { return mixer_ ? mixer_->buffered() : 0; }
void Memory::finish_audio() {
    if (mixer_) mixer_->finish();
}
uint64_t Memory::audio_dropped() const { return mixer_ ? mixer_->dropped() : 0; }

}  // namespace gbx::gba
