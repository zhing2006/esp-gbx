include(FetchContent)

# Keep downloads reproducible and separate from ESP-IDF's managed_components.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    URL https://www.libsdl.org/release/SDL3-3.4.18.tar.gz
    URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)

set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/../lv_conf.h" CACHE PATH "" FORCE)
set(LV_BUILD_USE_KCONFIG OFF CACHE BOOL "" FORCE)
set(LV_BUILD_SET_CONFIG_OPTS OFF CACHE BOOL "" FORCE)
set(LV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LV_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lvgl
    URL https://github.com/lvgl/lvgl/archive/refs/tags/v9.6.0.tar.gz
    URL_HASH SHA256=b20ee3acc1bba13c62d854f9ebd62e4c51e0b443b1e0225892e86442defa84df)
FetchContent_MakeAvailable(SDL3 lvgl)
if(MSVC)
    # LVGL combines part and state enums in its style selectors.
    target_compile_options(lvgl PRIVATE /wd5287)
endif()

find_program(GBX_UV uv REQUIRED)
set(assets_bin "${CMAKE_BINARY_DIR}/assets.bin")
file(GLOB_RECURSE assets_inputs CONFIGURE_DEPENDS
    "${GBX_ROOT}/assets/ui/common/*"
    "${GBX_ROOT}/assets/ui/m5stack_tab5/*"
    "${GBX_ROOT}/tools/ui/*.py")
add_custom_command(OUTPUT "${assets_bin}"
    COMMAND "${GBX_UV}" run --quiet "${GBX_ROOT}/tools/ui/pack_assets.py"
            --board m5stack_tab5 --output "${assets_bin}"
    DEPENDS ${assets_inputs}
    WORKING_DIRECTORY "${GBX_ROOT}"
    COMMENT "Packing UI assets for m5stack_tab5"
    VERBATIM)
# Copy even when only the assets changed, including multi-config generators.
add_custom_target(gbx_assets ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${assets_bin}" "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/assets.bin"
    DEPENDS "${assets_bin}"
    VERBATIM)
