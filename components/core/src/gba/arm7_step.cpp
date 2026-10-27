// The interpreter on the flat bus. Only the single-step tests use this bus,
// so this copy stays out of the execution core in internal RAM.

#include "executor.h"

namespace gbx::gba {

void arm7_step(Arm7State& state, const Bus& bus) {
    Executor(state, bus).step();
}

void arm7_interrupt(Arm7State& state, const Bus& bus) {
    Executor(state, bus).interrupt();
}

}  // namespace gbx::gba
