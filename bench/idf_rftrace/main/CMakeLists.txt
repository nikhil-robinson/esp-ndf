idf_component_register(SRCS "main.c" PRIV_REQUIRES esp_wifi esp_phy nvs_flash bt)
if(DEFINED ENV{BTTRACE})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BTTRACE=1)
endif()
