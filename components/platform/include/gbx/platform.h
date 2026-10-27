#pragma once

#include <cstddef>
#include <cstdint>

// Services the emulation core and the app get from the platform. The firmware
// implements them in src/esp, the PC build in src/host.
namespace gbx::platform {

enum class LogLevel : uint8_t { error, warn, info, debug };

// The board's directory name under boards/, such as "sila_p4"; "pc" on the PC.
const char* board_id();

// Writes one printf-style log line under the given tag.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
void log(LogLevel level, const char* tag, const char* format, ...);

// Monotonic time in microseconds. Only differences between two calls are meaningful.
uint64_t monotonic_us();

// Puts the calling task to sleep for at least `ms` milliseconds.
void sleep_ms(uint32_t ms);

// Storage that may be as slow as the rest of emulated memory, such as
// cartridge RAM. The firmware takes it from PSRAM when the board has PSRAM,
// whatever its size, so internal RAM stays free for the JIT code pool and
// hot state. Word aligned; nullptr when out of memory. Release with
// free_external().
void* allocate_external(size_t bytes);
void free_external(void* memory);

// Storage for state that every emulated access touches, such as the
// machine's memory. The firmware takes it from internal RAM, or from PSRAM
// when internal RAM is short. Word aligned; nullptr when out of memory.
// Release with free_internal().
void* allocate_internal(size_t bytes);
void free_internal(void* memory);

// Starts loading external memory that is about to be written into the
// data cache, in the background, so that the writes need not wait for the
// lines; skipped while an earlier one runs. One task uses it. A hint only:
// nothing on the PC.
void preload_external(const void* address, size_t bytes);

// Free heap memory that can be allocated now, in bytes: internal RAM and
// PSRAM counted apart, as the firmware runs out of either one separately.
// known is false where the platform cannot tell, as on the PC.
struct HeapStats {
    bool known;
    size_t internal_free;
    size_t external_free;
};
HeapStats heap_stats();

// Time each CPU core has spent idle since boot, for the load shown in the
// telemetry. known is false where the platform cannot tell, as on the PC.
// One caller at a time.
struct CpuIdle {
    bool known;
    int cores;            // idle_us holds this many
    uint64_t idle_us[2];  // per core
    uint64_t at_us;       // monotonic_us() when read
};
CpuIdle cpu_idle();

// The time of day from the board's battery-backed clock, in microseconds
// since 1970-01-01 00:00 in the clock's own time zone. The clock is read once
// and then counted on with monotonic_us(). trusted is false when the board
// has no such clock or its clock lost its time; `us` then counts from an
// unknown origin. The PC uses the system clock, which counts UTC and which it
// trusts. Any task.
struct WallTime {
    bool trusted;
    int64_t us;
};
WallTime wall_time();
// A wall_time() as the local time of day, for showing it: the board's clock
// already keeps it, the PC adds its time zone's offset. Times are compared
// and counted in wall_time() terms. Any task.
int64_t local_time(int64_t us);
// Sets the board's clock to `us` as wall_time() counts it, from 2000 to 2099,
// and wall_time() trusts it from then on. False when the board has no clock
// to set or the write fails; the PC never sets its system clock. Any task.
bool set_wall_time(int64_t us);

}  // namespace gbx::platform
