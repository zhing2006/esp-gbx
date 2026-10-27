#pragma once

#include <cstdint>
#include <span>

namespace gbx::test {

// Executes ARM7_RUN requests (arm7_execution.h) from the card's cases.bin on
// the interpreter and the JIT. A reader task on the other core streams the
// selected records while the transport task executes, judges, compares and
// sends. open() allocates every buffer,
// nothing is allocated per request or case. The transport owner invokes run()
// synchronously.
class Arm7Runner {
public:
    bool open(const char* path);
    bool run(std::span<const uint8_t> request);
};

}  // namespace gbx::test
