# Espressif ESP32-S31-Korvo-1 V1.1 with the ESP32-S3-LCD-EV-Board-SUB3
# screen, see docs/boards/esp32_s31_korvo_1.md
idf_component_register(
    SRCS "board.cpp"
    REQUIRES board
    # Load PSRAM configuration and startup in the test firmware as well.
    PRIV_REQUIRES esp_psram esp_lcd esp_driver_i2c esp_driver_gpio freertos)
