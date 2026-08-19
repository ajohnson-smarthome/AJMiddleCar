idf_component_register(
    SRCS "ota_api.c" "main.c" "pca9685.c" "mixer.c" "motors.c" "car.c" "calibration.c" "wifi_ap.c" "http_server.c" "control_proto.c" "ws_control.c" "watchdog.c" "recovery.c" "calib_api.c" "status_api.c" "ramp.c" "ramp_api.c" "trim_api.c" "recovery_api.c" "telemetry.c" "wheel.c" "dims.c" "wheel_api.c" "dims_api.c" "cfg_json.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_timer heap esp_app_format app_update json
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_driver_i2c
)
