#include "gbx/esp/board_support.h"

#include <algorithm>

#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "es8311_codec.h"
#include "es8388_codec.h"
#include "es8389_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#if SOC_MIPI_DSI_SUPPORTED
#include "esp_ldo_regulator.h"
#endif

namespace gbx::board_support {

namespace {

constexpr const char* kTag = "board";
constexpr int kMonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

#if SOC_MIPI_DSI_SUPPORTED
constexpr int kDsiPhyLdoChannel = 3;
esp_ldo_channel_handle_t dsi_phy_ldo = nullptr;
esp_ldo_channel_handle_t io_ldo = nullptr;
#endif
esp_codec_dev_handle_t codec_device = nullptr;
int backlight_bits = 0;  // 0 until backlight_init() succeeds
int backlight_max = 100;

}  // namespace

#if SOC_MIPI_DSI_SUPPORTED
bool power_dsi_phy(int millivolts) {
    if (dsi_phy_ldo != nullptr) {
        return true;
    }
    esp_ldo_channel_config_t config{
        .chan_id = kDsiPhyLdoChannel,
        .voltage_mv = millivolts,
        .voltage_stable_delay_us = 0,
        .flags = {},
    };
    esp_err_t err = esp_ldo_acquire_channel(&config, &dsi_phy_ldo);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "DSI PHY LDO: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool power_io_domain() {
    if (io_ldo != nullptr) {
        return true;
    }
    esp_ldo_channel_config_t config{};
    config.chan_id = 4;
    config.voltage_mv = 3300;
    config.voltage_stable_delay_us = 1000;
    esp_err_t err = esp_ldo_acquire_channel(&config, &io_ldo);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "GPIO39-48 IO supply: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

DsiBus new_dsi_bus(int lanes, float lane_mbps) {
    DsiBus result;
    esp_lcd_dsi_bus_config_t bus_config{
        .bus_id = 0,
        .num_data_lanes = static_cast<uint8_t>(lanes),
        // 0 lets the driver pick the default for the chip revision. The
        // _DEFAULT constant is XTAL, which only P4 v3.0 and later have.
        .phy_clk_src = static_cast<mipi_dsi_phy_pllref_clock_source_t>(0),
        .lane_bit_rate_mbps = lane_mbps,
        .flags = {},
    };
    esp_err_t err = esp_lcd_new_dsi_bus(&bus_config, &result.bus);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "DSI bus: %s", esp_err_to_name(err));
        return {};
    }
    esp_lcd_dbi_io_config_t io_config{
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_dbi(result.bus, &io_config, &result.io);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "DSI command channel: %s", esp_err_to_name(err));
        esp_lcd_del_dsi_bus(result.bus);
        return {};
    }
    return result;
}

esp_lcd_dpi_panel_config_t dpi_config(float pixel_clock_mhz, const esp_lcd_video_timing_t& timing, int frame_buffers) {
    return {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = pixel_clock_mhz,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = static_cast<uint8_t>(frame_buffers),
        .video_timing = timing,
        .flags = {},
    };
}
#endif

void output_pin(gpio_num_t pin, bool level) {
    // Zero-initialized and assigned: the struct has a field only some chips have.
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = GPIO_MODE_OUTPUT;
    gpio_config(&config);
    gpio_set_level(pin, level);
}

bool backlight_init(gpio_num_t pin, uint32_t frequency_hz, int bits, int max_percent) {
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = static_cast<ledc_timer_bit_t>(bits);
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = frequency_hz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    esp_err_t err = ledc_timer_config(&timer);
    if (err == ESP_OK) {
        ledc_channel_config_t channel{};
        channel.gpio_num = pin;
        channel.speed_mode = LEDC_LOW_SPEED_MODE;
        channel.channel = LEDC_CHANNEL_0;
        channel.timer_sel = LEDC_TIMER_0;
        channel.duty = 0;
        err = ledc_channel_config(&channel);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "backlight PWM: %s", esp_err_to_name(err));
        // Keep the pin defined: low, so the backlight stays off.
        output_pin(pin, false);
        return false;
    }
    backlight_bits = bits;
    backlight_max = std::clamp(max_percent, 0, 100);
    return true;
}

