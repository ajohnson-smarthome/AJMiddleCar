idf_component_register(SRCS "main.c" "usb_net.c" "status_api.c" "api_util.c" "api_guard.c"
                             "net_api.c" "net_cfg.c"
                             "ota_api.c" "wifi_sta.c" "wifi_state.c" "relay_udp.c" "udp_sess.c"
                             "relay_tcp.c" "tcp_pending.c" "relay_stats.c" "screens.c"
                             "fonts_cyrillic.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_netif esp_event esp_http_server esp_app_format
                                     app_update esp_partition cjson esp_wifi lwip esp_timer
                                     u8g2)

# fonts_cyrillic.c's three font arrays are not yet referenced from any .c file (the display
# driver that calls u8g2_SetFont() on them is a later task) -- ESP-IDF builds with both
# -fdata-sections and --gc-sections, so without an anchor the linker would discard each font's
# unreferenced section even though the whole-archived object that defines it is linked in. Force
# them to survive, the same way ESP-IDF itself keeps its own otherwise-unreferenced
# component-registration symbols alive (see the "-u ..." flags idf.py's own link line carries).
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,u8g2_font_10x20_t_cyrillic"
    "-Wl,-u,u8g2_font_9x15_t_cyrillic"
    "-Wl,-u,u8g2_font_6x12_t_cyrillic")
