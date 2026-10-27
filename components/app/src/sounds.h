#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include "gbx/ui/pack.h"

namespace gbx::app {

// The menus' sound cues, from the asset pack (tools/ui/menu_sounds.py). The
// menus ask for a cue on the app's task; the game's audio task plays it,
// alone while no game runs and added to the game's sound while one does, so
// the cues need no output of their own and follow the volume setting with
// the game. A new cue cuts off the one playing.
class Sounds {
public:
    enum class Cue : uint8_t { none, move, confirm, back, denied, dialog };

    // Finds the cues at `rate`; a pack without them plays none.
    void load(const ui::Pack& pack, uint32_t rate);
    // Any task.
    void play(Cue cue);

    // The audio task: whether a cue has frames left to play.
    bool playing();
    // The audio task: adds the next frames of the cue to interleaved stereo
    // `frames`, as many as they hold.
    void mix(std::span<int16_t> frames);

private:
    void take();

    std::array<std::span<const int16_t>, 6> clips_{};
    std::atomic<uint32_t> request_{0};  // the cue in the low byte, a count above
    uint32_t taken_ = 0;                // the audio task's last request
    std::span<const int16_t> rest_;     // what the audio task has left to play
};

}  // namespace gbx::app
