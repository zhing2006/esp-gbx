#pragma once

#include <atomic>
#include <cstdint>
#include <span>

#include "gbx/common/scheduler.h"

namespace gbx {
struct TraceRecord {
    Tick time;
    uint16_t pc;
    uint16_t address;
    uint8_t data;
    uint8_t operation;  // 0=read, 1=write; instruction fetches are reads.
};

// Storage is supplied at a safe boundary. One CPU-side producer and one
// off-core consumer; no allocation, blocking, formatting or aggregation here.
// Test capture uses the same records, but ANY drop invalidates the result.
class TraceBuffer {
public:
    void reset(std::span<TraceRecord> records) {
        records_ = records; head_ = 0; tail_ = 0; dropped_ = 0;
    }
    void push(TraceRecord record) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= records_.size()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        records_[head % records_.size()] = record;
        head_.store(head + 1, std::memory_order_release);
    }
    bool pop(TraceRecord& record) {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        record = records_[tail % records_.size()];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }
    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
private:
    std::span<TraceRecord> records_;
    std::atomic<uint32_t> head_{0}, tail_{0}, dropped_{0};
};
}  // namespace gbx
