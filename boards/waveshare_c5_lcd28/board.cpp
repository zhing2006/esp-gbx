#include "gbx/esp/board.h"

#include <algorithm>
#include <array>
#include <bit>

#include "cst3530_touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gbx/esp/board_support.h"

// Pins, the IO expander, vendor sources and timing constraints are in
// docs/boards/waveshare_c5_lcd28.md.
namespace gbx::board {

namespace {

constexpr const char* kTag = "board";

// ST7789V2 behind a 2.8-inch 240 x 320 panel, portrait as it scans out.
constexpr int kWidth = 240;
constexpr int kHeight = 320;

// One I2C bus with 2 kOhm pull-ups (R54, R55): CH32V003 (0x24), CST3530
// (0x58), ES8311 (0x18, CE low), PCF85063A (0x51), QMI8658 and SHTC3. The
// platform reads the touch and the codec changes volume from different
// tasks; the I2C driver serializes their transfers.
constexpr gpio_num_t kI2cSda = GPIO_NUM_0;
constexpr gpio_num_t kI2cScl = GPIO_NUM_1;

// SPI2 carries the panel and the TF card, each with its own chip select.
// MISO is not an IOMUX pin of SPI2, so the bus goes through the GPIO matrix.
constexpr spi_host_device_t kSpiHost = SPI2_HOST;
constexpr gpio_num_t kSpiClk = GPIO_NUM_6;
constexpr gpio_num_t kSpiMosi = GPIO_NUM_7;
constexpr gpio_num_t kSpiMiso = GPIO_NUM_8;
constexpr gpio_num_t kLcdDc = GPIO_NUM_9;
constexpr gpio_num_t kLcdCs = GPIO_NUM_10;
constexpr gpio_num_t kSdCs = GPIO_NUM_23;
// The ST7789V2's shortest write cycle is 16 ns (datasheet 7.4.2), 62.5 MHz.
// SPI2 divides PLL_F160M, and 160 MHz / 3 is the fastest clock under it.
constexpr int kLcdClockHz = 160'000'000 / 3;
// A whole screen in one transfer; rows of the frame buffer, 480 bytes, keep
// the DMA's alignment for PSRAM.
constexpr int kMaxTransfer = kWidth * kHeight * 2;

// CH32V003 at 0x24, running Waveshare's IO expander firmware: register 02h
// sets a pin as output with its bit, 03h holds the output levels, 05h the
// backlight PWM (0 to 255) and 06h the battery ADC, two bytes, low first.
constexpr uint8_t kExpanderAddress = 0x24;
constexpr uint8_t kExpanderMode = 0x02;
constexpr uint8_t kExpanderOutput = 0x03;
constexpr uint8_t kExpanderPwm = 0x05;
constexpr uint8_t kTouchReset = 1 << 0;  // EXIO0, low resets the CST3530
constexpr uint8_t kLcdReset = 1 << 1;    // EXIO1, low resets the ST7789V2
constexpr uint8_t kAmplifier = 1 << 3;   // EXIO3, PA_CTRL of the NS4150B, 10 kOhm pull-down
// The panel takes commands 120 ms after its reset ends (ST7789V2 datasheet,
// reset timing); the vendor's examples give the touch 10 to 500 ms.
constexpr int64_t kLcdResetUs = 120'000;
constexpr int64_t kTouchStartUs = 500'000;

// ES8311 without MCLK: the codec takes its clock from BCLK. Which of
// GPIO26 and GPIO27 carries the DAC's data is not settled, see the board
// docs; GPIO26 is the vendor BSP's choice.
constexpr board_support::I2sPins kI2s{.mclk = GPIO_NUM_NC, .bclk = GPIO_NUM_24, .ws = GPIO_NUM_25, .dout = GPIO_NUM_26};

constexpr uint8_t kClockAddress = 0x51;  // PCF85063A

i2c_master_bus_handle_t shared_bus = nullptr;
i2c_master_dev_handle_t expander = nullptr;
uint8_t expander_outputs = 0;
int64_t reset_released_us = 0;

bool expander_write(uint8_t reg, uint8_t value) {
    const uint8_t message[2] = {reg, value};
    const esp_err_t err = expander ? i2c_master_transmit(expander, message, sizeof message, 50) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "CH32V003 register 0x%02x: %s", reg, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool set_outputs(uint8_t pins, bool high) {
    expander_outputs = high ? expander_outputs | pins : expander_outputs & ~pins;
    return expander_write(kExpanderOutput, expander_outputs);
}

void wait_until(int64_t us) {
    const int64_t now = esp_timer_get_time();
    if (now < us) {
        vTaskDelay(pdMS_TO_TICKS((us - now + 999) / 1000));
    }
}

// The card starts in SD mode, where it reads the panel's traffic on MOSI as
// commands. At least 74 clocks with CS high and CMD0 with CS low switch it
// to SPI mode (SD Physical Layer Simplified Specification 7.2.1), where it
// ignores the bus while CS is high. Without a card nothing answers.
void card_to_spi_mode() {
    spi_device_interface_config_t config{};
    config.clock_speed_hz = 400'000;
    config.mode = 0;
    config.spics_io_num = -1;
    config.queue_size = 1;
    spi_device_handle_t device = nullptr;
    if (spi_bus_add_device(kSpiHost, &config, &device) != ESP_OK) {
        return;
    }
    auto send = [&](const uint8_t* bytes, size_t size, uint8_t* received = nullptr) {
        spi_transaction_t t{};
        t.length = size * 8;
        t.tx_buffer = bytes;
        t.rx_buffer = received;
        spi_device_polling_transmit(device, &t);
    };
    static constexpr uint8_t kIdle[10] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    send(kIdle, sizeof kIdle);
    gpio_set_level(kSdCs, 0);
    static constexpr uint8_t kCmd0[6] = {0x40, 0, 0, 0, 0, 0x95};
    send(kCmd0, sizeof kCmd0);
    uint8_t r1 = 0xff;
    for (int i = 0; i < 8 && r1 == 0xff; ++i) {
        send(kIdle, 1, &r1);
    }
    gpio_set_level(kSdCs, 1);
    send(kIdle, 1);
    spi_bus_remove_device(device);
    ESP_LOGD(kTag, "TF card CMD0: R1 0x%02x", r1);
}

}  // namespace

const char* name() {
    return "Waveshare ESP32-C5-Touch-LCD-2.8";
}

bool init() {
    // Both chip selects high before the bus carries anything.
    board_support::output_pin(kSdCs, true);
    board_support::output_pin(kLcdCs, true);

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
    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = kExpanderAddress;
    device.scl_speed_hz = 400000;
    err = i2c_master_bus_add_device(shared_bus, &device, &expander);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "CH32V003 device: %s", esp_err_to_name(err));
        return false;
    }
    // Backlight off, the amplifier off, the panel and the touch in reset;
    // the levels go out before the pins turn into outputs.
    expander_outputs = 0;
    if (!expander_write(kExpanderPwm, 0) || !expander_write(kExpanderOutput, expander_outputs) ||
        !expander_write(kExpanderMode, kTouchReset | kLcdReset | kAmplifier)) {
        ESP_LOGE(kTag, "CH32V003 at 0x%02x does not answer", kExpanderAddress);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!set_outputs(kTouchReset | kLcdReset, true)) {
        return false;
    }
    reset_released_us = esp_timer_get_time();

