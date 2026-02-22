# =============================================================================
# utils.cmake  –  Helpers for the esp-ndf bare-metal build system
# =============================================================================

# -----------------------------------------------------------------------------
# ndf_add_executable(name [SOURCES src1 ...] [INCLUDES inc1 ...] [LIBS lib1 ...])
#
# Creates a bare-metal firmware target that:
#   1. Compiles to an ELF
#   2. Post-processes the ELF into a raw binary  (.bin)
#   3. Generates a text dump                     (.lst)
#   4. Reports section sizes
# -----------------------------------------------------------------------------
function(ndf_add_executable TARGET_NAME)
    cmake_parse_arguments(NDF_EXE "" "" "SOURCES;INCLUDES;LIBS;DEFINES" ${ARGN})

    # ---- ELF target --------------------------------------------------------
    add_executable(${TARGET_NAME}.elf ${NDF_EXE_SOURCES})

    target_include_directories(${TARGET_NAME}.elf PRIVATE
        ${NDF_EXE_INCLUDES}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/board/${NDF_BOARD}/include
        ${CMAKE_SOURCE_DIR}/arch/${NDF_ARCH}/include
    )

    if(NDF_EXE_DEFINES)
        target_compile_definitions(${TARGET_NAME}.elf PRIVATE ${NDF_EXE_DEFINES})
    endif()

    if(NDF_EXE_LIBS)
        target_link_libraries(${TARGET_NAME}.elf PRIVATE ${NDF_EXE_LIBS})
    endif()

    # Linker script
    target_link_options(${TARGET_NAME}.elf PRIVATE
        -T ${NDF_LINKER_SCRIPT}
        -Wl,--Map=${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.map
        -Wl,--print-memory-usage
    )

    set_target_properties(${TARGET_NAME}.elf PROPERTIES
        LINK_DEPENDS ${NDF_LINKER_SCRIPT}
    )

    # ---- Raw binary (.bin) -------------------------------------------------
    add_custom_command(TARGET ${TARGET_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary
                ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
                ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.bin
        COMMENT "[objcopy] Generating ${TARGET_NAME}.bin"
    )

    # ---- Disassembly listing (.lst) ----------------------------------------
    add_custom_command(TARGET ${TARGET_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_OBJDUMP} -d -S
                ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
                > ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.lst
        COMMENT "[objdump]  Generating ${TARGET_NAME}.lst"
    )

    # ---- Section sizes -----------------------------------------------------
    add_custom_command(TARGET ${TARGET_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_SIZE} --format=sysv
                ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
        COMMENT "[size]     Section sizes for ${TARGET_NAME}"
    )

    # ---- esptool image wrapping (.img.bin) ---------------------------------
    # Wraps the ELF in an ESP32 image format (with ROM header/checksum)
    # Requires esptool.py to be available on PATH or pointed at via ESPTOOL_PATH
    find_program(ESPTOOL_EXECUTABLE esptool.py esptool
        HINTS $ENV{ESPTOOL_PATH} ${CMAKE_SOURCE_DIR}/tools
        DOC "esptool.py path"
    )

    if(ESPTOOL_EXECUTABLE)
        add_custom_command(TARGET ${TARGET_NAME}.elf POST_BUILD
            COMMAND ${ESPTOOL_EXECUTABLE}
                    --chip esp32s3
                    elf2image
                    --flash_mode  ${FLASH_MODE}
                    --flash_freq  ${FLASH_FREQ}
                    --flash_size  ${FLASH_SIZE}
                    -o ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.img.bin
                    ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
            COMMENT "[esptool]  Generating ${TARGET_NAME}.img.bin (flashable image)"
        )

        # ---- Flash target --------------------------------------------------
        add_custom_target(flash-${TARGET_NAME}
            DEPENDS ${TARGET_NAME}.elf
            COMMAND ${ESPTOOL_EXECUTABLE}
                    --chip esp32s3
                    write_flash
                    ${APP_FLASH_OFFSET}
                    ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.img.bin
            COMMENT "[flash]    Flashing ${TARGET_NAME} to ${APP_FLASH_OFFSET}"
        )
    else()
        message(WARNING
            "esptool.py not found. 'flash-${TARGET_NAME}' target unavailable. "
            "Install via: pip install esptool"
        )
    endif()
endfunction()


# -----------------------------------------------------------------------------
# ndf_add_library(name [SOURCES ...] [INCLUDES ...])
# A thin wrapper around add_library for SDK sub-libraries.
# -----------------------------------------------------------------------------
function(ndf_add_library LIB_NAME)
    cmake_parse_arguments(NDF_LIB "" "" "SOURCES;INCLUDES" ${ARGN})

    add_library(${LIB_NAME} STATIC ${NDF_LIB_SOURCES})

    target_include_directories(${LIB_NAME} PUBLIC
        ${NDF_LIB_INCLUDES}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/board/${NDF_BOARD}/include
        ${CMAKE_SOURCE_DIR}/arch/${NDF_ARCH}/include
    )
endfunction()


# -----------------------------------------------------------------------------
# ndf_print_size(target) — convenience wrapper to print size after build
# -----------------------------------------------------------------------------
function(ndf_print_size TARGET_NAME)
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_SIZE} --format=sysv ${TARGET_NAME}
    )
endfunction()
