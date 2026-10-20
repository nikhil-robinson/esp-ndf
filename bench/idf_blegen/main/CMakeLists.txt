idf_component_register(SRCS "main.c" "../../../drivers/rf/rf_regi2c.c" "../../../drivers/rf/rf_pll.c"
                            "../../../drivers/rf/rf_init.c" "../../../drivers/rf/rf_dump.c"
                       INCLUDE_DIRS "../../../drivers/rf" "../../../drivers/ble"
                       PRIV_REQUIRES bt nvs_flash esp_rom esp_timer)
target_compile_definitions(${COMPONENT_LIB} PRIVATE VSTEP=$ENV{VSTEP})
if(DEFINED ENV{FREQDUMP})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FREQDUMP=1)
endif()
