# Game page and menus: asset pack, touch controls, VFD text, the menu font and
# drawing, and the LVGL screens. Shared by the firmware and the PC build; depends only on the
# platform interface and LVGL. Headless builds exclude the LVGL screens.
set(ui_srcs "src/pack.cpp" "src/controls.cpp" "src/vfd.cpp" "src/menu_font.cpp" "src/menu_draw.cpp")
if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${ui_srcs} "src/game_page.cpp" "src/menu_view.cpp"
        INCLUDE_DIRS "include"
        REQUIRES platform)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC GBX_UI_LVGL=1)
else()
    add_library(gbx_ui STATIC ${ui_srcs})
    target_include_directories(gbx_ui PUBLIC include)
    target_link_libraries(gbx_ui PUBLIC gbx_platform)
    if(GBX_HOST_GUI)
        target_sources(gbx_ui PRIVATE src/game_page.cpp src/menu_view.cpp)
        target_link_libraries(gbx_ui PUBLIC lvgl::lvgl)
        target_compile_definitions(gbx_ui PUBLIC GBX_UI_LVGL=1)
    else()
        target_compile_definitions(gbx_ui PUBLIC GBX_UI_LVGL=0)
    endif()
endif()
