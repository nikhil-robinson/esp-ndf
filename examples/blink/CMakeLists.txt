# =============================================================================
# examples/blink/CMakeLists.txt
# =============================================================================

ndf_add_executable(blink
    SOURCES
        main.c
    INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/board/${NDF_BOARD}/include
        ${CMAKE_SOURCE_DIR}/arch/${NDF_ARCH}/include
        ${CMAKE_SOURCE_DIR}/drivers
    LIBS
        arch_xtensa       # startup, vectors, interrupt controller
        board_esp32s3     # board_init, clock config, board_delay_*
        drivers           # GPIO, UART
        ndf_libc          # memset, memcpy, string ops
)
