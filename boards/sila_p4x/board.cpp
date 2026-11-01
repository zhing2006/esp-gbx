#include "gbx/esp/board.h"

#include "axs15260_touch.h"
#include "driver/i2c_master.h"
#include "esp_lcd_axs15260d.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gbx/esp/board_support.h"

// P4X wiring and the AXS15260D display: docs/boards/sila_p4x.md.
namespace gbx::board {

namespace {

constexpr const char* kTag = "board";

// The 6.2-inch AXS15260D panel scans out portrait, 452 x 1280.
constexpr int kWidth = 452;
constexpr int kHeight = 1280;
constexpr gpio_num_t kLcdReset = GPIO_NUM_26;
constexpr gpio_num_t kBacklight = GPIO_NUM_30;  // high turns the backlight on

// The AXS15260D holds both the display and the touch controller, which share
// reset on the display connector. I2C1 is also wired to AXP2101; this board
// component leaves its power rail settings alone.
constexpr gpio_num_t kTouchSda = GPIO_NUM_28;
constexpr gpio_num_t kTouchScl = GPIO_NUM_29;
constexpr gpio_num_t kTouchInt = GPIO_NUM_27;
constexpr gpio_num_t kAmplifierEnable = GPIO_NUM_31;
constexpr gpio_num_t kSdPowerDisable = GPIO_NUM_51;

// ES8389 (0x10, AD0 low) shares I2C0 with SC7A20 and the camera's control;
// the bus has 1 kOhm pull-ups on the board. The schematic wires MCLK, which
// the upstream code leaves unused. GPIO31 switches both NS4150B, which run
// from VSYS: that is the battery's voltage without USB power, so the codec's
// level is limited as for a 3.3 V supply.
constexpr gpio_num_t kAudioSda = GPIO_NUM_7;
constexpr gpio_num_t kAudioScl = GPIO_NUM_8;
constexpr board_support::I2sPins kI2s{.mclk = GPIO_NUM_13, .bclk = GPIO_NUM_12, .ws = GPIO_NUM_10, .dout = GPIO_NUM_9};

i2c_master_bus_handle_t shared_bus = nullptr;

}  // namespace

const char* name() {
    return "Sila P4X AIBox";
}

bool init() {
    // SY7200AABC EN/PWM dimming works from 20 kHz to 1 MHz, see docs/boards/sila_p4x.md.
    // TEMPORARY: R26 on this board measures about 1 ohm, not the schematic's
    // 10 ohm, so full duty overdrives the 6.2-inch backlight. Full brightness
    // is 5% duty until the resistor is changed or the module's LED current
    // is known.
    board_support::backlight_init(kBacklight, 25000, 11, 5);
    board_support::output_pin(kAmplifierEnable, false);
    // Q1 in the SD sub-sheet switches the card's 3.3 V rail, active low.
    board_support::output_pin(kSdPowerDisable, false);
    if (!board_support::power_io_domain()) return false;
    i2c_master_bus_config_t config{};
    config.i2c_port = 1;
    config.sda_io_num = kTouchSda;
    config.scl_io_num = kTouchScl;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&config, &shared_bus);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "shared touch/PMU I2C bus: %s", esp_err_to_name(err));
        return false;
    }
    // TODO: AXP2101 status, SC7A20 and C5 connectivity.
    return true;
}

Sdmmc sdmmc() {
    return {.slot = 0, .clk = 43, .cmd = 44, .data = {39, 40, 41, 42}};
}

Display display_init(int frame_buffers) {
    if (!board_support::power_dsi_phy(2500)) {
        return {};
    }
    // GPIO26 resets display and touch together, here only. The AXS15260D
    // takes reset low for at least 10 ms and its init sequence 160 ms after
    // release (tRT1), as in the AIBox code.
    board_support::output_pin(kLcdReset, false);
    vTaskDelay(pdMS_TO_TICKS(11));
    gpio_set_level(kLcdReset, 1);
    vTaskDelay(pdMS_TO_TICKS(170));
    // PLL_F240M / 4 = 60 MHz exactly. The vendor's porches with HFP 79 and
    // VFP 252 give 631 x 1592 total periods, 59.728118 Hz; the vertical
    // blanking, about 3.3 ms, keeps the vendor's time for the touch scan.
    // The lanes run at 1000 Mbps as in the AIBox code: at 960 Mbps the panel
    // did not keep its image.
    auto dsi = board_support::new_dsi_bus(2, 1000);
    if (dsi.io == nullptr) {
        return {};
    }
    esp_lcd_video_timing_t timing{
        .h_size = kWidth,
        .v_size = kHeight,
        .hsync_pulse_width = 10,
        .hsync_back_porch = 90,
        .hsync_front_porch = 79,
        .vsync_pulse_width = 50,
        .vsync_back_porch = 10,
        .vsync_front_porch = 252,
    };
    esp_lcd_dpi_panel_config_t dpi = board_support::dpi_config(60, timing, frame_buffers);
    // The DSI bridge widens the RGB565 frame buffers to the RGB888 the panel
    // is set up for (COLMOD 0x77 from 24 bits per pixel). The panel shows
    // RGB565 packets in wrong colors.
    dpi.out_color_format = LCD_COLOR_FMT_RGB888;
    axs15260d_vendor_config_t vendor{};
    vendor.mipi_config.dsi_bus = dsi.bus;
    vendor.mipi_config.dpi_config = &dpi;
    // No esp_lcd_panel_reset(): without a reset GPIO it sends a soft reset,
    // which the AIBox code does not.
    esp_lcd_panel_dev_config_t dev{
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 24,
        .reset_gpio_num = GPIO_NUM_NC,
        .vendor_config = &vendor,
        .flags = {},
    };
    esp_lcd_panel_handle_t panel = nullptr;
    esp_err_t err = esp_lcd_new_panel_axs15260d(dsi.io, &dev, &panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel);
    }
    // As in the AIBox code, DISPON once more after the video stream started.
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "AXS15260D: %s", esp_err_to_name(err));
        return {};
    }
    // The UI is landscape, 1280 x 452, rotated clockwise by 90 degrees:
    // UI (x, y) = (panel y, 451 - panel x).
    return {.panel = panel, .width = kWidth, .height = kHeight, .rotation = Rotation::cw90};
}

void set_backlight(int percent) {
    board_support::backlight_set(percent);
}

Touch touch_init() {
    if (shared_bus == nullptr) return {};
    esp_lcd_touch_handle_t handle = nullptr;
    esp_err_t err = new_axs15260_touch(shared_bus, kTouchInt, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "AXS15260 touch: %s", esp_err_to_name(err));
        return {};
    }
    // The controller reports panel coordinates.
    return {.handle = handle, .width = kWidth, .height = kHeight};
}

Audio audio_init(uint32_t rate, uint32_t buffers, uint32_t frames) {
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = 0;
    bus_config.sda_io_num = kAudioSda;
    bus_config.scl_io_num = kAudioScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "I2C0 for audio: %s", esp_err_to_name(err));
        return {};
    }
    i2s_chan_handle_t tx = board_support::new_i2s_tx(kI2s, rate, buffers, frames);
    if (tx == nullptr || !board_support::open_codec({.chip = board_support::CodecChip::es8389,
                                                     .bus = bus,
                                                     .address = 0x10,
                                                     .amplifier = kAmplifierEnable,
                                                     .amplifier_volts = 3.3f},
                                                    tx, rate)) {
        return {};
    }
    // Each DAC channel drives its own amplifier and speaker, left and right.
    return {.tx = tx, .mono = false};
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
