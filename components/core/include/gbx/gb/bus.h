#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace gbx::gb {

enum class BusOperation : uint8_t {
    read,
    write,
    idle,
    // Observe the bus with the CPU clock stopped. No M-cycle elapses.
    hold,
};

// Each read, write or idle call consumes one M-cycle. The implementation must
// advance devices to the access time BEFORE accessing memory, including for
// opcode fetches. It owns the clock and any address mapping. A hold only samples
// the stopped bus; it must not advance devices or access memory.
//
// One function pointer keeps the interpreter independent of the memory map
// without virtual calls or a copy of the decoder for every bus type. Tracing is
// a separate bus implementation, so a normal access has no trace callback or
// trace-enabled branch. Revisit dispatch costs only after measuring on hardware.
struct Bus {
    void* context;
    uint8_t (*cycle)(void* context, BusOperation operation, uint16_t address, uint8_t data);

    uint8_t read(uint16_t address) const { return cycle(context, BusOperation::read, address, 0); }
    void write(uint16_t address, uint8_t data) const { cycle(context, BusOperation::write, address, data); }
    void idle() const { cycle(context, BusOperation::idle, 0, 0); }
    void hold() const { cycle(context, BusOperation::hold, 0, 0); }
};

// The caller owns the memory and keeps it alive while this bus is in use.
// There are deliberately no IO registers, even at FF00..FFFF.
class FlatBus {
public:
    explicit FlatBus(std::span<uint8_t, 65536> memory) : memory_(memory) {}
    Bus bus() { return {this, cycle}; }
    uint64_t mcycles() const { return mcycles_; }

private:
    static uint8_t cycle(void* context, BusOperation operation, uint16_t address, uint8_t data);
    std::span<uint8_t, 65536> memory_;
    uint64_t mcycles_ = 0;
};

// M-cycle boundary samples, not T-cycle waveforms. Signal bits are active-high:
// read=1, write=2, memory request=4. Idle/hold samples have no active signals.
// Address/data retain the last transfer for inactive cycles, following the
// convention in SingleStepTests/sm83/README.MD. They do not describe internal
// IDU/ALU buses or claim that electrically disconnected pins are driven.
struct BusSample {
    uint16_t address = 0;
    uint8_t data = 0;
    uint8_t signals = 0;
};

// Test-only decorator for any bus. Storage is supplied before execution; no
// instruction or cycle allocates. An undersized buffer is reported, not hidden.
// The decorator and its underlying bus context must outlive the returned Bus.
class TraceBus {
public:
    TraceBus(Bus underlying, std::span<BusSample> samples) : underlying_(underlying), samples_(samples) {}
    Bus bus() { return {this, cycle}; }
    size_t size() const { return size_; }
    bool overflowed() const { return size_ > samples_.size(); }

private:
    static uint8_t cycle(void* context, BusOperation operation, uint16_t address, uint8_t data);
    Bus underlying_;
    std::span<BusSample> samples_;
    BusSample pins_{};
    size_t size_ = 0;
};

}  // namespace gbx::gb
