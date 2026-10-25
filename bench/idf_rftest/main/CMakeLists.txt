idf_component_register(SRCS "main.c" "../../../components/drivers/rf/rf_regi2c.c" "../../../components/drivers/rf/rf_pll.c"
                       INCLUDE_DIRS "../../../components/drivers/rf"
                       PRIV_REQUIRES esp_wifi esp_phy nvs_flash)
