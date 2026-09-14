/* generated from contract/car-api.json by tools/gen_contract.py - do not edit */

#include "cfg_contract.h"

static const int32_t CFG_WHEEL_QUADRATURE_ALLOWED[] = { 1, 2, 4 };

static const cfg_field_t CFG_RAMP_FIELDS[] = {
    { "rise_ms", CFG_INT, 0, 2000, 300, NULL, 0, 1 },
};
static const cfg_field_t CFG_TRIM_FIELDS[] = {
    { "balance_pct", CFG_INT, -30, 30, 0, NULL, 0, 1 },
};
static const cfg_field_t CFG_RECOVER_FIELDS[] = {
    { "enabled", CFG_BOOL, 0, 1, 1, NULL, 0, 1 },
    { "window_ms", CFG_INT, 1000, 10000, 5000, NULL, 0, 1 },
};
static const cfg_field_t CFG_WHEEL_FIELDS[] = {
    { "diameter_mm", CFG_INT, 20, 150, 65, NULL, 0, 1 },
    { "encoder_ppr", CFG_INT, 1, 1000, 11, NULL, 0, 1 },
    { "gear_ratio", CFG_FIXED, 100, 30000, 900, NULL, 0, 100 },
    { "quadrature", CFG_ENUM, 1, 4, 4, CFG_WHEEL_QUADRATURE_ALLOWED, 3, 1 },
};
static const cfg_field_t CFG_DIMS_FIELDS[] = {
    { "track_mm", CFG_INT, 60, 300, 130, NULL, 0, 1 },
    { "wheelbase_mm", CFG_INT, 90, 360, 210, NULL, 0, 1 },
};
static const cfg_field_t CFG_VIDEO_FIELDS[] = {
    { "bitrate_kbps", CFG_INT, 500, 3000, 1500, NULL, 0, 1 },
};

static const cfg_domain_t CFG_DOMAINS[] = {
    { "ramp", "ramp", CFG_RAMP_FIELDS, 1 },
    { "trim", "trim", CFG_TRIM_FIELDS, 1 },
    { "recovery", "recover", CFG_RECOVER_FIELDS, 2 },
    { "wheel", "wheel", CFG_WHEEL_FIELDS, 4 },
    { "chassis", "dims", CFG_DIMS_FIELDS, 2 },
    { "video", "video", CFG_VIDEO_FIELDS, 1 },
};

#define CFG_CONFIG_PATH "/config"
#define CFG_DOMAIN_COUNT 6
#define CFG_MAX_FIELDS 4

#define RT_PROTO 2
#define RT_PORT 4210
#define RT_MAX_DATAGRAM 320
#define RT_MAX_COMMAND 96
#define RT_COMMAND_HZ 10
#define RT_TELEMETRY_HZ 5
#define RT_WATCHDOG_MS 300
#define RT_SESSION_IDLE_MS 10000
#define RT_KEY_PROTO "proto"
#define RT_KEY_TYPE "type"
#define RT_KEY_SESSION "session"
#define RT_KEY_SEQ "seq"
#define RT_KEY_THROTTLE "throttle"
#define RT_KEY_TURN "turn"
#define RT_KEY_KEY "key"
#define RT_TYPE_HELLO "hello"
#define RT_TYPE_HELLO_ACK "hello_ack"
#define RT_TYPE_DRIVE "drive"
#define RT_TYPE_BYE "bye"
#define RT_TYPE_TELEMETRY "telemetry"
#define RT_TYPE_VIEW "view"

#define KEY_PROTO "proto"
#define KEY_OK "ok"
#define KEY_ERROR "error"
#define KEY_ERROR_CODE "code"
#define KEY_ERROR_MESSAGE "message"
#define KEY_ERROR_FIELD "field"

#define PATH_ROOT "/"
#define PATH_STATUS "/status"
#define PATH_CONFIG "/config"
#define PATH_CALIBRATION "/calibration"
#define PATH_SPIN "/calibration/spin"
#define PATH_OTA "/ota"
#define PATH_SNAPSHOT "/snapshot"

#define KEY_GROUP_DEVICE "device"
#define KEY_DEVICE_ID "id"
#define KEY_DEVICE_FW "fw"
#define KEY_DEVICE_BUILD "build"
#define KEY_DEVICE_ROLLED_BACK "rolled_back"

