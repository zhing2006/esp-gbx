#pragma once

namespace gbx::console {

// Reads commands from the serial console on a task of its own, see
// docs/debug-design.md. Call once, after the board is up.
void start();

#ifdef GBX_PROFILER
// A command of the profiling build, see profile.cpp. False when the line is
// none of them.
bool profile_command(char* line);
#endif

}  // namespace gbx::console
