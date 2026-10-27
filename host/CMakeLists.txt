cmake_minimum_required(VERSION 3.22)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()

# PC build: the interpreter, the app and the host platform, for development and
# debugging. Configure with a preset from CMakePresets.json in this directory.
project(esp-gbx-host LANGUAGES C CXX)

option(GBX_HOST_GUI "Build the SDL3/LVGL desktop UI" ON)
option(GBX_GBA_RELAXED_TIMING "GBA: ROM fetches ignore the prefetch buffer; the JIT's wait loops skip whole passes" ON)
if(GBX_GBA_RELAXED_TIMING)
    # The PC runs no JIT; the compiler's tools see the firmware's code.
    add_compile_definitions(GBX_GBA_RELAXED_TIMING)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(GBX_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
if(GBX_HOST_GUI)
    include(cmake/Gui.cmake)
endif()
if(MSVC)
    # Remove CMake's default exception flag after configuring third-party code.
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()
add_subdirectory("${GBX_ROOT}/components/platform" platform)
add_subdirectory("${GBX_ROOT}/components/ui" ui)
add_subdirectory("${GBX_ROOT}/components/jit" jit)
add_subdirectory("${GBX_ROOT}/components/core" core)
add_subdirectory("${GBX_ROOT}/components/app" app)

add_executable(gbx main.cpp)
target_link_libraries(gbx PRIVATE gbx_app)
if(GBX_HOST_GUI)
    target_link_libraries(gbx PRIVATE SDL3::SDL3)
    add_dependencies(gbx gbx_assets)
    if(WIN32)
        # CommandLineToArgvW() for the ROM's path, see main.cpp.
        target_link_libraries(gbx PRIVATE shell32)
    endif()
endif()

include(CTest)
set(gbx_targets gbx gbx_app gbx_ui gbx_platform gbx_core gbx_jit)
if(BUILD_TESTING)
    # Outputs encoder cases for comparison with the external RISC-V assembler.
    # It never executes generated RV32 instructions on the host.
    add_executable(gbx_rv32_encoding ../tests/jit/encoding.cpp)
    target_link_libraries(gbx_rv32_encoding PRIVATE gbx_jit)
    list(APPEND gbx_targets gbx_rv32_encoding)
    # Frame hashes of a GBA game run from an instant save, for comparing
    # renderer changes, see tools/gba_frames.cpp.
    add_executable(gbx_gba_frames tools/gba_frames.cpp)
    target_link_libraries(gbx_gba_frames PRIVATE gbx_core)
    list(APPEND gbx_targets gbx_gba_frames)
    # The size of the ARM7 JIT's code for a game, see tools/jit_size.cpp.
    add_executable(gbx_jit_size tools/jit_size.cpp)
    target_include_directories(gbx_jit_size PRIVATE "${GBX_ROOT}/components/core/src")
    target_link_libraries(gbx_jit_size PRIVATE gbx_core)
    list(APPEND gbx_targets gbx_jit_size)
    # The control flow a game runs, see tools/gba_flow.cpp.
    add_executable(gbx_gba_flow tools/gba_flow.cpp)
    target_link_libraries(gbx_gba_flow PRIVATE gbx_core)
    list(APPEND gbx_targets gbx_gba_flow)
    # Key scripts that reach a scene from power-on, see tools/gba_script.cpp.
    add_executable(gbx_gba_script tools/gba_script.cpp)
    target_link_libraries(gbx_gba_script PRIVATE gbx_core)
    list(APPEND gbx_targets gbx_gba_script)
    add_executable(gbx_settings_test tests/settings.cpp)
    target_link_libraries(gbx_settings_test PRIVATE gbx_app)
    list(APPEND gbx_targets gbx_settings_test)
    add_test(NAME host_settings COMMAND gbx_settings_test)
endif()
if(BUILD_TESTING AND GBX_HOST_GUI)
    add_executable(gbx_host_test tests/gui.cpp)
    target_link_libraries(gbx_host_test PRIVATE gbx_app SDL3::SDL3)
    add_dependencies(gbx_host_test gbx_assets)
    list(APPEND gbx_targets gbx_host_test)
    add_test(NAME host_gui COMMAND gbx_host_test)
    set_tests_properties(host_gui PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIO_DRIVER=dummy"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        TIMEOUT 30)
elseif(BUILD_TESTING)
    add_test(NAME host_headless COMMAND gbx)
    set_tests_properties(host_headless PROPERTIES PASS_REGULAR_EXPRESSION "this build has no UI")
endif()

# Do not impose project warnings or C++ flags on third-party libraries.
foreach(target IN LISTS gbx_targets)
    if(MSVC)
        # C5030: the gnu:: attributes, such as noinline, place the firmware's
        # hot code; MSVC does without them.
        target_compile_options(${target} PRIVATE /W4 /wd5030 /EHs-c- /GR- /utf-8)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _HAS_EXCEPTIONS=0)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
    endif()
endforeach()
