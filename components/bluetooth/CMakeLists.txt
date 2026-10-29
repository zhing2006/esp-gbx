# What the Bluetooth gamepad (GBX_BT_GAMEPAD, components/platform/src/esp/bt/)
# builds with: NimBLE, the LP core's toolchain, NVS and, through
# idf_component.yml, ESP-Hosted. platform takes this component through its
# own idf_component.yml only with the option, because IDF expands component
# requirements before it knows CONFIG values. Boards without the option build
# none of them.
idf_component_register(REQUIRES bt ulp nvs_flash)
