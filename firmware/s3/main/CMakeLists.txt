idf_component_register(SRCS "main.c" "usb_net.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_netif)
