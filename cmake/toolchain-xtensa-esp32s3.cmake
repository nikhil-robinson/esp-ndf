# =============================================================================
# Toolchain file for Xtensa ESP32-S3 (LX7 core) bare-metal builds
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-xtensa-esp32s3.cmake ...
#
# Toolchain download:
#   https://github.com/espressif/crosstool-NG/releases
#   Filename: xtensa-esp32s3-elf-<version>-<host>.tar.gz
#
# After extracting, either:
#   1. Add <toolchain>/bin to PATH, or
#   2. Set XTENSA_TOOLCHAIN_PATH in environment or as cmake variable
# =============================================================================

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR xtensa)

# Allow override via environment or -D flag
if(DEFINED ENV{XTENSA_TOOLCHAIN_PATH})
    set(XTENSA_TOOLCHAIN_PATH "$ENV{XTENSA_TOOLCHAIN_PATH}")
endif()

# Toolchain prefix
set(CROSS_COMPILE "xtensa-esp32s3-elf-")

if(XTENSA_TOOLCHAIN_PATH)
    set(TC_PREFIX "${XTENSA_TOOLCHAIN_PATH}/bin/${CROSS_COMPILE}")
else()
    set(TC_PREFIX "${CROSS_COMPILE}")
endif()

# ----- Compiler / Linker / Tool assignments --------------------------------
set(CMAKE_C_COMPILER   "${TC_PREFIX}gcc"     CACHE FILEPATH "C compiler")
set(CMAKE_CXX_COMPILER "${TC_PREFIX}g++"     CACHE FILEPATH "C++ compiler")
set(CMAKE_ASM_COMPILER "${TC_PREFIX}gcc"     CACHE FILEPATH "Assembler")
set(CMAKE_AR           "${TC_PREFIX}ar"      CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB       "${TC_PREFIX}ranlib"  CACHE FILEPATH "Ranlib")
set(CMAKE_OBJCOPY      "${TC_PREFIX}objcopy" CACHE FILEPATH "Objcopy")
set(CMAKE_OBJDUMP      "${TC_PREFIX}objdump" CACHE FILEPATH "Objdump")
set(CMAKE_SIZE         "${TC_PREFIX}size"    CACHE FILEPATH "Size utility")

# ----- CPU / ABI flags -----------------------------------------------------
# -mlongcalls: required for Xtensa to handle long-range calls (L32R instruction)
# -mtext-section-literals: place literals adjacent to code (Harvard arch)
# -ffunction-sections / -fdata-sections: enable dead-code stripping via --gc-sections
set(XTENSA_CPU_FLAGS
    "-mlongcalls"
    "-mtext-section-literals"
    "-ffunction-sections"
    "-fdata-sections"
    "-fno-stack-protector"
    "-fomit-frame-pointer"
)
string(JOIN " " XTENSA_CPU_FLAGS_STR ${XTENSA_CPU_FLAGS})

# ----- Basic compiler flags ------------------------------------------------
set(COMMON_FLAGS
    "${XTENSA_CPU_FLAGS_STR}"
    "-Os"               # Optimize for size (typical embedded default)
    "-g3"               # Full debug info
    "-Wall"
    "-Wextra"
    "-Wshadow"
    "-Wno-unused-parameter"
    "-fno-exceptions"
    "-fno-rtti"
    "-ffreestanding"    # No hosted C library
    "-nostdlib"         # Do NOT link standard libs
)
string(JOIN " " COMMON_FLAGS_STR ${COMMON_FLAGS})

set(CMAKE_C_FLAGS_INIT   "${COMMON_FLAGS_STR} -std=gnu17"    CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_INIT "${COMMON_FLAGS_STR} -std=gnu++17"  CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS_INIT "${XTENSA_CPU_FLAGS_STR} -x assembler-with-cpp" CACHE STRING "" FORCE)

# ----- Linker flags --------------------------------------------------------
# --gc-sections: remove unused sections (requires -ffunction-sections above)
# --print-memory-usage: helpful for embedded builds
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-nostdlib -Wl,--gc-sections -Wl,-Map=\${PROJECT_BINARY_DIR}/\${PROJECT_NAME}.map"
    CACHE STRING "" FORCE
)

# ----- Disable default search paths (cross-compile) -----------------------
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Prevent CMake from running the C compiler with a test exe (fails on bare-metal)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

message(STATUS "[Toolchain] Target: xtensa-esp32s3-elf (Xtensa LX7)")
message(STATUS "[Toolchain] Compiler: ${CMAKE_C_COMPILER}")
