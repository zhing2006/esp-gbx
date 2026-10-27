#pragma once

// Sampling profiler of the profiling test firmware (GBX_PROFILER), see
// docs/hardware-testing.md. Other builds contain none of it.
#ifdef GBX_PROFILER
#include <atomic>
#include <cstdint>

#include "gbx/executable.h"
#include "gbx/gb/jit/sm83_jit.h"
#include "gbx/gb/system.h"
#include "gbx/gba/jit/arm7_jit.h"

namespace gbx::test::profiler {

// One emulation job: a timer interrupt on the job's core counts the
// interrupted program counter in 8-byte buckets of the application text,
// internal RAM, which holds IRAM code and JIT code, and the chip's mask ROM,
// and in 16-byte buckets of
// the generated code outside internal RAM. A JIT job also keeps the Sm83Jit
// counters accumulated between start() and stop().
class Profile {
public:
    Profile() = default;
    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;
    ~Profile();
    // On the core to sample, before the job runs. A failed start leaves
    // the job unprofiled. With a gate, samples count only while it is set,
    // so that another core's profile covers the job's own run.
    void start(const gb::Sm83Jit* jit, const std::atomic<bool>* gate = nullptr);
    // A GBA job: the JIT's code regions, and the CPU core's waits for the
    // renderer as counters.
    void start(const gba::Arm7Jit* jit);
    void stop();
    // LOG frames for the PC runner; the transport owner calls this after the
    // job's result. Sends nothing for a job that was not profiled.
    bool send(uint8_t backend, unsigned core) const;
    // From the timer interrupt only.
    void record(uint32_t pc);

private:
    static constexpr size_t kCodeRegions = 4;
    uint32_t* text_ = nullptr;
    uint32_t* ram_ = nullptr;
    uint32_t* rom_ = nullptr;
    platform::ExecutableMemory code_[kCodeRegions]{};
    uint32_t* code_samples_[kCodeRegions]{};
    size_t code_count_ = 0;
    void* timer_ = nullptr;
    const std::atomic<bool>* gate_ = nullptr;
    uint32_t samples_ = 0, other_ = 0;
    uintptr_t pool_ = 0, gateway_ = 0, store_ = 0;
    size_t pool_bytes_ = 0, gateway_bytes_ = 0, store_bytes_ = 0;
    bool jit_ = false, gba_ = false;
    gb::Sm83JitCounters counters_{};
    gb::OffloadCounters offload_{};
};

}  // namespace gbx::test::profiler
#endif
