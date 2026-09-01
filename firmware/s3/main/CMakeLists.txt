idf_component_register(SRCS "main.c" "usb_net.c" "status_api.c" "api_util.c" "api_guard.c"
                             "net_api.c" "net_cfg.c"
                             "ota_api.c" "wifi_sta.c" "wifi_state.c" "relay_udp.c" "udp_sess.c"
                             "relay_tcp.c" "tcp_pending.c" "relay_stats.c" "screens.c"
                             "fonts_cyrillic.c" "display.c" "display_hal.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_netif esp_event esp_http_server esp_app_format
                                     app_update esp_partition cjson esp_wifi lwip esp_timer
                                     u8g2 esp_driver_i2c esp_driver_gpio)
