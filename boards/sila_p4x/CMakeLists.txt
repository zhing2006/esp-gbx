# Sila P4X, see docs/boards/sila_p4x.md
idf_component_register(
    SRCS "board.cpp" "axs15260_touch.cpp"
    REQUIRES board
    # Load PSRAM configuration and startup in the test firmware as well.
    PRIV_REQUIRES esp_psram esp_lcd esp_driver_i2c esp_driver_gpio esp_timer)
