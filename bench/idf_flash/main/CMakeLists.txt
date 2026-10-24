idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES spi_flash esp_rom)
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
