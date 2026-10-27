/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_st7102.h"
#include "st7102_interface.h"

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save surrent value of LCD_CMD_COLMOD register
    const st7102_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int reset_level: 1;
    } flags;
    // To save the original functions of MIPI DPI panel
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
} st7102_panel_t;

static const char *TAG = "st7102";

static esp_err_t panel_st7102_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7102_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7102_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7102_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st7102_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7102_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

esp_err_t esp_lcd_new_panel_st7102_mipi(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                         esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    st7102_vendor_config_t *vendor_config = (st7102_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor_config && vendor_config->mipi_config.dpi_config && vendor_config->mipi_config.dsi_bus, ESP_ERR_INVALID_ARG, TAG,
                        "invalid vendor config");

    esp_err_t ret = ESP_OK;
    st7102_panel_t *st7102 = (st7102_panel_t *)calloc(1, sizeof(st7102_panel_t));
    ESP_RETURN_ON_FALSE(st7102, ESP_ERR_NO_MEM, TAG, "no mem for st7102 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        st7102->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        st7102->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        st7102->colmod_val = 0x55;
        break;
    case 18: // RGB666
        st7102->colmod_val = 0x66;
        break;
    case 24: // RGB888
        st7102->colmod_val = 0x77;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    uint8_t ID[3];
    ESP_GOTO_ON_ERROR(esp_lcd_panel_io_rx_param(io, 0x04, ID, 3), err, TAG, "read ID failed");
    ESP_LOGI(TAG, "LCD ID: %02X %02X %02X", ID[0], ID[1], ID[2]);

    st7102->io = io;
    st7102->init_cmds = vendor_config->init_cmds;
    st7102->init_cmds_size = vendor_config->init_cmds_size;
    st7102->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7102->flags.reset_level = panel_dev_config->flags.reset_active_high;

    // Create MIPI DPI panel
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus, vendor_config->mipi_config.dpi_config, &panel_handle), err, TAG,
                      "create MIPI DPI panel failed");
    ESP_LOGD(TAG, "new MIPI DPI panel @%p", panel_handle);

    // Save the original functions of MIPI DPI panel
    st7102->del = panel_handle->del;
    st7102->init = panel_handle->init;
    // Overwrite the functions of MIPI DPI panel
    panel_handle->del = panel_st7102_del;
    panel_handle->init = panel_st7102_init;
    panel_handle->reset = panel_st7102_reset;
    panel_handle->mirror = panel_st7102_mirror;
    panel_handle->invert_color = panel_st7102_invert_color;
    panel_handle->disp_on_off = panel_st7102_disp_on_off;
    panel_handle->user_data = st7102;
    *ret_panel = panel_handle;
    ESP_LOGD(TAG, "new st7102 panel @%p", st7102);

    return ESP_OK;

err:
    if (st7102) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st7102);
    }
    return ret;
}

