#include "cst3530_touch.h"

#include <algorithm>
#include <array>

#include "esp_check.h"
#include "esp_log.h"

// Register addresses and the report format follow the vendor's driver, see
// refs/boards/waveshare_c5_lcd28/esp_lcd_touch_cst3530.c.txt and
// docs/boards/waveshare_c5_lcd28.md.
namespace gbx::board {

namespace {

constexpr const char* kTag = "cst3530";
constexpr uint16_t kAddress = 0x58;
constexpr uint32_t kSpeedHz = 400000;
constexpr int kWidth = 240;
constexpr int kHeight = 320;
constexpr int kMaxPoints = 5;
// Registers have 32-bit addresses, sent high byte first. The report starts
// with four bytes whose last holds the point count in its low nibble; then
// five bytes a point: X and Y low bytes, strength, Y and X high nibbles,
// and the event (1 press, 2 hold, 0 lift) over the finger ID. The first
// point comes with the report, the others from kNextPoints. Writing
// kEndRead, with no data, hands the report back to the controller.
constexpr uint32_t kReport = 0xd0070000;
constexpr uint32_t kNextPoints = 0xd0070900;
constexpr uint32_t kEndRead = 0xd00002ab;
constexpr int kHeaderBytes = 4;
constexpr int kPointBytes = 5;

struct Controller {
    esp_lcd_touch_t base{};
    i2c_master_dev_handle_t device = nullptr;
};

Controller controller;

std::array<uint8_t, 4> address(uint32_t reg) {
    return {static_cast<uint8_t>(reg >> 24), static_cast<uint8_t>(reg >> 16), static_cast<uint8_t>(reg >> 8),
            static_cast<uint8_t>(reg)};
}

// The address and the read as two transfers, as the vendor's driver does.
esp_err_t read(uint32_t reg, uint8_t* data, size_t size) {
    const auto a = address(reg);
    esp_err_t err = i2c_master_transmit(controller.device, a.data(), a.size(), 50);
    return err == ESP_OK ? i2c_master_receive(controller.device, data, size, 50) : err;
}

esp_err_t end_read() {
    const auto a = address(kEndRead);
    return i2c_master_transmit(controller.device, a.data(), a.size(), 50);
}

esp_err_t read_data(esp_lcd_touch_handle_t tp) {
    uint8_t report[kHeaderBytes + kMaxPoints * kPointBytes];
    esp_err_t err = read(kReport, report, kHeaderBytes + kPointBytes);
    int listed = report[3] & 0x0f;
    if (err == ESP_OK && listed > 1 && listed <= kMaxPoints) {
        err = read(kNextPoints, report + kHeaderBytes + kPointBytes, (listed - 1) * kPointBytes);
    }
    if (err != ESP_OK) {
        return err;
    }
    err = end_read();
    esp_lcd_touch_point_data_t points[kMaxPoints];
    int count = 0;
    for (int i = 0; listed <= kMaxPoints && i < listed; ++i) {
        const uint8_t* p = report + kHeaderBytes + i * kPointBytes;
        const int x = (p[3] & 0x0f) << 8 | p[0];
        const int y = (p[3] & 0xf0) << 4 | p[1];
        if ((p[4] >> 4) != 0 && x < kWidth && y < kHeight) {
            points[count++] = {.track_id = static_cast<uint8_t>(p[4] & 0x0f),
                               .x = static_cast<uint16_t>(x),
                               .y = static_cast<uint16_t>(y),
                               .strength = p[2]};
        }
    }
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = count;
    std::copy_n(points, count, tp->data.coords);
    portEXIT_CRITICAL(&tp->data.lock);
    return err;
}

bool get_xy(esp_lcd_touch_handle_t tp, uint16_t* x, uint16_t* y, uint16_t* strength, uint8_t* point_num,
            uint8_t max_point_num) {
    portENTER_CRITICAL(&tp->data.lock);
    *point_num = std::min(tp->data.points, max_point_num);
    for (int i = 0; i < *point_num; ++i) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength != nullptr) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return *point_num > 0;
}

esp_err_t del(esp_lcd_touch_handle_t) {
    i2c_master_bus_rm_device(controller.device);
    controller.device = nullptr;
    return ESP_OK;
}

}  // namespace

esp_err_t new_cst3530_touch(i2c_master_bus_handle_t bus, esp_lcd_touch_handle_t* out) {
    Controller& c = controller;
    ESP_RETURN_ON_FALSE(c.device == nullptr, ESP_ERR_INVALID_STATE, kTag, "already created");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, kAddress, 50), kTag, "no answer at 0x%02x", kAddress);
    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = kAddress;
    device.scl_speed_hz = kSpeedHz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device, &c.device), kTag, "I2C device");
    esp_lcd_touch_t& tp = c.base;
    portMUX_INITIALIZE(&tp.data.lock);
    tp.read_data = read_data;
    tp.get_xy = get_xy;
    tp.del = del;
    tp.config.x_max = kWidth;
    tp.config.y_max = kHeight;
    tp.config.rst_gpio_num = GPIO_NUM_NC;
    tp.config.int_gpio_num = GPIO_NUM_NC;
    *out = &tp;
    return ESP_OK;
}

}  // namespace gbx::board
