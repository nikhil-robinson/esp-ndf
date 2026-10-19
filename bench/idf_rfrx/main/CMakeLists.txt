if(NOT DEFINED ENV{OURS})
    set(OURS 0)
else()
    set(OURS $ENV{OURS})
endif()
idf_component_register(SRCS "main.c" "../../../drivers/rf/rf_regi2c.c" "../../../drivers/rf/rf_pll.c"
                            "../../../drivers/rf/rf_init.c" "../../../drivers/rf/rf_dump.c"
                       INCLUDE_DIRS "../../../drivers/rf"
                       PRIV_REQUIRES esp_wifi esp_phy nvs_flash heap esp_timer)
target_compile_definitions(${COMPONENT_LIB} PRIVATE OURS=${OURS} BISECT=$ENV{BISECT}0 WIFI=$ENV{WIFI}0)
