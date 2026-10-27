# Frontend-neutral RV32 code generation and cache ownership.
set(jit_sources "src/assembler.cpp" "src/cache.cpp" "src/cache_lookup.cpp" "src/store.cpp")
if(ESP_PLATFORM)
    idf_component_register(SRCS ${jit_sources} INCLUDE_DIRS "include" REQUIRES platform
                           LDFRAGMENTS "linker.lf")
    # See rv32::kZb: chip revisions before v3.0 lack Zba, Zbb and Zbs.
    if(CONFIG_IDF_TARGET_ESP32P4 AND NOT CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
        target_compile_definitions(${COMPONENT_LIB} PUBLIC GBX_RV32_ZB=1)
    endif()
else()
    add_library(gbx_jit STATIC ${jit_sources})
    target_include_directories(gbx_jit PUBLIC include)
    target_link_libraries(gbx_jit PUBLIC gbx_platform)
endif()
