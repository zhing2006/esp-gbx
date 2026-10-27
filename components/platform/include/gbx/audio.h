#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Sound output: interleaved stereo 16-bit frames at a fixed rate. A board
// with one speaker plays the two channels mixed. One task at a time writes;
// the other functions may run on any task.
namespace gbx::platform::audio {

// Opens the output at `rate` frames per second. Returns false when there is
// no output; the other functions then do nothing.
bool open(uint32_t rate);
void close();

// Hands frames to the output, waiting up to timeout_ms while its buffers are
// full, and returns how many it took. Playback starts once the buffers are
// full, so filling them is not an underrun.
size_t write(std::span<const int16_t> frames, uint32_t timeout_ms);

// Frames taken but not played yet. The output counts them by its buffers, so
// this is accurate only to a buffer.
size_t buffered();

// Times playback ran out of frames since open(); a run of empty buffers
// counts once. Nothing counts while paused or refilling.
uint32_t underruns();

// 0 to 100.
void set_volume(int percent);
void set_muted(bool muted);

// Follows the headphone jack on boards that have one: the speaker plays
// while no headphones are in, and headphones get both channels. Call it
// every few milliseconds from one task; it reads the jack at its own pace.
void update();

// pause() stops counting underruns: the output plays what it holds, then
// silence. After resume(), counting starts again once the buffers are full.
void pause();
void resume();

}  // namespace gbx::platform::audio