void backlight_set(int percent) {
    if (backlight_bits == 0) {
        return;
    }
    // A duty of 1 << bits keeps the pin high: full brightness without PWM.
    const uint32_t full = 1u << backlight_bits;
    const uint32_t duty = static_cast<uint32_t>(std::clamp(percent, 0, 100) * backlight_max) * full / 10000;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

int64_t clock_seconds(uint8_t year, uint8_t month, uint8_t day, uint8_t hours, uint8_t minutes, uint8_t seconds) {
    auto decode = [](uint8_t bcd) { return (bcd >> 4) <= 9 && (bcd & 15) <= 9 ? (bcd >> 4) * 10 + (bcd & 15) : -1; };
    const int y = decode(year), mo = decode(month), d = decode(day);
    const int h = decode(hours), mi = decode(minutes), s = decode(seconds);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
        return -1;
    }
    // 2000 to 2099: every fourth year is a leap year.
    const bool leap = y % 4 == 0;
    if (d > kMonthDays[mo - 1] + (mo == 2 && leap ? 1 : 0)) {
        return -1;
    }
    int64_t days = 10957 + y * 365 + (y + 3) / 4;  // 1970-01-01 to January 1 of the year
    for (int m = 1; m < mo; ++m) {
        days += kMonthDays[m - 1] + (m == 2 && leap ? 1 : 0);
    }
    days += d - 1;
    return ((days * 24 + h) * 60 + mi) * 60 + s;
}

bool clock_registers(int64_t seconds, std::span<uint8_t, 7> out) {
    // 2000-01-01 00:00 to 2100-01-01 00:00.
    if (seconds < 946'684'800 || seconds >= 4'102'444'800) {
        return false;
    }
    auto bcd = [](int v) { return static_cast<uint8_t>(v / 10 << 4 | v % 10); };
    int days = static_cast<int>(seconds / 86400);
    const int time = static_cast<int>(seconds % 86400);
    const int weekday = (days + 4) % 7;  // 1970-01-01 was a Thursday
    days -= 10957;
    int year = 0;
    while (days >= (year % 4 == 0 ? 366 : 365)) {
        days -= year % 4 == 0 ? 366 : 365;
        ++year;
    }
    int month = 1;
    while (days >= kMonthDays[month - 1] + (month == 2 && year % 4 == 0 ? 1 : 0)) {
        days -= kMonthDays[month - 1] + (month == 2 && year % 4 == 0 ? 1 : 0);
        ++month;
    }
    out[0] = bcd(time % 60);
    out[1] = bcd(time / 60 % 60);
    out[2] = bcd(time / 3600);
    out[3] = static_cast<uint8_t>(1 << weekday);
    out[4] = bcd(days + 1);
    out[5] = bcd(month);
    out[6] = bcd(year);
    return true;
}

bool read_registers(i2c_master_bus_handle_t bus, uint8_t address, uint8_t first, std::span<uint8_t> out) {
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = address;
    config.scl_speed_hz = 100000;
    i2c_master_dev_handle_t device = nullptr;
    esp_err_t err = i2c_master_bus_add_device(bus, &config, &device);
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(device, &first, 1, out.data(), out.size(), 50);
        i2c_master_bus_rm_device(device);
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "I2C 0x%02x register 0x%02x: %s", address, first, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool write_registers(i2c_master_bus_handle_t bus, uint8_t address, uint8_t first, std::span<const uint8_t> bytes) {
    uint8_t message[16];
    if (bytes.size() >= sizeof message) {
        return false;
    }
    message[0] = first;
    std::copy(bytes.begin(), bytes.end(), message + 1);
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = address;
    config.scl_speed_hz = 100000;
    i2c_master_dev_handle_t device = nullptr;
    esp_err_t err = i2c_master_bus_add_device(bus, &config, &device);
    if (err == ESP_OK) {
        err = i2c_master_transmit(device, message, bytes.size() + 1, 50);
        i2c_master_bus_rm_device(device);
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "I2C 0x%02x register 0x%02x: %s", address, first, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

i2s_chan_handle_t new_i2s_tx(const I2sPins& pins, uint32_t rate, uint32_t buffers, uint32_t frames) {
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = buffers;
    channel.dma_frame_num = frames;
    // A buffer nobody refilled plays as silence, not as the old sound again.
    channel.auto_clear_after_cb = true;
    i2s_chan_handle_t tx = nullptr;
    esp_err_t err = i2s_new_channel(&channel, &tx, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "I2S channel: %s", esp_err_to_name(err));
        return nullptr;
    }
    i2s_std_config_t config{
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = pins.mclk,
            .bclk = pins.bclk,
            .ws = pins.ws,
            .dout = pins.dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {},
        },
    };
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_init_std_mode(tx, &config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "I2S standard mode: %s", esp_err_to_name(err));
        i2s_del_channel(tx);
        return nullptr;
    }
    return tx;
}

