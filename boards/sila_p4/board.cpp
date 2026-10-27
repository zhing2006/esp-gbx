#include "gbx/esp/board.h"

#include "driver/i2c_master.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7703.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "gbx/esp/board_support.h"

// Pins, vendor sources and timing constraints are in docs/boards/sila_p4.md.
namespace gbx::board {

namespace {

constexpr const char* kTag = "board";

constexpr int kWidth = 720;
constexpr int kHeight = 720;
constexpr gpio_num_t kLcdReset = GPIO_NUM_24;
constexpr gpio_num_t kBacklight = GPIO_NUM_29;  // high turns the backlight on

// GT911 on its own I2C bus. Pulling GPIO28 low did not reset it during the
// probe, so whether TP_RST really reaches the controller is unconfirmed.
constexpr gpio_num_t kTouchSda = GPIO_NUM_26;
constexpr gpio_num_t kTouchScl = GPIO_NUM_27;
constexpr gpio_num_t kTouchReset = GPIO_NUM_28;
constexpr gpio_num_t kTouchInt = GPIO_NUM_25;

// ES8311 (0x18, CE low) and ES7210 share their own I2C bus. The NS4150B
// amplifier runs from AU_+3.3V, so the codec's level is limited to that supply.
constexpr gpio_num_t kAudioSda = GPIO_NUM_7;
constexpr gpio_num_t kAudioScl = GPIO_NUM_8;
constexpr board_support::I2sPins kI2s{.mclk = GPIO_NUM_13, .bclk = GPIO_NUM_12, .ws = GPIO_NUM_10, .dout = GPIO_NUM_9};
constexpr gpio_num_t kAmplifier = GPIO_NUM_53;

}  // namespace

const char* name() {
    return "Sila P4";
}

bool init() {
    // SY7200 EN/PWM dimming works from 20 kHz to 1 MHz, see docs/boards/sila_p4x.md.
    board_support::backlight_init(kBacklight, 25000, 11);
    board_support::output_pin(kAmplifier, false);
    // TODO: GPIO47 key, battery gauge.
    return board_support::power_io_domain();
}

Sdmmc sdmmc() {
    return {.slot = 1, .clk = 43, .cmd = 44, .data = {39, 40, 41, 42}};
}

Display display_init(int frame_buffers) {
    if (!board_support::power_dsi_phy(2500)) {
        return {};
    }
    // Keep the vendor's 2 lanes at 480 Mbps. A 40 MHz pixel clock with
    // 872 x 768 total periods gives 59.728593 Hz, close to GB's 59.727501 Hz.
    // 40 MHz divides PLL_F240M exactly, avoiding IDF's implicit HFP adjustment.
    auto dsi = board_support::new_dsi_bus(2, 480);
    if (dsi.io == nullptr) {
        return {};
    }
    esp_lcd_video_timing_t timing{
        .h_size = kWidth,
        .v_size = kHeight,
        .hsync_pulse_width = 20,
        .hsync_back_porch = 80,
        .hsync_front_porch = 52,
        .vsync_pulse_width = 4,
        .vsync_back_porch = 12,
        .vsync_front_porch = 32,
    };
    esp_lcd_dpi_panel_config_t dpi = board_support::dpi_config(40, timing, frame_buffers);
    st7703_vendor_config_t vendor{};
    vendor.mipi_config.dsi_bus = dsi.bus;
    vendor.mipi_config.dpi_config = &dpi;
    vendor.flags.use_mipi_interface = 1;
    esp_lcd_panel_dev_config_t dev{
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
        .reset_gpio_num = kLcdReset,
        .vendor_config = &vendor,
        .flags = {},
    };
    esp_lcd_panel_handle_t panel = nullptr;
    esp_err_t err = esp_lcd_new_panel_st7703(dsi.io, &dev, &panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_reset(panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ST7703: %s", esp_err_to_name(err));
        return {};
    }
    return {.panel = panel, .width = kWidth, .height = kHeight, .rotation = Rotation::none};
}

void set_backlight(int percent) {
    board_support::backlight_set(percent);
}

Touch touch_init() {
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = -1;
    bus_config.sda_io_num = kTouchSda;
    bus_config.scl_io_num = kTouchScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "touch I2C bus: %s", esp_err_to_name(err));
        return {};
    }
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    io_config.scl_speed_hz = 100000;
    io_config.control_phase_bytes = 1;
    io_config.lcd_cmd_bits = 16;
    io_config.flags.disable_control_phase = 1;
    esp_lcd_panel_io_handle_t io = nullptr;
    err = esp_lcd_new_panel_io_i2c(bus, &io_config, &io);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "touch I2C device: %s", esp_err_to_name(err));
        return {};
    }
    // With an address in driver_data the driver holds INT low through the
    // reset, which selects 0x5d, and waits 60 ms before the first read.
    static esp_lcd_touch_io_gt911_config_t address{.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS};
    esp_lcd_touch_config_t config{};
    config.x_max = kWidth;
    config.y_max = kHeight;
    config.rst_gpio_num = kTouchReset;
    config.int_gpio_num = kTouchInt;
    config.driver_data = &address;
    esp_lcd_touch_handle_t handle = nullptr;
    err = esp_lcd_touch_new_i2c_gt911(io, &config, &handle);
    if (err != ESP_OK) {
        // Neither address answering has meant a loose touch cable on this board.
        ESP_LOGE(kTag, "GT911: %s; 0x5d %s, 0x14 %s", esp_err_to_name(err),
                 i2c_master_probe(bus, 0x5d, 50) == ESP_OK ? "answers" : "is silent",
                 i2c_master_probe(bus, 0x14, 50) == ESP_OK ? "answers" : "is silent");
        return {};
    }
    // The GT911 configuration reports 720x720, the same as the panel.
    return {.handle = handle, .width = kWidth, .height = kHeight};
}

Audio audio_init(uint32_t rate, uint32_t buffers, uint32_t frames) {
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = -1;
    bus_config.sda_io_num = kAudioSda;
    bus_config.scl_io_num = kAudioScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "audio I2C bus: %s", esp_err_to_name(err));
        return {};
    }
    i2s_chan_handle_t tx = board_support::new_i2s_tx(kI2s, rate, buffers, frames);
    if (tx == nullptr || !board_support::open_codec({.chip = board_support::CodecChip::es8311,
                                                     .bus = bus,
                                                     .address = 0x18,
                                                     .amplifier = kAmplifier,
                                                     .amplifier_volts = 3.3f},
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