#define KEY_GROUP_LINK "link"
#define KEY_LINK_RX_HZ "rx_hz"
#define KEY_LINK_RSSI_DBM "rssi_dbm"
#define KEY_LINK_TIMEOUTS "timeouts"

#define KEY_GROUP_MOTORS "motors"
#define KEY_MOTORS_BUS "bus"
#define MOTORS_BUS_OK "ok"
#define MOTORS_BUS_DOWN "down"
#define MOTORS_BUS_COUNT 2
#define KEY_MOTORS_CALIBRATED "calibrated"
#define KEY_MOTORS_OWNER "owner"
#define MOTORS_OWNER_IDLE "idle"
#define MOTORS_OWNER_RECOVERING "recovering"
#define MOTORS_OWNER_CONSOLE "console"
#define MOTORS_OWNER_REMOTE "remote"
#define MOTORS_OWNER_CALIBRATION "calibration"
#define MOTORS_OWNER_UPDATE "update"
#define MOTORS_OWNER_SAFE_STOP "safe_stop"
#define MOTORS_OWNER_COUNT 7

#define KEY_GROUP_RADIO "radio"
#define KEY_RADIO_FW "fw"
#define KEY_RADIO_EXPECTED "expected"
#define KEY_RADIO_STATE "state"
#define RADIO_STATE_OK "ok"
#define RADIO_STATE_MISMATCH "mismatch"
#define RADIO_STATE_UNAVAILABLE "unavailable"
#define RADIO_STATE_COUNT 3

#define KEY_GROUP_STORAGE "storage"
#define KEY_STORAGE_RESET_AT_BOOT "reset_at_boot"

#define KEY_GROUP_SYSTEM "system"
#define KEY_SYSTEM_UPTIME_S "uptime_s"
#define KEY_SYSTEM_FREE_HEAP "free_heap"

#define KEY_GROUP_VIDEO "video"
#define KEY_VIDEO_STATE "state"
#define VIDEO_STATE_OFF "off"
#define VIDEO_STATE_IDLE "idle"
#define VIDEO_STATE_STREAMING "streaming"
#define VIDEO_STATE_COUNT 3
#define KEY_VIDEO_FPS "fps"
#define KEY_VIDEO_KBPS "kbps"
#define KEY_VIDEO_DROPPED "dropped"

#define ERR_BAD_JSON "bad_json"
#define ERR_MISSING_FIELD "missing_field"
#define ERR_UNKNOWN_FIELD "unknown_field"
#define ERR_WRONG_TYPE "wrong_type"
#define ERR_OUT_OF_RANGE "out_of_range"
#define ERR_NOT_ALLOWED "not_allowed"
#define ERR_BUSY "busy"
#define ERR_TOO_SMALL "too_small"
#define ERR_NOT_FIRMWARE "not_firmware"
#define ERR_WRITE_FAILED "write_failed"
#define ERR_INTERNAL "internal"

#define CORNER_FRONT_LEFT "front_left"
#define CORNER_FRONT_RIGHT "front_right"
#define CORNER_REAR_LEFT "rear_left"
#define CORNER_REAR_RIGHT "rear_right"
#define CORNER_COUNT 4
#define DIRECTION_FORWARD "forward"
#define DIRECTION_REVERSE "reverse"
#define CALIB_PAIRS 4
#define KEY_CALIB_CALIBRATED "calibrated"
#define KEY_CALIB_WHEELS "wheels"
#define KEY_CALIB_CORNER "corner"
#define KEY_CALIB_PAIR "pair"
#define KEY_CALIB_INVERTED "inverted"
#define KEY_CALIB_DIRECTION "direction"

#define VIDEO_PORT 4211
#define VIDEO_WIDTH 1280
#define VIDEO_HEIGHT 960
#define VIDEO_FPS 15
#define VIDEO_SENSOR_FPS 45
#define VIDEO_CHUNK_BYTES 1400
#define VIDEO_HEADER_BYTES 12
#define VIDEO_WIRE_PROTO 1
#define VIDEO_SUBSCRIBE_MS 1000
#define VIDEO_SUBSCRIBE_TIMEOUT_MS 3000
#define VIDEO_KEYFRAME_S 3
#define VIDEO_IDR_MIN_MS 250
#define VIDEO_ROTATION 0
#define VIDEO_MIRROR 0
