idf_component_register(SRCS "main.c" PRIV_REQUIRES esp_wifi nvs_flash)
target_compile_definitions(${COMPONENT_LIB} PRIVATE DUMP_RX_STATE=1)
