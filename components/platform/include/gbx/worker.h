#pragma once

#include <cstdint>

namespace gbx::platform {
// Created outside execution paths. A notification is a counting semaphore;
// release/acquire publish and consume all preceding memory accesses.
class Notification {
public:
    Notification() = default;
    ~Notification();
    Notification(const Notification&) = delete;
    Notification& operator=(const Notification&) = delete;
    bool initialize(unsigned capacity, unsigned initial = 0);
    void wait();
    // Waits up to timeout_ms; false when no signal came.
    bool wait_for(uint32_t timeout_ms);
    void signal();
private:
    void* state_ = nullptr;
};

// Where a worker's stack lives on the firmware. External memory suits tasks
// that only handle files, menus or console commands, and leaves internal RAM
// to the emulation; tasks on the emulation's paths keep internal stacks,
// away from the JIT code pools where the heap allows.
enum class StackMemory : uint8_t { internal, external };

class Worker {
public:
    Worker() = default;
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    // Firmware pins to core. Hosts use an ordinary worker thread. The task
    // name, stack size, priority and stack memory apply to the firmware.
    bool start(void (*entry)(void*), void* context, unsigned core, const char* name = "gbx-render",
               unsigned stack_bytes = 4096, unsigned priority = 5, StackMemory stack = StackMemory::internal);
    void join();
private:
    void* state_ = nullptr;
};
unsigned current_core();
}  // namespace gbx::platform
