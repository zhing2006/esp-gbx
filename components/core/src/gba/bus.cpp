#include "gbx/gba/bus.h"

namespace gbx::gba {

uint32_t TestBus::cycle(void* context, uint32_t request, uint32_t address, uint32_t data) {
    auto& self = *static_cast<TestBus*>(context);
    ++self.cycles_;
    const uint32_t kind = request & 3;
    if (kind == kIdle) return 0;
    const unsigned size = 1u << ((request >> 2) & 3);
    const uint32_t mask = size == 4 ? 0xffffffff : (1u << (8 * size)) - 1;
    if (kind == kFetch) {
        data = (address == self.base_ ? self.opcode_ : address) & mask;
    } else if (kind == kRead) {
        data = 0;
        const auto& expected = self.expected_;
        if (self.size_ < expected.size() && expected[self.size_].kind == kRead &&
            expected[self.size_].address == address) {
            data = expected[self.size_].data;
        } else {
            for (const auto& record : expected) {
                if (record.kind == kRead && record.address == address) {
                    data = record.data;
                    break;
                }
            }
        }
    }
    if (self.size_ < self.actual_.size()) {
        auto& record = self.actual_[self.size_];
        record.kind = static_cast<uint8_t>(kind);
        record.size = static_cast<uint8_t>(size);
        record.access = static_cast<uint8_t>((request & kSequential ? 1 : 0) | (kind == kFetch ? 2 : 0) |
                                             (request & kLock ? 8 : 0));
        record.cycle = static_cast<uint8_t>(self.cycles_);
        record.address = address;
        record.data = data;
    }
    ++self.size_;
    return data;
}

}  // namespace gbx::gba
