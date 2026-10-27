# Test firmware: runs the cases sent by the runner on the PC, see docs/architecture.md
idf_build_get_property(board GBX_BOARD)
idf_build_get_property(profiler GBX_PROFILER)
set(srcs "device.cpp" "transport.cpp" "rom_test.cpp" "mapped_blocks.cpp" "mapper_dma.cpp" "cpu_peripherals.cpp"
         "sm83_runner.cpp" "sm83_blocks.cpp" "test_data.cpp" "jit_test.cpp" "jit_abi.S" "rv32_extensions.S" "../protocol/frame.cpp"
         "../display/backend.cpp" "../audio/mixer.cpp")
# The GBA's tests, in a firmware with the GBA, see components/core/Kconfig.
if(NOT CONFIG_GBX_SYSTEMS_GB_ONLY)
    list(APPEND srcs "gba_rom_test.cpp" "arm7_runner.cpp" "arm7_blocks.cpp" "gba_blocks.cpp")
endif()
set(requires core jit platform board "${board}" esp_app_format esp_hw_support esp_mm
             esp_driver_uart esp_driver_usb_serial_jtag esp_timer mbedtls)
set(preset "${board}-test-${GBX_BUILD_TYPE}")
if(profiler)
    list(APPEND srcs "profiler.cpp")
    list(APPEND requires esp_driver_gptimer)
    set(preset "${board}-test-profile")
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "../protocol"
    PRIV_INCLUDE_DIRS "." "../../components/platform/src/esp"
    PRIV_REQUIRES ${requires})
target_compile_definitions(${COMPONENT_LIB} PRIVATE GBX_TEST_PRESET="${preset}")

# Re-evaluate on every build so a source edit cannot retain a stale baseline ID.
# The generator preserves the header timestamp when the identity is unchanged.
set(reference_header "${CMAKE_CURRENT_BINARY_DIR}/reference_identity.h")
add_custom_target(gbx_reference_identity
    COMMAND "${PYTHON}" "${PROJECT_DIR}/tools/reference_identity.py"
        --root "${PROJECT_DIR}" --board "${board}" --sdkconfig "${SDKCONFIG}"
        --lock "${CMAKE_BINARY_DIR}/dependencies.lock" --compiler "${CMAKE_CXX_COMPILER}"
        --idf "$ENV{IDF_PATH}" --commands "${CMAKE_BINARY_DIR}/compile_commands.json"
        --output "${reference_header}"
    BYPRODUCTS "${reference_header}" "${CMAKE_CURRENT_BINARY_DIR}/reference_identity.json"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} gbx_reference_identity)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
