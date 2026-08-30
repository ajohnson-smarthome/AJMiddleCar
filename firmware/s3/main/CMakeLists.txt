idf_component_register(SRCS "main.c" "usb_net.c" "status_api.c" "api_util.c" "net_api.c" "net_cfg.c"
                             "ota_api.c" "wifi_sta.c" "wifi_state.c" "relay_udp.c" "udp_sess.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_netif esp_event esp_http_server esp_app_format
                                     app_update esp_partition cjson esp_wifi lwip)
