#include "gbx/platform.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <utility>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gbx/esp/board.h"
#include "malloc.h"
#include "rom/cache.h"
#include "sdkconfig.h"

namespace gbx::platform {

namespace {

esp_log_level_t to_esp_level(LogLevel level) {
    switch (level) {
        case LogLevel::error: return ESP_LOG_ERROR;
        case LogLevel::warn: return ESP_LOG_WARN;
        case LogLevel::info: return ESP_LOG_INFO;
        case LogLevel::debug: return ESP_LOG_DEBUG;
    }
    std::unreachable();
}

}  // namespace

const char* board_id() {
    return GBX_BOARD_ID;
}

void log(LogLevel level, const char* tag, const char* format, ...) {
    char message[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof message, format, args);
    va_end(args);
    ESP_LOG_LEVEL(to_esp_level(level), tag, "%s", message);
}

uint64_t monotonic_us() {
    return static_cast<uint64_t>(esp_timer_get_time());
}

void sleep_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void* allocate_external(size_t bytes) {
    if (void* memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return memory;
    return heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

void free_external(void* memory) {
    heap_caps_free(memory);
}

// The L1 data cache's manual preload (ESP32-P4 ROM): one at a time, which
// suspends the automatic preload until it ends.
void preload_external(const void* address, size_t bytes) {
#if CONFIG_IDF_TARGET_ESP32P4
    static bool busy = false;
    static uint32_t autoload = 0;
    if (busy) {
        if (!Cache_L1_DCache_Preload_Done()) return;
        Cache_End_L1_DCache_Preload(autoload);
    }
    autoload = Cache_Start_L1_DCache_Preload(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(address)),
                                             static_cast<uint32_t>(bytes), 0);
    busy = true;
#else
    (void)address;
    (void)bytes;
#endif
}

void* allocate_internal(size_t bytes) {
    // MALLOC_CAP_DMA keeps to L2MEM: the LP SRAM in the heap is internal
    // too, but the HP cores reach it on the LP bus, which is slower than
    // PSRAM through the caches.
    if (void* memory = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)) return memory;
#ifdef GBX_PROFILER
    // Runs on, only slower: the caller is the one to find in the map.
    ESP_LOGW("platform", "%u bytes for %p in PSRAM: internal RAM is short", static_cast<unsigned>(bytes),
             __builtin_return_address(0));
#endif
    return heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

void free_internal(void* memory) {
    heap_caps_free(memory);
}

HeapStats heap_stats() {
    return {.known = true,
            .internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            .external_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)};
}

CpuIdle cpu_idle() {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER && \
    CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64
    constexpr int kCores = std::min(portNUM_PROCESSORS, 2);
    // An idle task's counter, in microseconds, grows when the task is
    // switched out, so a core that stays idle with nothing to switch to
    // would look busy. When the idle task was running at the previous read
    // and still is, with its counter unchanged, it ran the whole time in
    // between, and that time is added until the counter catches up.
    static uint64_t counted[kCores], added[kCores], last_at;
    static bool was_running[kCores];
    const uint64_t now = esp_timer_get_time();
    CpuIdle out{.known = true, .cores = kCores, .idle_us = {}, .at_us = now};
    for (int core = 0; core < kCores; ++core) {
        const uint64_t counter = ulTaskGetIdleRunTimeCounterForCore(core);
        const bool running = xTaskGetCurrentTaskHandleForCore(core) == xTaskGetIdleTaskHandleForCore(core);
        if (counter != counted[core]) {
            counted[core] = counter;
            added[core] = 0;
        } else if (running && was_running[core]) {
            added[core] += now - last_at;
        }
        was_running[core] = running;
        out.idle_us[core] = counter + added[core];
    }
    last_at = now;
    return out;
#else
    return {.known = false, .cores = 0, .idle_us = {}, .at_us = monotonic_us()};
#endif
}

namespace {

// The wall time at esp_timer 0. The chip is read once, over I2C; after that
// the time counts on with esp_timer, which keeps the same rate to well under
// a second a day.
std::mutex clock_mutex;
bool clock_read = false;
WallTime clock_origin{};

}  // namespace

WallTime wall_time() {
    std::lock_guard lock(clock_mutex);
    if (!clock_read) {
        const auto clock = board::read_clock();
        clock_origin = {.trusted = clock.valid, .us = clock.seconds * 1'000'000 - esp_timer_get_time()};
        clock_read = true;
        if (!clock.valid) {
            ESP_LOGW("clock", "the board clock is not set, its time is not trusted");
        }
    }
    return {.trusted = clock_origin.trusted, .us = clock_origin.us + esp_timer_get_time()};
}

int64_t local_time(int64_t us) {
    // The board's clock is set to the local time.
    return us;
}

bool set_wall_time(int64_t us) {
    std::lock_guard lock(clock_mutex);
    // The chip starts the new second when it is written.
    if (!board::set_clock(us / 1'000'000)) return false;
    clock_origin = {.trusted = true, .us = us / 1'000'000 * 1'000'000 - esp_timer_get_time()};
    clock_read = true;
    return true;
}

}  // namespace gbx::platform

// The firmware builds without exceptions, and libstdc++'s nothrow operator
// new calls the throwing one in a try block: a failed allocation aborts in
// __cxa_allocate_exception instead of returning nullptr. These take their
// place and call what the throwing ones call, malloc() and memalign(), so
// the memory comes from where it did and operator delete frees it.
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    return std::malloc(bytes ? bytes : 1);
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    return std::malloc(bytes ? bytes : 1);
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return memalign(static_cast<std::size_t>(alignment), bytes ? bytes : 1);
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t& tag) noexcept {
    return ::operator new(bytes, alignment, tag);
}
