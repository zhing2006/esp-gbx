#pragma once

#include <cstdint>
#include <span>

#include "gbx/ui/pack_format.h"

namespace gbx::ui {

inline constexpr int kMaxControls = 16;

// A touch point on the logical screen.
struct TouchPoint {
    float x;
    float y;
};

enum class DpadDirection : uint8_t { none, up, down, left, right, up_left, up_right, down_left, down_right };

// What one control is doing after hit testing.
struct ControlHit {
    bool touched = false;
    DpadDirection direction = DpadDirection::none;  // D-pad only
};

// Hit-tests the touch points against the controls. Each point goes to the
// control whose shape is nearest, if it lies within that control's margin, so
// several controls can be held at once with several fingers. `hits` has one
// element per control and is overwritten.
// TODO: decide whether a finger between A and B should press both.
void hit_test(std::span<const pack_format::Control> controls, std::span<const TouchPoint> points,
              std::span<ControlHit> hits);

// Steadies hit_test() results across samples. A press shows at once. A
// release shows only after the control has been free for kReleaseSamples
// samples in a row, so a touch report that drops a finger for one sample does
// not read as a release and a new press. A failed touch read keeps the last
// state for up to kMaxMissedSamples samples, then counts as no touch.
class ReleaseFilter {
public:
    static constexpr int kReleaseSamples = 2;
    static constexpr int kMaxMissedSamples = 5;

    // Filters one sample in place. `read_ok` is false when the touch
    // controller could not be read; `hits` is then ignored.
    void apply(std::span<ControlHit> hits, bool read_ok);

private:
    ControlHit held_[kMaxControls]{};
    uint8_t free_samples_[kMaxControls]{};
    int missed_samples_ = 0;
};

}  // namespace gbx::ui
