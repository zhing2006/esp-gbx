#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace gbx::platform {

// Owned instruction storage, rounded to the platform's cache alignment.
// Only aligned uint32_t instruction stores are supported. Not copyable ownership:
// release exactly once. Do not overwrite instructions or release an allocation
// while its code is active. Frontend-owned inline data that is never executed
// may be updated by the owner core with aligned word stores; this does not
// publish instructions or permit concurrent execution from another core.
struct ExecutableMemory {
    uint32_t* words = nullptr;
    size_t size_bytes = 0;
};

// Where generated code lives. pool: the JIT code pool, in internal RAM
// unless the build runs all generated code from PSRAM
// (CONFIG_GBX_JIT_CODE_PSRAM). external: PSRAM, for code that runs where it
// is stored; empty where the platform has none or cannot run code there.
enum class CodeMemory : uint8_t { pool, external };

// False where generated code cannot run at all, as on the host, which has no
// RV32 execution; the JIT backend is then not offered.
bool executable_available();

// Returns empty on failure/unsupported platforms. The host has no RV32 execution.
ExecutableMemory allocate_executable(size_t bytes, CodeMemory where = CodeMemory::pool);
// The largest block allocate_executable() could return now for the pool; 0
// on the host.
size_t largest_executable();
void free_executable(ExecutableMemory& memory);

// Publish a word-aligned subrange before exposing its entry point. Rounding is
// confined to this allocation, so invalidation cannot discard unrelated data.
// One pinned owner task must generate, publish AND execute on the same core.
// Other cores must not enter this code; cross-core lifetime/fence synchronization
// is not supplied here. Do not call during flash operations or from an ISR.
bool publish_executable(const ExecutableMemory& memory, size_t offset, size_t bytes);

#ifdef GBX_PROFILER
// The live allocations outside internal RAM, for attributing profiler
// samples: copies up to out.size() of them and returns how many it copied.
size_t external_executables(std::span<ExecutableMemory> out);
#endif

}  // namespace gbx::platform
