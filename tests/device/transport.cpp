#include "transport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "gbx/platform.h"
#include "sdkconfig.h"

namespace gbx::test::transport {
namespace {

using protocol::Frame;
using protocol::Kind;
constexpr uint32_t kAckTimeoutMs = 10000;
constexpr uint64_t kAssemblyTimeoutUs = 10000000;
// The drivers copy frames through their own buffers, so these can be in
// PSRAM, which leaves internal RAM to the JIT code pool.
EXT_RAM_BSS_ATTR std::array<uint8_t, protocol::kMaxFrame> tx;
EXT_RAM_BSS_ATTR std::array<uint8_t, protocol::kMaxFrame> rx;
size_t rx_size = 0;
size_t rx_consumed = 0;
uint64_t partial_since = 0;
uint32_t session_id = 0;
uint32_t sent = 0;
uint32_t acknowledged = 0;
std::array<uint8_t, protocol::kNonceSize> active_nonce{};
std::array<uint8_t, protocol::kNonceSize> pending_nonce{};
bool pending_hello = false;
std::array<uint8_t, protocol::kMaxHelloPayload> hello_reply{};
size_t hello_reply_size = 0;
bool started = false;

struct LogFragment {
    uint16_t length;
    char text[192];
};
QueueHandle_t logs = nullptr;
std::atomic<uint32_t> dropped_logs{0};
std::atomic<uint32_t> truncated_logs{0};

// IDF Log V2 can call vprintf separately for prefix, body and newline.
int capture_log(const char* format, va_list args) {
    LogFragment fragment{};
    const int length = std::vsnprintf(fragment.text, sizeof(fragment.text), format, args);
    if (length <= 0) {
        return length;
    }
    fragment.length = static_cast<uint16_t>(std::min(length, static_cast<int>(sizeof(fragment.text) - 1)));
    if (length >= static_cast<int>(sizeof(fragment.text))) {
        truncated_logs.fetch_add(1, std::memory_order_relaxed);
    }
    if (xPortInIsrContext() || xQueueSend(logs, &fragment, 0) != pdTRUE) {
        dropped_logs.fetch_add(1, std::memory_order_relaxed);
    }
    return length;
}

bool write_raw(std::span<const uint8_t> bytes) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    // USB's ring buffer enqueues whole writes. Bound each write well below it.
    while (!bytes.empty()) {
        const size_t count = std::min(bytes.size(), size_t{2048});
        const int written = usb_serial_jtag_write_bytes(bytes.data(), count, pdMS_TO_TICKS(kAckTimeoutMs));
        if (written != static_cast<int>(count)) {
            return false;
        }
        bytes = bytes.subspan(count);
    }
    return true;
#elif CONFIG_ESP_CONSOLE_UART
    // TX has no software ring: uart_write_bytes blocks, without a timeout API.
    return uart_write_bytes(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM),
                            bytes.data(), bytes.size()) == static_cast<int>(bytes.size());
#else
#error Unsupported console transport
#endif
}

int read_raw(std::span<uint8_t> bytes, uint32_t timeout_ms) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return usb_serial_jtag_read_bytes(bytes.data(), bytes.size(), pdMS_TO_TICKS(timeout_ms));
#else
    return uart_read_bytes(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM), bytes.data(),
                           bytes.size(), pdMS_TO_TICKS(timeout_ms));
#endif
}

bool emit(Kind kind, uint16_t flags, uint32_t sequence, std::span<const uint8_t> payload) {
    if (!started || session_id == 0) {
        return false;
    }
    const size_t size = protocol::encode(tx, {kind, flags, session_id, sequence, payload});
    return size != 0 && write_raw(std::span(tx).first(size));
}

