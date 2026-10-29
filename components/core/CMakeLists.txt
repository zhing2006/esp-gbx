# Emulation code shared by the firmware and the interpreter-only PC build.
set(core_srcs "src/common/scheduler.cpp" "src/gb/sm83.cpp" "src/gb/bus.cpp"
              "src/gb/ppu.cpp" "src/gb/memory.cpp" "src/gb/cartridge.cpp" "src/gb/system.cpp"
              "src/gb/jit/sm83_jit.cpp" "src/gb/jit/sm83_jit_runtime.cpp"
              "src/gb/apu.cpp" "src/gb/rtc.cpp" "src/gb/mixer.cpp" "src/gb/offload.cpp"
              "src/gb/snapshot.cpp" "src/gb/compatibility.cpp")
set(core_gba_srcs "src/gba/arm7.cpp" "src/gba/arm7_step.cpp" "src/gba/bus.cpp"
                  "src/gba/arm7_mapped.cpp" "src/gba/gba_scheduler.cpp" "src/gba/gba_memory.cpp"
                  "src/gba/gba_io.cpp" "src/gba/gba_irq.cpp" "src/gba/gba_timer.cpp" "src/gba/gba_dma.cpp" "src/gba/gba_ppu.cpp"
                  "src/gba/gba_sound.cpp" "src/gba/gba_mixer.cpp" "src/gba/gba_rtc.cpp" "src/gba/gba_snapshot.cpp"
                  "src/gba/gba_renderer.cpp" "src/gba/gba_system.cpp" "src/gba/rom_cache.cpp" "src/gba/backup.cpp"
                  "src/gba/jit/arm7_jit.cpp" "src/gba/jit/arm7_jit_runtime.cpp" "src/gba/jit/arm7_gateway.cpp"
                  "src/gba/jit/arm7_compiler.cpp" "src/gba/jit/arm7_compiler_flags.cpp"
                  "src/gba/jit/arm7_compiler_ops.cpp" "src/gba/jit/arm7_compiler_arm.cpp"
                  "src/gba/jit/arm7_compiler_thumb.cpp" "src/gba/jit/arm7_compiler_mapped.cpp")
# The systems whose games the build runs, GBX_HAS_GB and GBX_HAS_GBA, see
# Kconfig and gbx/systems.h. The PC build runs both. Without the GBA its
# core and JIT are left out; without the GB its core stays, see Kconfig.
set(gbx_has_gb 1)
set(gbx_has_gba 1)
if(CONFIG_GBX_SYSTEMS_GB_ONLY)
    set(gbx_has_gba 0)
elseif(CONFIG_GBX_SYSTEMS_GBA_ONLY)
    set(gbx_has_gb 0)
endif()
if(gbx_has_gba)
    list(APPEND core_srcs ${core_gba_srcs})
endif()
# GBX_SINGLE_CORE: a chip with one core, where the emulation task does the
# PPU's and APU's work itself, see src/gb/offload.h. The PC build has a
# worker thread.
set(gbx_single_core 0)
if(CONFIG_FREERTOS_UNICORE)
    set(gbx_single_core 1)
endif()
# PSRAM budgets that each board sets for its PSRAM, see Kconfig: the GB
# JIT's second level and the PSRAM it leaves, and the GBA's ROM cache, in
# bytes. The PC build takes the defaults.
set(gbx_gb_jit_store_kb 8192)
set(gbx_gba_rom_cache_kb 14336)
set(gbx_gb_jit_store_leave_kb 0)
if(CONFIG_GBX_GB_JIT_STORE_LEAVE_KB)
    set(gbx_gb_jit_store_leave_kb ${CONFIG_GBX_GB_JIT_STORE_LEAVE_KB})
endif()
# The GBA JIT's second level and the PSRAM it leaves.
set(gbx_gba_jit_store_kb 4096)
set(gbx_gba_jit_store_leave_kb 0)
if(CONFIG_GBX_GBA_JIT_STORE_KB)
    set(gbx_gba_jit_store_kb ${CONFIG_GBX_GBA_JIT_STORE_KB})
endif()
if(CONFIG_GBX_GBA_JIT_STORE_LEAVE_KB)
    set(gbx_gba_jit_store_leave_kb ${CONFIG_GBX_GBA_JIT_STORE_LEAVE_KB})
endif()
# And the JITs' code pools in internal RAM.
set(gbx_gb_jit_pool_kb 192)
set(gbx_gba_jit_pool_kb 192)
if(CONFIG_GBX_GB_JIT_POOL_KB)
    set(gbx_gb_jit_pool_kb ${CONFIG_GBX_GB_JIT_POOL_KB})
endif()
if(CONFIG_GBX_GBA_JIT_POOL_KB)
    set(gbx_gba_jit_pool_kb ${CONFIG_GBX_GBA_JIT_POOL_KB})
endif()
if(CONFIG_GBX_GB_JIT_STORE_KB)
    set(gbx_gb_jit_store_kb ${CONFIG_GBX_GB_JIT_STORE_KB})
endif()
if(CONFIG_GBX_GBA_ROM_CACHE_KB)
    set(gbx_gba_rom_cache_kb ${CONFIG_GBX_GBA_ROM_CACHE_KB})
endif()
math(EXPR gbx_gb_jit_store_bytes "${gbx_gb_jit_store_kb} * 1024")
math(EXPR gbx_gba_rom_cache_bytes "${gbx_gba_rom_cache_kb} * 1024")
math(EXPR gbx_gb_jit_pool_bytes "${gbx_gb_jit_pool_kb} * 1024")
math(EXPR gbx_gb_jit_store_leave_bytes "${gbx_gb_jit_store_leave_kb} * 1024")
math(EXPR gbx_gba_jit_pool_bytes "${gbx_gba_jit_pool_kb} * 1024")
math(EXPR gbx_gba_jit_store_bytes "${gbx_gba_jit_store_kb} * 1024")
math(EXPR gbx_gba_jit_store_leave_bytes "${gbx_gba_jit_store_leave_kb} * 1024")
set(core_definitions GBX_HAS_GB=${gbx_has_gb} GBX_HAS_GBA=${gbx_has_gba}
                     GBX_GB_JIT_STORE_BYTES=${gbx_gb_jit_store_bytes} GBX_GBA_ROM_CACHE_BYTES=${gbx_gba_rom_cache_bytes}
                     GBX_GB_JIT_POOL_BYTES=${gbx_gb_jit_pool_bytes}
                     GBX_GB_JIT_STORE_LEAVE_BYTES=${gbx_gb_jit_store_leave_bytes}
                     GBX_GBA_JIT_POOL_BYTES=${gbx_gba_jit_pool_bytes}
                     GBX_GBA_JIT_STORE_BYTES=${gbx_gba_jit_store_bytes}
                     GBX_GBA_JIT_STORE_LEAVE_BYTES=${gbx_gba_jit_store_leave_bytes}
                     GBX_SINGLE_CORE=${gbx_single_core})

if(ESP_PLATFORM)
    idf_component_register(SRCS ${core_srcs} INCLUDE_DIRS "include" REQUIRES jit platform
                           LDFRAGMENTS "linker.lf")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ${core_definitions})
else()
    add_library(gbx_core STATIC ${core_srcs})
    target_include_directories(gbx_core PUBLIC include)
    target_link_libraries(gbx_core PUBLIC gbx_jit)
    target_compile_definitions(gbx_core PUBLIC ${core_definitions})
endif()
