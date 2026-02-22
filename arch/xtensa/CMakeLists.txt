# =============================================================================
# arch/xtensa/CMakeLists.txt  –  Xtensa LX7 architecture library
# =============================================================================

# Collect architecture sources
set(ARCH_SOURCES
    startup/start.S
    startup/vectors.S
    intr.c
)

ndf_add_library(arch_xtensa
    SOURCES   ${ARCH_SOURCES}
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/board/${NDF_BOARD}/include
)

# Expose the include directory to all targets that link against this lib
target_include_directories(arch_xtensa PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Force-link the startup/vector objects so gc-sections doesn't discard them
target_link_options(arch_xtensa INTERFACE
    -Wl,--whole-archive $<TARGET_FILE:arch_xtensa> -Wl,--no-whole-archive
)
