#pragma once

#include <array>
#include <cstdint>

#include "gbx/gb/apu.h"

// Batches from the APU on the CPU owner to the mixer on the other core.
namespace gbx::gb::sound {

// Output frames per emulated tick, Q32. Shared by synthesis and the
// producer's committed-frame accounting, including resampling correction.
inline uint64_t frames_per_tick(uint32_t rate, int32_t ppm) {
    return uint64_t{rate} * 512 * static_cast<uint64_t>(1'000'000 + ppm) / 1'000'000;
}

enum class Event : uint8_t {
    // A write to IO register 0xff00 + address, as stored, including wave
    // RAM writes that took effect.
    write,
    // The channels now on (NR52 bits 0-3): triggers, length timers, sweep
    // overflow, DAC switches and power changes.
    status,
    sweep,     // CH1's period after a sweep iteration (11 bits).
    envelope,  // A DIV-APU envelope step (every 8th).
    rate,      // Output frames per second; 0 stops synthesis.
    speed,     // Resampling adjustment, parts per million (signed, 24 bits).
    model,     // 1 on a CGB, whose high-pass filter is stronger; 0 on a DMG.
};
// Flags of a write entry. kEnvelopeNext goes with NRx4 writes.
inline constexpr uint8_t kEnvelopeNext = 1;  // The next DIV-APU step clocks envelopes.
inline constexpr uint8_t kLoad = 2;          // Restores a register without a write's effects.

struct Entry {
    uint32_t time;  // Ticks after Batch::start.
    Event event;
    uint8_t address, value, extra;
    uint32_t value24() const { return value | address << 8 | extra << 16; }
};

// Entries in time order, one queue item. The mixer synthesizes up to end,
// which no later batch precedes.
struct Batch {
    static constexpr unsigned kEntries = 256;
    Tick start = 0, end = 0;
    uint32_t count = 0;
    bool reset = false;  // Return to the power-up state before the entries.
    std::array<Entry, kEntries> entries;
};

}  // namespace gbx::gb::sound
