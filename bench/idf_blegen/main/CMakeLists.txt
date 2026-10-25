idf_component_register(SRCS "main.c" "../../../components/drivers/rf/rf_regi2c.c" "../../../components/drivers/rf/rf_pll.c"
                            "../../../components/drivers/rf/rf_init.c" "../../../components/drivers/rf/rf_dump.c"
                       INCLUDE_DIRS "../../../components/drivers/rf" "../../../components/drivers/ble"
                       PRIV_REQUIRES bt nvs_flash esp_rom esp_timer)
target_compile_definitions(${COMPONENT_LIB} PRIVATE VSTEP=$ENV{VSTEP})
if(DEFINED ENV{FREQDUMP})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FREQDUMP=1)
endif()
if(DEFINED ENV{ADVTEST})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ADVTEST=1)
endif()
