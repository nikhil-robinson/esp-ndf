idf_component_register(SRCS "main.c" "cmd_ble.c" "cmd_wifi.c" "cmd_net.c" INCLUDE_DIRS "."
    EMBED_TXTFILES "certs/ca.pem" "certs/other_ca.pem" "certs/server.pem" "certs/server_key.pem"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event bt driver esp_driver_usb_serial_jtag lwip esp_http_client esp_http_server esp_https_server esp-tls esp_timer esp_hw_support mqtt)

target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-zmuldefs")
