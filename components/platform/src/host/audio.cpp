#include "gbx/audio.h"

#include <algorithm>
#include <atomic>

#include <SDL3/SDL.h>

#include "gbx/platform.h"

// An SDL audio stream on the default playback device. SDL's audio thread
// takes one device buffer at a time from the stream, whose size depends on
// the device and its rate, and can change when the default device does. The
// stream holds two of the largest request seen, and at least as much as the
// firmware's DMA buffers, so the app's own queue holds the rest. A request
// the stream cannot fill is an underrun.
namespace gbx::platform::audio {
namespace {

constexpr const char* kTag = "host.audio";
constexpr int kMinHeldFrames = 1024;

SDL_AudioStream* stream = nullptr;
std::atomic<int> request_frames{0};  // the largest request, in the stream's frames
std::atomic<uint32_t> underrun_count{0};
std::atomic<bool> counting{false}, starving{false}, paused{false};
bool started = false;  // the device starts paused, see write()
float volume = 1.0f;
bool muted_now = false;

int held_frames() {
    return std::max(kMinHeldFrames, 2 * request_frames.load(std::memory_order_relaxed));
}

// The amounts are bytes of the stream's input, four to a stereo frame.
void SDLCALL on_request(void*, SDL_AudioStream*, int additional, int total) {
    if (total / 4 > request_frames.load(std::memory_order_relaxed)) {
        request_frames.store(total / 4, std::memory_order_relaxed);
    }
    if (additional > 0 && counting.load(std::memory_order_relaxed) &&
        !starving.exchange(true, std::memory_order_relaxed)) {
        underrun_count.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

bool open(uint32_t rate) {
    if (stream) return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log(LogLevel::warn, kTag, "no audio: %s", SDL_GetError());
        return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream || !SDL_SetAudioStreamGetCallback(stream, on_request, nullptr)) {
        log(LogLevel::warn, kTag, "no audio output: %s", SDL_GetError());
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    // The first requests, before on_request() has seen one.
    SDL_AudioSpec device{};
    int device_frames = 0;
    request_frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &device, &device_frames) && device.freq > 0) {
        request_frames = static_cast<int>((int64_t{device_frames} * rate + device.freq - 1) / device.freq);
    }
    log(LogLevel::info, kTag, "%u Hz on %s, device %d Hz in buffers of %d frames", static_cast<unsigned>(rate),
        SDL_GetCurrentAudioDriver(), device.freq, device_frames);
    return true;
}

void close() {
    if (!stream) return;
    SDL_DestroyAudioStream(stream);
    stream = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    counting = false;
    started = false;
}

size_t write(std::span<const int16_t> frames, uint32_t timeout_ms) {
    if (!stream) return 0;
    const uint64_t deadline = SDL_GetTicks() + timeout_ms;
    while (SDL_GetAudioStreamQueued(stream) >= held_frames() * 4) {
        if (SDL_GetTicks() >= deadline) return 0;
        SDL_Delay(1);
    }
    if (!SDL_PutAudioStreamData(stream, frames.data(), static_cast<int>(frames.size() * sizeof(int16_t)))) {
        return 0;
    }
    starving = false;
    const bool full = SDL_GetAudioStreamQueued(stream) >= held_frames() * 4;
    // A game starts the device once the stream is full, so it begins without
    // an underrun. Paused, a menu cue can be shorter than that and plays at
    // once.
    if (!started && (full || paused.load())) started = SDL_ResumeAudioStreamDevice(stream);
    if (!counting.load() && full) counting = !paused.load();
    return frames.size() / 2;
}

size_t buffered() {
    return stream ? static_cast<size_t>(std::max(SDL_GetAudioStreamQueued(stream), 0)) / 4 : 0;
}

uint32_t underruns() {
    return underrun_count.load(std::memory_order_relaxed);
}

// Muting keeps playing silence, so the stream drains at the same pace.
void set_volume(int percent) {
    volume = std::clamp(percent, 0, 100) / 100.0f;
    if (stream && !muted_now) SDL_SetAudioStreamGain(stream, volume);
}

void set_muted(bool muted) {
    muted_now = muted;
    if (stream) SDL_SetAudioStreamGain(stream, muted ? 0.0f : volume);
}

void update() {}

void pause() {
    paused = true;
    counting = false;
}

void resume() {
    paused = false;
}

}  // namespace gbx::platform::audio
