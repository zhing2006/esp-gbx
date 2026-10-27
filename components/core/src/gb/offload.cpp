#include "offload.h"

#ifdef GBX_PROFILER
#include "gbx/gb/system.h"
#include "gbx/platform.h"
#endif

namespace gbx::gb {
#ifdef GBX_PROFILER
OffloadCounters offload_counters;
#endif
Offload::~Offload() {
    if (!started_) return;
    stop_.store(true, std::memory_order_seq_cst);
    if (idle_.load(std::memory_order_seq_cst)) ready_.signal();
    worker_.join();
}
bool Offload::start() {
    if (started_) return true;
    if (!ready_.initialize(1) || !space_.initialize(1)) return false;
    started_ = worker_.start(run, this, platform::current_core() ^ 1, "gbx-render", 4096, priority_);
    return started_;
}
void Offload::wait(Queue queue, uint32_t target) {
    auto& q = queues_[queue];
    if (reached(q.completed.load(std::memory_order_acquire), target)) return;
#ifdef GBX_PROFILER
    const auto start = platform::monotonic_us();
#endif
    resume_queue_.store(queue, std::memory_order_relaxed);
    resume_.store(target, std::memory_order_relaxed);
    waiting_.store(true, std::memory_order_seq_cst);
    if (idle_.load(std::memory_order_seq_cst)) ready_.signal();
    while (!reached(q.completed.load(std::memory_order_seq_cst), target)) space_.wait();
    waiting_.store(false, std::memory_order_relaxed);
#ifdef GBX_PROFILER
    ++offload_counters.waits[queue];
    offload_counters.wait_us[queue] += platform::monotonic_us() - start;
#endif
}
bool Offload::pending(const std::array<uint32_t, kQueues>& taken) const {
    for (unsigned q = 0; q < kQueues; ++q)
        if (queues_[q].published.load(std::memory_order_seq_cst) != taken[q]) return true;
    return false;
}
void Offload::loop() {
    std::array<uint32_t, kQueues> taken{};
    for (;;) {
        // While the CPU core keeps producing, a short poll finds the next
        // item before sleeping would make the producer wake us.
        bool work = false;
        const unsigned polls = polls_.load(std::memory_order_relaxed);
        for (unsigned i = 0; i < polls && !(work = pending(taken)); ++i) {}
        while (!work) {
            idle_.store(true, std::memory_order_seq_cst);
            if (stop_.load(std::memory_order_seq_cst)) return;
            if (!(work = pending(taken))) ready_.wait();
            idle_.store(false, std::memory_order_relaxed);
        }
        for (unsigned queue = 0; queue < kQueues; ++queue) {
            auto& q = queues_[queue];
            if (q.published.load(std::memory_order_acquire) == taken[queue]) continue;
            q.consume(q.context, taken[queue]);
            const auto done = ++taken[queue];
            q.completed.store(done, std::memory_order_seq_cst);
            if (waiting_.load(std::memory_order_seq_cst) && resume_queue_.load(std::memory_order_relaxed) == queue &&
                reached(done, resume_.load(std::memory_order_relaxed)))
                space_.signal();
        }
    }
}
}  // namespace gbx::gb
