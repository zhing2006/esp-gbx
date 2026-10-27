#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <span>

namespace gbx::app {

// The errors behind the ERR lamp, newest first. The lamp stays lit until the
// user clears the list on the ERRORS page, like a warning light that needs
// acknowledging. The same text again moves to the front with the new time
// instead of filling the list. Any task.
class Faults {
public:
    // What the ERRORS page always shows: an entry takes up to two lines, and
    // three of those fill the page, so CLEAR never drops one unseen.
    static constexpr int kMax = 3;
    struct Fault {
        char text[160];
        uint64_t us;  // platform::monotonic_us() when it last happened
    };

    // Adds a printf-style text and logs it as an error.
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    void add(const char* format, ...);
    // Copies the faults, newest first; returns how many.
    int copy(std::span<Fault> out) const;
    int count() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::array<Fault, kMax> faults_{};
    int count_ = 0;
};

}  // namespace gbx::app
