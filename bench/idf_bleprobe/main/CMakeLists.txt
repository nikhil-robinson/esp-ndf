idf_component_register(SRCS "main.c" "../../../drivers/rf/rf_regi2c.c" "../../../drivers/rf/rf_pll.c"
                            "../../../drivers/rf/rf_init.c" "../../../drivers/rf/rf_dump.c"
                       INCLUDE_DIRS "../../../drivers/rf"
                       PRIV_REQUIRES bt nvs_flash esp_rom esp_timer)
if(DEFINED ENV{ONTO})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ONTO=$ENV{ONTO})
endif()
