cmake_minimum_required(VERSION 3.22)

# ESP-IDF project. Pick a configuration with a preset from CMakePresets.json:
#   idf.py --preset jc1060p470-debug build
# The PC build is a separate CMake project in host/.

set(GBX_BOARD "" CACHE STRING "Board directory under boards/")
set(GBX_BUILD_TYPE "" CACHE STRING "Build type: debug or release")
set(GBX_APP "main" CACHE STRING "Application: main (firmware) or device (test firmware)")
option(GBX_PROFILER "Sampling profiler and counters in a release firmware or test firmware" OFF)
# The GBA JIT's accuracy concession, on in the firmware that plays games.
# The test firmware judges the tests without it, see docs/roadmap.md,
# milestone 6.
if(GBX_APP STREQUAL "device")
    set(gbx_relaxed_default OFF)
else()
    set(gbx_relaxed_default ON)
endif()
option(GBX_GBA_RELAXED_TIMING "GBA: ROM fetches ignore the prefetch buffer; the JIT's wait loops skip whole passes"
       ${gbx_relaxed_default})

if(NOT GBX_BOARD OR NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${GBX_BOARD}")
    message(FATAL_ERROR "Unknown board '${GBX_BOARD}'. Use a preset: idf.py --preset <board>-<debug|release> build")
endif()
if(NOT GBX_BUILD_TYPE MATCHES "^(debug|release)$")
    message(FATAL_ERROR "GBX_BUILD_TYPE must be debug or release, got '${GBX_BUILD_TYPE}'")
endif()
if(NOT GBX_APP MATCHES "^(main|device)$")
    message(FATAL_ERROR "GBX_APP must be main or device, got '${GBX_APP}'")
endif()
if(GBX_PROFILER AND NOT GBX_BUILD_TYPE STREQUAL "release")
    message(FATAL_ERROR "GBX_PROFILER needs a release build, use a <board>-profile or <board>-test-profile preset")
endif()

# Each preset keeps its generated sdkconfig in its own build directory.
# IDF loads sdkconfig.defaults.<target> after the common defaults, so SoC
# policy is shared by every board. Build type, profiling and board defaults
# follow.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.${GBX_BUILD_TYPE}")
if(GBX_PROFILER)
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.profile")
endif()
list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/boards/${GBX_BOARD}/sdkconfig.defaults")

# ESP-IDF finds main/ and the components in components/ by itself
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/boards/${GBX_BOARD}"
    "${CMAKE_CURRENT_LIST_DIR}/tests/device")

# Build only the selected application and the components it requires
set(COMPONENTS ${GBX_APP})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Applications require the board component through this property
idf_build_set_property(GBX_BOARD "${GBX_BOARD}")
# Profiling builds compile the profiler and the JIT counters into every
# component; other builds contain none of their code.
idf_build_set_property(GBX_PROFILER "${GBX_PROFILER}")
if(GBX_PROFILER)
    idf_build_set_property(COMPILE_DEFINITIONS "GBX_PROFILER" APPEND)
endif()
if(GBX_GBA_RELAXED_TIMING)
    idf_build_set_property(COMPILE_DEFINITIONS "GBX_GBA_RELAXED_TIMING" APPEND)
endif()

# Each preset pulls in different components, so each keeps its own lock file.
# Direct dependencies are pinned with == in the components' idf_component.yml.
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_BINARY_DIR}/dependencies.lock")

project(esp-gbx)

include(cmake/core_layout.cmake)

# LVGL 9.6.0's PPA draw unit sets an esp_err_t it never reads
# (src/draw/espressif/ppa/lv_draw_ppa.c). The warning is upstream's.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-unused-but-set-variable)

# The UI asset pack for the assets partition, built from assets/ui by
# tools/ui/pack_assets.py. `idf.py assets` builds it, `idf.py assets-flash`
# writes only this partition and `idf.py flash` includes it. The test firmware
# has no UI and leaves the partition alone.
if(GBX_APP STREQUAL "main")
    find_program(GBX_UV uv)
    if(NOT GBX_UV)
        message(FATAL_ERROR "uv is required to build the UI asset pack, see https://docs.astral.sh/uv/")
    endif()
    partition_table_get_partition_info(assets_size "--partition-name assets" "size")
    set(assets_bin "${CMAKE_BINARY_DIR}/assets.bin")
    file(GLOB_RECURSE assets_inputs CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/assets/ui/common/*"
        "${CMAKE_CURRENT_LIST_DIR}/assets/ui/${GBX_BOARD}/*"
        "${CMAKE_CURRENT_LIST_DIR}/tools/ui/*.py")
    add_custom_command(OUTPUT "${assets_bin}"
        COMMAND "${GBX_UV}" run --quiet "${CMAKE_CURRENT_LIST_DIR}/tools/ui/pack_assets.py"
                --board "${GBX_BOARD}" --output "${assets_bin}" --limit "${assets_size}"
        DEPENDS ${assets_inputs}
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        COMMENT "Packing UI assets for ${GBX_BOARD}"
        VERBATIM)
    add_custom_target(assets ALL DEPENDS "${assets_bin}")
    esp_partition_register_target(assets "${assets_bin}" ALWAYS_PLAINTEXT DEPENDS assets FLASH_IN_PROJECT)
endif()

# Our components build as gnu++23 like the PC build; ESP-IDF defaults to gnu++26
idf_build_get_property(build_components BUILD_COMPONENTS)
foreach(component app ui core jit platform board main device "${GBX_BOARD}")
    if(component IN_LIST build_components)
        idf_component_get_property(lib "${component}" COMPONENT_LIB)
        target_compile_options("${lib}" PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>")
    endif()
endforeach()
