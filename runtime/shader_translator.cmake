# Providers implement recompiler/nvn_shader.h (SPIR-V and binding reflection) and
# export their cache fingerprint in nvn_traductor_huella.h via this target.
set(RISE_SHADER_TRANSLATOR_DIR "${CMAKE_CURRENT_LIST_DIR}/recompiler" CACHE PATH
    "CMake source directory providing the rise_shader_translator target")
add_subdirectory("${RISE_SHADER_TRANSLATOR_DIR}" "${CMAKE_BINARY_DIR}/recompiler")
if(NOT TARGET rise_shader_translator)
  message(FATAL_ERROR "Shader translator must export rise_shader_translator: ${RISE_SHADER_TRANSLATOR_DIR}")
endif()