static const st7102_lcd_init_cmd_t vendor_specific_init_default[] = {
//  {cmd, { data }, data_size, delay_ms}
    {0x99, (uint8_t []){0x71,0x02,0xa2}, 3, 0},
    {0x99, (uint8_t []){0x71,0x02,0xa3}, 3, 0},
    {0x99, (uint8_t []){0x71,0x02,0xa4}, 3, 0},
   	{0xA4, (uint8_t []){0x31}, 1, 0},
    {0xB0, (uint8_t []){0x22,0x61,0x1E,0x61,0x2F,0x39,0x39}, 7, 0},
    {0xB7, (uint8_t []){0x46,0x46}, 2, 0},
    {0xBF, (uint8_t []){0x50,0x50}, 2, 0},
    {0xD7, (uint8_t []){0x00,0x10,0x8C,0x08,0xF0,0xF0}, 6, 0},
    {0xA3, (uint8_t []){0x40,0x03,0x8C,0x40,0x45,0x00,0x00,0x00,0x00,0x00,
                        0x1E,0x01,0x00,0x12,0x00,0x45,0x05,0x00,0x00,0x00,
                        0x00,0x1E,0x01,0x00,0x12,0x20,0x52,0x00,0x05,0x00,
                        0x00,0xFF}, 32, 0},
    {0xA6, (uint8_t []){0x08,0x00,0x24,0x55,0x35,0x00,0x76,0x40,0x4E,0x4E,
                        0x00,0x24,0x55,0x00,0x00,0x40,0x40,0x4E,0x4E,0x02,
                        0xAC,0x51,0x00,0xCC,0x40,0x40,0x4E,0x4E,0x00,0xAC,
                        0x11,0x00,0x00,0x40,0x40,0x4E,0x4E,0x00,0x00,0x06,
                        0x00,0x00,0x00,0x00}, 44, 0},
    {0xA7, (uint8_t []){0x19,0x19,0x00,0x64,0x40,0x07,0x16,0x40,0x00,0x44,
                        0x43,0x4E,0x4E,0x00,0x64,0x40,0x25,0x34,0x00,0x00,
                        0x42,0x41,0x4E,0x4E,0x00,0x64,0x40,0x4B,0x5A,0x00,
                        0x00,0x42,0x41,0x4E,0x4E,0x00,0x24,0x40,0x69,0x78,
                        0x00,0x00,0x40,0x40,0x4E,0x4E,0x00,0x44}, 48, 0},
    {0xAC, (uint8_t []){0x00,0x1C,0x04,0x1A,0x19,0x1B,0x1B,0x18,0x06,0x13,
                        0x19,0x11,0x1B,0x08,0x18,0x0A,0x01,0x1C,0x04,0x1A,
                        0x19,0x1B,0x1B,0x18,0x06,0x12,0x19,0x10,0x1B,0x09,
                        0x18,0x0B,0xBF,0xAA,0xBF,0xAA,0x00}, 37, 0},
    {0xAD, (uint8_t []){0xCC,0x40,0x46,0x11,0x04,0x6F,0x6F}, 7, 0}, 
    {0xE8, (uint8_t []){0x30,0x07,0x05,0x6A,0x6A,0x9C,0x00,0xE2,0x04,0x00,
                        0x00,0x00,0x00,0xEF}, 14, 0},
    {0x75, (uint8_t []){0x03,0x04}, 2, 0},
    {0xE7, (uint8_t []){0x8B,0x3C,0x00,0x0C,0xF0,0x5D,0x00,0x5D,0x00,0x5D,
                        0x00,0x5D,0x00,0xFF,0x00,0x08,0x7B,0x00,0x00,0xC8,
                        0x6A,0x5A,0x08,0x1A,0x3C,0x00,0xA1,0x01,0x8C,0x01,
                        0x7F,0xF0,0x22}, 33, 0},
    {0xE9, (uint8_t []){0x3C,0x7F,0x08,0x10,0x1A,0x7A,0x22,0x1A,0x33}, 9, 0},
    {0xC8, (uint8_t []){0x00,0x00,0x15,0x26,0x44,0x00,0x78,0x03,0xBE,0x06,
                        0x11,0x1C,0x09,0x8A,0x03,0x21,0xD4,0x01,0x11,0x0F,
                        0x22,0x4A,0x0F,0x8F,0x0A,0x32,0xF0,0x0A,0x41,0x0D,
                        0xF3,0x80,0x0D,0xAE,0xC5,0x03,0xC4}, 37, 0},
    {0xC9, (uint8_t []){0x00,0x00,0x15,0x26,0x44,0x00,0x78,0x03,0xBE,0x06,
                        0x11,0x1C,0x09,0x8A,0x03,0x21,0xD4,0x01,0x11,0x0F,
                        0x22,0x4A,0x0F,0x8F,0x0A,0x32,0xF0,0x0A,0x41,0x0D,
                        0xF3,0x80,0x0D,0xAE,0xC5,0x03,0xC4}, 37, 0},
    {0x99, (uint8_t []){0x71,0x02,0x00}, 3, 0},  
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x29, (uint8_t []){0x00}, 0, 20},
	{0x35, (uint8_t []){0x00}, 1, 0},
    {0x36, (uint8_t []){0x00}, 1, 0},
};

