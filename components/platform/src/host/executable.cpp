#include "gbx/executable.h"

namespace gbx::platform {

// PC builds can compile the encoder/cache, but cannot execute RV32 blocks.
bool executable_available() { return false; }
ExecutableMemory allocate_executable(size_t, CodeMemory) { return {}; }
size_t largest_executable() { return 0; }
void free_executable(ExecutableMemory& memory) { memory = {}; }
bool publish_executable(const ExecutableMemory&, size_t, size_t) { return false; }
#ifdef GBX_PROFILER
size_t external_executables(std::span<ExecutableMemory>) { return 0; }
#endif

}  // namespace gbx::platform
