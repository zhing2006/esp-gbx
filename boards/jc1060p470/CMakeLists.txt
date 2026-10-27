# GUITION JC1060P470, see docs/boards/jc1060p470.md
idf_component_register(
    SRCS "board.cpp"
    REQUIRES board
    PRIV_REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio)
