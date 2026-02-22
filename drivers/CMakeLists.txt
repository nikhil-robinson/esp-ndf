# =============================================================================
# drivers/CMakeLists.txt  –  Peripheral driver library
# =============================================================================

set(DRIVER_SOURCES
    gpio/gpio.c
    uart/uart.c
)

ndf_add_library(drivers
    SOURCES   ${DRIVER_SOURCES}
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}          # e.g. #include "drivers/gpio.h"
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/board/${NDF_BOARD}/include
        ${CMAKE_SOURCE_DIR}/arch/${NDF_ARCH}/include
)

target_include_directories(drivers PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
# Link the arch interrupt module
target_link_libraries(drivers PUBLIC arch_xtensa)
