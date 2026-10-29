#include "gbx/esp/board.h"

#include <algorithm>

#include "driver/i2c_master.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "gbx/esp/board_support.h"

// Pins, vendor sources and timing constraints are in docs/boards/esp32_s31_korvo_1.md.
namespace gbx::board {

namespace {

constexpr const char* kTag = "board";

// The ESP32-S3-LCD-EV-Board-SUB3 on the LCD connector J14: an 800 x 480 RGB
// panel with an ST7262E43 that scans out landscape and needs neither an
// init sequence nor a reset, and a GT1151 touch controller. RGB565 on 16
// data lines; the screen board ties the panel's low colour bits to B3, G2
// and R3.
constexpr int kWidth = 800;
constexpr int kHeight = 480;
constexpr gpio_num_t kPclk = GPIO_NUM_40;
constexpr gpio_num_t kDe = GPIO_NUM_43;
constexpr gpio_num_t kHsync = GPIO_NUM_44;
constexpr gpio_num_t kVsync = GPIO_NUM_45;
// Bits 0-15 of a pixel: B3-B7, G2-G7, R3-R7.
constexpr gpio_num_t kData[16] = {
    GPIO_NUM_8,  GPIO_NUM_9,  GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15,
    GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_33, GPIO_NUM_34, GPIO_NUM_35, GPIO_NUM_36,
};
// PLL_F160M divided by 3 + 1/33 and by 2: 26.4 MHz. 877 x 504 periods give
// 59.727426 Hz, 1.3 ppm below the GB frame rate. The ST7262E43 takes
// 23-27 MHz, 808-896 periods a line and 488-504 lines. Its back porches
// count from the sync pulse; 43 and 12 are the blanking its SYNC mode
// defaults to (H_BLANKING, V_BLANKING), should the panel ignore DE.
constexpr uint32_t kPixelClock = 26'400'000;

// GT1151, its I2C address fixed at 0x14. The screen board pulls its reset
// up and leaves its interrupt on a test point: the platform polls it.
constexpr uint8_t kTouchAddress = 0x14;

// One I2C bus with 4.7 kOhm pull-ups: GT1151 (0x14), ES8389 (0x10) and the
// camera connector. The platform reads the touch and the codec changes
// volume from different tasks; the I2C driver serializes their transfers.
constexpr gpio_num_t kI2cSda = GPIO_NUM_0;
constexpr gpio_num_t kI2cScl = GPIO_NUM_1;

constexpr board_support::I2sPins kI2s{.mclk = GPIO_NUM_2, .bclk = GPIO_NUM_3, .ws = GPIO_NUM_4, .dout = GPIO_NUM_5};
// PA_CTRL switches both NS4150B, high on, with pull-downs on the board. The
// amplifiers run from VCC_5V.
constexpr gpio_num_t kAmplifier = GPIO_NUM_7;
// SD_CTRL drives the gate of the P-MOSFET Q1 that feeds the card, low on.
// Q1's gate is pulled down, so the card has power from reset; the pin is
// also a strapping pin (boot mode bit 1).
constexpr gpio_num_t kCardPower = GPIO_NUM_39;

i2c_master_bus_handle_t shared_bus = nullptr;

}  // namespace

const char* name() {
    return "Espressif ESP32-S31-Korvo-1";
}

bool init() {
    board_support::output_pin(kCardPower, false);
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
    config.timings.hsync_pulse_width = 4;
    config.timings.hsync_back_porch = 39;
    config.timings.hsync_front_porch = 34;
    config.timings.vsync_pulse_width = 4;
    config.timings.vsync_back_porch = 8;
    config.timings.vsync_front_porch = 12;
    // The panel latches data on the rising edge: change it on the falling one.
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
    // Held with the panel's left edge up, the portrait UI turns by 270
    // degrees onto the landscape panel. The screen board pulls up its
    // backlight boost's enable and DISP: the backlight is on with the
    // board's 5 V.
    return {.panel = panel,
            .driver = PanelDriver::rgb,
            .width = kWidth,
            .height = kHeight,
            .rotation = Rotation::cw270,
            .backlight = false};
}

void set_backlight(int) {}

Touch touch_init() {
    if (shared_bus == nullptr) {
        return {};
    }
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
    // The GT1151 reports touches in the GT911's registers from 0x814E. The
    // driver gets no pins: the screen board has no reset or interrupt line
    // for it, and its GT911 address selection does not apply to this chip.
    esp_lcd_touch_config_t config{};
    config.x_max = kWidth;
    config.y_max = kHeight;
    config.rst_gpio_num = GPIO_NUM_NC;
    config.int_gpio_num = GPIO_NUM_NC;
    esp_lcd_touch_handle_t handle = nullptr;
    err = esp_lcd_touch_new_i2c_gt911(io, &config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "GT1151: %s", esp_err_to_name(err));
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
