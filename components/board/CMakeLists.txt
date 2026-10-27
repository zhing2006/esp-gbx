# Board interface that every board under boards/ implements, and the steps the
# boards share. Firmware only; the PC build has no boards.
idf_component_register(
    SRCS "src/board_support.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_i2s
    PRIV_REQUIRES esp_hw_support esp_driver_ledc)
