# =============================================================================
# board/esp32s3/CMakeLists.txt  –  ESP32-S3 board support package
# =============================================================================

ndf_add_library(board_esp32s3
    SOURCES
        board_init.c
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/arch/${NDF_ARCH}/include
        ${CMAKE_SOURCE_DIR}/drivers
)

target_include_directories(board_esp32s3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
