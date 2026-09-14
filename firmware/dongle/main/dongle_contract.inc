/* generated from contract/dongle-api.json by tools/gen_contract.py - do not edit */

#ifndef DONGLE_CONTRACT_INC
#define DONGLE_CONTRACT_INC

#define DONGLE_PROTO 1
#define DONGLE_DEVICE "ajdongle"
#define DONGLE_HOST "192.168.7.1"
#define DONGLE_PORT 8080
#define DONGLE_RELAY_HTTP_PORT 80
#define DONGLE_RELAY_RT_PORT 4210
#define DONGLE_RELAY_VIDEO_PORT 4211
#define DONGLE_RELAY_VIDEO_MAX_KBPS 2500

#define DONGLE_SSID_MIN 1
#define DONGLE_SSID_MAX 32
#define DONGLE_PASS_MIN 8
#define DONGLE_PASS_MAX 63

#define DONGLE_PATH_STATUS "/status"
#define DONGLE_PATH_WIFI "/wifi"
#define DONGLE_PATH_OTA "/ota"

#define DONGLE_KEY_PROTO "proto"
#define DONGLE_KEY_OK "ok"
#define DONGLE_KEY_ERROR "error"
#define DONGLE_KEY_ERROR_CODE "code"
#define DONGLE_KEY_ERROR_MESSAGE "message"
#define DONGLE_KEY_ERROR_FIELD "field"

#define DONGLE_KEY_GROUP_DEVICE "device"
#define DONGLE_KEY_DEVICE_ID "id"
#define DONGLE_KEY_DEVICE_FW "fw"
#define DONGLE_KEY_DEVICE_BUILD "build"
#define DONGLE_KEY_DEVICE_ROLLED_BACK "rolled_back"
#define DONGLE_KEY_DEVICE_IDF "idf"

#define DONGLE_KEY_GROUP_USB "usb"
#define DONGLE_KEY_USB_STATE "state"
#define DONGLE_USB_STATE_UP "up"
#define DONGLE_USB_STATE_DOWN "down"
#define DONGLE_USB_STATE_COUNT 2

#define DONGLE_KEY_GROUP_WIFI "wifi"
#define DONGLE_KEY_WIFI_SSID "ssid"
#define DONGLE_KEY_WIFI_CONFIGURED "configured"
#define DONGLE_KEY_WIFI_STATE "state"
#define DONGLE_WIFI_STATE_IDLE "idle"
#define DONGLE_WIFI_STATE_SEARCHING "searching"
#define DONGLE_WIFI_STATE_JOINING "joining"
#define DONGLE_WIFI_STATE_CONNECTED "connected"
#define DONGLE_WIFI_STATE_FAILED "failed"
#define DONGLE_WIFI_STATE_COUNT 5
#define DONGLE_KEY_WIFI_RSSI_DBM "rssi_dbm"
#define DONGLE_KEY_WIFI_CHANNEL "channel"
#define DONGLE_KEY_WIFI_ATTEMPTS "attempts"
#define DONGLE_KEY_WIFI_ATTEMPTS_USED "used"
#define DONGLE_KEY_WIFI_ATTEMPTS_MAX "max"

#define DONGLE_KEY_GROUP_RELAY "relay"
#define DONGLE_KEY_RELAY_TO_CAR_HZ "to_car_hz"
#define DONGLE_KEY_RELAY_TO_PHONE_HZ "to_phone_hz"
#define DONGLE_KEY_RELAY_UDP_SESSIONS "udp_sessions"
#define DONGLE_KEY_RELAY_TCP_CONNECTIONS "tcp_connections"
#define DONGLE_KEY_RELAY_LAST_ERROR "last_error"
#define DONGLE_KEY_RELAY_LAST_ERROR_ERRNO "errno"
#define DONGLE_KEY_RELAY_LAST_ERROR_MESSAGE "message"
#define DONGLE_KEY_RELAY_LAST_ERROR_COUNT "count"
#define DONGLE_KEY_RELAY_LAST_ERROR_AGE_S "age_s"
#define DONGLE_KEY_RELAY_VIDEO_SESSIONS "video_sessions"
#define DONGLE_KEY_RELAY_VIDEO_KBPS "video_kbps"
#define DONGLE_KEY_RELAY_VIDEO_DROPPED "video_dropped"

#define DONGLE_KEY_GROUP_SYSTEM "system"
#define DONGLE_KEY_SYSTEM_UPTIME_S "uptime_s"
#define DONGLE_KEY_SYSTEM_FREE_HEAP "free_heap"

#define DONGLE_WIFI_REQ_SSID "ssid"
#define DONGLE_WIFI_REQ_PASSWORD "password"

#define DONGLE_ERR_BAD_JSON "bad_json"
#define DONGLE_ERR_MISSING_FIELD "missing_field"
#define DONGLE_ERR_UNKNOWN_FIELD "unknown_field"
#define DONGLE_ERR_WRONG_TYPE "wrong_type"
#define DONGLE_ERR_BAD_LENGTH "bad_length"
#define DONGLE_ERR_BAD_CHARS "bad_chars"
#define DONGLE_ERR_RADIO_REFUSED "radio_refused"
#define DONGLE_ERR_TOO_SMALL "too_small"
#define DONGLE_ERR_NOT_FIRMWARE "not_firmware"
#define DONGLE_ERR_WRITE_FAILED "write_failed"
#define DONGLE_ERR_BUSY "busy"
#define DONGLE_ERR_INTERNAL "internal"

#endif /* DONGLE_CONTRACT_INC */
