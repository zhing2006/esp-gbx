# M5Stack Tab5, see docs/boards/m5stack_tab5.md
idf_component_register(
    SRCS "board.cpp"
    REQUIRES board
    PRIV_REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio freertos)
# The UI layout the asset pack is built from, see Kconfig
if(CONFIG_GBX_TAB5_X3_LAYOUT)
    idf_build_set_property(GBX_UI_LAYOUT m5stack_tab5_x3)
endif()
