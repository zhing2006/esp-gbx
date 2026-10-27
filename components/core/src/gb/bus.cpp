#include "gbx/gb/bus.h"

namespace gbx::gb {

uint8_t FlatBus::cycle(void* context, BusOperation operation, uint16_t address, uint8_t data) {
    auto& self = *static_cast<FlatBus*>(context);
    if (operation == BusOperation::hold) {
        return 0;
    }
    // A mapped bus will synchronize devices here, at the start of this cycle.
    uint8_t result = 0;
    if (operation == BusOperation::read) {
        result = self.memory_[address];
    } else if (operation == BusOperation::write) {
        self.memory_[address] = data;
    }
    ++self.mcycles_;
    return result;
}

uint8_t TraceBus::cycle(void* context, BusOperation operation, uint16_t address, uint8_t data) {
    auto& self = *static_cast<TraceBus*>(context);
    const uint8_t result = self.underlying_.cycle(self.underlying_.context, operation, address, data);
    self.pins_.signals = 0;
    if (operation == BusOperation::read || operation == BusOperation::write) {
        self.pins_.address = address;
        self.pins_.data = operation == BusOperation::read ? result : data;
        self.pins_.signals = operation == BusOperation::read ? 5 : 6;
    }
    if (self.size_ < self.samples_.size()) {
        self.samples_[self.size_] = self.pins_;
    }
    ++self.size_;
    return result;
}

}  // namespace gbx::gb
