#include "axs15260_touch.h"

#include <algorithm>
#include <atomic>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

// Report format and read pacing follow the AIBox driver and its touch task,
// see refs/boards/sila_p4x/upstream-axs15260.c.txt and docs/boards/sila_p4x.md.
namespace gbx::board {

namespace {

constexpr const char* kTag = "axs15260";
constexpr uint16_t kAddress = 0x3b;
constexpr uint32_t kSpeedHz = 100000;
constexpr uint8_t kVersionRegister = 0x0c;
constexpr int kWidth = 452;
constexpr int kHeight = 1280;
constexpr int kMaxPoints = 5;
constexpr int kPointBytes = 6;
// A gesture byte, a status byte with the point count in its low nibble, then
// the points: event and X high bits, X, track ID and Y high bits, Y, weight
// and area.
constexpr int kReportBytes = 2 + kMaxPoints * kPointBytes;
// While a finger is down, read at this period even without an INT edge.
constexpr int64_t kTrackUs = 20000;
// Empty reports in a row that end a touch. The AIBox driver keeps the touch
// over a single empty report, which the controller sends during drags.
constexpr int kReleaseReports = 2;

struct Controller {
    esp_lcd_touch_t base{};
    i2c_master_dev_handle_t device = nullptr;
    std::atomic<bool> reported{false};  // INT fell since the last read
    bool tracking = false;              // the points hold a finger
    int empty_reports = 0;
    int64_t read_us = 0;
};

Controller controller;

void on_interrupt(esp_lcd_touch_handle_t) {
    controller.reported.store(true, std::memory_order_relaxed);
}

esp_err_t read_data(esp_lcd_touch_handle_t tp) {
    Controller& c = controller;
    const int64_t now = esp_timer_get_time();
    // Reads while the controller has no new report return frames that start
    // at the wrong byte, so an idle controller is only read after INT fell.
    bool reported = c.reported.exchange(false, std::memory_order_relaxed);
    if (!reported && !(c.tracking && now - c.read_us >= kTrackUs)) {
        return ESP_OK;
    }
    c.read_us = now;
    uint8_t report[kReportBytes];
    esp_err_t err = i2c_master_receive(c.device, report, sizeof report, 50);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_touch_point_data_t points[kMaxPoints];
    int count = 0;
    const int listed = report[1] & 0x0f;
    const int flags = report[1] >> 4;
    // Other gestures, counts and status flags (ESD) mark a report as garbage.
    if (report[0] <= 0x0f && listed <= kMaxPoints && (flags == 0 || flags == 0x4 || flags == 0x8)) {
        for (int i = 0; i < listed; ++i) {
            const uint8_t* p = report + 2 + i * kPointBytes;
            const int event = p[0] >> 6;  // 0 down, 1 up, 2 contact
            const int x = (p[0] & 0x0f) << 8 | p[1];
            const int y = (p[2] & 0x0f) << 8 | p[3];
            if ((event == 0 || event == 2) && x < kWidth && y < kHeight) {
                points[count++] = {.track_id = static_cast<uint8_t>(p[2] >> 4),
                                   .x = static_cast<uint16_t>(x),
                                   .y = static_cast<uint16_t>(y),
                                   .strength = p[4]};
            }
        }
    }
    if (count == 0 && c.tracking && ++c.empty_reports < kReleaseReports) {
        return ESP_OK;
    }
    c.empty_reports = 0;
    c.tracking = count > 0;
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = count;
    std::copy_n(points, count, tp->data.coords);
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
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

esp_err_t del(esp_lcd_touch_handle_t tp) {
    Controller& c = controller;
    esp_lcd_touch_register_interrupt_callback(tp, nullptr);
    gpio_reset_pin(tp->config.int_gpio_num);
    i2c_master_bus_rm_device(c.device);
    c.device = nullptr;
    return ESP_OK;
}

}  // namespace

esp_err_t new_axs15260_touch(i2c_master_bus_handle_t bus, gpio_num_t interrupt, esp_lcd_touch_handle_t* out) {
    Controller& c = controller;
    ESP_RETURN_ON_FALSE(c.device == nullptr, ESP_ERR_INVALID_STATE, kTag, "already created");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, kAddress, 50), kTag, "no answer at 0x%02x", kAddress);
    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = kAddress;
    device.scl_speed_hz = kSpeedHz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device, &c.device), kTag, "I2C device");
    uint8_t reg = kVersionRegister;
    uint8_t version[2] = {};
    if (i2c_master_transmit_receive(c.device, &reg, 1, version, sizeof version, 50) == ESP_OK) {
        ESP_LOGI(kTag, "firmware %02x%02x", version[0], version[1]);
    } else {
        ESP_LOGW(kTag, "could not read the firmware version");
    }

    esp_lcd_touch_t& tp = c.base;
    portMUX_INITIALIZE(&tp.data.lock);
    tp.read_data = read_data;
    tp.get_xy = get_xy;
    tp.del = del;
    tp.config.x_max = kWidth;
    tp.config.y_max = kHeight;
    tp.config.rst_gpio_num = GPIO_NUM_NC;
    tp.config.int_gpio_num = interrupt;

    gpio_config_t pin{};
    pin.pin_bit_mask = 1ULL << interrupt;
    pin.mode = GPIO_MODE_INPUT;
    pin.pull_up_en = GPIO_PULLUP_ENABLE;
    pin.intr_type = GPIO_INTR_NEGEDGE;
    esp_err_t err = gpio_config(&pin);
    if (err == ESP_OK) {
        err = esp_lcd_touch_register_interrupt_callback(&tp, on_interrupt);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "INT on GPIO%d: %s", interrupt, esp_err_to_name(err));
        i2c_master_bus_rm_device(c.device);
        c.device = nullptr;
        return err;
    }
    *out = &tp;
    return ESP_OK;
}

}  // namespace gbx::board
