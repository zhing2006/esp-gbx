# Menu and game flow, shared by the firmware and the PC build: the menus, the
# game, cartridge saves, the ROM library, the settings file and the menus'
# sound cues.
set(app_srcs "src/app.cpp" "src/game.cpp" "src/menus.cpp" "src/saves.cpp" "src/library.cpp" "src/rom_info.cpp"
    "src/settings.cpp" "src/faults.cpp" "src/sounds.cpp" "src/slots.cpp" "src/color_fix.cpp" "src/sha256.cpp")
if(ESP_PLATFORM)
    # Key recordings exist only in profiling builds, see src/replay.h.
    idf_build_get_property(profiler GBX_PROFILER)
    if(profiler)
        list(APPEND app_srcs "src/replay.cpp")
    endif()
    idf_component_register(
        SRCS ${app_srcs}
        INCLUDE_DIRS "include"
        REQUIRES platform ui core
        LDFRAGMENTS "linker.lf")
else()
    add_library(gbx_app STATIC ${app_srcs})
    target_include_directories(gbx_app PUBLIC include)
    target_link_libraries(gbx_app PUBLIC gbx_platform gbx_ui gbx_core)
endif()