bool drain_logs() {
    if (session_id == 0) {
        return true;
    }
    // Each LOG has cumulative dropped/truncated counters (u32 each), then text.
    // Even a burst of diagnostics cannot monopolize the owner task indefinitely.
    for (unsigned i = 0; i < 8; ++i) {
        LogFragment fragment{};
        if (xQueueReceive(logs, &fragment, 0) != pdTRUE) {
            break;
        }
        std::array<uint8_t, 8 + sizeof(fragment.text)> payload{};
        protocol::store32(payload.data(), dropped_logs.load(std::memory_order_relaxed));
        protocol::store32(payload.data() + 4, truncated_logs.load(std::memory_order_relaxed));
        std::memcpy(payload.data() + 8, fragment.text, fragment.length);
        if (!emit(Kind::log, 0, 0, std::span(payload).first(8 + fragment.length))) {
            return false;
        }
    }
    return true;
}

Receive read_frame(Frame& frame, uint32_t timeout_ms) {
    const uint64_t deadline = platform::monotonic_us() + static_cast<uint64_t>(timeout_ms) * 1000;
    for (;;) {
        if (rx_consumed != 0) {
            rx_size -= rx_consumed;
            std::memmove(rx.data(), rx.data() + rx_consumed, rx_size);
            rx_consumed = 0;
            partial_since = 0;
        }
        size_t consumed = 0;
        const auto status = protocol::parse(std::span(rx).first(rx_size), frame, consumed);
        rx_consumed = consumed;
        if (status == protocol::Parse::complete) {
            return Receive::message;
        }
        if (status == protocol::Parse::corrupt) {
            return Receive::error;
        }
        if (status == protocol::Parse::noise) {
            if (session_id != 0) {
                return Receive::error;
            }
            continue;  // Boot text is allowed only outside an active session.
        }
        const auto now = platform::monotonic_us();
        if (rx_size >= 4) {
            if (partial_since == 0) {
                partial_since = now;
            } else if (now - partial_since >= kAssemblyTimeoutUs) {
                rx_consumed = rx_size;
                return Receive::error;
            }
        }
        if (now >= deadline) {
            return Receive::timeout;
        }
        const uint32_t wait_ms = std::min<uint32_t>(20, static_cast<uint32_t>((deadline - now + 999) / 1000));
        const int count = read_raw(std::span(rx).subspan(rx_size), wait_ms);
        if (count < 0) {
            return Receive::error;
        }
        rx_size += static_cast<size_t>(count);
    }
}

bool accept_ack(const Frame& frame) {
    if (frame.session != session_id || frame.flags != 0 || !frame.payload.empty() ||
        frame.sequence < acknowledged || frame.sequence > sent) {
        return false;
    }
    acknowledged = frame.sequence;
    return true;
}

bool is_hello(const Frame& frame) {
    return frame.kind == Kind::hello && frame.flags == 0 && frame.session == 0 &&
           frame.sequence == 0 && frame.payload.size() == protocol::kNonceSize;
}

bool same_nonce(std::span<const uint8_t> nonce) {
    return session_id != 0 && nonce.size() == active_nonce.size() &&
           std::equal(nonce.begin(), nonce.end(), active_nonce.begin());
}

