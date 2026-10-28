# Game page and menus: asset pack, touch controls, VFD text, the menu font and
# drawing, and the LVGL screens. Shared by the firmware and the PC build; depends only on the
# platform interface and LVGL. Headless builds exclude the LVGL screens.
set(ui_srcs "src/pack.cpp" "src/controls.cpp" "src/vfd.cpp" "src/menu_font.cpp" "src/menu_draw.cpp")
# GBX_UI_STATIC_SHELL, see Kconfig; the PC build draws the presses.
# GBX_UI_GBA_FRAMES: the menus hold frames up to the GBA's 240x160, or only
# the GB's 160x144 in a build without the GBA, see components/core/Kconfig.
if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${ui_srcs} "src/game_page.cpp" "src/menu_view.cpp"
        INCLUDE_DIRS "include"
        REQUIRES platform)
    if(CONFIG_GBX_UI_STATIC_SHELL)
        set(static_shell 1)
    else()
        set(static_shell 0)
    endif()
    if(CONFIG_GBX_SYSTEMS_GB_ONLY)
        set(gba_frames 0)
    else()
        set(gba_frames 1)
    endif()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC GBX_UI_LVGL=1 GBX_UI_STATIC_SHELL=${static_shell}
                                                       GBX_UI_GBA_FRAMES=${gba_frames})
else()
    add_library(gbx_ui STATIC ${ui_srcs})
    target_include_directories(gbx_ui PUBLIC include)
    target_link_libraries(gbx_ui PUBLIC gbx_platform)
    target_compile_definitions(gbx_ui PUBLIC GBX_UI_STATIC_SHELL=0 GBX_UI_GBA_FRAMES=1)
    if(GBX_HOST_GUI)
        target_sources(gbx_ui PRIVATE src/game_page.cpp src/menu_view.cpp)
        target_link_libraries(gbx_ui PUBLIC lvgl::lvgl)
        target_compile_definitions(gbx_ui PUBLIC GBX_UI_LVGL=1)
    else()
        target_compile_definitions(gbx_ui PUBLIC GBX_UI_LVGL=0)
    endif()
endif()
