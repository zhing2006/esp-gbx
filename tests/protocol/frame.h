#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Transport framing only. Execution requests and CPU result schemas are separate.
namespace gbx::test::protocol {

inline constexpr uint8_t kVersion = 3;
inline constexpr size_t kNonceSize = 8;
inline constexpr size_t kMaxHelloPayload = 512;
inline constexpr size_t kHeaderSize = 24;
inline constexpr size_t kMaxPayload = 8192;
inline constexpr size_t kMaxFrame = kHeaderSize + kMaxPayload;
inline constexpr uint16_t kReliable = 1;
inline constexpr uint32_t kWindow = 4;

// Little-endian header: magic "GBXP", u8 version=3, u8 kind, u16 flags,
// u32 session, u32 sequence, u32 payload length, u32 CRC, then payload.
// CRC-32/ISO-HDLC (zlib.crc32) covers the first 20 header bytes and payload.
// HELLO uses session/sequence zero and an opaque 8-byte connection nonce.
// HELLO_REPLY starts with the echoed nonce; its remaining metadata is at most
// kMaxHelloPayload - kNonceSize bytes. Repeating a nonce keeps the same active
// session. A new nonce cancels any old transfer and establishes a new session.
// Reliable DATA sequence numbers start at 1 in that session; ACK.sequence
// cumulatively acknowledges DATA. Other sequences are zero. One control
// request at a time; only ACK or a reconnecting HELLO may interrupt DATA.
// A lost/corrupt frame or ACK timeout fails the transfer, never implies success.
// Retransmission and resuming execution are not implemented yet.
enum class Kind : uint8_t {
    hello = 1,
    hello_reply = 2,
    ping = 3,
    pong = 4,
    data = 5,
    ack = 6,
    error = 7,
    log = 8,
    run = 9,
    done = 10,
    jit_test = 11,  // Native backend checks; layout in jit_test.h.
    rom_test = 12,  // Whole-machine requests; layout in rom_test.h.
    display_test = 13,  // Display pattern checks; layout in display_test.h.
    audio_test = 14,  // Mixer accounting checks; layout in audio_test.h.
    arm7_run = 15,  // ARM7TDMI single-step requests; layout in arm7_execution.h.
    gba_rom_test = 16,  // Whole GBA requests; layout in gba_rom_test.h.
};

struct Frame {
    Kind kind;
    uint16_t flags;
    uint32_t session;
    uint32_t sequence;
    std::span<const uint8_t> payload;
};

enum class Parse { incomplete, noise, corrupt, complete };

uint16_t load16(const uint8_t* p);
uint32_t load32(const uint8_t* p);
void store16(uint8_t* p, uint16_t value);
void store32(uint8_t* p, uint32_t value);

size_t encode(std::span<uint8_t> output, const Frame& frame);
// `consumed` identifies bytes the caller may discard. On success, payload is
// a view into input and must be used before those bytes are discarded.
Parse parse(std::span<const uint8_t> input, Frame& frame, size_t& consumed);

}  // namespace gbx::test::protocol
