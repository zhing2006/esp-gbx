#include "sounds.h"

#include <algorithm>
#include <iterator>
#include <tuple>

namespace gbx::app {

void Sounds::load(const ui::Pack& pack, uint32_t rate) {
    static constexpr const char* kNames[] = {"", "move", "confirm", "back", "denied", "dialog"};
    static_assert(std::size(kNames) == std::tuple_size_v<decltype(clips_)>);
    for (size_t i = 1; i < clips_.size(); ++i) clips_[i] = pack.sound(kNames[i], rate);
}

void Sounds::play(Cue cue) {
    if (cue == Cue::none) return;
    uint32_t request = request_.load(std::memory_order_relaxed);
    while (!request_.compare_exchange_weak(request, ((request >> 8) + 1) << 8 | static_cast<uint32_t>(cue),
                                           std::memory_order_release, std::memory_order_relaxed)) {}
}

void Sounds::take() {
    const uint32_t request = request_.load(std::memory_order_acquire);
    if (request == taken_) return;
    taken_ = request;
    rest_ = clips_[request & 0xff];
}

bool Sounds::playing() {
    take();
    return !rest_.empty();
}

void Sounds::mix(std::span<int16_t> frames) {
    take();
    const size_t n = std::min(frames.size() / 2, rest_.size());
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 2; ++c) {
            int16_t& out = frames[i * 2 + c];
            out = static_cast<int16_t>(std::clamp(out + rest_[i], -32768, 32767));
        }
    }
    rest_ = rest_.subspan(n);
}

}  // namespace gbx::app
