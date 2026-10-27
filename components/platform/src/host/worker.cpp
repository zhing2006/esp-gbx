#include "gbx/worker.h"

#include <chrono>
#include <new>
#include <semaphore>
#include <thread>

namespace gbx::platform {
using Semaphore = std::counting_semaphore<>;
Notification::~Notification() { delete static_cast<Semaphore*>(state_); }
bool Notification::initialize(unsigned, unsigned initial) {
    delete static_cast<Semaphore*>(state_);
    state_ = new (std::nothrow) Semaphore(initial);
    return state_ != nullptr;
}
void Notification::wait() { static_cast<Semaphore*>(state_)->acquire(); }
bool Notification::wait_for(uint32_t timeout_ms) {
    return static_cast<Semaphore*>(state_)->try_acquire_for(std::chrono::milliseconds(timeout_ms));
}
void Notification::signal() { static_cast<Semaphore*>(state_)->release(); }
Worker::~Worker() { join(); }
bool Worker::start(void (*entry)(void*), void* context, unsigned, const char*, unsigned, unsigned, StackMemory) {
    state_ = new (std::nothrow) std::thread(entry, context);
    return state_ != nullptr;
}
void Worker::join() {
    if (!state_) return;
    auto* thread = static_cast<std::thread*>(state_);
    thread->join(); delete thread; state_ = nullptr;
}
unsigned current_core() { return 1; }
}  // namespace gbx::platform
