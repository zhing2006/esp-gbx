#ifndef LV_CONF_H
#define LV_CONF_H

// Match the firmware's raw image formats and keep all LVGL work on the main thread.
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_DEF_REFR_PERIOD 16
#define LV_DRAW_BUF_ALIGN 64
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_SUPPORT_RGB565 1
#define LV_DRAW_SW_SUPPORT_RGB565A8 1
#define LV_USE_IMAGE 1
#define LV_USE_CANVAS 1

// The platform uses SDL3 directly; LVGL's built-in SDL driver uses SDL2.
#define LV_USE_SDL 0
#define LV_USE_DRAW_SDL 0
#define LV_USE_THORVG 0
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0

#endif  // LV_CONF_H
