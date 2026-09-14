# generated from contract/car-api.json by tools/gen_contract.py - do not edit
import math

PROTO = 2
DEVICE = 'ajmiddlecar'
ENVELOPE = {'proto': 'proto', 'ok': 'ok', 'error': 'error', 'code': 'code', 'message': 'message', 'field': 'field'}
ENDPOINTS = {'root': '/', 'status': '/status', 'config': '/config', 'calibration': '/calibration', 'spin': '/calibration/spin', 'ota': '/ota', 'snapshot': '/snapshot'}
ERRORS = ['bad_json', 'missing_field', 'unknown_field', 'wrong_type', 'out_of_range', 'not_allowed', 'busy', 'too_small', 'not_firmware', 'write_failed', 'internal']
STATUS_GROUPS = ['device', 'link', 'motors', 'radio', 'storage', 'system', 'video']
GROUPS = {   'device': {   'swift': 'DeviceInfo',
                  'doc': 'Who is answering. The same object in the hello reply and in /status.',
                  'fields': [   {   'name': 'id',
                                    'type': 'str',
                                    'doc': 'the device name; ajmiddlecar for this car'},
                                {   'name': 'fw',
                                    'type': 'str',
                                    'doc': 'firmware version, v<semver>+<build>'},
                                {   'name': 'build',
                                    'type': 'int',
                                    'doc': 'the number after + in fw, as an integer'},
                                {   'name': 'rolled_back',
                                    'type': 'bool',
                                    'doc': 'the bootloader reverted the last update'}]},
    'link': {   'swift': 'LinkInfo',
                'doc': 'The control link as the car sees it.',
                'fields': [   {   'name': 'rx_hz',
                                  'type': 'int',
                                  'doc': 'drive datagrams received per second'},
                              {   'name': 'rssi_dbm',
                                  'type': 'int',
                                  'nullable': True,
                                  'doc': "the driving station's signal at the car, null when "
                                         'not measured'},
                              {   'name': 'timeouts',
                                  'type': 'int',
                                  'doc': 'control-watchdog trips since boot'}]},
    'motors': {   'swift': 'MotorsInfo',
                  'doc': 'The actuator: its bus, its calibration, and who is commanding it.',
                  'fields': [   {   'name': 'bus',
                                    'type': 'state',
                                    'swift': 'MotorsBus',
                                    'values': ['ok', 'down'],
                                    'doc': 'both PWM boards up and the last write accepted'},
                                {   'name': 'calibrated',
                                    'type': 'bool',
                                    'doc': 'a valid wheel calibration is loaded'},
                                {   'name': 'owner',
                                    'type': 'state',
                                    'swift': 'MotorsOwner',
                                    'values': [   'idle',
                                                  'recovering',
                                                  'console',
                                                  'remote',
                                                  'calibration',
                                                  'update',
                                                  'safe_stop'],
                                    'doc': 'which source owns the actuator'}]},
    'radio': {   'swift': 'RadioInfo',
                 'doc': "The C6 radio co-processor's firmware against what this build expects.",
                 'fields': [   {   'name': 'fw',
                                   'type': 'str',
                                   'nullable': True,
                                   'doc': "the radio's version, null when it did not answer"},
                               {   'name': 'expected',
                                   'type': 'str',
                                   'doc': 'the version this build was made for'},
                               {   'name': 'state',
                                   'type': 'state',
                                   'swift': 'RadioState',
                                   'values': ['ok', 'mismatch', 'unavailable'],
                                   'doc': 'ok when fw equals expected'}]},
    'storage': {   'swift': 'StorageInfo',
                   'doc': 'What happened to the settings at this boot.',
                   'fields': [   {   'name': 'reset_at_boot',
                                     'type': 'bool',
                                     'doc': 'the NVS format changed and every setting went '
                                            'back to its default'}]},
    'system': {   'swift': 'SystemInfo',
                  'doc': 'Uptime and memory.',
                  'fields': [   {   'name': 'uptime_s',
                                    'type': 'int',
                                    'doc': 'seconds since boot'},
                                {   'name': 'free_heap',
                                    'type': 'int',
                                    'doc': 'free heap in bytes'}]},
    'video': {   'swift': 'VideoInfo',
                 'doc': 'The camera and the FPV stream.',
                 'fields': [   {   'name': 'state',
                                   'type': 'state',
                                   'swift': 'VideoState',
                                   'values': ['off', 'idle', 'streaming'],
                                   'doc': 'off: no sensor answered at boot; idle: sensor in '
                                          'standby, nobody watching; streaming: encoding for '
                                          'the driver'},
                               {   'name': 'fps',
                                   'type': 'int',
                                   'doc': 'frames encoded in the last second'},
                               {   'name': 'kbps',
                                   'type': 'int',
                                   'doc': 'kbit sent in the last second'},
                               {   'name': 'dropped',
                                   'type': 'int',
                                   'doc': 'frames not sent since boot: the sender was still '
                                          'busy with the previous frame, the encoder '
                                          'overflowed, or the frame needed more than 255 '
                                          'chunks'}]}}
