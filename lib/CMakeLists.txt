# =============================================================================
# lib/CMakeLists.txt  –  Minimal standard library replacements
# =============================================================================

ndf_add_library(ndf_libc
    SOURCES
        string.c
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}   # allow #include "lib/string.h"
        ${CMAKE_SOURCE_DIR}/include
)

target_include_directories(ndf_libc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
