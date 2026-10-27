# Sila P4, see docs/boards/sila_p4.md
idf_component_register(
    SRCS "board.cpp"
    REQUIRES board
    # Load PSRAM configuration and startup in the test firmware as well.
    PRIV_REQUIRES esp_psram esp_lcd esp_driver_i2c esp_driver_gpio)
