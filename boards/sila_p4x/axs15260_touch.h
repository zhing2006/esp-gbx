#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"

namespace gbx::board {

// The touch half of the AXS15260D display controller, at 0x3B on `bus`. It
// reports panel coordinates, 452 x 1280, and pulls `interrupt` low when it
// has a new report. Create it once the display runs; it never drives the
// reset line it shares with the display.
esp_err_t new_axs15260_touch(i2c_master_bus_handle_t bus, gpio_num_t interrupt, esp_lcd_touch_handle_t* out);

}  // namespace gbx::board
