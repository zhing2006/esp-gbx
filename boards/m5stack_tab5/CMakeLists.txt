# M5Stack Tab5, see docs/boards/m5stack_tab5.md
idf_component_register(
    SRCS "board.cpp"
    REQUIRES board
    PRIV_REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio freertos)
