#include "frame.h"

#include <array>
#include <cstring>

namespace gbx::test::protocol {
namespace {

constexpr std::array<uint32_t, 256> crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        uint32_t value = i;
        for (unsigned bit = 0; bit < 8; ++bit) {
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0U);
        }
        table[i] = value;
    }
    return table;
}

constexpr auto kCrcTable = crc_table();

uint32_t update(uint32_t crc, std::span<const uint8_t> bytes) {
    for (const auto byte : bytes) {
        crc = (crc >> 8) ^ kCrcTable[(crc ^ byte) & 255];
    }
    return crc;
}

uint32_t checksum(std::span<const uint8_t> header, std::span<const uint8_t> payload) {
    return ~update(update(0xffffffffU, header.first(20)), payload);
}

}  // namespace

uint16_t load16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t load32(const uint8_t* p) {
    return static_cast<uint32_t>(load16(p)) | (static_cast<uint32_t>(load16(p + 2)) << 16);
}

void store16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

void store32(uint8_t* p, uint32_t value) {
    store16(p, static_cast<uint16_t>(value));
    store16(p + 2, static_cast<uint16_t>(value >> 16));
}

size_t encode(std::span<uint8_t> output, const Frame& frame) {
    const size_t size = kHeaderSize + frame.payload.size();
    if (frame.payload.size() > kMaxPayload || output.size() < size || (frame.flags & ~kReliable) != 0) {
        return 0;
    }
    std::memcpy(output.data(), "GBXP", 4);
    output[4] = kVersion;
    output[5] = static_cast<uint8_t>(frame.kind);
    store16(output.data() + 6, frame.flags);
    store32(output.data() + 8, frame.session);
    store32(output.data() + 12, frame.sequence);
    store32(output.data() + 16, static_cast<uint32_t>(frame.payload.size()));
    if (!frame.payload.empty()) {
        std::memcpy(output.data() + kHeaderSize, frame.payload.data(), frame.payload.size());
    }
    store32(output.data() + 20, checksum(output, frame.payload));
    return size;
}

Parse parse(std::span<const uint8_t> input, Frame& frame, size_t& consumed) {
    consumed = 0;
    if (input.size() < 4) {
        return Parse::incomplete;
    }
    if (std::memcmp(input.data(), "GBXP", 4) != 0) {
        consumed = 1;
        return Parse::noise;
    }
    if (input.size() < kHeaderSize) {
        return Parse::incomplete;
    }
    const auto flags = load16(input.data() + 6);
    const auto length = load32(input.data() + 16);
    if (input[4] != kVersion || (flags & ~kReliable) != 0 || length > kMaxPayload) {
        consumed = 1;
        return Parse::corrupt;
    }
    const size_t size = kHeaderSize + length;
    if (input.size() < size) {
        return Parse::incomplete;
    }
    const auto payload = input.subspan(kHeaderSize, length);
    if (load32(input.data() + 20) != checksum(input, payload)) {
        consumed = 1;
        return Parse::corrupt;
    }
    frame = {static_cast<Kind>(input[5]), flags, load32(input.data() + 8),
             load32(input.data() + 12), payload};
    consumed = size;
    return Parse::complete;
}

}  // namespace gbx::test::protocol