NETWORK = {'ssid': 'AJMiddleCar', 'password': 'drive1234'}
RT = {'port': 4210, 'max_datagram': 320, 'max_command': 96, 'command_hz': 10, 'telemetry_hz': 5, 'watchdog_ms': 300, 'session_idle_ms': 10000, 'keys': {'proto': 'proto', 'type': 'type', 'session': 'session', 'seq': 'seq', 'throttle': 'throttle', 'turn': 'turn', 'key': 'key'}, 'types': {'hello': 'hello', 'hello_ack': 'hello_ack', 'drive': 'drive', 'bye': 'bye', 'telemetry': 'telemetry', 'view': 'view'}, 'doc': 'Every datagram carries proto and type. session is the session id: producers send 8 hex characters; acceptors take 1-15 alphanumerics. drive and bye carry seq; hello and view do not. view (on the video port) may carry key:true to ask for a keyframe.'}
TELEMETRY_GROUPS = ['link', 'motors', 'system', 'video']
CALIBRATION = {'swift': 'Calibration', 'wheel_swift': 'CalibWheel', 'corners': ['front_left', 'front_right', 'rear_left', 'rear_right'], 'directions': ['forward', 'reverse'], 'pairs': 4, 'keys': {'calibrated': 'calibrated', 'wheels': 'wheels', 'corner': 'corner', 'pair': 'pair', 'inverted': 'inverted', 'direction': 'direction'}, 'doc': 'GET /calibration returns calibrated and the wheels table (empty when not calibrated). POST /calibration takes wheels: four corners, each once, pairs 0..3 each once, inverted per wheel. POST /calibration/spin takes pair and direction.'}
VIDEO = {'port': 4211, 'width': 1280, 'height': 960, 'fps': 15, 'sensor_fps': 45, 'chunk_bytes': 1400, 'header_bytes': 12, 'wire_proto': 1, 'subscribe_ms': 1000, 'subscribe_timeout_ms': 3000, 'keyframe_s': 3, 'idr_min_ms': 250, 'rotation': 0, 'mirror': False, 'doc': 'The FPV stream: H.264 Annex B chunks on UDP `port`, one chunk per datagram. Header, big-endian: u8 proto (wire_proto), u8 flags (bit0 keyframe), u8 stream (+1 per stream start), u8 reserved (0), u16 frame (from 0 per stream, modulo 2^16), u8 chunk, u8 count, u32 captured_ms. Every chunk but the last is exactly chunk_bytes. The phone subscribes with a `view` datagram on the same port every subscribe_ms; subscribe_timeout_ms without one stops the stream. keyframe_s is the planned IDR period; a `view` with key:true forces one, at most every idr_min_ms. rotation/mirror are applied by the viewer.', 'vectors': [{'name': 'keyframe, first chunk', 'header': {'proto': 1, 'flags': 1, 'stream': 3, 'frame': 7, 'chunk': 0, 'count': 30, 'captured_ms': 812345}, 'bytes': '010103000007001e000c6539', 'valid': True}, {'name': "p-frame at the counter's edge, last chunk", 'header': {'proto': 1, 'flags': 0, 'stream': 255, 'frame': 65535, 'chunk': 2, 'count': 3, 'captured_ms': 4294967295}, 'bytes': '0100ff00ffff0203ffffffff', 'valid': True}, {'name': 'foreign proto', 'bytes': '020103000007001e000c6539', 'valid': False}, {'name': 'chunk not below count', 'bytes': '0101030000071e1e000c6539', 'valid': False}, {'name': 'count zero', 'bytes': '010103000007000000000000', 'valid': False}, {'name': 'reserved byte set', 'bytes': '010103010007001e000c6539', 'valid': False}, {'name': 'unknown flag', 'bytes': '010203000007001e000c6539', 'valid': False}]}
CONFIG_PATH = '/config'
DOMAINS = {   'ramp': {   'nvs_key': 'ramp',
                'defaults': {'rise_ms': 300},
                'fields': [   {   'name': 'rise_ms',
                                  'type': 'int',
                                  'min': 0,
                                  'max': 2000,
                                  'default': 300,
                                  'doc': 'time from zero to full scale in ms; 0 disables the '
                                         'ramp',
                                  'scale': 1}]},
    'trim': {   'nvs_key': 'trim',
                'defaults': {'balance_pct': 0},
                'fields': [   {   'name': 'balance_pct',
                                  'type': 'int',
                                  'min': -30,
                                  'max': 30,
                                  'default': 0,
                                  'doc': 'percentage by which the faster side is slowed',
                                  'scale': 1}]},
    'recovery': {   'nvs_key': 'recover',
                    'defaults': {'enabled': True, 'window_ms': 5000},
                    'fields': [   {   'name': 'enabled',
                                      'type': 'bool',
                                      'default': True,
                                      'doc': 'retrace on unexpected silence; when false the '
                                             'car stops instead',
                                      'scale': 1},
                                  {   'name': 'window_ms',
                                      'type': 'int',
                                      'min': 1000,
                                      'max': 10000,
                                      'default': 5000,
                                      'doc': 'how far back the breadcrumb history reaches',
                                      'scale': 1}]},
    'wheel': {   'nvs_key': 'wheel',
                 'defaults': {   'diameter_mm': 65,
                                 'encoder_ppr': 11,
                                 'gear_ratio': 900,
                                 'quadrature': 4},
                 'fields': [   {   'name': 'diameter_mm',
                                   'type': 'int',
                                   'min': 20,
                                   'max': 150,
                                   'default': 65,
                                   'doc': 'wheel diameter in mm',
                                   'scale': 1},
                               {   'name': 'encoder_ppr',
                                   'type': 'int',
                                   'min': 1,
                                   'max': 1000,
                                   'default': 11,
                                   'doc': 'encoder pulses per motor-shaft revolution, one '
                                          'channel',
                                   'scale': 1},
                               {   'name': 'gear_ratio',
                                   'type': 'fixed',
                                   'scale': 100,
                                   'min': 100,
                                   'max': 30000,
                                   'default': 900,
                                   'doc': 'gear ratio as a decimal; 1:9 is 9.0 (held as ratio '
                                          'x100 inside)'},
                               {   'name': 'quadrature',
                                   'type': 'enum',
                                   'values': [1, 2, 4],
                                   'default': 4,
                                   'doc': 'quadrature edge multiplier',
                                   'scale': 1}]},
    'chassis': {   'nvs_key': 'dims',
                   'defaults': {'track_mm': 130, 'wheelbase_mm': 210},
                   'fields': [   {   'name': 'track_mm',
                                     'type': 'int',
                                     'min': 60,
                                     'max': 300,
                                     'default': 130,
                                     'doc': 'lateral distance between left and right wheel '
                                            'centres',
                                     'scale': 1},
                                 {   'name': 'wheelbase_mm',
                                     'type': 'int',
                                     'min': 90,
                                     'max': 360,
                                     'default': 210,
                                     'doc': 'longitudinal distance between front and rear '
                                            'wheel centres',
                                     'scale': 1}]},
    'video': {   'nvs_key': 'video',
                 'defaults': {'bitrate_kbps': 1000},
                 'fields': [   {   'name': 'bitrate_kbps',
                                   'type': 'int',
                                   'min': 500,
                                   'max': 3000,
                                   'default': 1000,
                                   'doc': "target H.264 bitrate in kbit/s; the adapter's USB "
                                          'is the ceiling — measured at ~1.5 Mbit/s sustained '
                                          'on the bench (2026-09-15), above which chunks are '
                                          'lost, so the default sits under it with room for '
                                          'keyframes',
                                   'scale': 1}]}}


