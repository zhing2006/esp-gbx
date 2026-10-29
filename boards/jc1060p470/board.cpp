#include "gbx/esp/board.h"

#include <array>
#include <iterator>

#include "driver/i2c_master.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gbx/esp/board_support.h"

// Pins, timing and the measured results are in docs/boards/jc1060p470.md.
namespace gbx::board {

namespace {

constexpr const char* kTag = "board";

constexpr int kWidth = 1024;  // the panel is landscape; the UI is held upright
constexpr int kHeight = 600;
constexpr gpio_num_t kLcdReset = GPIO_NUM_27;
constexpr gpio_num_t kBacklight = GPIO_NUM_23;  // MP3202 EN, pulled down: high turns it on

// The only I2C bus: ES8311, GT911, RX8025T and the expansion connectors.
constexpr gpio_num_t kI2cSda = GPIO_NUM_7;
constexpr gpio_num_t kI2cScl = GPIO_NUM_8;
constexpr gpio_num_t kTouchInt = GPIO_NUM_21;
constexpr gpio_num_t kTouchReset = GPIO_NUM_22;

// ES8311 at 0x18 on the shared bus. NS4150 runs from VOUT-BAT, the IP5306's
// 5 V output, and its enable has a pull-down.
constexpr board_support::I2sPins kI2s{.mclk = GPIO_NUM_13, .bclk = GPIO_NUM_12, .ws = GPIO_NUM_10, .dout = GPIO_NUM_9};
constexpr gpio_num_t kAmplifier = GPIO_NUM_20;
constexpr int kTouchWidth = 800;  // the GT911 configuration reports 800x480
constexpr int kTouchHeight = 480;

struct InitCommand {
    uint8_t cmd;
    uint8_t size;
    uint8_t data[14];
    uint16_t delay_ms;
};

// JD9165 sequence for the old panel from refs/boards/jc1060p470/jd9165-init.dtsi.txt;
// each REGFLAG_DELAY there is the delay of the command before it.
constexpr InitCommand kInitSequence[] = {
    {0x30, 1, {0x00}, 0},
    {0xF7, 4, {0x49, 0x61, 0x02, 0x00}, 0},
    {0x30, 1, {0x01}, 0},
    {0x04, 1, {0x0C}, 0},
    {0x05, 1, {0x00}, 0},
    {0x06, 1, {0x00}, 0},
    {0x0B, 1, {0x11}, 0},
    {0x17, 1, {0x00}, 0},
    {0x20, 1, {0x04}, 0},
    {0x1F, 1, {0x05}, 0},
    {0x23, 1, {0x00}, 0},
    {0x25, 1, {0x19}, 0},
    {0x28, 1, {0x18}, 0},
    {0x29, 1, {0x04}, 0},
    {0x2A, 1, {0x01}, 0},
    {0x2B, 1, {0x04}, 0},
    {0x2C, 1, {0x01}, 0},
    {0x30, 1, {0x02}, 0},
    {0x01, 1, {0x22}, 0},
    {0x03, 1, {0x12}, 0},
    {0x04, 1, {0x00}, 0},
    {0x05, 1, {0x64}, 0},
    {0x0A, 1, {0x08}, 0},
    {0x0B, 11, {0x0A, 0x1A, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x08, 0x1F, 0x1D}, 0},
    {0x0C, 11, {0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 0},
    {0x0D, 11, {0x16, 0x1B, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x09, 0x1E, 0x1C}, 0},
    {0x0E, 11, {0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 0},
    {0x0F, 11, {0x16, 0x1B, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1C, 0x1E, 0x09, 0x07}, 0},
    {0x10, 11, {0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 0},
    {0x11, 11, {0x0A, 0x1A, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1D, 0x1F, 0x08, 0x06}, 0},
    {0x12, 11, {0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 0},
    {0x14, 4, {0x00, 0x00, 0x11, 0x11}, 0},
    {0x18, 1, {0x99}, 0},
    {0x30, 1, {0x06}, 0},
    {0x12, 14, {0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 0},
    {0x13, 14, {0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 0},
    {0x30, 1, {0x0A}, 0},
    {0x02, 1, {0x4F}, 0},
    {0x0B, 1, {0x40}, 0},
    {0x12, 1, {0x3E}, 0},
    {0x13, 1, {0x78}, 0},
    {0x30, 1, {0x0D}, 0},
    {0x0D, 1, {0x04}, 0},
    {0x10, 1, {0x0C}, 0},
    {0x11, 1, {0x0C}, 0},
    {0x12, 1, {0x0C}, 0},
    {0x13, 1, {0x0C}, 0},
    {0x30, 1, {0x00}, 0},
    {0x11, 0, {}, 120},
    {0x29, 0, {}, 20},
};

i2c_master_bus_handle_t i2c_bus = nullptr;

}  // namespace

const char* name() {
    return "GUITION JC1060P470";
}

bool init() {
    // 5 kHz as in the ESP32-P4 Function EV Board BSP the vendor's example uses.
    board_support::backlight_init(kBacklight, 5000, 10);
    board_support::output_pin(kAmplifier, false);
    if (!board_support::power_io_domain()) {
        return false;
    }
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = -1;
    bus_config.sda_io_num = kI2cSda;
    bus_config.scl_io_num = kI2cScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "I2C bus: %s", esp_err_to_name(err));
        return false;
    }
    // TODO: battery ADC.
    return true;
}

// GPIO39-44 are slot 0's IO MUX pins. Slot 1 is the C6's SDIO, see
// docs/boards/jc1060p470.md.
Sdmmc sdmmc() {
    return {.slot = 0, .clk = 43, .cmd = 44, .data = {39, 40, 41, 42}};
}

Display display_init(int frame_buffers) {
    if (!board_support::power_dsi_phy(2500)) {
        return {};
    }
    // Keep 2 lanes at 550 Mbps. At 48 MHz, 1242 x 647 total periods target
    // GB's 59.727501 Hz; IDF's rounded DSI host line gives about 59.730 Hz.
    auto dsi = board_support::new_dsi_bus(2, 550);
    if (dsi.io == nullptr) {
        return {};
    }
    esp_lcd_video_timing_t timing{
        .h_size = kWidth,
        .v_size = kHeight,
        .hsync_pulse_width = 24,
        .hsync_back_porch = 136,
        .hsync_front_porch = 58,
        .vsync_pulse_width = 2,
        .vsync_back_porch = 21,
        .vsync_front_porch = 24,
    };
    esp_lcd_dpi_panel_config_t dpi = board_support::dpi_config(48, timing, frame_buffers);

    static std::array<jd9165_lcd_init_cmd_t, std::size(kInitSequence)> commands;
    for (size_t i = 0; i < commands.size(); ++i) {
        const InitCommand& c = kInitSequence[i];
        commands[i] = {c.cmd, c.size ? c.data : nullptr, c.size, c.delay_ms};
    }
    jd9165_vendor_config_t vendor{};
    vendor.init_cmds = commands.data();
    vendor.init_cmds_size = static_cast<uint16_t>(commands.size());
    vendor.mipi_config.dsi_bus = dsi.bus;
    vendor.mipi_config.dpi_config = &dpi;
    esp_lcd_panel_dev_config_t dev{
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
        .reset_gpio_num = kLcdReset,
        .vendor_config = &vendor,
        .flags = {},
    };
    esp_lcd_panel_handle_t panel = nullptr;
    esp_err_t err = esp_lcd_new_panel_jd9165(dsi.io, &dev, &panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_reset(panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "JD9165: %s", esp_err_to_name(err));
        return {};
    }
    // With the USB ports at the bottom, panel row 0 runs down the right edge of the UI.
    return {.panel = panel, .width = kWidth, .height = kHeight, .rotation = Rotation::cw270};
}

void set_backlight(int percent) {
    board_support::backlight_set(percent);
}

Touch touch_init() {
    if (i2c_bus == nullptr) {
        return {};
    }
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    io_config.scl_speed_hz = 100000;
    io_config.control_phase_bytes = 1;
    io_config.lcd_cmd_bits = 16;
    io_config.flags.disable_control_phase = 1;
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_err_t err = esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &io);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "touch I2C device: %s", esp_err_to_name(err));
        return {};
    }
    esp_lcd_touch_config_t config{};
    config.x_max = kTouchWidth;
    config.y_max = kTouchHeight;
    config.rst_gpio_num = kTouchReset;
    config.int_gpio_num = kTouchInt;
    esp_lcd_touch_handle_t handle = nullptr;
    err = esp_lcd_touch_new_i2c_gt911(io, &config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "GT911: %s", esp_err_to_name(err));
        return {};
    }
    return {.handle = handle, .width = kTouchWidth, .height = kTouchHeight};
}

Audio audio_init(uint32_t rate, uint32_t buffers, uint32_t frames) {
    if (i2c_bus == nullptr) {
        return {};
    }
    i2s_chan_handle_t tx = board_support::new_i2s_tx(kI2s, rate, buffers, frames);
    if (tx == nullptr || !board_support::open_codec({.chip = board_support::CodecChip::es8311,
                                                     .bus = i2c_bus,
                                                     .address = 0x18,
                                                     .amplifier = kAmplifier,
                                                     .amplifier_volts = 5.0f},
                                                    tx, rate)) {
        return {};
    }
    // ES8311 has one DAC, fed with the left channel; NS4150 drives one speaker.
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

// RX8025T, see docs/boards/jc1060p470.md#rtc. The driver lives here rather
// than in drivers/ because no other board has this chip; it only reads.
Clock read_clock() {
    // Epson RX-8025T application manual, register table: 00h to 06h hold the
    // seconds, minutes, hours (24-hour), weekday, day, month and year in BCD,
    // 0Eh is the flag register with VLF in bit 1.
    std::array<uint8_t, 16> r{};
    if (i2c_bus == nullptr || !board_support::read_registers(i2c_bus, 0x32, 0x00, r)) {
        return {};
    }
    if (r[0x0e] & 0x02) {
        ESP_LOGW(kTag, "RX8025T: VLF set, the clock lost its time");
        return {};
    }
    const int64_t seconds = board_support::clock_seconds(r[6], r[5] & 0x1f, r[4] & 0x3f, r[2] & 0x3f, r[1] & 0x7f,
                                                         r[0] & 0x7f);
    if (seconds < 0) {
        ESP_LOGW(kTag, "RX8025T: invalid date %02x-%02x-%02x %02x:%02x:%02x", r[6], r[5], r[4], r[2], r[1], r[0]);
        return {};
    }
    return {.valid = true, .seconds = seconds};
}

bool set_clock(int64_t seconds) {
    // 0Fh control, then 00h to 06h: the register address wraps from 0Fh to
    // 00h (Epson RX-8025T application manual 8.6.5).
    std::array<uint8_t, 8> write{};
    std::array<uint8_t, 3> r{};  // 0Dh extension, 0Eh flags, 0Fh control
    if (i2c_bus == nullptr || !board_support::clock_registers(seconds, std::span(write).subspan<1>()) ||
        !board_support::read_registers(i2c_bus, 0x32, 0x0d, r)) {
        return false;
    }
    // After VLF every register is undefined and is to be set again (8.2.3):
    // the extension register 00h, which keeps TEST 0 and the timer off, and
    // control 40h, the default 2 s compensation with interrupts off.
    // Otherwise only TEST is cleared, which must never be written 1 (8.2.4).
    const bool lost = r[1] & 0x02;
    const uint8_t extension = lost ? 0x00 : r[0] & 0x7f;
    const uint8_t control = lost ? 0x40 : r[2] & 0xfe;
    // The manual gives no steps for setting the time. RESET (0Fh bit 0)
    // holds the counters below a second until the write ends, when it clears
    // itself (8.2.2), so the clock starts at the new second.
    write[0] = control | 0x01;
    if (!board_support::write_registers(i2c_bus, 0x32, 0x0d, std::array<uint8_t, 1>{extension}) ||
        !board_support::write_registers(i2c_bus, 0x32, 0x0f, write)) {
        return false;
    }
    // Only 0 can be written to the flags; none of the others is used. VLF may
    // not clear before the oscillator runs, up to 3 s after power-up (6).
    for (int i = 0; i < 300; ++i) {
        std::array<uint8_t, 1> flags{};
        if (!board_support::write_registers(i2c_bus, 0x32, 0x0e, std::array<uint8_t, 1>{0x00}) ||
            !board_support::read_registers(i2c_bus, 0x32, 0x0e, flags)) {
            return false;
        }
        if ((flags[0] & 0x02) == 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(kTag, "RX8025T: VLF stays set");
    return false;
}

}  // namespace gbx::board
