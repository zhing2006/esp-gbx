#include "gbx/audio.h"

#include <algorithm>
#include <atomic>

#include "driver/i2s_common.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gbx/esp/board.h"

// The board's I2S channel runs from open() on. Its DMA buffers cycle through
// whatever the writer filled and send zeros from buffers nobody refilled, so
// a late writer is heard as silence. The driver reports such a buffer with
// on_send_q_ovf: every other buffer was free when one more finished.
namespace gbx::platform::audio {
namespace {

constexpr const char* kTag = "audio";
// Four buffers of 256 frames hold 21 ms at 48 kHz: enough to cover the
// writer's task switches, short enough that the app's own queue, which it
// can measure, holds most of the sound waiting to play.
constexpr uint32_t kBuffers = 4;
constexpr uint32_t kBufferFrames = 256;

board::Audio output;
std::atomic<uint32_t> pending{0};  // frames written, by buffers sent
std::atomic<uint32_t> underrun_count{0};
std::atomic<bool> counting{false};  // playing: buffers are full and not paused
std::atomic<bool> starving{false};  // the current run of empty buffers is counted
std::atomic<bool> paused{false}, refill{false};
uint32_t filled = 0;  // frames written since open() or resume(), writer only
// Headphones in: the writer sends both channels instead of the speaker's mix.
std::atomic<bool> stereo{false};
// The jack, read by update(): three equal reads 20 ms apart switch the
// output, so a plug going in does not switch it back and forth.
constexpr int64_t kJackPeriodUs = 20'000;
constexpr int kJackReads = 3;
struct Jack {
    int64_t next_us = 0;
    board::Headphones read = board::Headphones::unknown;
    int reads = 0;
    board::Headphones shown = board::Headphones::unknown;  // what the output follows
    bool failed = false;  // the last switch failed and was logged
} jack;
int16_t mixed[kBufferFrames * 2];

IRAM_ATTR bool on_sent(i2s_chan_handle_t, i2s_event_data_t* event, void*) {
    const auto frames = static_cast<uint32_t>(event->size / 4);
    uint32_t now = pending.load(std::memory_order_relaxed);
    while (now && !pending.compare_exchange_weak(now, now > frames ? now - frames : 0, std::memory_order_relaxed)) {}
    return false;
}

IRAM_ATTR bool on_empty(i2s_chan_handle_t, i2s_event_data_t*, void*) {
    if (counting.load(std::memory_order_relaxed) && !starving.exchange(true, std::memory_order_relaxed)) {
        underrun_count.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

}  // namespace

bool open(uint32_t rate) {
    if (output.tx) return true;
    output = board::audio_init(rate, kBuffers, kBufferFrames);
    if (!output.tx) {
        ESP_LOGW(kTag, "no sound output");
        return false;
    }
    // The board leaves the channel running, and callbacks can only change on
    // a stopped channel. Nothing was written yet, so it restarts with silence.
    i2s_event_callbacks_t callbacks{};
    callbacks.on_sent = on_sent;
    callbacks.on_send_q_ovf = on_empty;
    if (i2s_channel_disable(output.tx) != ESP_OK ||
        i2s_channel_register_event_callback(output.tx, &callbacks, nullptr) != ESP_OK ||
        i2s_channel_enable(output.tx) != ESP_OK) {
        ESP_LOGE(kTag, "cannot watch the output's buffers");
        output = {};
        return false;
    }
    ESP_LOGI(kTag, "%u Hz, %u buffers of %u frames, %s", static_cast<unsigned>(rate), static_cast<unsigned>(kBuffers),
             static_cast<unsigned>(kBufferFrames), output.mono ? "mono" : "stereo");
    return true;
}

void close() {
    // The board keeps the channel and codec; nothing is written any more.
    counting.store(false);
}

size_t write(std::span<const int16_t> frames, uint32_t timeout_ms) {
    if (!output.tx) return 0;
    const size_t total = frames.size() / 2;
    size_t taken = 0;
    while (taken < total) {
        const size_t n = std::min<size_t>(total - taken, kBufferFrames);
        const int16_t* data = frames.data() + taken * 2;
        if (output.mono && !stereo.load(std::memory_order_relaxed)) {
            // One speaker plays the average of the two channels.
            for (size_t i = 0; i < n; ++i) {
                const auto sum = (data[i * 2] + data[i * 2 + 1]) / 2;
                mixed[i * 2] = mixed[i * 2 + 1] = static_cast<int16_t>(sum);
            }
            data = mixed;
        }
        size_t bytes = 0;
        i2s_channel_write(output.tx, data, n * 4, &bytes, timeout_ms);
        const auto written = static_cast<uint32_t>(bytes / 4);
        if (!written) break;
        taken += written;
        pending.fetch_add(written, std::memory_order_relaxed);
        starving.store(false, std::memory_order_relaxed);
        if (refill.exchange(false)) filled = 0;
        if (!counting.load(std::memory_order_relaxed) && !paused.load() &&
            (filled += written) >= kBuffers * kBufferFrames) {
            counting.store(true, std::memory_order_relaxed);
        }
    }
    return taken;
}

size_t buffered() {
    return pending.load(std::memory_order_relaxed);
}

uint32_t underruns() {
    return underrun_count.load(std::memory_order_relaxed);
}

void update() {
    if (!output.tx || !output.headphones) return;
    const int64_t now = esp_timer_get_time();
    if (now < jack.next_us) return;
    jack.next_us = now + kJackPeriodUs;
    const board::Headphones read = board::headphones();
    if (read == board::Headphones::unknown || read != jack.read) {
        jack.read = read;
        jack.reads = 1;
        return;
    }
    if (++jack.reads < kJackReads || read == jack.shown) return;
    // Headphones in: the speaker goes off before both channels go out, as
    // it plays only the left one. Out: the mix comes first.
    const bool in = read == board::Headphones::present;
    if (in) {
        if (!board::set_speaker(false)) {
            if (!jack.failed) ESP_LOGW(kTag, "cannot switch the speaker off");
            jack.failed = true;
            return;
        }
        stereo.store(true, std::memory_order_relaxed);
    } else {
        stereo.store(false, std::memory_order_relaxed);
        if (!board::set_speaker(true)) {
            if (!jack.failed) ESP_LOGW(kTag, "cannot switch the speaker on");
            jack.failed = true;
            return;
        }
    }
    jack.failed = false;
    jack.shown = read;
    ESP_LOGI(kTag, "%s", in ? "headphones in, speaker off" : "no headphones, speaker on");
}

void set_volume(int percent) {
    board::audio_volume(std::clamp(percent, 0, 100));
}

void set_muted(bool muted) {
    board::audio_mute(muted);
}

void pause() {
    paused.store(true);
    counting.store(false);
}

void resume() {
    refill.store(true);
    paused.store(false);
}

}  // namespace gbx::platform::audio
