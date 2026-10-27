#pragma once

#include <cstdint>
#include <span>

#include "frame.h"

namespace gbx::test::transport {

// The caller task is the sole sender/receiver. ESP log callbacks only queue
// text fragments; they never call the transport. No raw stdio after start().
// UART preserves CONFIG_ESP_CONSOLE_UART_BAUDRATE from the console setup;
// USB Serial/JTAG has no UART baud rate.
bool start();
// Repeated HELLOs with the same nonce keep their active session and counters.
bool begin_session(std::span<const uint8_t> nonce);
// Does not discard a reconnecting HELLO saved while waiting for an ACK.
void abandon_session();

enum class Receive { message, timeout, error };
// Payload is valid until the next receive/send/finish call. ACKs are consumed
// internally. A timeout with no complete frame does not end the session.
Receive receive(protocol::Frame& frame, uint32_t timeout_ms);
bool send(protocol::Kind kind, std::span<const uint8_t> payload);
// Up to four 8 KiB DATA batches may be in flight. A full window waits for ACK;
// finish waits for all accepted batches. Errors/timeouts or a new HELLO fail
// the transfer. Return to receive() afterward so a saved HELLO is handled.
bool send_data(std::span<const uint8_t> payload);
bool finish();

}  // namespace gbx::test::transport
