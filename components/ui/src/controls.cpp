#include "gbx/ui/controls.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gbx::ui {

namespace {

using pack_format::Control;
using pack_format::ControlKind;
using pack_format::Shape;

// Signed distance from the point to the control's shape: negative inside.
float distance(const Control& c, TouchPoint p) {
    float dx = p.x - c.cx;
    float dy = p.y - c.cy;
    switch (c.shape) {
        case Shape::circle:
            return std::hypot(dx, dy) - c.a;
        case Shape::capsule: {
            float ux = std::cos(c.angle);
            float uy = std::sin(c.angle);
            float t = std::clamp(dx * ux + dy * uy, -c.a, c.a);
            return std::hypot(dx - t * ux, dy - t * uy) - c.b;
        }
        case Shape::rect: {
            float ox = std::abs(dx) - c.a;
            float oy = std::abs(dy) - c.b;
            float outside = std::hypot(std::max(ox, 0.0f), std::max(oy, 0.0f));
            return outside + std::min(std::max(ox, oy), 0.0f);
        }
    }
    return INFINITY;
}

// Eight 45-degree sectors around the D-pad center; screen y points down.
DpadDirection direction(const Control& c, TouchPoint p) {
    float dx = p.x - c.cx;
    float dy = p.y - c.cy;
    if (std::hypot(dx, dy) < c.dead) {
        return DpadDirection::none;
    }
    int sector = static_cast<int>(std::lround(std::atan2(dy, dx) / (std::numbers::pi_v<float> / 4)));
    static constexpr DpadDirection kSectors[8] = {
        DpadDirection::right, DpadDirection::down_right, DpadDirection::down,     DpadDirection::down_left,
        DpadDirection::left,  DpadDirection::up_left,    DpadDirection::up,       DpadDirection::up_right,
    };
    return kSectors[(sector + 8) % 8];
}

}  // namespace

void hit_test(std::span<const Control> controls, std::span<const TouchPoint> points, std::span<ControlHit> hits) {
    std::fill(hits.begin(), hits.end(), ControlHit{});
    size_t n = std::min(controls.size(), hits.size());
    for (TouchPoint p : points) {
        size_t best = n;
        float best_distance = INFINITY;
        for (size_t i = 0; i < n; ++i) {
            float d = distance(controls[i], p);
            if (d <= controls[i].margin && d < best_distance) {
                best = i;
                best_distance = d;
            }
        }
        if (best == n || hits[best].touched) {
            continue;
        }
        hits[best].touched = true;
        if (controls[best].kind == ControlKind::dpad) {
            hits[best].direction = direction(controls[best], p);
        }
    }
}

void ReleaseFilter::apply(std::span<ControlHit> hits, bool read_ok) {
    size_t n = std::min(hits.size(), static_cast<size_t>(kMaxControls));
    if (!read_ok) {
        if (missed_samples_ < kMaxMissedSamples) {
            ++missed_samples_;
            std::copy_n(held_, n, hits.begin());
            return;
        }
        std::fill(hits.begin(), hits.end(), ControlHit{});
    } else {
        missed_samples_ = 0;
    }
    for (size_t i = 0; i < n; ++i) {
        if (hits[i].touched) {
            held_[i] = hits[i];
            free_samples_[i] = 0;
        } else if (held_[i].touched && ++free_samples_[i] >= kReleaseSamples) {
            held_[i] = ControlHit{};
        }
        hits[i] = held_[i];
    }
}

}  // namespace gbx::ui