bool open_codec(const Codec& codec, i2s_chan_handle_t tx, uint32_t rate) {
    audio_codec_i2s_cfg_t i2s{.port = I2S_NUM_0, .rx_handle = nullptr, .tx_handle = tx, .clk_src = 0};
    const audio_codec_data_if_t* data = audio_codec_new_i2s_data(&i2s);
    // esp_codec_dev takes the address shifted left, as on the wire.
    audio_codec_i2c_cfg_t i2c{.port = 0, .addr = static_cast<uint8_t>(codec.address << 1), .bus_handle = codec.bus,
                              .clock_speed_hz = 0};
    const audio_codec_ctrl_if_t* control = audio_codec_new_i2c_ctrl(&i2c);
    const audio_codec_gpio_if_t* gpio = audio_codec_new_gpio();
    if (data == nullptr || control == nullptr || gpio == nullptr) {
        ESP_LOGE(kTag, "codec interfaces: I2C device at 0x%02x", codec.address);
        return false;
    }
    // The level is limited so the amplifier's output stays within its supply.
    // The amplifier's own gain is not known from the schematics, so it adds
    // nothing here.
    const esp_codec_dev_hw_gain_t gain{.pa_voltage = codec.amplifier_volts, .codec_dac_voltage = 3.3f, .pa_gain = 0};
    const audio_codec_if_t* chip = nullptr;
    if (codec.chip == CodecChip::es8311) {
        es8311_codec_cfg_t config{};
        config.ctrl_if = control;
        config.gpio_if = gpio;
        config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
        config.pa_pin = GPIO_NUM_NC;
        config.use_mclk = true;
        config.hw_gain = gain;
        chip = es8311_codec_new(&config);
    } else if (codec.chip == CodecChip::es8388) {
        es8388_codec_cfg_t config{};
        config.ctrl_if = control;
        config.gpio_if = gpio;
        config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
        config.pa_pin = GPIO_NUM_NC;
        config.hw_gain = gain;
        chip = es8388_codec_new(&config);
    } else {
        es8389_codec_cfg_t config{};
        config.ctrl_if = control;
        config.gpio_if = gpio;
        config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
        config.pa_pin = GPIO_NUM_NC;
        config.use_mclk = true;
        config.hw_gain = gain;
        // Output only: no DAC reference in the ADC's right channel.
        config.no_dac_ref = true;
        chip = es8389_codec_new(&config);
    }
    if (chip == nullptr) {
        ESP_LOGE(kTag, "codec at 0x%02x does not answer", codec.address);
        return false;
    }
    // esp_codec_dev configures its amplifier pin again, which the GPIO driver
    // reports as a conflict with the board's setup; the pin is switched here.
    esp_codec_dev_cfg_t device{.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = chip, .data_if = data};
    codec_device = esp_codec_dev_new(&device);
    esp_codec_dev_sample_info_t format{};
    format.bits_per_sample = 16;
    format.channel = 2;
    format.sample_rate = rate;
    // Opening disables the channel to set the format, which fails with an
    // error on a channel that was never enabled.
    i2s_channel_enable(tx);
    if (codec_device == nullptr || esp_codec_dev_open(codec_device, &format) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(kTag, "codec output at %u Hz", static_cast<unsigned>(rate));
        return false;
    }
    if (codec.amplifier != GPIO_NUM_NC) gpio_set_level(codec.amplifier, 1);
    return true;
}

void codec_volume(int percent) {
    if (codec_device) esp_codec_dev_set_out_vol(codec_device, percent);
}

void codec_mute(bool muted) {
    if (codec_device) esp_codec_dev_set_out_mute(codec_device, muted);
}

}  // namespace gbx::board_support
