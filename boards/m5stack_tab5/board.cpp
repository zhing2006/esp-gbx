#include "gbx/esp/board.h"

#include <array>

#include "driver/i2c_master.h"
#include "esp_io_expander_pi4ioe5v6408.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7121.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gbx/esp/board_support.h"

// Pins, IO expanders, timing and the measured results are in
// docs/boards/m5stack_tab5.md. Only the ST7121 panel revision is supported.
namespace gbx::board {

namespace {

constexpr const char* kTag = "board";

constexpr int kWidth = 720;  // the panel is portrait, like the UI
constexpr int kHeight = 1280;
constexpr gpio_num_t kBacklight = GPIO_NUM_22;  // ME2212 EN, pulled down: high turns it on
constexpr gpio_num_t kI2cSda = GPIO_NUM_31;
constexpr gpio_num_t kI2cScl = GPIO_NUM_32;
constexpr gpio_num_t kTouchInt = GPIO_NUM_23;

// IO expander 0x43
constexpr uint32_t kSpeaker = IO_EXPANDER_PIN_NUM_1;     // NS4150B enable, pulled down
constexpr uint32_t kHeadphones = IO_EXPANDER_PIN_NUM_7;  // HP_DET, high with headphones in
constexpr uint32_t kLcdReset = IO_EXPANDER_PIN_NUM_4;
constexpr uint32_t kTouchReset = IO_EXPANDER_PIN_NUM_5;

// ES8388 at 0x10 on the shared bus plays; NS4150B runs from SYS_INT5V and
// amplifies LOUT1 only.
constexpr board_support::I2sPins kI2s{.mclk = GPIO_NUM_30, .bclk = GPIO_NUM_27, .ws = GPIO_NUM_29, .dout = GPIO_NUM_26};

i2c_master_bus_handle_t i2c_bus = nullptr;
esp_io_expander_handle_t expander_43 = nullptr;
esp_io_expander_handle_t expander_44 = nullptr;

void delay_ms(int ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// Resets the panel and the touch controller through expander 0x43. The panel
// side of LCD_RST has no pull-up to its 1.8 V rail, so the expander only ever
// drives it low and releases it as an input with the expander's pull-up,
// following the factory firmware.
void reset_panel_and_touch() {
    uint32_t both = kLcdReset | kTouchReset;
    esp_io_expander_set_output_mode(expander_43, both, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    esp_io_expander_set_pullupdown(expander_43, both, IO_EXPANDER_PULL_UP);
    esp_io_expander_set_level(expander_43, both, 0);
    esp_io_expander_set_dir(expander_43, both, IO_EXPANDER_OUTPUT);
    delay_ms(10);
    esp_io_expander_set_level(expander_43, kTouchReset, 1);
    esp_io_expander_set_dir(expander_43, kLcdReset, IO_EXPANDER_INPUT);
    delay_ms(100);
}

}  // namespace

const char* name() {
    return "M5Stack Tab5";
}

bool init() {
    // 5 kHz as in esp-bsp.
    board_support::backlight_init(kBacklight, 5000, 10);
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
    // The expanders are not reset with the chip, so they may still hold the
    // previous firmware's outputs. Creating them resets both: the driver
    // makes every pin an output in high impedance (OUT_H_IM 0xff) with a
    // pull-down, which leaves the speaker, the C6, USB-A power and battery
    // charging switched off. A pin drives only once it is set push-pull.
    // TODO: switch on charging (0x44 P7) and the other rails when they are needed.
    err = esp_io_expander_new_i2c_pi4ioe5v6408(i2c_bus, ESP_IO_EXPANDER_I2C_PI4IOE5V6408_ADDRESS_LOW, &expander_43);
    if (err == ESP_OK) {
        err = esp_io_expander_new_i2c_pi4ioe5v6408(i2c_bus, ESP_IO_EXPANDER_I2C_PI4IOE5V6408_ADDRESS_HIGH,
                                                    &expander_44);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "IO expanders: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

Sdmmc sdmmc() {
    // Both the card and the GPIO39-48 IO domain use the external 3.3 V rail.
    return {.slot = 1, .clk = 43, .cmd = 44, .data = {39, 40, 41, 42}};
}

Display display_init(int frame_buffers) {
    if (expander_43 == nullptr || !board_support::power_dsi_phy(2500)) {
        return {};
    }
    reset_panel_and_touch();
    // Keep the ST7121 sync/back porches and 965 Mbps lane request from
    // esp-bsp. IDF realizes 960 Mbps and an 80 MHz DPI clock; 872 x 1536
    // total periods give 59.728593 Hz, close to GB's 59.727501 Hz.
    auto dsi = board_support::new_dsi_bus(2, 965);
    if (dsi.io == nullptr) {
        return {};
    }
    esp_lcd_video_timing_t timing{
        .h_size = kWidth,
        .v_size = kHeight,
        .hsync_pulse_width = 2,
        .hsync_back_porch = 40,
        .hsync_front_porch = 110,
        .vsync_pulse_width = 20,
        .vsync_back_porch = 24,
        .vsync_front_porch = 212,
    };
    esp_lcd_dpi_panel_config_t dpi = board_support::dpi_config(80, timing, frame_buffers);
    st7121_vendor_config_t vendor{};
    vendor.mipi_config.dsi_bus = dsi.bus;
    vendor.mipi_config.dpi_config = &dpi;
    // No reset GPIO: esp_lcd_panel_reset() sends the soft reset 0x01 that the
    // measured bring-up sent before the init sequence.
    esp_lcd_panel_dev_config_t dev{
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
        .reset_gpio_num = GPIO_NUM_NC,
        .vendor_config = &vendor,
        .flags = {},
    };
    esp_lcd_panel_handle_t panel = nullptr;
    esp_err_t err = esp_lcd_new_panel_st7121(dsi.io, &dev, &panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_reset(panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ST7121: %s", esp_err_to_name(err));
        return {};
    }
    return {.panel = panel, .width = kWidth, .height = kHeight, .rotation = Rotation::none};
}

void set_backlight(int percent) {
    board_support::backlight_set(percent);
}

Touch touch_init() {
    if (i2c_bus == nullptr) {
        return {};
    }
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS;
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
    // The ST7121's touch half speaks the ST7123 protocol; the factory firmware
    // and esp-bsp use this driver for it too. Its reset is expander 0x43 P5,
    // released in reset_panel_and_touch().
    esp_lcd_touch_config_t config{};
    config.x_max = kWidth;
    config.y_max = kHeight;
    config.rst_gpio_num = GPIO_NUM_NC;
    config.int_gpio_num = kTouchInt;
    esp_lcd_touch_handle_t handle = nullptr;
    err = esp_lcd_touch_new_i2c_st7123(io, &config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ST7121 touch: %s", esp_err_to_name(err));
        return {};
    }
    return {.handle = handle, .width = kWidth, .height = kHeight};
}

Audio audio_init(uint32_t rate, uint32_t buffers, uint32_t frames) {
    if (expander_43 == nullptr) {
        return {};
    }
    i2s_chan_handle_t tx = board_support::new_i2s_tx(kI2s, rate, buffers, frames);
    if (tx == nullptr || !board_support::open_codec({.chip = board_support::CodecChip::es8388,
                                                     .bus = i2c_bus,
                                                     .address = 0x10,
                                                     .amplifier_volts = 5.0f},
                                                    tx, rate)) {
        return {};
    }
    // The speaker's amplifier is switched by the expander, not esp_codec_dev,
    // and stays off until the output knows whether headphones are in. HP_DET
    // is driven by the jack, so P7 has no pull, as in the factory firmware.
    esp_err_t err = esp_io_expander_set_level(expander_43, kSpeaker, 0);
    if (err == ESP_OK) err = esp_io_expander_set_output_mode(expander_43, kSpeaker, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    if (err == ESP_OK) err = esp_io_expander_set_dir(expander_43, kSpeaker, IO_EXPANDER_OUTPUT);
    if (err == ESP_OK) err = esp_io_expander_set_dir(expander_43, kHeadphones, IO_EXPANDER_INPUT);
    if (err == ESP_OK) err = esp_io_expander_set_pullupdown(expander_43, kHeadphones, IO_EXPANDER_PULL_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "speaker and headphone pins: %s", esp_err_to_name(err));
        return {};
    }
    // The speaker plays only the left channel; headphones get both.
    return {.tx = tx, .mono = true, .headphones = true};
}

void audio_volume(int percent) {
    board_support::codec_volume(percent);
}

void audio_mute(bool muted) {
    board_support::codec_mute(muted);
}

Headphones headphones() {
    uint32_t level = 0;
    if (expander_43 == nullptr || esp_io_expander_get_level(expander_43, kHeadphones, &level) != ESP_OK) {
        return Headphones::unknown;
    }
    // The factory firmware's bsp_headphone_detect() takes P7 high as in.
    return level ? Headphones::present : Headphones::absent;
}

bool set_speaker(bool on) {
    return expander_43 != nullptr && esp_io_expander_set_level(expander_43, kSpeaker, on ? 1 : 0) == ESP_OK;
}

// RX8130CE, see docs/boards/m5stack_tab5.md#rtc. The driver lives here rather
// than in drivers/ because no other board has this chip; it only reads.
Clock read_clock() {
    // RX8130CE application manual, register table: 10h to 16h hold the
    // seconds, minutes, hours (24-hour), weekday, day, month and year in BCD;
    // 1Dh is the flag register with VLF in bit 1 (14.5, oscillation stop
    // detection) and 1Eh control register 0 with STOP in bit 6.
    std::array<uint8_t, 15> r{};
    if (i2c_bus == nullptr || !board_support::read_registers(i2c_bus, 0x32, 0x10, r)) {
        return {};
    }
    if (r[0x0d] & 0x02) {
        ESP_LOGW(kTag, "RX8130CE: VLF set, the clock lost its time");
        return {};
    }
    if (r[0x0e] & 0x40) {
        ESP_LOGW(kTag, "RX8130CE: STOP set, the clock is not counting");
        return {};
    }
    const int64_t seconds = board_support::clock_seconds(r[6], r[5] & 0x1f, r[4] & 0x3f, r[2] & 0x3f, r[1] & 0x7f,
                                                         r[0] & 0x7f);
    if (seconds < 0) {
        ESP_LOGW(kTag, "RX8130CE: invalid date %02x-%02x-%02x %02x:%02x:%02x", r[6], r[5], r[4], r[2], r[1], r[0]);
        return {};
    }
    return {.valid = true, .seconds = seconds};
}

bool set_clock(int64_t seconds) {
    std::array<uint8_t, 7> time{};
    std::array<uint8_t, 3> r{};  // 1Dh flags, 1Eh control 0, 1Fh control 1
    if (i2c_bus == nullptr || !board_support::clock_registers(seconds, time) ||
        !board_support::read_registers(i2c_bus, 0x32, 0x1d, r)) {
        return false;
    }
    auto write = [](uint8_t first, std::span<const uint8_t> bytes) {
        return board_support::write_registers(i2c_bus, 0x32, first, bytes);
    };
    // RX8130CE application manual: after VLF every register is to be set
    // again (14.5), here as in the clock-only setup of 18.4: no digital
    // offset (30h), extension register 04h (1Ch), interrupts off.
    const bool lost = r[0] & 0x02;
    if (lost && (!write(0x30, std::array<uint8_t, 1>{0x00}) || !write(0x1c, std::array<uint8_t, 1>{0x04}))) {
        return false;
    }
    // STOP (1Eh bit 6) while the time is written (18.5); TEST (bit 7) is
    // always written 0 (13.3.5). INIEN and CHGEN (1Fh bits 4 and 5) as the
    // vendor's setup leaves them, for the rechargeable backup on VBAT (4.1).
    const uint8_t control = lost ? 0x00 : r[1] & 0x3f;
    if (!write(0x1e, std::array<uint8_t, 1>{static_cast<uint8_t>(control | 0x40)}) || !write(0x10, time) ||
        !write(0x1f, std::array<uint8_t, 1>{static_cast<uint8_t>(r[2] | 0x30)}) ||
        !write(0x1e, std::array<uint8_t, 1>{control})) {
        return false;
    }
    // VLF clears when 0 is written once the oscillator runs, which takes up
    // to a second after power-up (18.1, table 4); writing 1 to the other flags
    // leaves them as they are.
    for (int i = 0; i < 120; ++i) {
        std::array<uint8_t, 1> flags{};
        if (!write(0x1d, std::array<uint8_t, 1>{0xfd}) || !board_support::read_registers(i2c_bus, 0x32, 0x1d, flags)) {
            return false;
        }
        if ((flags[0] & 0x02) == 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(kTag, "RX8130CE: VLF stays set");
    return false;
}

}  // namespace gbx::board
