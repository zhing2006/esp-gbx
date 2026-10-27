#pragma once

#include <array>
#include <cstdint>

#include "gbx/common/scheduler.h"

// Batches from the sound registers on the CPU's core to the mixer on the
// other core (gba_sound.cpp, gba_mixer.cpp).
namespace gbx::gba::sound {

// Output frames per CPU cycle, Q32: rate / 2^24, adjusted by ppm. Shared by
// synthesis and the producer's count of committed frames.
inline uint64_t frames_per_cycle(uint32_t rate, int32_t ppm) {
    return uint64_t{rate} * 256 * static_cast<uint64_t>(1'000'000 + ppm) / 1'000'000;
}

enum class Event : uint8_t {
    // A byte written to I/O offset `address` (60h-89h), as stored; with
    // kLoad it restores the register without a write's effects.
    write,
    // A byte of wave RAM: bank * 16 + offset in `address`.
    wave,
    // The PSG channels on (SOUNDCNT_X bits 0-3): triggers, length timers,
    // sweep overflow and DACs turned off.
    status,
    sweep,     // Channel 1's frequency after a sweep step, 11 bits.
    envelope,  // A step of the envelopes (64 Hz).
    // A sample a FIFO took: FIFO A (0) or B (1) in `address`, the signed
    // byte in `value`. Only samples that change the FIFO's output come.
    sample,
    rate,      // Output frames per second; 0 stops synthesis.
    speed,     // Resampling adjustment, parts per million (signed, 24 bits).
};
inline constexpr uint8_t kLoad = 1;

struct Entry {
    uint32_t time;  // Cycles after Batch::start.
    Event event;
    uint8_t address, value, extra;
    uint32_t value24() const { return value | address << 8 | extra << 16; }
};

// Entries in time order, one queue item: register writes and the FIFOs'
// samples in one stream, so the mixer takes them as they happened. The
// mixer synthesizes up to end, which no later batch precedes. A frame of
// 8-bit samples at 32 kHz on both FIFOs is about 1100 entries.
struct Batch {
    static constexpr unsigned kEntries = 1024;
    Tick start = 0, end = 0;
    uint32_t count = 0;
    bool reset = false;  // Return to the power-on state before the entries.
    std::array<Entry, kEntries> entries;
};

}  // namespace gbx::gba::sound
