#pragma once

#include <cstdint>
#include <span>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_types.h"
#include "esp_lcd_types.h"
#include "soc/soc_caps.h"
#if SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_mipi_dsi.h"
#endif

// Steps the boards take the same way. Boards call these from their board.cpp
// and fill in their own pins, timings and panel driver.
namespace gbx::board_support {

#if SOC_MIPI_DSI_SUPPORTED
// Powers the MIPI DSI PHY from the P4's on-chip LDO channel 3.
bool power_dsi_phy(int millivolts);

// Keeps LDO4 at 3.3 V for boards whose GPIO39-48 IO domain uses that rail.
// The board owns this supply for its lifetime, independently of SD mounting.
bool power_io_domain();

struct DsiBus {
    esp_lcd_dsi_bus_handle_t bus = nullptr;
    esp_lcd_panel_io_handle_t io = nullptr;  // DBI command channel with 8-bit commands and parameters
};

// Creates DSI controller 0 and its command channel.
DsiBus new_dsi_bus(int lanes, float lane_mbps);

// DPI panel settings shared by all boards: RGB565 in the frame buffers, the
// default DPI clock source and virtual channel 0.
// All boards target the GB/GBA rate, 4194304 / 70224 Hz. Choose the pixel
// clock and porches together, accounting for IDF's bridge/DSI rounding.
esp_lcd_dpi_panel_config_t dpi_config(float pixel_clock_mhz, const esp_lcd_video_timing_t& timing, int frame_buffers);
#endif

// Configures a push-pull output and sets its level.
void output_pin(gpio_num_t pin, bool level);

// Drives a backlight enable pin, high on, with LEDC PWM at `frequency_hz` and
// `bits` of duty resolution, starting off. The frequency is the backlight
// driver's, see the board docs. `max_percent` is the duty at full brightness.
// One backlight per board.
bool backlight_init(gpio_num_t pin, uint32_t frequency_hz, int bits, int max_percent = 100);
// 0 to 100 percent of full brightness.
void backlight_set(int percent);

// A date and time read from a clock chip's BCD registers. Returns seconds
// since 1970-01-01, or -1 if a field is not valid BCD or out of range.
// `year` is 0 to 99 for 2000 to 2099.
int64_t clock_seconds(uint8_t year, uint8_t month, uint8_t day, uint8_t hours, uint8_t minutes, uint8_t seconds);
// The other way: the BCD seconds, minutes, hours, weekday, day, month and
// year of `seconds` since 1970-01-01. The weekday is one bit, Sunday in bit
// 0, as Epson's RTCs keep it. False outside 2000 to 2099.
bool clock_registers(int64_t seconds, std::span<uint8_t, 7> out);

// Reads `out.size()` registers from `first` on, for chips that count the
// register address up during a read, such as Epson's RTCs.
bool read_registers(i2c_master_bus_handle_t bus, uint8_t address, uint8_t first, std::span<uint8_t> out);
// Writes up to 15 registers from `first` on in one transaction, for chips
// that count the register address up during a write.
bool write_registers(i2c_master_bus_handle_t bus, uint8_t address, uint8_t first, std::span<const uint8_t> bytes);

struct I2sPins {
    gpio_num_t mclk;
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout;
};

// Creates I2S0's TX channel in standard mode for 16-bit stereo at `rate`,
// MCLK at 256 times the rate. The DMA sends zeros from buffers nobody
// refilled. The channel is not enabled.
i2s_chan_handle_t new_i2s_tx(const I2sPins& pins, uint32_t rate, uint32_t buffers, uint32_t frames);

enum class CodecChip : uint8_t { es8311, es8388, es8389 };

struct Codec {
    CodecChip chip;
    i2c_master_bus_handle_t bus;
    uint8_t address;  // 7-bit I2C address
    gpio_num_t amplifier = GPIO_NUM_NC;  // enable pin, high on; an output the board drives low from init()
    float amplifier_volts;  // the amplifier's supply, which limits the codec's level
};

// Sets the codec up through esp_codec_dev and opens it for 16-bit stereo
// output at `rate` on the TX channel, then switches the amplifier on. The
// channel is left enabled. Only one codec at a time. Returns false on failure.
bool open_codec(const Codec& codec, i2s_chan_handle_t tx, uint32_t rate);
void codec_volume(int percent);
void codec_mute(bool muted);

}  // namespace gbx::board_support
