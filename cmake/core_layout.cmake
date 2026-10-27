# Keep the execution core at the start of internal RAM, ahead of IDF's code.
# Adapt IDF's generated script rather than maintaining a copy of its memory
# layout. The linker fragments say which of the core's code goes there.
idf_component_get_property(gbx_system_lib esp_system COMPONENT_LIB)
get_target_property(gbx_system_options ${gbx_system_lib} INTERFACE_LINK_OPTIONS)
set(gbx_original_sections "SHELL:-T sections.ld")
if(NOT gbx_original_sections IN_LIST gbx_system_options)
    message(FATAL_ERROR "IDF sections.ld link option changed; review core layout integration")
endif()
set(gbx_sections "${CMAKE_BINARY_DIR}/gbx_sections.ld")
list(TRANSFORM gbx_system_options REPLACE "^SHELL:-T sections\\.ld$" "SHELL:-T \"${gbx_sections}\"")
set_property(TARGET ${gbx_system_lib} PROPERTY INTERFACE_LINK_OPTIONS "${gbx_system_options}")

idf_build_get_property(gbx_elf EXECUTABLE)
set(gbx_idf_sections "${CMAKE_BINARY_DIR}/esp-idf/esp_system/ld/sections.ld")
set(gbx_fragments "${PROJECT_DIR}/components/core/linker.lf" "${PROJECT_DIR}/components/jit/linker.lf")
add_custom_command(OUTPUT "${gbx_sections}"
    COMMAND "${PYTHON}" "${PROJECT_DIR}/tools/link_core_layout.py"
        "${gbx_idf_sections}" "${gbx_sections}" ${gbx_fragments}
    DEPENDS "${gbx_idf_sections}" "${PROJECT_DIR}/tools/link_core_layout.py" ${gbx_fragments}
    VERBATIM)
add_custom_target(gbx_core_layout DEPENDS "${gbx_sections}")
add_dependencies(gbx_core_layout __ldgen_output_sections.ld)
add_dependencies(${gbx_elf} gbx_core_layout)
set_property(TARGET ${gbx_elf} APPEND PROPERTY LINK_DEPENDS "${gbx_sections}")