bool wait_for_ack() {
    const uint64_t deadline = platform::monotonic_us() + static_cast<uint64_t>(kAckTimeoutMs) * 1000;
    const auto previous = acknowledged;
    while (acknowledged == previous) {
        const auto now = platform::monotonic_us();
        if (now >= deadline) {
            return false;
        }
        Frame frame{};
        const auto result = read_frame(frame, static_cast<uint32_t>((deadline - now + 999) / 1000));
        if (result != Receive::message) {
            return false;
        }
        if (is_hello(frame)) {
            if (same_nonce(frame.payload)) {
                if (hello_reply_size == 0 ||
                    !emit(Kind::hello_reply, 0, 0, std::span(hello_reply).first(hello_reply_size))) {
                    return false;
                }
                continue;
            }
            // Cancel this transfer, but let the owner handle the reconnect.
            // Copy the nonce because the receive buffer will be reused.
            std::copy(frame.payload.begin(), frame.payload.end(), pending_nonce.begin());
            pending_hello = true;
            return false;
        }
        if (frame.kind != Kind::ack || !accept_ack(frame)) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool start() {
    if (started) {
        return true;
    }
    esp_err_t error;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t config{.tx_buffer_size = 16384, .rx_buffer_size = 16384};
    error = usb_serial_jtag_driver_install(&config);
#else
    error = uart_driver_install(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM), 16384, 0, 0, nullptr, 0);
#endif
    if (error != ESP_OK) {
        ESP_LOGE("test.link", "Console driver: %s", esp_err_to_name(error));
        return false;
    }
    logs = xQueueCreate(16, sizeof(LogFragment));
    if (logs == nullptr) {
        ESP_LOGE("test.link", "Cannot allocate log queue");
        return false;
    }
    std::fflush(stdout);
    // All regular runtime logs now go through frames. ROM/panic output can
    // still bypass this hook and is treated as a broken session by the runner.
    esp_log_set_vprintf(capture_log);
    started = true;
    return true;
}

bool begin_session(std::span<const uint8_t> nonce) {
    if (nonce.size() != active_nonce.size()) {
        return false;
    }
    if (same_nonce(nonce)) {
        return true;
    }
    const auto old = session_id;
    do {
        session_id = esp_random();
    } while (session_id == 0 || session_id == old);
    std::copy(nonce.begin(), nonce.end(), active_nonce.begin());
    sent = acknowledged = 0;
    hello_reply_size = 0;
    return true;
}

void abandon_session() {
    session_id = sent = acknowledged = 0;
    hello_reply_size = 0;
}

Receive receive(Frame& frame, uint32_t timeout_ms) {
    if (pending_hello) {
        pending_hello = false;
        frame = {Kind::hello, 0, 0, 0, pending_nonce};
        return Receive::message;
    }
    if (!drain_logs()) {
        return Receive::error;
    }
    const uint64_t deadline = platform::monotonic_us() + static_cast<uint64_t>(timeout_ms) * 1000;
    for (;;) {
        const auto now = platform::monotonic_us();
        if (now >= deadline) {
            return Receive::timeout;
        }
        const auto result = read_frame(frame, static_cast<uint32_t>((deadline - now + 999) / 1000));
        if (result != Receive::message) {
            return result;
        }
        if (frame.kind == Kind::ack) {
            if (!accept_ack(frame)) {
                return Receive::error;
            }
            continue;
        }
        if (frame.flags != 0 || frame.sequence != 0) {
            return Receive::error;
        }
        if (is_hello(frame)) {
            return Receive::message;
        }
        if (session_id == 0 || frame.session != session_id) {
            return Receive::error;
        }
        return Receive::message;
    }
}

bool send(Kind kind, std::span<const uint8_t> payload) {
    if (kind == Kind::hello_reply &&
        (payload.size() > hello_reply.size() || payload.size() < protocol::kNonceSize ||
         !same_nonce(payload.first(protocol::kNonceSize)))) {
        return false;
    }
    if (!emit(kind, 0, 0, payload)) {
        return false;
    }
    if (kind == Kind::hello_reply) {
        std::copy(payload.begin(), payload.end(), hello_reply.begin());
        hello_reply_size = payload.size();
    }
    return true;
}

bool send_data(std::span<const uint8_t> payload) {
    if (sent == UINT32_MAX || payload.size() > protocol::kMaxPayload) {
        return false;
    }
    while (sent - acknowledged >= protocol::kWindow) {
        if (!wait_for_ack()) {
            return false;
        }
    }
    if (!drain_logs() || !emit(Kind::data, protocol::kReliable, sent + 1, payload)) {
        return false;
    }
    ++sent;
    return true;
}

bool finish() {
    while (acknowledged < sent) {
        if (!wait_for_ack()) {
            return false;
        }
    }
    return true;
}

}  // namespace gbx::test::transport
