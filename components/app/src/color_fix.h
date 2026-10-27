#pragma once

#include <cstdint>

namespace gbx::app {

// COLOR FIX, the LCD color correction of the games in color: the colors of
// a GBC's screen, from measurements of one, see tools/lcd_colors.py. The
// table has 32768 RGB565 colors indexed by BGR555, as a game sets them, for
// platform::display::set_color_table(). It is built by the first call, on
// the app's task at start, in external memory; nullptr without memory.
// Any task after that.
const uint16_t* color_fix_table();
// A BGR555 color through the table, as BGR555, for the menus' frames.
uint16_t color_fix(uint16_t color);

}  // namespace gbx::app
