/* generated from contract/dongle-api.json by tools/gen_contract.py - do not edit */

#ifndef DONGLE_CONTRACT_INC
#define DONGLE_CONTRACT_INC

#define DONGLE_DEVICE "ajdongle"
#define DONGLE_HOST "192.168.7.1"
#define DONGLE_PORT 8080
#define DONGLE_RELAY_HTTP_PORT 80
#define DONGLE_RELAY_RT_PORT 4210

#define DONGLE_PATH_STATUS "/status"
#define DONGLE_PATH_NET "/net"
#define DONGLE_PATH_OTA "/ota"

#define DONGLE_SSID_MIN 1
#define DONGLE_SSID_MAX 32
#define DONGLE_PASS_MIN 8
#define DONGLE_PASS_MAX 63

#define DONGLE_KEY_DEVICE "device"
#define DONGLE_KEY_FW "fw"
#define DONGLE_KEY_IDF "idf"
#define DONGLE_KEY_USB "usb"
#define DONGLE_KEY_ROLLBACK "rollback"
#define DONGLE_KEY_NET "net"
#define DONGLE_KEY_NET_SSID "ssid"
#define DONGLE_KEY_NET_STATE "state"
#define DONGLE_KEY_NET_RSSI "rssi"

#define DONGLE_NETKEY_SSID "ssid"
#define DONGLE_NETKEY_PASSWORD "password"
#define DONGLE_NETKEY_CONFIGURED "configured"

#define DONGLE_STATE_IDLE "idle"
#define DONGLE_STATE_SEARCHING "searching"
#define DONGLE_STATE_JOINING "joining"
#define DONGLE_STATE_CONNECTED "connected"
#define DONGLE_STATE_FAILED "failed"

#define DONGLE_USB_STATE_UP "up"

#endif /* DONGLE_CONTRACT_INC */
