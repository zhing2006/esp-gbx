#include "gbx/esp/board.h"

#include <algorithm>

#include "driver/i2c_master.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gbx/esp/board_support.h"

// Pins, vendor sources and timing constraints are in docs/boards/wks_s31_cb.md.
namespace gbx::board {

namespace {

constexpr const char* kTag = "board";

// The WKS43WV067-WCT on the 40-pin RGB connector: an 800 x 480 RGB panel
// that scans out landscape and needs neither an init sequence nor a reset,
// and a GT1151Q touch controller. RGB565 on 16 data lines; the connector
// ties the panel's low colour bits to the high ones.
constexpr int kWidth = 800;
constexpr int kHeight = 480;
constexpr gpio_num_t kPclk = GPIO_NUM_40;
constexpr gpio_num_t kDe = GPIO_NUM_43;
constexpr gpio_num_t kHsync = GPIO_NUM_44;
constexpr gpio_num_t kVsync = GPIO_NUM_45;
// Bits 0-15 of a pixel: B3-B7, G2-G7, R3-R7. R6 is GPIO36 and R7 GPIO35.
constexpr gpio_num_t kData[16] = {
    GPIO_NUM_8,  GPIO_NUM_9,  GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15,
    GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_33, GPIO_NUM_34, GPIO_NUM_36, GPIO_NUM_35,
};
// PLL_F160M divided by 2 + 11/21 and by 2: 160 MHz * 21 / 106 = 31.698113
// MHz. With the vendor's sync widths and back porches (rgb_lcd.h) and the
// front porches set to 36 and 31, 972 x 546 periods give 59.727523 Hz, 0.4
// ppm above the GB frame rate. The screen's spec allows 889-1143 periods a
// line, 513-767 lines and up to 50 MHz.
constexpr uint32_t kPixelClock = 31'698'113;
// BL_CTR drives the MP3302's EN through 10 kOhm on the screen, which pulls
// EN down with 4.7 kOhm: high turns the backlight on.
constexpr gpio_num_t kBacklight = GPIO_NUM_38;

// GT1151Q, its I2C address fixed at 0x14. The platform polls it, so its
// interrupt line only has to stay an input.
constexpr uint8_t kTouchAddress = 0x14;
constexpr gpio_num_t kTouchReset = GPIO_NUM_39;
constexpr gpio_num_t kTouchInt = GPIO_NUM_55;

// One I2C bus with 4.7 kOhm pull-ups: GT1151Q (0x14), ES8389 (0x10, AD0
// low) and the camera connector. The platform reads the touch and the codec
// changes volume from different tasks; the I2C driver serializes their
// transfers.
constexpr gpio_num_t kI2cSda = GPIO_NUM_0;
constexpr gpio_num_t kI2cScl = GPIO_NUM_1;

constexpr board_support::I2sPins kI2s{.mclk = GPIO_NUM_2, .bclk = GPIO_NUM_3, .ws = GPIO_NUM_4, .dout = GPIO_NUM_5};
// PA_CTRL switches both NS4150B, high on, with pull-downs on the board. The
// amplifiers run from SYS_5V.
constexpr gpio_num_t kAmplifier = GPIO_NUM_7;

i2c_master_bus_handle_t shared_bus = nullptr;

}  // namespace

const char* name() {
    return "Waiken-Smart ESP32-S31 CB";
}

bool init() {
    // The MP3302's EN takes PWM; the screen's spec gives no frequency range.
    board_support::backlight_init(kBacklight, 20000, 10);
    board_support::output_pin(kAmplifier, false);
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
    // SDMMC slot 0's IOMUX pins, 3.3 V, without external pull-ups.
    return {.slot = 0, .clk = 24, .cmd = 25, .data = {20, 21, 22, 23}};
}

Display display_init(int frame_buffers) {
    esp_lcd_rgb_panel_config_t config{};
    config.clk_src = LCD_CLK_SRC_PLL160M;
    config.timings.pclk_hz = kPixelClock;
    config.timings.h_res = kWidth;
    config.timings.v_res = kHeight;
    config.timings.hsync_pulse_width = 48;
    config.timings.hsync_back_porch = 88;
    config.timings.hsync_front_porch = 36;
    config.timings.vsync_pulse_width = 3;
    config.timings.vsync_back_porch = 32;
    config.timings.vsync_front_porch = 31;
    // Data changes on the falling edge, as in the vendor's rgb_lcd.c.
    config.timings.flags.pclk_active_neg = true;
    config.data_width = 16;
    config.in_color_format = LCD_COLOR_FMT_RGB565;
    config.num_fbs = static_cast<size_t>(frame_buffers);
    config.dma_burst_size = 64;
    config.hsync_gpio_num = kHsync;
    config.vsync_gpio_num = kVsync;
    config.de_gpio_num = kDe;
    config.pclk_gpio_num = kPclk;
    config.disp_gpio_num = GPIO_NUM_NC;
    std::copy(std::begin(kData), std::end(kData), config.data_gpio_nums);
    config.flags.fb_in_psram = true;
    esp_lcd_panel_handle_t panel = nullptr;
    esp_err_t err = esp_lcd_new_rgb_panel(&config, &panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_reset(panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "RGB panel: %s", esp_err_to_name(err));
        return {};
    }
    // Held with the panel's right edge up, the portrait UI turns clockwise
    // onto the landscape panel.
    return {.panel = panel,
            .driver = PanelDriver::rgb,
            .width = kWidth,
            .height = kHeight,
            .rotation = Rotation::cw90};
}

void set_backlight(int percent) {
    board_support::backlight_set(percent);
}

Touch touch_init() {
    if (shared_bus == nullptr) {
        return {};
    }
    // GT1151Q programming guide 4.1 and 4.2: INT an input with pull-up,
    // RESET low for at least 100 us, then high; the host talks to it 60 ms
    // later.
    gpio_config_t int_config{};
    int_config.pin_bit_mask = 1ULL << kTouchInt;
    int_config.mode = GPIO_MODE_INPUT;
    int_config.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&int_config);
    board_support::output_pin(kTouchReset, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(kTouchReset, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = kTouchAddress;
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
    // The GT1151Q reports touches in the GT911's registers from 0x814E. The
    // driver gets no pins: the reset is done, and its GT911 address
    // selection does not apply to this chip.
    esp_lcd_touch_config_t config{};
    config.x_max = kWidth;
    config.y_max = kHeight;
    config.rst_gpio_num = GPIO_NUM_NC;
    config.int_gpio_num = GPIO_NUM_NC;
    esp_lcd_touch_handle_t handle = nullptr;
    err = esp_lcd_touch_new_i2c_gt911(io, &config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "GT1151Q: %s", esp_err_to_name(err));
        return {};
    }
    // Its configuration reports an 800 x 480 range in the panel's scan
    // orientation.
    return {.handle = handle, .width = kWidth, .height = kHeight};
}

Audio audio_init(uint32_t rate, uint32_t buffers, uint32_t frames) {
    if (shared_bus == nullptr) {
        return {};
    }
    i2s_chan_handle_t tx = board_support::new_i2s_tx(kI2s, rate, buffers, frames);
    if (tx == nullptr || !board_support::open_codec({.chip = board_support::CodecChip::es8389,
                                                     .bus = shared_bus,
                                                     .address = 0x10,
                                                     .amplifier = kAmplifier,
                                                     .amplifier_volts = 5.0f},
                                                    tx, rate)) {
        return {};
    }
    // ES8389's left and right outputs each drive an NS4150B and a speaker.
    return {.tx = tx};
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
