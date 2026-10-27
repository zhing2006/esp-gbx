# OSPTEK ESP32-P4C5-Module, see docs/boards/osptek_p4c5.md
idf_component_register(
    SRCS "board.cpp"
    REQUIRES board
    # Load PSRAM configuration and startup in the test firmware as well.
    PRIV_REQUIRES esp_psram esp_lcd esp_driver_i2c esp_driver_gpio freertos)
