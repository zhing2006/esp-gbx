#pragma once

#include <array>
#include <cstdio>
#include <span>

#include "execution.h"
#include "gbx/gb/jit/sm83_jit.h"

namespace gbx::test {

// Allocates its workspaces only when opening the pack, never per instruction.
// The transport owner invokes run() synchronously; no other task uses the file.
class Sm83Runner {
public:
    ~Sm83Runner();
    bool open(const char* path);
    bool run(std::span<const uint8_t> request);

private:
    struct Group { uint16_t opcode; uint32_t count, offset, size; };
    std::FILE* file_ = nullptr;
    uint8_t* memory_ = nullptr;
    uint8_t* group_data_ = nullptr;
    size_t capacity_ = 0;
    gb::Sm83Jit jit_;
    std::array<Group, 512> groups_{};
    size_t group_count_ = 0;
    std::array<uint32_t, execution::kMaxSelected> selected_{};
};

}  // namespace gbx::test