def lround(x):
    """C's lround: half away from zero, so 9.005 x 100 is 901 here and on the car."""
    return int(math.copysign(math.floor(abs(x) + 0.5), x))


def to_wire(key, values):
    """A domain's internal integers -> the JSON the car answers: fixed fields as decimals."""
    out = {}
    for f in DOMAINS[key]["fields"]:
        v = values[f["name"]]
        out[f["name"]] = v / f["scale"] if f["type"] == "fixed" else v
    return out


def from_wire(key, obj):
    """A validated domain object -> internal integers: fixed fields x scale, rounded."""
    out = {}
    for f in DOMAINS[key]["fields"]:
        v = obj[f["name"]]
        out[f["name"]] = lround(v * f["scale"]) if f["type"] == "fixed" else v
    return out


def validate_config(body):
    """Return (True, None) or (False, (code, field, message)). Mirrors cfg_api.c exactly:
    the body is an object of domain objects, each present domain complete, unknown keys
    refused at both levels, numbers typed the way cJSON types them (a JSON boolean is not
    a number; a fraction is not an integer)."""
    if not isinstance(body, dict):
        return False, ("bad_json", "", "expected a JSON object")
    if not body:
        return False, ("missing_field", "", "no configuration domain in the body")
    for key in body:
        if key not in DOMAINS:
            return False, ("unknown_field", key, f"{key} is not a configuration domain")
    for key, domain in DOMAINS.items():
        if key not in body:
            continue
        obj = body[key]
        if not isinstance(obj, dict):
            return False, ("wrong_type", key, f"{key} must be an object")
        names = {f["name"] for f in domain["fields"]}
        for k in obj:
            if k not in names:
                return False, ("unknown_field", f"{key}.{k}", f"{key} has no field {k}")
        for f in domain["fields"]:
            name = f["name"]
            where = f"{key}.{name}"
            if name not in obj:
                return False, ("missing_field", where, f"{where} is required")
            v = obj[name]
            if f["type"] == "bool":
                if not isinstance(v, bool):
                    return False, ("wrong_type", where, f"{where} must be a boolean")
                continue
            # bool is a subclass of int in Python, so True would sneak past a plain
            # isinstance check. cJSON_IsNumber does not accept a JSON boolean either.
            if isinstance(v, bool) or not isinstance(v, (int, float)):
                return False, ("wrong_type", where, f"{where} must be a number")
            if f["type"] == "fixed":
                scaled = lround(v * f["scale"])
                if not (f["min"] <= scaled <= f["max"]):
                    lo, hi = f["min"] / f["scale"], f["max"] / f["scale"]
                    return False, ("out_of_range", where, f"{where} must be {lo:g}..{hi:g}")
                continue
            if v != int(v):
                return False, ("wrong_type", where, f"{where} must be an integer")
            v = int(v)
            if f["type"] == "enum":
                if v not in f["values"]:
                    return False, ("not_allowed", where, f"{where} must be one of {f['values']}")
            elif not (f["min"] <= v <= f["max"]):
                return False, ("out_of_range", where, f"{where} must be {f['min']}..{f['max']}")
    return True, None
