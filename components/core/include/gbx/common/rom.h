#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace gbx {
// Borrowed ROM storage. A mapped window remains valid until the owner replaces
// the image. GB pins its whole image; a future paged GBA reader can acquire
// smaller windows at mapping boundaries without adding lookups to GB reads.
class Rom {
public:
    void attach(std::span<const uint8_t> bytes) { bytes_ = bytes; }
    std::span<const uint8_t> bytes() const { return bytes_; }
    const uint8_t* window(size_t offset, size_t size) const {
        return offset <= bytes_.size() && size <= bytes_.size() - offset ? bytes_.data() + offset : nullptr;
    }
private:
    std::span<const uint8_t> bytes_;
};
}  // namespace gbx