static esp_err_t panel_st7102_del(esp_lcd_panel_t *panel)
{
    st7102_panel_t *st7102 = (st7102_panel_t *)panel->user_data;

    if (st7102->reset_gpio_num >= 0) {
        gpio_reset_pin(st7102->reset_gpio_num);
    }
    // Delete MIPI DPI panel
    st7102->del(panel);
    ESP_LOGD(TAG, "del st7102 panel @%p", st7102);
    free(st7102);

    return ESP_OK;
}

static esp_err_t panel_st7102_init(esp_lcd_panel_t *panel)
{
    st7102_panel_t *st7102 = (st7102_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7102->io;
    const st7102_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_command1_enable = true;
    bool is_cmd_overwritten = false;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7102_PAGE_CMD1, (uint8_t []) {
        0x00
    }, 1), TAG, "Write cmd failed");
    // Set color format
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t []) {
        st7102->madctl_val
    }, 1), TAG, "Write cmd failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t []) {
        st7102->colmod_val
    }, 1), TAG, "Write cmd failed");

    // vendor specific initialization, it can be different between manufacturers
    // should consult the LCD supplier for initialization sequence code
    if (st7102->init_cmds) {
        init_cmds = st7102->init_cmds;
        init_cmds_size = st7102->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(st7102_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal
        if (is_command1_enable && (init_cmds[i].data_bytes > 0)) {
            switch (init_cmds[i].cmd) {
            case LCD_CMD_MADCTL:
                is_cmd_overwritten = true;
                st7102->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            case LCD_CMD_COLMOD:
                is_cmd_overwritten = true;
                st7102->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            default:
                is_cmd_overwritten = false;
                break;
            }

            if (is_cmd_overwritten) {
                is_cmd_overwritten = false;
                ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence",
                         init_cmds[i].cmd);
            }
        }

        // Send command
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));

        // Check if the current cmd is the command1 enable cmd
        if ((init_cmds[i].cmd == ST7102_PAGE_CMD2 || init_cmds[i].cmd == ST7102_PAGE_CMD3) && init_cmds[i].data_bytes > 0) {
            is_command1_enable = false;
        } else if (init_cmds[i].cmd == ST7102_PAGE_CMD1 && init_cmds[i].data_bytes > 0) {
            is_command1_enable = true;
        }
    }
    ESP_LOGD(TAG, "send init commands success");

    ESP_RETURN_ON_ERROR(st7102->init(panel), TAG, "init MIPI DPI panel failed");

    return ESP_OK;
}

static esp_err_t panel_st7102_reset(esp_lcd_panel_t *panel)
{
    st7102_panel_t *st7102 = (st7102_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7102->io;

    // Perform hardware reset
    if (st7102->reset_gpio_num >= 0) {
        gpio_set_level(st7102->reset_gpio_num, st7102->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st7102->reset_gpio_num, !st7102->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else if (io) { // Perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_st7102_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7102_panel_t *st7102 = (st7102_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7102->io;
    uint8_t command = 0;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");

    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");

    return ESP_OK;
}

static esp_err_t panel_st7102_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7102_panel_t *st7102 = (st7102_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7102->io;
    uint8_t madctl_val = st7102->madctl_val;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");

    // Control mirror through LCD command
    if (mirror_x) {
        madctl_val |= BIT(6);
    } else {
        madctl_val &= ~BIT(6);
    }
    if (mirror_y) {
        madctl_val |= BIT(7);
    } else {
        madctl_val &= ~BIT(7);
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t []) {
        madctl_val
    }, 1), TAG, "send command failed");
    st7102->madctl_val = madctl_val;

    return ESP_OK;
}

static esp_err_t panel_st7102_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7102_panel_t *st7102 = (st7102_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7102->io;
    int command = 0;

    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
#endif
