# Waveshare ESP32-C5-Touch-LCD-2.8, see docs/boards/waveshare_c5_lcd28.md
idf_component_register(
    SRCS "board.cpp" "cst3530_touch.cpp"
    REQUIRES board
    # Load PSRAM configuration and startup in the test firmware as well.
    PRIV_REQUIRES esp_psram esp_lcd esp_driver_i2c esp_driver_gpio esp_driver_spi esp_timer freertos)
