#include "audio_test.h"
#include "transport.h"
#include "../../components/core/src/gb/mixer.h"

#include <array>
#include <memory>
#include <new>
#include <string_view>

namespace gbx::test::audio_test {
namespace {
struct Gate {
    platform::Notification entered, release;
    static void consume(void* context, uint32_t) {
        auto& gate = *static_cast<Gate*>(context);
        gate.entered.signal();
        gate.release.wait();
    }
};

bool check(std::array<uint32_t, kCheckCount>& values) {
    Gate gate;
    gb::Offload offload;
    std::unique_ptr<gb::Mixer> mixer(new (std::nothrow) gb::Mixer(offload));
    if (!mixer || !gate.entered.initialize(1) || !gate.release.initialize(1)) return false;
    offload.attach(gb::Offload::rows, 2, Gate::consume, &gate);
    if (!offload.start()) return false;
    // Hold the real worker on its other queue. No sound batch can have run
    // when the producer reads the first three values below.
    offload.acquire(gb::Offload::rows);
    offload.submit(gb::Offload::rows, true);
    gate.entered.wait();

    constexpr Tick interval = kTicksPerSecond / 64;
    const auto setting = [](gb::sound::Event event, uint32_t value) {
        return gb::sound::Entry{0, event, static_cast<uint8_t>(value >> 8),
                               static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 16)};
    };
    const auto submit = [&](Tick start, Tick end, bool reset, uint32_t rate, int32_t ppm, bool configure) {
        auto& batch = mixer->acquire();
        batch.start = start;
        batch.end = end;
        batch.reset = reset;
        batch.count = configure ? 2 : 0;
        if (configure) {
            batch.entries[0] = setting(gb::sound::Event::rate, rate);
            batch.entries[1] = setting(gb::sound::Event::speed, static_cast<uint32_t>(ppm));
        }
        mixer->submit(batch);
    };
    submit(0, interval, true, 48000, 0, true);
    values[0] = mixer->buffered(); // 48000 / 64 = 750, still entirely pending.
    values[1] = mixer->queued();
    submit(interval, 2 * interval, false, 44100, 5000, true);
    submit(2 * interval, 3 * interval, false, 0, 0, false);
    // 750 + floor(2 * 44100 * 1.005 / 64) = 2135. Rounding each batch
    // separately would lose a frame, so this checks fractional carry too.
    values[2] = mixer->buffered();
    gate.release.signal();
    mixer->finish();
    values[3] = mixer->buffered();
    values[4] = mixer->queued();
    std::array<int16_t, 512> pcm{};
    values[5] = mixer->read(pcm);
    values[6] = mixer->buffered();

    submit(3 * interval, 4 * interval, true, 32000, -5000, true);
    mixer->finish();
    // Reset discards the old unread PCM and starts a new fractional phase.
    values[7] = mixer->buffered(); // floor(32000 * 0.995 / 64) = 497.
    values[8] = mixer->queued();
    while (mixer->read(pcm)) {}
    values[9] = mixer->buffered();

    // Dropped PCM must not leave permanent reservations that stop pacing.
    submit(4 * interval, 24 * interval, false, 0, 0, false);
    mixer->finish();
    values[10] = mixer->buffered();
    values[11] = mixer->queued();
    values[12] = static_cast<uint32_t>(mixer->dropped());
    while (mixer->read(pcm)) {}
    values[13] = mixer->buffered();
    return true;
}
}  // namespace

bool run(uint32_t request_id) {
    std::array<uint32_t, kCheckCount> values{};
    if (!check(values)) {
        constexpr std::string_view message = "Cannot initialize mixer accounting check";
        return transport::send(protocol::Kind::error,
                               {reinterpret_cast<const uint8_t*>(message.data()), message.size()});
    }
    for (uint16_t i = 0; i < values.size(); ++i) {
        uint8_t data[12];
        protocol::store16(data, kVersion);
        protocol::store16(data + 2, i);
        protocol::store32(data + 4, request_id);
        protocol::store32(data + 8, values[i]);
        if (!transport::send_data(data)) return false;
    }
    if (!transport::finish()) return false;
    uint8_t done[8];
    protocol::store16(done, kVersion);
    protocol::store16(done + 2, kCheckCount);
    protocol::store32(done + 4, request_id);
    return transport::send(protocol::Kind::done, done);
}
}  // namespace gbx::test::audio_test
