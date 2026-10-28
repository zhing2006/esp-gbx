# Platform interface (include/) and its implementations: src/esp/ for the
# firmware, src/host/ for the PC build. The firmware side drives the panel and
# touch the board component sets up.
if(ESP_PLATFORM)
    set(display_sources "src/esp/display.cpp" "src/esp/display_pixels.cpp")
    if(CONFIG_GBX_DISPLAY_ACCEL)
        list(APPEND display_sources "src/esp/display_accel.cpp")
    else()
        list(APPEND display_sources "src/esp/display_cpu.cpp")
    endif()
    if(CONFIG_LV_USE_PPA)
        list(APPEND display_sources "src/esp/display_lvgl.cpp")
    endif()
    if(CONFIG_LV_USE_CUSTOM_MALLOC)
        list(APPEND display_sources "src/esp/lvgl_memory.cpp")
    endif()
    # The USB components come from idf_component.yml only with the option.
    set(input_sources "")
    if(CONFIG_GBX_USB_GAMEPAD)
        list(APPEND input_sources "src/esp/usb_gamepad.cpp")
    endif()
    idf_component_register(
        SRCS "src/esp/platform.cpp" ${display_sources} ${input_sources}
             "src/esp/assets.cpp" "src/esp/storage.cpp"
             "src/esp/files.cpp" "src/esp/audio.cpp" "src/esp/executable.cpp" "src/esp/worker.cpp"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "src/esp"
        # espcoredump loads the core dump Kconfig, so a crash is saved to the
        # coredump partition; reading it back at startup is still a TODO.
        PRIV_REQUIRES board esp_lcd esp_timer esp_partition espcoredump esp_driver_i2s
                      fatfs sdmmc esp_driver_sdmmc esp_driver_sdspi esp_hw_support esp_mm)
    # CONFIG values are unavailable during IDF's early dependency expansion.
    # The pinned LVGL port includes the target-supported drivers in the graph;
    # only the selected accelerated implementation links them from here.
    if(CONFIG_GBX_DISPLAY_ACCEL)
        idf_component_optional_requires(PRIVATE esp_driver_ppa esp_driver_dma)
    endif()
    idf_build_get_property(board GBX_BOARD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GBX_BOARD_ID="${board}")
    # LVGL, linked after this library, calls the allocator; nothing here does.
    if(CONFIG_LV_USE_CUSTOM_MALLOC)
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")
    endif()
else()
    find_package(Threads REQUIRED)
    add_library(gbx_platform STATIC src/host/platform.cpp src/host/executable.cpp src/host/worker.cpp
                src/host/files.cpp)
    target_link_libraries(gbx_platform PUBLIC Threads::Threads)
    target_include_directories(gbx_platform PUBLIC include)
    if(GBX_HOST_GUI)
        target_sources(gbx_platform PRIVATE src/host/display.cpp src/host/audio.cpp)
        target_link_libraries(gbx_platform PRIVATE SDL3::SDL3 lvgl::lvgl)
    else()
        target_sources(gbx_platform PRIVATE src/host/display_headless.cpp)
    endif()
endif()