    spi_bus_config_t bus{};
    bus.mosi_io_num = kSpiMosi;
    bus.miso_io_num = kSpiMiso;
    bus.sclk_io_num = kSpiClk;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kMaxTransfer;
    err = spi_bus_initialize(kSpiHost, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "SPI bus: %s", esp_err_to_name(err));
        return false;
    }
    card_to_spi_mode();
    return true;
}

Sdmmc sdmmc() {
    return {.slot = kSpiHost, .clk = kSpiClk, .cmd = kSpiMosi, .data = {kSpiMiso, -1, -1, -1},
            .bus = Sdmmc::Bus::spi, .cs = kSdCs};
}

Display display_init(int) {
    if (shared_bus == nullptr) {
        return {};
    }
    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = kLcdCs;
    io_config.dc_gpio_num = kLcdDc;
    io_config.spi_mode = 0;
    io_config.pclk_hz = kLcdClockHz;
    io_config.trans_queue_depth = 4;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    // The platform's frame buffer is in PSRAM; the DMA reads it directly.
    io_config.flags.psram_dma_direct = 1;
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_err_t err = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_config, &io);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "panel IO: %s", esp_err_to_name(err));
        return {};
    }
    // RAMCTRL's ENDIAN bit takes RGB565 low byte first, as the UI and the
    // frame buffer hold it, like the vendor's Arduino setup (B0h 00h E8h).
    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    panel_config.bits_per_pixel = 16;
    esp_lcd_panel_handle_t panel = nullptr;
    err = esp_lcd_new_panel_st7789(io, &panel_config, &panel);
    if (err == ESP_OK) {
        wait_until(reset_released_us + kLcdResetUs);
        err = esp_lcd_panel_init(panel);
    }
    // The vendor's settings after sleep out (Display_ST7789.cpp): porches,
    // gate voltages, VCOM, power, 60 Hz frame rate and gamma. IDF's driver
    // has set MADCTL, COLMOD and RAMCTRL.
    struct Command {
        uint8_t command;
        uint8_t size;
        uint8_t data[14];
    };
    static constexpr Command kInit[] = {
        {0xb2, 5, {0x0c, 0x0c, 0x00, 0x33, 0x33}},
        {0xb7, 1, {0x75}},
        {0xbb, 1, {0x1a}},
        {0xc0, 1, {0x2c}},
        {0xc2, 2, {0x01, 0xff}},
        {0xc3, 1, {0x13}},
        {0xc4, 1, {0x20}},
        {0xc6, 1, {0x0f}},
        {0xd0, 2, {0xa4, 0xa1}},
        {0xd6, 1, {0xa1}},
        {0xe0, 14, {0xd0, 0x0d, 0x14, 0x0d, 0x0d, 0x09, 0x38, 0x44, 0x4e, 0x3a, 0x17, 0x18, 0x2f, 0x30}},
        {0xe1, 14, {0xd0, 0x09, 0x0f, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4d, 0x38, 0x15, 0x16, 0x2c, 0x2e}},
    };
    for (const Command& c : kInit) {
        if (err == ESP_OK) err = esp_lcd_panel_io_tx_param(io, c.command, c.data, c.size);
    }
    // The panel shows colors inverted without INVON, as the vendor sends it.
    if (err == ESP_OK) err = esp_lcd_panel_invert_color(panel, true);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ST7789V2: %s", esp_err_to_name(err));
        return {};
    }
    return {.panel = panel,
            .driver = PanelDriver::spi,
            .width = kWidth,
            .height = kHeight,
            .rotation = Rotation::none,
            .io = io};
}

