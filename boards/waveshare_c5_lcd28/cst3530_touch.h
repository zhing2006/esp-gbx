#pragma once

#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"

namespace gbx::board {

// The CST3530 touch controller at 0x58 on `bus`, released from reset by the
// caller. It reports panel coordinates, 240 x 320. The platform polls it, so
// its interrupt line is not used.
esp_err_t new_cst3530_touch(i2c_master_bus_handle_t bus, esp_lcd_touch_handle_t* out);

}  // namespace gbx::board
