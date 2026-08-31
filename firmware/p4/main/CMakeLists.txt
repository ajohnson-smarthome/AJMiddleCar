# The C6's image rides inside the car's image, so one OTA updates both processors
# (docs/superpowers/specs/2026-08-31-radio-in-one-image-design.md). tools/release.sh builds it
# from the pinned esp_hosted component and drops it here; it is git-ignored, because it is a
# build product of a pinned dependency and checking it in would create the second version this
# whole design exists to remove.
#
# An everyday build must not require it — nobody working on the mixer should have to build a
# co-processor image first — and EMBED_FILES fails outright on a missing file. So an absent
# image becomes an empty one, and the firmware reads the embedded length to decide whether it
# has anything to offer the radio. There is no separate flag: zero bytes IS the statement.
set(RADIO_IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/radio_image.bin")
if(NOT EXISTS "${RADIO_IMAGE}")
    file(WRITE "${RADIO_IMAGE}" "")
    message(STATUS "no radio_image.bin — this build carries no co-processor image")
endif()

idf_component_register(
    SRCS "ota_api.c" "main.c" "pca9685.c" "mixer.c" "motors.c" "car.c" "calibration.c" "wifi_ap.c" "http_server.c" "control_proto.c" "rt_link.c" "recovery.c" "calib_api.c" "status_api.c" "ramp.c" "link.c" "cfg_api.c" "telemetry.c" "wheel.c" "dims.c" "cfg_json.c" "api_util.c" "radio_ota.c" "radio_flash.c"
    INCLUDE_DIRS "."
    EMBED_FILES "radio_image.bin"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_timer heap esp_app_format app_update lwip
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_driver_uart esp_driver_i2c
)
