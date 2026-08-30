idf_component_register(SRCS "main.c" "usb_net.c" "status_api.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_event esp_netif esp_http_server esp_app_format)
