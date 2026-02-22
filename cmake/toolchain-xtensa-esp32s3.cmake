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

# Newer Espressif toolchains (esp-13.2.0_20240530+) use the unified prefix
# xtensa-esp-elf- instead of the chip-specific xtensa-esp32s3-elf- prefix.
# Both produce identical Xtensa LX7 output; the unified one is what ships
# with ESP-IDF v5.x in ~/.espressif/tools/xtensa-esp-elf/.
set(CROSS_COMPILE "xtensa-esp-elf-")

# Auto-detect the ESP-IDF .espressif toolchain if not set explicitly
if(NOT XTENSA_TOOLCHAIN_PATH)
    # Build path with forward slashes to avoid CMake escape-sequence issues
    set(_ESPRESSIF_TC
        "$ENV{USERPROFILE}/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20240530/xtensa-esp-elf")
    # Normalise any Windows backslashes
    file(TO_CMAKE_PATH "${_ESPRESSIF_TC}" _ESPRESSIF_TC)
    if(EXISTS "${_ESPRESSIF_TC}/bin/${CROSS_COMPILE}gcc.exe")
        set(XTENSA_TOOLCHAIN_PATH "${_ESPRESSIF_TC}")
        message(STATUS "[toolchain] Auto-detected: ${XTENSA_TOOLCHAIN_PATH}")
    endif()
endif()

if(XTENSA_TOOLCHAIN_PATH)
    # Normalise separators so cmake glob/exists work on Windows
    file(TO_CMAKE_PATH "${XTENSA_TOOLCHAIN_PATH}" XTENSA_TOOLCHAIN_PATH)
    set(TC_PREFIX "${XTENSA_TOOLCHAIN_PATH}/bin/${CROSS_COMPILE}")
else()
    set(TC_PREFIX "${CROSS_COMPILE}")
endif()

# ----- Ninja: auto-detect from .espressif if not already on PATH -----------
if(NOT CMAKE_MAKE_PROGRAM)
    set(_NINJA_HINT
        "$ENV{USERPROFILE}/.espressif/tools/ninja/1.12.1/ninja.exe")
    file(TO_CMAKE_PATH "${_NINJA_HINT}" _NINJA_HINT)
    if(EXISTS "${_NINJA_HINT}")
        set(CMAKE_MAKE_PROGRAM "${_NINJA_HINT}" CACHE FILEPATH "Ninja" FORCE)
        message(STATUS "[toolchain] Ninja: ${CMAKE_MAKE_PROGRAM}")
    endif()
endif()

# ----- Compiler / Linker / Tool assignments --------------------------------
# On Windows executables have a .exe suffix; the full path must include it
# so CMake can verify the executable exists.
if(CMAKE_HOST_WIN32)
    set(EXE_EXT ".exe")
else()
    set(EXE_EXT "")
endif()

set(CMAKE_C_COMPILER   "${TC_PREFIX}gcc${EXE_EXT}"     CACHE FILEPATH "C compiler")
set(CMAKE_CXX_COMPILER "${TC_PREFIX}g++${EXE_EXT}"     CACHE FILEPATH "C++ compiler")
set(CMAKE_ASM_COMPILER "${TC_PREFIX}gcc${EXE_EXT}"     CACHE FILEPATH "Assembler")
set(CMAKE_AR           "${TC_PREFIX}ar${EXE_EXT}"      CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB       "${TC_PREFIX}ranlib${EXE_EXT}"  CACHE FILEPATH "Ranlib")
set(CMAKE_OBJCOPY      "${TC_PREFIX}objcopy${EXE_EXT}" CACHE FILEPATH "Objcopy")
set(CMAKE_OBJDUMP      "${TC_PREFIX}objdump${EXE_EXT}" CACHE FILEPATH "Objdump")
set(CMAKE_SIZE         "${TC_PREFIX}size${EXE_EXT}"    CACHE FILEPATH "Size utility")

