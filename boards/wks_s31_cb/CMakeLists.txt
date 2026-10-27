# Waiken-Smart ESP32-S31 CB V1.1 with the WKS43WV067-WCT screen, see
# docs/boards/wks_s31_cb.md
idf_component_register(
    SRCS "board.cpp"
    REQUIRES board
    # Load PSRAM configuration and startup in the test firmware as well.
    PRIV_REQUIRES esp_psram esp_lcd esp_driver_i2c esp_driver_gpio freertos)
