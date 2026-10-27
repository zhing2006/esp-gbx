# Emulation code shared by the firmware and the interpreter-only PC build.
set(core_srcs "src/common/scheduler.cpp" "src/gb/sm83.cpp" "src/gb/bus.cpp"
              "src/gb/ppu.cpp" "src/gb/memory.cpp" "src/gb/cartridge.cpp" "src/gb/system.cpp"
              "src/gb/jit/sm83_jit.cpp" "src/gb/jit/sm83_jit_runtime.cpp"
              "src/gb/apu.cpp" "src/gb/rtc.cpp" "src/gb/mixer.cpp" "src/gb/offload.cpp"
              "src/gb/snapshot.cpp" "src/gb/compatibility.cpp"
              "src/gba/arm7.cpp" "src/gba/arm7_step.cpp" "src/gba/bus.cpp"
              "src/gba/arm7_mapped.cpp" "src/gba/gba_scheduler.cpp" "src/gba/gba_memory.cpp"
              "src/gba/gba_io.cpp" "src/gba/gba_irq.cpp" "src/gba/gba_timer.cpp" "src/gba/gba_dma.cpp" "src/gba/gba_ppu.cpp"
              "src/gba/gba_sound.cpp" "src/gba/gba_mixer.cpp" "src/gba/gba_rtc.cpp" "src/gba/gba_snapshot.cpp"
              "src/gba/gba_renderer.cpp" "src/gba/gba_system.cpp" "src/gba/rom_cache.cpp" "src/gba/backup.cpp"
              "src/gba/jit/arm7_jit.cpp" "src/gba/jit/arm7_jit_runtime.cpp" "src/gba/jit/arm7_gateway.cpp"
              "src/gba/jit/arm7_compiler.cpp" "src/gba/jit/arm7_compiler_flags.cpp"
              "src/gba/jit/arm7_compiler_ops.cpp" "src/gba/jit/arm7_compiler_arm.cpp"
              "src/gba/jit/arm7_compiler_thumb.cpp" "src/gba/jit/arm7_compiler_mapped.cpp")
if(ESP_PLATFORM)
    idf_component_register(SRCS ${core_srcs} INCLUDE_DIRS "include" REQUIRES jit platform
                           LDFRAGMENTS "linker.lf")
else()
    add_library(gbx_core STATIC ${core_srcs})
    target_include_directories(gbx_core PUBLIC include)
    target_link_libraries(gbx_core PUBLIC gbx_jit)
endif()