# ----- CPU / ABI flags -----------------------------------------------------
# -mlongcalls: required for Xtensa to handle long-range calls (L32R instruction)
# -mtext-section-literals: place literals adjacent to code (Harvard arch)
#
# ENDIANNESS NOTE:
#   The unified xtensa-esp-elf toolchain (esp-13.2.0+) defaults to big-endian
#   Xtensa.  ESP32-S3 is little-endian.  The correct way to select LE is via
#   the chip-specific dynconfig:
#       -mdynconfig=<toolchain>/lib/xtensa_esp32s3.so
#   This DLL also configures the correct Xtensa LX7 ISA options (window ABI,
#   call0 ABI, loop, etc.) for the ESP32-S3 core.
#
# NOTE: -mlongcalls and -mtext-section-literals are ONLY for C/C++, NOT ASM.
# Passing them to GAS breaks the Xtensa vector table layout.
if(XTENSA_TOOLCHAIN_PATH)
    set(_DYNCONFIG_PATH "${XTENSA_TOOLCHAIN_PATH}/lib/xtensa_esp32s3.so")
    file(TO_CMAKE_PATH "${_DYNCONFIG_PATH}" _DYNCONFIG_PATH)
    if(EXISTS "${_DYNCONFIG_PATH}")
        set(XTENSA_DYNCONFIG_FLAG "-mdynconfig=${_DYNCONFIG_PATH}"
            CACHE STRING "ESP32-S3 Xtensa dynconfig" FORCE)
        message(STATUS "[toolchain] dynconfig: ${_DYNCONFIG_PATH}")
    else()
        message(WARNING
            "[toolchain] xtensa_esp32s3.so not found at ${_DYNCONFIG_PATH}. "
            "The ELF may default to big-endian (wrong for ESP32-S3). "
            "Set XTENSA_TOOLCHAIN_PATH to the toolchain root.")
        set(XTENSA_DYNCONFIG_FLAG "" CACHE STRING "" FORCE)
    endif()
else()
    set(XTENSA_DYNCONFIG_FLAG "" CACHE STRING "" FORCE)
    message(WARNING "[toolchain] XTENSA_TOOLCHAIN_PATH not set; dynconfig skipped. ELF may be big-endian.")
endif()

set(XTENSA_CPU_FLAGS
    "${XTENSA_DYNCONFIG_FLAG}"   # chip-specific ISA + little-endian output
    "-ffunction-sections"
    "-fdata-sections"
    "-fno-stack-protector"
    "-fomit-frame-pointer"
)
# Remove empty entries that appear when dynconfig is not found
list(REMOVE_ITEM XTENSA_CPU_FLAGS "")
string(JOIN " " XTENSA_CPU_FLAGS_STR ${XTENSA_CPU_FLAGS})

# C/C++ also get the long-call and literal flags
set(XTENSA_CC_EXTRA_FLAGS  "-mlongcalls -mtext-section-literals")

# ----- Basic compiler flags ------------------------------------------------
set(COMMON_FLAGS
    "${XTENSA_CPU_FLAGS_STR}"
    "-Os"               # Optimize for size (typical embedded default)
    "-g3"               # Full debug info
    "-Wall"
    "-Wextra"
    "-Wshadow"
    "-Wno-unused-parameter"
    "-ffreestanding"    # No hosted C library
    "-nostdlib"         # Do NOT link standard libs
)
string(JOIN " " COMMON_FLAGS_STR ${COMMON_FLAGS})

set(CMAKE_C_FLAGS_INIT   "${XTENSA_CC_EXTRA_FLAGS} ${COMMON_FLAGS_STR} -std=gnu17"                       CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_INIT "${XTENSA_CC_EXTRA_FLAGS} ${COMMON_FLAGS_STR} -std=gnu++17 -fno-exceptions -fno-rtti" CACHE STRING "" FORCE)
# ASM: no -mlongcalls / -mtext-section-literals to avoid literal-pool/org issues
set(CMAKE_ASM_FLAGS_INIT "${XTENSA_CPU_FLAGS_STR} -x assembler-with-cpp" CACHE STRING "" FORCE)

# ----- Linker flags --------------------------------------------------------
# --gc-sections: remove unused sections (requires -ffunction-sections above)
# NOTE: Do NOT add -Map= here; each ndf_add_executable target sets its own map.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-nostdlib -Wl,--gc-sections"
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
