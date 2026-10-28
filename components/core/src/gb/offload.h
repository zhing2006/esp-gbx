#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "gbx/worker.h"

namespace gbx::gb {
// The worker thread on the other core, shared by the PPU's scanline rows and
// the APU's sound batches. Each queue is a single-producer, single-consumer
// ring: the CPU owner fills slots in order and the worker consumes them in
// the same order. Rows and sound batches never depend on each other, so the
// worker serves the queues in turn, one item from each queue with pending
// work, and neither waits behind the other's backlog. A full queue makes
// the CPU owner wait, whichever queue it is.
//
// Each side writes only its own counters, so a handoff costs no lock while
// both cores are busy. A side sleeps only when it cannot continue, and the
// other side wakes it in batches: an idle worker after half a queue of new
// items or an urgent submission, a waiting producer once half of the queue
// it waits for is free again. The seq_cst stores and loads of a counter and
// the other side's flag make a missed wakeup impossible; a spurious one
// only repeats the check.
//
// With GBX_SINGLE_CORE there is no worker: the CPU owner consumes each item
// as it submits it. On one core a worker only adds task switches, and its
// queues take PSRAM.
class Offload {
public:
    enum Queue : unsigned { rows, sound, kQueues };
    // submit() consumes the item, so a queue needs room only for the one
    // being filled.
    static constexpr bool kInline = GBX_SINGLE_CORE;
    // Consumes item `index`, counting from zero, on the worker thread, or
    // in submit() when inline.
    using Consume = void (*)(void* context, uint32_t index);
    Offload() = default;
    Offload(const Offload&) = delete;
    Offload& operator=(const Offload&) = delete;
    // Stops the worker after the items it has already taken.
    ~Offload();
    // The worker's task priority on the firmware, before start(): 5 by
    // default, above the present task, LVGL and the app's tasks, as the
    // GB's 16 rows last only 2 ms.
    void set_priority(unsigned priority) { priority_ = priority; }
    bool start();
    // Before the first submission to the queue; capacity is even.
    void attach(Queue queue, uint32_t capacity, Consume consume, void* context) {
        auto& q = queues_[queue];
        q.capacity = capacity; q.consume = consume; q.context = context;
    }
    // Producer side, on the CPU owner. acquire() returns the index of the
    // next item to fill once its slot is free; submit() hands it over.
    uint32_t acquire(Queue queue) {
        auto& q = queues_[queue];
        if (!kInline && q.submitted - q.completed.load(std::memory_order_acquire) == q.capacity)
            wait(queue, q.submitted - q.capacity / 2);
        return q.submitted;
    }
    void submit(Queue queue, bool now = false) {
        auto& q = queues_[queue];
        if constexpr (kInline) {
            q.consume(q.context, q.submitted++);
            return;
        }
        ++q.submitted;
        q.published.store(q.submitted, std::memory_order_seq_cst);
        if ((now || q.submitted - q.completed.load(std::memory_order_relaxed) >= q.capacity / 2) &&
            idle_.load(std::memory_order_seq_cst)) ready_.signal();
    }
    // Until the worker has consumed every submitted item of the queue.
    void finish(Queue queue) { wait(queue, queues_[queue].submitted); }
    // Until the worker has consumed the items before index `count`.
    void finish_before(Queue queue, uint32_t count) { wait(queue, count); }
    // Whether the worker polls for the next item before it sleeps, see
    // kPolls; on at first. Any task; the worker takes it at its next poll.
    void set_polling(bool polling) { polls_.store(polling ? kPolls : 0, std::memory_order_relaxed); }
private:
    // A poll takes about 125 us on ESP32-P4 at 360 MHz. While a game runs
    // at normal speed, nearly every next item comes within 50 us of the
    // last, so a poll runs out about once a frame, after the run ends; the
    // present task and LVGL, below the worker's priority, lose that time.
    // Without polling the worker sleeps whenever it runs out, and costs
    // the CPU core a wakeup for every half queue.
    static constexpr unsigned kPolls = 1024;
    unsigned priority_ = 5;
    std::atomic<unsigned> polls_{kPolls};
    struct State {
        uint32_t capacity = 0;
        Consume consume = nullptr;
        void* context = nullptr;
        uint32_t submitted = 0;  // Producer only.
        std::atomic<uint32_t> published{0}, completed{0};
    };
    std::array<State, kQueues> queues_;
    platform::Notification ready_, space_;
    platform::Worker worker_;
    bool started_ = false;
    // A waiting producer: the queue and the completed count it needs.
    std::atomic<bool> waiting_{false}, idle_{false}, stop_{false};
    std::atomic<uint32_t> resume_{0};
    std::atomic<unsigned> resume_queue_{0};
    static bool reached(uint32_t count, uint32_t target) { return static_cast<int32_t>(count - target) >= 0; }
    void wait(Queue queue, uint32_t target);
    bool pending(const std::array<uint32_t, kQueues>& taken) const;
    static void run(void* context) { static_cast<Offload*>(context)->loop(); }
    void loop();
};
}  // namespace gbx::gb
