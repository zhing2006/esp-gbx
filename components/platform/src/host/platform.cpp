#include "gbx/platform.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>

namespace gbx::platform {

const char* board_id() {
    return "pc";
}

void log(LogLevel level, const char* tag, const char* format, ...) {
    static constexpr char kLevelLetters[] = {'E', 'W', 'I', 'D'};
    std::fprintf(stderr, "%c %s: ", kLevelLetters[static_cast<int>(level)], tag);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

uint64_t monotonic_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void sleep_ms(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void* allocate_external(size_t bytes) {
    return std::malloc(bytes);
}

void free_external(void* memory) {
    std::free(memory);
}

void preload_external(const void*, size_t) {}

void* allocate_internal(size_t bytes) {
    return std::malloc(bytes);
}

void free_internal(void* memory) {
    std::free(memory);
}

HeapStats heap_stats() {
    return {.known = false, .internal_free = 0, .external_free = 0};
}

CpuIdle cpu_idle() {
    return {.known = false, .cores = 0, .idle_us = {}, .at_us = monotonic_us()};
}

WallTime wall_time() {
    using namespace std::chrono;
    return {.trusted = true, .us = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count()};
}

int64_t local_time(int64_t us) {
    // The system clock counts UTC: add the zone's offset at that moment.
    const std::time_t at = static_cast<std::time_t>(us / 1'000'000);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &at) != 0) return us;
    const int64_t offset = static_cast<int64_t>(_mkgmtime(&local)) - at;
#else
    if (!localtime_r(&at, &local)) return us;
    const int64_t offset = local.tm_gmtoff;
#endif
    return us + offset * 1'000'000;
}

bool set_wall_time(int64_t) {
    return false;
}

}  // namespace gbx::platform
