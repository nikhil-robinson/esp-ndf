idf_component_register(SRCS "main.c" PRIV_REQUIRES esp_wifi esp_phy nvs_flash esp_event esp_netif)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PHY_ONLY=$ENV{PHY_ONLY})