void set_backlight(int percent) {
    // Q1 (8050) switches the LEDs' low side with LCD_BL, the expander's PWM.
    expander_write(kExpanderPwm, static_cast<uint8_t>(std::clamp(percent, 0, 100) * 255 / 100));
}

Touch touch_init() {
    if (shared_bus == nullptr) {
        return {};
    }
    // The touch answers some time after its reset; the vendor waits up to
    // 500 ms.
    esp_lcd_touch_handle_t handle = nullptr;
    esp_err_t err = ESP_FAIL;
    for (;;) {
        err = new_cst3530_touch(shared_bus, &handle);
        if (err == ESP_OK || esp_timer_get_time() >= reset_released_us + kTouchStartUs) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "CST3530: %s", esp_err_to_name(err));
        return {};
    }
    return {.handle = handle, .width = kWidth, .height = kHeight};
}

Audio audio_init(uint32_t rate, uint32_t buffers, uint32_t frames) {
    if (shared_bus == nullptr) {
        return {};
    }
    i2s_chan_handle_t tx = board_support::new_i2s_tx(kI2s, rate, buffers, frames);
    // The NS4150B runs from 3V3.
    if (tx == nullptr || !board_support::open_codec({.chip = board_support::CodecChip::es8311,
                                                     .bus = shared_bus,
                                                     .address = 0x18,
                                                     .amplifier_volts = 3.3f,
                                                     .mclk = false},
                                                    tx, rate)) {
        return {};
    }
    if (!set_outputs(kAmplifier, true)) {
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

// PCF85063A, see docs/boards/waveshare_c5_lcd28.md#rtc. The driver lives here
// rather than in drivers/ because no other board has this chip.
Clock read_clock() {
    // PCF85063A datasheet 7.2 and 7.3: 00h Control_1 with STOP in bit 5 and
    // 12_24 in bit 1, then 04h to 0Ah the seconds with OS in bit 7, minutes,
    // hours, day, weekday, month and year in BCD.
    std::array<uint8_t, 11> r{};
    if (shared_bus == nullptr || !board_support::read_registers(shared_bus, kClockAddress, 0x00, r)) {
        return {};
    }
    if (r[4] & 0x80) {
        ESP_LOGW(kTag, "PCF85063A: OS set, the clock lost its time");
        return {};
    }
    if (r[0] & 0x22) {
        ESP_LOGW(kTag, "PCF85063A: stopped or in 12-hour mode, Control_1 0x%02x", r[0]);
        return {};
    }
    const int64_t seconds = board_support::clock_seconds(r[10], r[9] & 0x1f, r[7] & 0x3f, r[6] & 0x3f, r[5] & 0x7f,
                                                         r[4] & 0x7f);
    if (seconds < 0) {
        ESP_LOGW(kTag, "PCF85063A: invalid date %02x-%02x-%02x %02x:%02x:%02x", r[10], r[9], r[7], r[6], r[5], r[4]);
        return {};
    }
    return {.valid = true, .seconds = seconds};
}

bool set_clock(int64_t seconds) {
    std::array<uint8_t, 7> time{};
    std::array<uint8_t, 1> control{};
    if (shared_bus == nullptr || !board_support::clock_registers(seconds, time) ||
        !board_support::read_registers(shared_bus, kClockAddress, 0x00, control)) {
        return false;
    }
    // The weekday is a number, 0 to 6, and comes after the day.
    const std::array<uint8_t, 7> registers = {time[0], time[1], time[2], time[4],
                                              static_cast<uint8_t>(std::countr_zero(time[3])), time[5], time[6]};
    auto write = [](uint8_t first, std::span<const uint8_t> bytes) {
        return board_support::write_registers(shared_bus, kClockAddress, first, bytes);
    };
    // STOP while the time is written (7.2.1.2), 24-hour mode; CAP_SEL in
    // bit 0 stays. Writing the seconds clears OS.
    const uint8_t running = control[0] & 0x01;
    return write(0x00, std::array<uint8_t, 1>{static_cast<uint8_t>(running | 0x20)}) && write(0x04, registers) &&
           write(0x00, std::array<uint8_t, 1>{running});
}

}  // namespace gbx::board
