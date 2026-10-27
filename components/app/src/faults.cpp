#include "faults.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "gbx/platform.h"

namespace gbx::app {

void Faults::add(const char* format, ...) {
    Fault fault{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(fault.text, sizeof fault.text, format, args);
    va_end(args);
    fault.us = platform::monotonic_us();
    platform::log(platform::LogLevel::error, "fault", "%s", fault.text);
    std::lock_guard lock(mutex_);
    int at = 0;
    while (at < count_ && std::strcmp(faults_[at].text, fault.text) != 0) ++at;
    if (at == count_) {
        // A new text pushes the oldest out when the list is full.
        at = std::min(count_, kMax - 1);
        count_ = std::min(count_ + 1, kMax);
    }
    std::copy_backward(faults_.begin(), faults_.begin() + at, faults_.begin() + at + 1);
    faults_[0] = fault;
}

int Faults::copy(std::span<Fault> out) const {
    std::lock_guard lock(mutex_);
    const int n = std::min(count_, static_cast<int>(out.size()));
    std::copy_n(faults_.begin(), n, out.begin());
    return n;
}

int Faults::count() const {
    std::lock_guard lock(mutex_);
    return count_;
}

void Faults::clear() {
    std::lock_guard lock(mutex_);
    count_ = 0;
}

}  // namespace gbx::app
