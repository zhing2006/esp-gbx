# Firmware entry point and the serial console; profiling builds add their
# commands, see docs/debug-design.md
idf_build_get_property(board GBX_BOARD)
idf_build_get_property(profiler GBX_PROFILER)
set(srcs "main.cpp" "console.cpp")
set(requires app board "${board}" esp_hw_support esp_psram esp_driver_uart esp_driver_usb_serial_jtag)
if(profiler)
    list(APPEND srcs "profile.cpp")
    list(APPEND requires esp_timer esp_driver_gptimer core)
endif()
idf_component_register(
    SRCS ${srcs}
    PRIV_REQUIRES ${requires})
