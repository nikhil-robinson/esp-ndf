# =============================================================================
# esp-ndf  –  Bare-Metal ESP32-S3 SDK
# Top-level CMakeLists.txt
#
# Quick start:
#   cd build
#   cmake .. \
#     -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-xtensa-esp32s3.cmake \
#     -DNDF_BOARD=esp32s3 \
#     -DNDF_ARCH=xtensa
#   cmake --build .
# =============================================================================

cmake_minimum_required(VERSION 3.20)

# ----- Guard: toolchain must be set before project() -----------------------
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-xtensa-esp32s3.cmake"
        CACHE FILEPATH "Default toolchain")
    message(STATUS "[cmake] Defaulting to toolchain: ${CMAKE_TOOLCHAIN_FILE}")
endif()

# ----- Project declaration -------------------------------------------------
project(esp_ndf
    VERSION 0.1.0
    DESCRIPTION "Bare-metal ESP32-S3 SDK"
    LANGUAGES C CXX ASM
)

# ----- Helpers & board settings -------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(utils)

# Board/arch defaults (can be overridden on cmake command line)
set(NDF_ARCH  "xtensa"  CACHE STRING "Target architecture (xtensa | riscv)")
set(NDF_BOARD "esp32s3" CACHE STRING "Target board (esp32s3 | esp32c3 | esp32c6)")

# Load board settings (defines NDF_LINKER_SCRIPT, flash params, compile defs)
include("${CMAKE_SOURCE_DIR}/cmake/${NDF_BOARD}.cmake")

# ----- SDK sub-libraries ---------------------------------------------------
add_subdirectory(arch/${NDF_ARCH})          # startup, vectors, arch lib
add_subdirectory(board/${NDF_BOARD})        # board init, clock, SoC HAL
add_subdirectory(drivers)                   # peripheral drivers
add_subdirectory(lib)                       # minimal libc replacements

# ----- Examples ------------------------------------------------------------
option(BUILD_EXAMPLES "Build example applications" ON)
if(BUILD_EXAMPLES)
    add_subdirectory(examples/blink)
endif()

# ----- Summary -----------------------------------------------------------
message(STATUS "")
message(STATUS "========================================")
message(STATUS "  esp-ndf Bare-Metal SDK")
message(STATUS "  ARCH  : ${NDF_ARCH}")
message(STATUS "  BOARD : ${NDF_BOARD}")
message(STATUS "  BUILD : ${CMAKE_BUILD_TYPE}")
message(STATUS "========================================")
message(STATUS "")
