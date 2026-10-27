#include "gbx/esp/board.h"

#include "driver/i2c_master.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gbx/esp/board_support.h"

// Pins, vendor sources and timing constraints are in docs/boards/osptek_p4c5.md.
namespace gbx::board {

namespace {

constexpr const char* kTag = "board";

// The 4.3-inch YDP430BT009-V1 screen: an ST7102 that scans out portrait,
// 480 x 800, and an ST7123 touch controller. It reaches the carrier through
// OSPTEK's 4.3-inch adapter board and the 15-pin DSI connector J6, which
// carries the DSI lanes, I2C and 3.3 V only. LCD_RST, TP_RST, TP_INT and
// LCD_BL come out on the adapter's header J2 instead. Only LCD_BL is wired,
// by a jumper to GPIO23: the panel is reset by command and the touch is
// polled. The adapter pulls LCD_BL high, so without the jumper the backlight
// is on whenever the board has power.
constexpr int kWidth = 480;
constexpr int kHeight = 800;
constexpr gpio_num_t kBacklight = GPIO_NUM_23;  // high turns the backlight on

// One I2C bus with 10 kOhm pull-ups on the carrier and on the adapter:
// ST7123 (0x55), ES8311 (0x18, CE low) and, through a level shifter, the
// camera. The platform reads the touch and the codec changes volume from
// different tasks; the I2C driver serializes their transfers.
constexpr gpio_num_t kI2cSda = GPIO_NUM_7;
constexpr gpio_num_t kI2cScl = GPIO_NUM_8;

constexpr board_support::I2sPins kI2s{.mclk = GPIO_NUM_13, .bclk = GPIO_NUM_12, .ws = GPIO_NUM_10, .dout = GPIO_NUM_9};
// NS4150B's STD pin, high on, with a pull-down on the carrier. The amplifier
// runs from VIN_5V.
constexpr gpio_num_t kAmplifier = GPIO_NUM_53;

// What OSPTEK's ST7102 driver sends before the panel's own sequence: page
// 1, no mirroring (MADCTL), 24 bits per pixel (COLMOD), because the ST7102
// takes only 24-bit packed pixels over DSI.
// Then the sequence of the screen's vendor for this glass, GX09C ST7102 +
// BOE 4.3 inch, 2 lanes, from the ESP-IDF 6.1 example. Each entry is a
// command, its parameter count and the parameters.
constexpr uint8_t kInitSequence[] = {
    0xF0, 1, 0x00,
    0x36, 1, 0x00,
    0x3A, 1, 0x77,
    0x99, 3, 0x71, 0x02, 0xA2,
    0x99, 3, 0x71, 0x02, 0xA3,
    0x99, 3, 0x71, 0x02, 0xA4,
    0xB0, 7, 0x22, 0x57, 0x1E, 0x61, 0x2F, 0x57, 0x61,
    0xB7, 2, 0x64, 0x64,
    0xBF, 2, 0xB4, 0xB4,
    0xC8, 37, 0x00, 0x00, 0x13, 0x24, 0x44, 0x00, 0x74, 0x03, 0xB8, 0x04, 0x11, 0x16, 0x08, 0x86, 0x04, 0x21,
    0xD3, 0x02, 0x10, 0x0F, 0x22, 0x4D, 0x0E, 0x90, 0x09, 0x32, 0xF0, 0x0B, 0x40, 0x0E, 0xF3, 0x7D, 0x0E,
    0xA9, 0xBF, 0x03, 0xC4,
    0xC9, 37, 0x00, 0x00, 0x13, 0x24, 0x44, 0x00, 0x74, 0x03, 0xB8, 0x04, 0x11, 0x16, 0x08, 0x86, 0x04, 0x21,
    0xD3, 0x02, 0x10, 0x0F, 0x22, 0x4D, 0x0E, 0x90, 0x09, 0x32, 0xF0, 0x0B, 0x40, 0x0E, 0xF3, 0x7D, 0x0E,
    0xA9, 0xBF, 0x03, 0xC4,
    0xD7, 6, 0x10, 0x0C, 0x36, 0x19, 0x90, 0x90,
    0xA3, 32, 0x51, 0x03, 0x80, 0xCF, 0x44, 0x00, 0x00, 0x00, 0x00, 0x04, 0x78, 0x78, 0x00, 0x1A, 0x00, 0x45,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x02, 0x20, 0x52, 0x00, 0x05, 0x00, 0x00, 0xFF,
    0xA6, 44, 0x02, 0x00, 0x24, 0x55, 0x35, 0x00, 0x38, 0x00, 0x78, 0x78, 0x00, 0x24, 0x55, 0x36, 0x00, 0x37,
    0x00, 0x78, 0x78, 0x02, 0xAC, 0x51, 0x3A, 0x00, 0x00, 0x00, 0x78, 0x78, 0x03, 0xAC, 0x21, 0x00, 0x04,
    0x00, 0x00, 0x78, 0x78, 0x3E, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
    0xA7, 48, 0x19, 0x19, 0x00, 0x64, 0x40, 0x07, 0x16, 0x40, 0x00, 0x04, 0x03, 0x78, 0x78, 0x00, 0x64, 0x40,
    0x25, 0x34, 0x00, 0x00, 0x02, 0x01, 0x78, 0x78, 0x00, 0x64, 0x40, 0x4B, 0x5A, 0x00, 0x00, 0x02, 0x01,
    0x78, 0x78, 0x00, 0x24, 0x40, 0x69, 0x78, 0x00, 0x00, 0x00, 0x00, 0x78, 0x78, 0x00, 0x44,
    0xAC, 37, 0x08, 0x0A, 0x11, 0x00, 0x13, 0x03, 0x1B, 0x18, 0x06, 0x1A, 0x19, 0x1B, 0x1B, 0x1B, 0x18, 0x1B,
    0x09, 0x0B, 0x10, 0x02, 0x12, 0x01, 0x1B, 0x18, 0x06, 0x1A, 0x19, 0x1B, 0x1B, 0x1B, 0x18, 0x1B, 0xFF,
    0x67, 0xFF, 0x67, 0x00,
    0xAD, 7, 0xCC, 0x40, 0x46, 0x11, 0x04, 0x78, 0x78,
    0xE8, 14, 0x30, 0x07, 0x00, 0x94, 0x94, 0x9C, 0x00, 0xE2, 0x04, 0x00, 0x00, 0x00, 0x00, 0xEF,
    0xE7, 33, 0x8B, 0x3C, 0x00, 0x0C, 0xF0, 0x5D, 0x00, 0x5D, 0x00, 0x5D, 0x00, 0x5D, 0x00, 0xFF, 0x00, 0x08,
    0x7B, 0x00, 0x00, 0xC8, 0x6A, 0x5A, 0x08, 0x1A, 0x3C, 0x00, 0x81, 0x01, 0xCC, 0x01, 0x7F, 0xF0, 0x22,
};

i2c_master_bus_handle_t shared_bus = nullptr;

// Software reset, the init sequence, then sleep out and display on with the
// vendor's delays.
esp_err_t init_panel(esp_lcd_panel_io_handle_t io) {
    esp_err_t err = esp_lcd_panel_io_tx_param(io, 0x01, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    for (size_t i = 0; err == ESP_OK && i < sizeof kInitSequence; i += 2 + kInitSequence[i + 1]) {
        err = esp_lcd_panel_io_tx_param(io, kInitSequence[i], &kInitSequence[i + 2], kInitSequence[i + 1]);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_io_tx_param(io, 0x11, nullptr, 0);
        vTaskDelay(pdMS_TO_TICKS(600));
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_io_tx_param(io, 0x29, nullptr, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return err;
}

}  // namespace

const char* name() {
    return "OSPTEK ESP32-P4C5-Module";
}

bool init() {
    // The adapter's SY7200 has no suffix on the schematic; EN/PWM dimming as
    // for the Sila P4's SY7200, 25 kHz, see docs/boards/sila_p4x.md.
    board_support::backlight_init(kBacklight, 25000, 11);
    board_support::output_pin(kAmplifier, false);
    if (!board_support::power_io_domain()) {
        return false;
    }
    i2c_master_bus_config_t config{};
    config.i2c_port = 0;
    config.sda_io_num = kI2cSda;
    config.scl_io_num = kI2cScl;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    esp_err_t err = i2c_new_master_bus(&config, &shared_bus);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "I2C bus: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

Sdmmc sdmmc() {
    return {.slot = 0, .clk = 43, .cmd = 44, .data = {39, 40, 41, 42}};
}

Display display_init(int frame_buffers) {
    if (!board_support::power_dsi_phy(2500)) {
        return {};
    }
    // 820 Mbps per lane as in OSPTEK's ESP-IDF 6.1 example.
    auto dsi = board_support::new_dsi_bus(2, 820);
    if (dsi.io == nullptr) {
        return {};
    }
    uint8_t id[3]{};
    esp_err_t err = esp_lcd_panel_io_rx_param(dsi.io, 0x04, id, sizeof id);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ST7102 ID: %s", esp_err_to_name(err));
        return {};
    }
    ESP_LOGI(kTag, "ST7102 ID %02x %02x %02x", id[0], id[1], id[2]);
    err = init_panel(dsi.io);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ST7102 init: %s", esp_err_to_name(err));
        return {};
    }
    // PLL_F240M / 6 = 40 MHz exactly. The vendor's porches with HFP 47 and
    // VFP 365 give 569 x 1177 total periods, 59.727059 Hz, 7 ppm below the
    // GB frame rate.
    esp_lcd_video_timing_t timing{
        .h_size = kWidth,
        .v_size = kHeight,
        .hsync_pulse_width = 2,
        .hsync_back_porch = 40,
        .hsync_front_porch = 47,
        .vsync_pulse_width = 2,
        .vsync_back_porch = 10,
        .vsync_front_porch = 365,
    };
    esp_lcd_dpi_panel_config_t dpi = board_support::dpi_config(40, timing, frame_buffers);
    // The DSI bridge widens the RGB565 frame buffers to the panel's RGB888.
    dpi.out_color_format = LCD_COLOR_FMT_RGB888;
    esp_lcd_panel_handle_t panel = nullptr;
    err = esp_lcd_new_panel_dpi(dsi.bus, &dpi, &panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "DPI panel: %s", esp_err_to_name(err));
        return {};
    }
    return {.panel = panel, .width = kWidth, .height = kHeight, .rotation = Rotation::none};
}

void set_backlight(int percent) {
    board_support::backlight_set(percent);
}

Touch touch_init() {
    if (shared_bus == nullptr) {
        return {};
    }
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS;
    io_config.scl_speed_hz = 400000;
    io_config.control_phase_bytes = 1;
    io_config.lcd_cmd_bits = 16;
    io_config.flags.disable_control_phase = 1;
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_err_t err = esp_lcd_new_panel_io_i2c(shared_bus, &io_config, &io);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "touch I2C device: %s", esp_err_to_name(err));
        return {};
    }
    esp_lcd_touch_config_t config{};
    config.x_max = kWidth;
    config.y_max = kHeight;
    config.rst_gpio_num = GPIO_NUM_NC;
    config.int_gpio_num = GPIO_NUM_NC;
    esp_lcd_touch_handle_t handle = nullptr;
    err = esp_lcd_touch_new_i2c_st7123(io, &config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ST7123: %s", esp_err_to_name(err));
        return {};
    }
    // The ST7123 firmware reports a 480 x 800 range, the panel's.
    return {.handle = handle, .width = kWidth, .height = kHeight};
}

Audio audio_init(uint32_t rate, uint32_t buffers, uint32_t frames) {
    if (shared_bus == nullptr) {
        return {};
    }
    i2s_chan_handle_t tx = board_support::new_i2s_tx(kI2s, rate, buffers, frames);
    if (tx == nullptr || !board_support::open_codec({.chip = board_support::CodecChip::es8311,
                                                     .bus = shared_bus,
                                                     .address = 0x18,
                                                     .amplifier = kAmplifier,
                                                     .amplifier_volts = 5.0f},
                                                    tx, rate)) {
        return {};
    }
    // ES8311 has one DAC, fed with the left channel; NS4150B drives one speaker.
    return {.tx = tx, .mono = true};
}

void audio_volume(int percent) {
    board_support::codec_volume(percent);
}

void audio_mute(bool muted) {
    board_support::codec_mute(muted);
}

Headphones headphones() {
    return Headphones::unknown;
}

bool set_speaker(bool) {
    return false;
}

Clock read_clock() {
    // The board has no clock chip.
    return {};
}

bool set_clock(int64_t) {
    return false;
}

}  // namespace gbx::board
