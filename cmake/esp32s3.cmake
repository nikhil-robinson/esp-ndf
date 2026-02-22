# =============================================================================
# esp32s3.cmake - Board/SoC specific CMake settings for ESP32-S3
#
# Included automatically when BOARD=esp32s3 and ARCH=xtensa
# =============================================================================

# ----- Identify the target -------------------------------------------------
set(NDF_ARCH  "xtensa"   CACHE STRING "Target architecture")
set(NDF_BOARD "esp32s3"  CACHE STRING "Target board/SoC")
set(NDF_CPU   "esp32s3"  CACHE STRING "Target CPU")

# ----- Flash geometry (default: 8MB flash, QIO mode) -----------------------
set(FLASH_SIZE   "8MB"  CACHE STRING "Flash size")
set(FLASH_MODE   "qio"  CACHE STRING "Flash mode: qio/qout/dio/dout")
set(FLASH_FREQ   "80m"  CACHE STRING "Flash frequency")
set(FLASH_OFFSET "0x0"  CACHE STRING "App partition flash offset")

# ----- App load address (where ROM bootloader jumps to) --------------------
# The ROM second-stage bootloader maps:
#   Flash     -> IROM  at 0x42000000 (I-bus, code execution)
#   Flash     -> DROM  at 0x3C000000 (D-bus, read-only data)
#   App image header is at flash_offset + 0x18 for the first segment
set(APP_FLASH_OFFSET "0x10000" CACHE STRING "App partition start in flash")

# ----- Memory sizes (defaults, can be overridden) --------------------------
# See arch/xtensa/ld/esp32s3.ld for the full map
set(IRAM_SIZE  "0x50000" CACHE STRING "IRAM size in bytes")
set(DRAM_SIZE  "0x60000" CACHE STRING "DRAM size in bytes")

# ----- Global compile definitions for this target -------------------------
add_compile_definitions(
    CONFIG_IDF_TARGET_ESP32S3=1
    CONFIG_ARCH_XTENSA=1
    ESP32S3=1
    __XTENSA__=1
)

# ----- Linker script -------------------------------------------------------
set(NDF_LINKER_SCRIPT
    "${CMAKE_SOURCE_DIR}/arch/xtensa/ld/esp32s3.ld"
    CACHE FILEPATH "Main linker script"
)

# ----- Partition table offset (where esptool expects the partition table) --
set(PARTITION_TABLE_OFFSET "0x8000" CACHE STRING "Partition table offset in flash")

message(STATUS "[Board]     ARCH=${NDF_ARCH}  BOARD=${NDF_BOARD}")
message(STATUS "[Flash]     SIZE=${FLASH_SIZE}  MODE=${FLASH_MODE}  FREQ=${FLASH_FREQ}")
