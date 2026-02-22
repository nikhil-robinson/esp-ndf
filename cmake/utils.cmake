# =============================================================================
# utils.cmake  –  Helpers for the esp-ndf bare-metal build system
# =============================================================================

# -----------------------------------------------------------------------------
# ndf_add_executable(name [SOURCES src1 ...] [INCLUDES inc1 ...] [LIBS lib1 ...])
#
# Creates a bare-metal firmware target that:
#   1. Compiles to an ELF
#   2. Runs esptool elf2image to produce a proper ESP32 flashable .bin
#      (objcopy -O binary is intentionally NOT used – it produces a flat raw
#       binary padded across the full VMA range, yielding ~96 MB for ESP32-S3
#       because DROM sits at 0x3C000020 and IROM at 0x42000020, a ~96 MB gap.)
#   3. Generates a text dump  (.lst)
#   4. Reports section sizes
#
# esptool is REQUIRED.  Install with:  pip install esptool
# Override the path via:  cmake -DESPTOOL_PATH=<dir> ...
#                     or:  set ESPTOOL_PATH env var
# -----------------------------------------------------------------------------
function(ndf_add_executable TARGET_NAME)
    cmake_parse_arguments(NDF_EXE "" "" "SOURCES;INCLUDES;LIBS;DEFINES" ${ARGN})

    # ---- Locate esptool (required) -----------------------------------------
    # Discovery order (stops at first success):
    #   1. Explicit override: -DESPTOOL_EXECUTABLE=... on the cmake command line
    #   2. python -m esptool  (works wherever pip install esptool was run)
    #   3. Standalone esptool / esptool.py on PATH or in common install dirs
    #   4. Windows Store Python per-package cache (deep path, never on PATH)

    # Allow a fully-explicit override at configure time
    if(NOT ESPTOOL_EXECUTABLE)
        if(DEFINED ENV{ESPTOOL_EXECUTABLE})
            set(ESPTOOL_EXECUTABLE "$ENV{ESPTOOL_EXECUTABLE}" CACHE STRING
                "esptool from ESPTOOL_EXECUTABLE env var" FORCE)
        endif()
    endif()

    # Strategy 2: python -m esptool  ← works on this machine (pip install esptool)
    if(NOT ESPTOOL_EXECUTABLE)
        find_program(_NDF_PYTHON NAMES python python3 python3.13 python3.12 python3.11
            DOC "Python interpreter for esptool module")
        if(_NDF_PYTHON)
            execute_process(
                COMMAND "${_NDF_PYTHON}" -m esptool version
                RESULT_VARIABLE _esptool_module_rc
                OUTPUT_VARIABLE _esptool_ver_out
                ERROR_QUIET
            )
            if(_esptool_module_rc EQUAL 0)
                # Store as a CMake list so COMMAND expands it correctly:
                # COMMAND ${ESPTOOL_EXECUTABLE} → python;-m;esptool → three args
                set(ESPTOOL_EXECUTABLE "${_NDF_PYTHON}" CACHE FILEPATH
                    "Python interpreter that has esptool module installed" FORCE)
                set(ESPTOOL_MODULE_ARGS "-m;esptool" CACHE STRING
                    "Args to invoke esptool as a Python module" FORCE)
                string(STRIP "${_esptool_ver_out}" _esptool_ver_out)
                message(STATUS "[esptool]  python module: ${_NDF_PYTHON} -m esptool  (${_esptool_ver_out})")
            endif()
        endif()
    endif()

    # Strategy 3: standalone executable on PATH / common install locations
    if(NOT ESPTOOL_EXECUTABLE)
        find_program(ESPTOOL_EXECUTABLE
            NAMES esptool.exe esptool.py esptool
            HINTS
                $ENV{ESPTOOL_PATH}
                ${CMAKE_SOURCE_DIR}/tools
                "$ENV{USERPROFILE}/.espressif/python_env/idf5.4_py3.12_env/Scripts"
                "$ENV{USERPROFILE}/.espressif/python_env/idf5.3_py3.11_env/Scripts"
                "$ENV{USERPROFILE}/.espressif/python_env/idf5.2_py3.11_env/Scripts"
                "$ENV{USERPROFILE}/AppData/Local/Programs/Python/Python313/Scripts"
                "$ENV{USERPROFILE}/AppData/Local/Programs/Python/Python312/Scripts"
                "$ENV{USERPROFILE}/AppData/Local/Programs/Python/Python311/Scripts"
                "$ENV{LOCALAPPDATA}/Programs/Python/Python313/Scripts"
                "$ENV{LOCALAPPDATA}/Programs/Python/Python312/Scripts"
            DOC "Standalone esptool executable"
        )
    endif()

    # Strategy 4: Windows Store Python — exe lives in a per-package cache
    if(NOT ESPTOOL_EXECUTABLE AND CMAKE_HOST_WIN32)
        file(GLOB _py_store_dirs
            "$ENV{LOCALAPPDATA}/Packages/PythonSoftwareFoundation.Python.*/LocalCache/local-packages/Python*/Scripts")
        foreach(_d ${_py_store_dirs})
            if(EXISTS "${_d}/esptool.exe")
                set(ESPTOOL_EXECUTABLE "${_d}/esptool.exe" CACHE FILEPATH
                    "esptool in Windows Store Python cache" FORCE)
                break()
            endif()
        endforeach()
    endif()

    if(NOT ESPTOOL_EXECUTABLE)
        message(FATAL_ERROR
            "\n"
            "  esptool not found!\n"
            "\n"
            "  esptool is required to produce a valid ESP32-S3 flashable image.\n"
            "  objcopy -O binary cannot be used here: the ESP32-S3 linker script\n"
            "  places sections at DROM=0x3C000020 and IROM=0x42000020, producing\n"
            "  a ~96 MB zero-padded raw binary that is NOT a valid flash image.\n"
            "\n"
            "  Install esptool:  pip install esptool\n"
            "\n"
            "  Or point cmake at it:\n"
            "    cmake -DESPTOOL_EXECUTABLE=C:/path/to/esptool.exe ...\n"
        )
    endif()

    # If found via python module, build the full command list once
    if(NOT DEFINED ESPTOOL_MODULE_ARGS)
        set(ESPTOOL_MODULE_ARGS "" CACHE INTERNAL "")
    endif()
    # _ESPTOOL_CMD is an internal CMake list used in every COMMAND below
    if(ESPTOOL_MODULE_ARGS)
        set(_ESPTOOL_CMD "${ESPTOOL_EXECUTABLE}" ${ESPTOOL_MODULE_ARGS})
    else()
        set(_ESPTOOL_CMD "${ESPTOOL_EXECUTABLE}")
    endif()

    message(STATUS "[esptool]  Command: ${_ESPTOOL_CMD}")

    # ---- ELF target --------------------------------------------------------
    add_executable(${TARGET_NAME}.elf ${NDF_EXE_SOURCES})

    target_include_directories(${TARGET_NAME}.elf PRIVATE
        ${NDF_EXE_INCLUDES}
        ${CMAKE_SOURCE_DIR}
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

    # ---- esptool: produce proper ESP32 flashable image (.bin) --------------
    #
    # esptool elf2image reads the ELF program headers and produces a compact
    # binary in the ESP32 image format (magic byte 0xE9, segment headers,
    # SHA-256 hash footer).  Only flash-mapped segments (IROM code, DROM
    # rodata, IRAM load-image) are included; the VMA gap between DROM and IROM
    # is never present in the output.  Typical output is a few dozen KB.
    #
    # Output: <target>.bin  (canonical flashable image, replaces objcopy .bin)
    set(_BIN ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.bin)

    add_custom_command(TARGET ${TARGET_NAME}.elf POST_BUILD
        COMMAND ${_ESPTOOL_CMD}
                --chip   esp32s3
                elf2image
                --flash_mode  ${FLASH_MODE}
                --flash_freq  ${FLASH_FREQ}
                --flash_size  ${FLASH_SIZE}
                --output ${_BIN}
                ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
        BYPRODUCTS ${_BIN}
        COMMENT "[esptool]  elf2image → ${TARGET_NAME}.bin (ESP32 image format)"
        VERBATIM
    )

    # ---- Disassembly listing (.lst) ----------------------------------------
    add_custom_command(TARGET ${TARGET_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_OBJDUMP} -d -S
                ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
                > ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.lst
        COMMENT "[objdump]  Generating ${TARGET_NAME}.lst"
        VERBATIM
    )

    # ---- Section sizes -----------------------------------------------------
    add_custom_command(TARGET ${TARGET_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_SIZE} --format=sysv
                ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
        COMMENT "[size]     Section sizes for ${TARGET_NAME}"
        VERBATIM
    )

    # ---- Flash target: write_flash -----------------------------------------
    # Depends on the ELF (which produces the .bin via POST_BUILD above).
    # Requires --port to be set; pass via:  cmake --build . --target flash-<name>
    # and set ESPPORT env var, e.g.  set ESPPORT=COM3
    add_custom_target(flash-${TARGET_NAME}
        DEPENDS ${TARGET_NAME}.elf
        COMMAND ${_ESPTOOL_CMD}
                --chip    esp32s3
                $<$<BOOL:$ENV{ESPPORT}>:--port> $<$<BOOL:$ENV{ESPPORT}>:$ENV{ESPPORT}>
                write_flash
                --flash_mode  ${FLASH_MODE}
                --flash_freq  ${FLASH_FREQ}
                --flash_size  ${FLASH_SIZE}
                ${APP_FLASH_OFFSET}
                ${_BIN}
        COMMENT "[flash]    write_flash ${APP_FLASH_OFFSET} ${TARGET_NAME}.bin"
        VERBATIM
    )

    # ---- Convenience: show flash layout ------------------------------------
    add_custom_target(image-info-${TARGET_NAME}
        DEPENDS ${TARGET_NAME}.elf
        COMMAND ${_ESPTOOL_CMD}
                --chip esp32s3
                image_info
                --version 2
                ${_BIN}
        COMMENT "[esptool]  Image info for ${TARGET_NAME}.bin"
        VERBATIM
    )

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
        ${CMAKE_SOURCE_DIR}
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
