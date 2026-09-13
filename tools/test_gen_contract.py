"""Host tests for the contract schema and its generator. Stdlib only."""
import json
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "contract" / "car-api.json"


def load():
    return json.loads(SCHEMA.read_text())


class TestSchema(unittest.TestCase):
    def test_top_level(self):
        s = load()
        self.assertEqual(s["proto"], 2)
        self.assertEqual(s["device"], "ajmiddlecar")
        self.assertEqual(s["network"]["ssid"], "AJMiddleCar")
        # No host: the app reaches the car only through the dongle.
        self.assertNotIn("host", s["network"])
        self.assertEqual(s["envelope"], {"proto": "proto", "ok": "ok", "error": "error",
                                         "code": "code", "message": "message", "field": "field"})
        self.assertEqual(s["endpoints"], {"root": "/", "status": "/status", "config": "/config",
                                          "calibration": "/calibration",
                                          "spin": "/calibration/spin", "ota": "/ota"})

    def test_rt_constants_and_vocabulary(self):
        rt = load()["rt"]
        self.assertEqual(rt["port"], 4210)
        self.assertEqual(rt["max_datagram"], 320)
        self.assertEqual(rt["max_command"], 96)
        self.assertLess(rt["max_command"], rt["max_datagram"])
        self.assertEqual(rt["command_hz"], 10)
        self.assertEqual(rt["telemetry_hz"], 5)
        self.assertEqual(rt["watchdog_ms"], 300)
        self.assertEqual(rt["session_idle_ms"], 10000)
        self.assertGreater(rt["session_idle_ms"], rt["watchdog_ms"] * 10)
        self.assertEqual(rt["keys"], {"proto": "proto", "type": "type", "session": "session",
                                      "seq": "seq", "throttle": "throttle", "turn": "turn"})
        self.assertEqual(rt["types"], {"hello": "hello", "hello_ack": "hello_ack",
                                       "drive": "drive", "bye": "bye", "telemetry": "telemetry"})

    def test_groups(self):
        g = load()["groups"]
        self.assertEqual(list(g), ["device", "link", "motors", "radio", "storage", "system"])
        names = {k: [f["name"] for f in v["fields"]] for k, v in g.items()}
        self.assertEqual(names["device"], ["id", "fw", "build", "rolled_back"])
        self.assertEqual(names["link"], ["rx_hz", "rssi_dbm", "timeouts"])
        self.assertEqual(names["motors"], ["bus", "calibrated", "owner"])
        self.assertEqual(names["radio"], ["fw", "expected", "state"])
        self.assertEqual(names["storage"], ["reset_at_boot"])
        self.assertEqual(names["system"], ["uptime_s", "free_heap"])
        owner = next(f for f in g["motors"]["fields"] if f["name"] == "owner")
        self.assertEqual(owner["type"], "state")
        self.assertEqual(owner["values"], ["idle", "recovering", "console", "remote",
                                           "calibration", "update", "safe_stop"])
        self.assertEqual(owner["swift"], "MotorsOwner")
        rssi = next(f for f in g["link"]["fields"] if f["name"] == "rssi_dbm")
        self.assertTrue(rssi.get("nullable"))
        for k, v in g.items():
            self.assertTrue(v["swift"], k)
            for f in v["fields"]:
                self.assertIn(f["type"], ("int", "bool", "str", "state"), f"{k}.{f['name']}")
                self.assertTrue(f["doc"].strip(), f"{k}.{f['name']}")
                if f["type"] == "state":
                    self.assertTrue(f["swift"], f"{k}.{f['name']}")
                    self.assertEqual(len(f["values"]), len(set(f["values"])))

    def test_telemetry_and_status_pick_groups_that_exist(self):
        s = load()
        self.assertEqual(s["telemetry"]["groups"], ["link", "motors", "system"])
        self.assertEqual(s["status"]["groups"],
                         ["device", "link", "motors", "radio", "storage", "system"])
        for name in s["telemetry"]["groups"] + s["status"]["groups"]:
            self.assertIn(name, s["groups"])
        self.assertEqual(s["telemetry"]["swift"], "Telemetry")
        self.assertEqual(s["status"]["swift"], "CarStatus")

    def test_config_domains(self):
        c = load()["config"]
        self.assertEqual(c["path"], "/config")
        self.assertEqual(c["swift"], "CarConfig")
        keys = [d["key"] for d in c["domains"]]
        self.assertEqual(keys, ["ramp", "trim", "recovery", "wheel", "chassis"])
        self.assertEqual([d["nvs_key"] for d in c["domains"]],
                         ["ramp", "trim", "recover", "wheel", "dims"])
        self.assertEqual([d["swift"] for d in c["domains"]],
                         ["Ramp", "Trim", "Recovery", "Wheel", "Chassis"])
        for d in c["domains"]:
            self.assertTrue(d["fields"], d["key"])
            for f in d["fields"]:
                where = f"{d['key']}.{f['name']}"
                self.assertTrue(re.fullmatch(r"[a-z][a-z0-9_]*", f["name"]), where)
                self.assertIn(f["type"], ("int", "bool", "enum", "fixed"), where)
                self.assertTrue(f["doc"].strip(), where)
                if f["type"] in ("int", "fixed"):
                    self.assertLess(f["min"], f["max"], where)
                    self.assertGreaterEqual(f["default"], f["min"], where)
                    self.assertLessEqual(f["default"], f["max"], where)
                    if f["type"] == "fixed":
                        self.assertGreater(f["scale"], 1, where)
                elif f["type"] == "enum":
                    self.assertIn(f["default"], f["values"], where)
                    self.assertEqual(len(f["values"]), len(set(f["values"])), where)
                else:
                    self.assertIsInstance(f["default"], bool, where)

    def test_ranges_match_the_firmware_today(self):
        """The schema must describe the firmware that exists, not one we imagined."""
        main = ROOT / "firmware" / "car" / "core" / "main"
        file_for_key = {"wheel": "wheel.h", "chassis": "dims.h", "recovery": "recovery.h",
                        "ramp": "ramp.c", "trim": "car.c"}
        src_by_file = {n: (main / n).read_text() for n in set(file_for_key.values())}
        expected = {
            ("wheel", "diameter_mm"): (20, 150), ("wheel", "encoder_ppr"): (1, 1000),
            ("wheel", "gear_ratio"): (100, 30000),
            ("chassis", "track_mm"): (60, 300), ("chassis", "wheelbase_mm"): (90, 360),
            ("recovery", "window_ms"): (1000, 10000),
            ("ramp", "rise_ms"): (0, 2000), ("trim", "balance_pct"): (-30, 30),
        }
        got = {}
        for d in load()["config"]["domains"]:
            for f in d["fields"]:
                if f["type"] in ("int", "fixed"):
                    got[(d["key"], f["name"])] = (f["min"], f["max"])
        self.assertEqual(got, expected)
        for (key, name), (lo, hi) in expected.items():
            src = src_by_file[file_for_key[key]]
            for bound in (lo, hi):
                pat = rf"(?<![\w.-]){re.escape(str(bound))}(?![\w.])"
                self.assertRegex(src, pat, f"{key} {name}: bound {bound} not in {file_for_key[key]}")

    def test_calibration_and_errors(self):
        s = load()
        c = s["calibration"]
        self.assertEqual(c["corners"], ["front_left", "front_right", "rear_left", "rear_right"])
        self.assertEqual(c["directions"], ["forward", "reverse"])
        self.assertEqual(c["pairs"], 4)
        self.assertEqual(c["keys"], {"calibrated": "calibrated", "wheels": "wheels",
                                     "corner": "corner", "pair": "pair",
                                     "inverted": "inverted", "direction": "direction"})
        self.assertEqual(s["errors"], ["bad_json", "missing_field", "unknown_field", "wrong_type",
                                       "out_of_range", "not_allowed", "busy", "too_small",
                                       "not_firmware", "write_failed", "internal"])


import filecmp
import subprocess
import sys
import tempfile

sys.path.insert(0, str(ROOT / "tools"))


class TestArtifactListing(unittest.TestCase):
    """The generator owns the list of what it writes; check_contract.sh asks rather
    than keeping a second copy that has to be edited in step."""

    def test_list_artifacts_names_every_whole_file_exactly(self):
        # An exact, ordered comparison, not assertIn: assertIn only proves the known
        # entries are present, so a table that gained an entry nobody meant to add
        # (or lost one silently) would still pass. This list is pinned to what
        # TARGETS actually holds today, car and dongle both — a real new artifact
        # updates this test deliberately, which is the point.
        out = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "gen_contract.py"), "--list-artifacts"],
            capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual(out, [
            "firmware/car/core/main/cfg_table.inc",
            "app/AJMiddleCar/Generated/CarAPI.swift",
            "tools/mock_car/generated.py",
            "firmware/dongle/main/dongle_contract.inc",
            "app/AJMiddleCar/Generated/DongleAPI.swift",
        ])

    def test_list_artifacts_excludes_spliced_files(self):
        # docs/protocol.md is spliced into hand-written prose and is compared by
        # region, so a caller that diffs whole files must not be handed it.
        out = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "gen_contract.py"), "--list-artifacts"],
            capture_output=True, text=True, check=True).stdout.split()
        self.assertNotIn("docs/protocol.md", out)


class TestDocEmitter(unittest.TestCase):
    def test_table_has_a_row_per_domain_with_ranges(self):
        import gen_contract
        out = gen_contract.emit_doc(load())
        self.assertIn("| `/wheel` |", out)
        self.assertIn("`diameter_mm` 20..150", out)
        self.assertIn("`quad` 1 \\| 2 \\| 4", out)
        self.assertIn("`enabled` true \\| false", out)
        for path in ("/ramp", "/trim", "/recover", "/wheel", "/dims"):
            self.assertIn(f"| `{path}` |", out)

    def test_splice_replaces_only_the_marked_region(self):
        import gen_contract
        doc = ("keep me\n" + gen_contract.MARK_BEGIN + "\nstale\n"
               + gen_contract.MARK_END + "\nkeep me too\n")
        out = gen_contract.splice(doc, "fresh")
        self.assertIn("keep me", out)
        self.assertIn("keep me too", out)
        self.assertIn("fresh", out)
        self.assertNotIn("stale", out)

    def test_splice_refuses_a_document_without_markers(self):
        import gen_contract
        with self.assertRaises(ValueError):
            gen_contract.splice("no markers here", "fresh")


class TestDeterminism(unittest.TestCase):
    def test_two_runs_are_byte_identical(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            for d in (a, b):
                r = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_contract.py"),
                                    "--out-dir", d], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
            same, diff, funny = filecmp.cmpfiles(
                a, b, [str(q.relative_to(a)) for q in pathlib.Path(a).rglob("*") if q.is_file()],
                shallow=False)
            self.assertEqual(diff, [])
            self.assertEqual(funny, [])
            self.assertTrue(same, "the generator wrote nothing")


class TestCommonEmitters(unittest.TestCase):
    def setUp(self):
        import gen_common
        self.c = gen_common
        self.s = load()

    def test_group_defines_cover_keys_states_and_counts(self):
        out = "\n".join(self.c.c_group_defines(self.s, ""))
        self.assertIn('#define KEY_GROUP_LINK "link"', out)
        self.assertIn('#define KEY_LINK_RSSI_DBM "rssi_dbm"', out)
        self.assertIn('#define MOTORS_OWNER_REMOTE "remote"', out)
        self.assertIn('#define MOTORS_OWNER_SAFE_STOP "safe_stop"', out)
        self.assertIn("#define MOTORS_OWNER_COUNT 7", out)
        self.assertIn('#define RADIO_STATE_UNAVAILABLE "unavailable"', out)
        prefixed = "\n".join(self.c.c_group_defines(self.s, "X_"))
        self.assertIn('#define X_KEY_GROUP_LINK "link"', prefixed)

    def test_object_fields_get_sub_keys(self):
        d = json.loads((ROOT / "contract" / "dongle-api.json").read_text())
        out = "\n".join(self.c.c_group_defines(d, "DONGLE_"))
        self.assertIn('#define DONGLE_KEY_WIFI_ATTEMPTS "attempts"', out)
        self.assertIn('#define DONGLE_KEY_WIFI_ATTEMPTS_USED "used"', out)
        self.assertIn('#define DONGLE_KEY_RELAY_LAST_ERROR_AGE_S "age_s"', out)
        self.assertIn('#define DONGLE_WIFI_STATE_SEARCHING "searching"', out)
        self.assertIn('#define DONGLE_USB_STATE_DOWN "down"', out)

    def test_envelope_errors_and_endpoints(self):
        env = "\n".join(self.c.c_envelope_defines(self.s, ""))
        self.assertIn('#define KEY_ERROR_CODE "code"', env)
        self.assertIn('#define KEY_PROTO "proto"', env)
        errs = "\n".join(self.c.c_error_defines(self.s, ""))
        self.assertIn('#define ERR_OUT_OF_RANGE "out_of_range"', errs)
        paths = "\n".join(self.c.c_endpoint_defines(self.s, ""))
        self.assertIn('#define PATH_SPIN "/calibration/spin"', paths)

    def test_swift_state_enum_has_unknown_and_round_trips(self):
        out = "\n".join(self.c.swift_state_enum("MotorsBus", ["ok", "down"], "doc"))
        self.assertIn("public enum MotorsBus: Equatable, Sendable, Codable {", out)
        self.assertIn("    case ok", out)
        self.assertIn("    case unknown(String)", out)
        self.assertIn('        case "down": self = .down', out)
        self.assertIn("        default: self = .unknown(rawValue)", out)
        self.assertIn("    public static let all: [MotorsBus] = [.ok, .down]", out)

    def test_swift_struct_types(self):
        fields = [{"name": "rx_hz", "type": "int", "doc": "a"},
                  {"name": "rssi_dbm", "type": "int", "nullable": True, "doc": "b"},
                  {"name": "bus", "type": "state", "swift": "MotorsBus", "values": ["ok"], "doc": "c"},
                  {"name": "to_car_hz", "type": "number", "doc": "d"},
                  {"name": "last_error", "type": "object", "swift": "E", "nullable": True,
                   "fields": [], "doc": "e"}]
        out = "\n".join(self.c.swift_struct("S", fields, "doc"))
        self.assertIn("    public var rx_hz: Int", out)
        self.assertIn("    public var rssi_dbm: Int?", out)
        self.assertIn("    public var bus: MotorsBus", out)
        self.assertIn("    public var to_car_hz: Double", out)
        self.assertIn("    public var last_error: E?", out)
        self.assertIn("public init(rx_hz: Int, rssi_dbm: Int?, bus: MotorsBus, to_car_hz: Double, "
                      "last_error: E?)", out)

    def test_swift_document_puts_proto_first_then_groups(self):
        out = "\n".join(self.c.swift_document("CarStatus", self.s, self.s["status"]["groups"], "d"))
        lines = [l for l in out.splitlines() if l.startswith("    public var ")]
        self.assertEqual(lines[0], "    public var proto: Int")
        self.assertEqual(lines[1], "    public var device: DeviceInfo")
        self.assertEqual(lines[-1], "    public var system: SystemInfo")

    def test_lround_is_half_away_from_zero(self):
        self.assertEqual(self.c.lround(900.5), 901)
        self.assertEqual(self.c.lround(900.4999), 900)
        self.assertEqual(self.c.lround(-2.5), -3)
        self.assertEqual(self.c.lround(2.5), 3)


class TestCEmitter(unittest.TestCase):
    def setUp(self):
        import gen_contract
        self.g = gen_contract
        self.s = load()
        self.out = self.g.emit_c(self.s)

    def test_table_carries_names_ranges_defaults_and_scale(self):
        self.assertIn('{ "rise_ms", CFG_INT, 0, 2000, 300, NULL, 0, 1 }', self.out)
        self.assertIn('{ "enabled", CFG_BOOL, 0, 1, 1, NULL, 0, 1 }', self.out)
        self.assertIn('{ "gear_ratio", CFG_FIXED, 100, 30000, 900, NULL, 0, 100 }', self.out)
        self.assertIn('{ "quadrature", CFG_ENUM, 1, 4, 4, CFG_WHEEL_QUADRATURE_ALLOWED, 3, 1 }',
                      self.out)
        self.assertIn("static const int32_t CFG_WHEEL_QUADRATURE_ALLOWED[] = { 1, 2, 4 };", self.out)

    def test_domains_are_keyed_not_pathed(self):
        self.assertIn('    { "ramp", "ramp", CFG_RAMP_FIELDS, 1 },', self.out)
        self.assertIn('    { "recovery", "recover", CFG_RECOVER_FIELDS, 2 },', self.out)
        self.assertIn('    { "chassis", "dims", CFG_DIMS_FIELDS, 2 },', self.out)
        self.assertIn('#define CFG_CONFIG_PATH "/config"', self.out)
        self.assertEqual(self.out.count("CFG_DOMAINS[] = {"), 1)
        self.assertIn("#define CFG_DOMAIN_COUNT 5", self.out)
        self.assertIn("#define CFG_MAX_FIELDS 4", self.out)

    def test_rt_symbols(self):
        for line in ("#define RT_PORT 4210", "#define RT_MAX_DATAGRAM 320", "#define RT_MAX_COMMAND 96",
                     "#define RT_PROTO 2", '#define RT_KEY_TYPE "type"', '#define RT_KEY_SESSION "session"',
                     '#define RT_KEY_THROTTLE "throttle"', '#define RT_KEY_TURN "turn"',
                     '#define RT_TYPE_HELLO_ACK "hello_ack"', '#define RT_TYPE_DRIVE "drive"',
                     '#define RT_TYPE_TELEMETRY "telemetry"'):
            self.assertIn(line, self.out.splitlines(), line)
        self.assertNotIn("RT_KEY_HELLO", self.out)
        self.assertNotIn("RT_KEY_BYE", self.out)
        self.assertNotIn("CTL_", self.out)

    def test_groups_envelope_errors_paths_and_calibration(self):
        for line in ('#define KEY_GROUP_MOTORS "motors"', '#define KEY_MOTORS_OWNER "owner"',
                     '#define MOTORS_OWNER_IDLE "idle"', "#define MOTORS_OWNER_COUNT 7",
                     '#define KEY_ERROR_FIELD "field"', '#define ERR_BUSY "busy"',
                     '#define PATH_CONFIG "/config"', '#define CORNER_REAR_RIGHT "rear_right"',
                     "#define CORNER_COUNT 4", '#define DIRECTION_REVERSE "reverse"',
                     "#define CALIB_PAIRS 4", '#define KEY_CALIB_INVERTED "inverted"'):
            self.assertIn(line, self.out.splitlines(), line)


class TestSwiftEmitter(unittest.TestCase):
    def setUp(self):
        import gen_contract
        self.out = gen_contract.emit_swift(load())

    def lines(self):
        return self.out.splitlines()

    def test_contract_constants(self):
        for line in ("    public static let proto = 2", '    public static let device = "ajmiddlecar"',
                     "    public static let rtPort: UInt16 = 4210", "    public static let maxCommand = 96",
                     '    public static let typeField = "type"', '    public static let sessionField = "session"',
                     '    public static let turnField = "turn"', '    public static let configPath = "/config"',
                     '    public static let spinPath = "/calibration/spin"',
                     '    public static let okField = "ok"', '    public static let errorField = "error"'):
            self.assertIn(line, self.lines(), line)
        self.assertNotIn("helloField", self.out)
        self.assertNotIn("yawField", self.out)
        self.assertIn('    public static let helloAck = "hello_ack"', self.lines())
        self.assertIn("public enum RTType {", self.out)

    def test_groups_and_documents(self):
        self.assertIn("public enum MotorsOwner: Equatable, Sendable, Codable {", self.out)
        self.assertIn("    case safe_stop", self.lines())
        self.assertIn("public struct LinkInfo: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public var rssi_dbm: Int?", self.lines())
        self.assertIn("public struct Telemetry: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public init(proto: Int, seq: Int, link: LinkInfo, motors: MotorsInfo, "
                      "system: SystemInfo) { self.proto = proto; self.seq = seq; self.link = link; "
                      "self.motors = motors; self.system = system }", self.lines())
        self.assertIn("public struct CarStatus: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public var radio: RadioInfo", self.lines())
        self.assertIn("    public var fw: String?", self.lines())   # RadioInfo.fw is nullable

    def test_config_structs(self):
        self.assertIn("public struct Wheel: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public var gear_ratio: Double", self.lines())
        self.assertIn("    public var quadrature: Int", self.lines())
        self.assertIn('    static let key = "wheel"', self.lines())
        self.assertIn("    static let `default` = Wheel(diameter_mm: 65, encoder_ppr: 11, gear_ratio: 9.0, "
                      "quadrature: 4)", self.lines())
        self.assertIn("    static let gear_ratioRange: ClosedRange<Double> = 1.0...300.0", self.lines())
        self.assertIn("    static let diameter_mmRange: ClosedRange<Int> = 20...150", self.lines())
        self.assertIn("    static let quadratureAllowed: [Int] = [1, 2, 4]", self.lines())
        self.assertIn("    static func pick(from c: CarConfig) -> Wheel? { c.wheel }", self.lines())
        self.assertIn("    static func wrap(_ v: Wheel) -> CarConfig { CarConfig(wheel: v) }", self.lines())
        self.assertIn("public struct CarConfig: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public var recovery: Recovery?", self.lines())
        self.assertIn("    public init(proto: Int? = nil, ramp: Ramp? = nil, trim: Trim? = nil, "
                      "recovery: Recovery? = nil, wheel: Wheel? = nil, chassis: Chassis? = nil) { "
                      "self.proto = proto; self.ramp = ramp; self.trim = trim; self.recovery = recovery; "
                      "self.wheel = wheel; self.chassis = chassis }", self.lines())
        self.assertNotIn("static let path", self.out)

    def test_calibration_and_errors(self):
        self.assertIn("public enum CalibCorner: Equatable, Sendable, Codable {", self.out)
        self.assertIn("    case front_left", self.lines())
        self.assertIn("public enum CalibDirection: Equatable, Sendable, Codable {", self.out)
        self.assertIn("public struct CalibWheel: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public var inverted: Bool", self.lines())
        self.assertIn("public struct Calibration: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public var wheels: [CalibWheel]", self.lines())
        self.assertIn("public enum CarErrorCode: Equatable, Sendable, Codable {", self.out)
        self.assertIn("    case out_of_range", self.lines())
        self.assertIn("public struct CarAPIError: Codable, Equatable, Sendable {", self.out)
        self.assertIn("        public var code: CarErrorCode", self.lines())

    def test_default_uses_the_schema_values(self):
        self.assertIn("    static let `default` = Recovery(enabled: true, window_ms: 5000)", self.lines())


class TestPythonEmitter(unittest.TestCase):
    def setUp(self):
        import gen_contract
        ns = {}
        exec(gen_contract.emit_python(load()), ns)
        self.ns = ns

    def test_telemetry_fields_reach_python(self):
        names = [f["name"] for f in self.ns["TELEMETRY_FIELDS"]]
        self.assertEqual(names[0], "seq")
        self.assertIn("rx_fps", names)
        self.assertIn("ctl", names)
        self.assertIn("bus_ok", names)

    def test_table(self):
        self.assertEqual(self.ns["PROTO"], 1)
        self.assertEqual(self.ns["DEVICE"], "ajmiddlecar")
        self.assertEqual(self.ns["RT"]["port"], 4210)
        self.assertEqual(self.ns["RT"]["session_idle_ms"], 10000)
        self.assertEqual(set(self.ns["DOMAINS"]),
                         {"/ramp", "/trim", "/recover", "/wheel", "/dims"})
        self.assertEqual(self.ns["DOMAINS"]["/recover"]["defaults"],
                         {"enabled": True, "window_ms": 5000})

    def test_validate_accepts_the_defaults(self):
        v = self.ns["validate"]
        for path, d in self.ns["DOMAINS"].items():
            ok, why = v(path, dict(d["defaults"]))
            self.assertTrue(ok, f"{path}: {why}")

    def test_validate_accepts_the_range_edges(self):
        v = self.ns["validate"]
        ok, why = v("/wheel", {"diameter_mm": 20, "ppr": 1000, "gear_x100": 100, "quad": 1})
        self.assertTrue(ok, why)
        ok, why = v("/trim", {"trim_pct": -30})
        self.assertTrue(ok, why)

    def test_validate_rejects_out_of_range(self):
        v = self.ns["validate"]
        ok, why = v("/wheel", {"diameter_mm": 200, "ppr": 11, "gear_x100": 2100, "quad": 4})
        self.assertFalse(ok)
        self.assertIn("diameter_mm", why)

    def test_validate_rejects_a_missing_field(self):
        v = self.ns["validate"]
        ok, why = v("/dims", {"track_mm": 130})
        self.assertFalse(ok)
        self.assertIn("wheelbase_mm", why)

    def test_validate_rejects_a_bad_enum_and_a_bad_type(self):
        v = self.ns["validate"]
        ok, why = v("/wheel", {"diameter_mm": 65, "ppr": 11, "gear_x100": 2100, "quad": 3})
        self.assertFalse(ok)
        self.assertIn("quad", why)
        ok, why = v("/recover", {"enabled": "yes", "window_ms": 5000})
        self.assertFalse(ok)
        self.assertIn("enabled", why)

    def test_validate_rejects_an_unknown_path(self):
        ok, why = self.ns["validate"]("/nope", {})
        self.assertFalse(ok)
        self.assertIn("/nope", why)

    def test_bool_is_not_accepted_as_an_int(self):
        """In Python True == 1, so a bool sneaks past a naive isinstance check."""
        v = self.ns["validate"]
        ok, why = v("/ramp", {"ramp_ms": True})
        self.assertFalse(ok)
        self.assertIn("ramp_ms", why)

    def test_ctl_symbols_are_name_keyed(self):
        """Reordering ctl_values in the schema must not silently re-rank the
        mock's arbiter against the car's hand-written link_src_t (whose build
        guard checks only the count). Name-keyed symbols make state.py immune
        to position, as C's CTL_RT and Swift's CtlOwner.rt already are."""
        for v in load()["ctl_values"]:
            self.assertEqual(self.ns[f"CTL_{v.upper()}"], v)


class TestDriftCheck(unittest.TestCase):
    def test_check_script_passes_on_a_clean_tree(self):
        r = subprocess.run(["bash", str(ROOT / "tools" / "check_contract.sh")],
                           capture_output=True, text=True, cwd=str(ROOT))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


class TestDongleSchema(unittest.TestCase):
    def load(self):
        return json.loads((ROOT / "contract" / "dongle-api.json").read_text())

    def test_identity_and_address_are_pinned(self):
        s = self.load()
        self.assertEqual(s["proto"], 1)
        self.assertEqual(s["device"], "ajdongle")
        self.assertEqual(s["network"], {"host": "192.168.7.1", "port": 8080,
                                        "doc": s["network"]["doc"]})
        self.assertEqual(s["endpoints"], {"status": "/status", "wifi": "/wifi", "ota": "/ota"})
        self.assertEqual(s["envelope"], {"proto": "proto", "ok": "ok", "error": "error",
                                         "code": "code", "message": "message", "field": "field"})

    def test_bounds_are_wpa2s(self):
        b = self.load()["bounds"]
        self.assertEqual((b["ssid_min"], b["ssid_max"]), (1, 32))
        self.assertEqual((b["pass_min"], b["pass_max"]), (8, 63))

    def test_it_names_no_car(self):
        text = (ROOT / "contract" / "dongle-api.json").read_text().lower()
        self.assertNotIn("ajmiddlecar", text)
        self.assertNotIn("drive1234", text)

    def test_groups(self):
        g = self.load()["groups"]
        self.assertEqual(list(g), ["device", "usb", "wifi", "relay", "system"])
        names = {k: [f["name"] for f in v["fields"]] for k, v in g.items()}
        self.assertEqual(names["device"], ["id", "fw", "build", "rolled_back", "idf"])
        self.assertEqual(names["usb"], ["state"])
        self.assertEqual(names["wifi"], ["ssid", "configured", "state", "rssi_dbm", "channel",
                                         "attempts"])
        self.assertEqual(names["relay"], ["to_car_hz", "to_phone_hz", "udp_sessions",
                                          "tcp_connections", "last_error"])
        self.assertEqual(names["system"], ["uptime_s", "free_heap"])
        for k, v in g.items():
            self.assertTrue(v["swift"].startswith("Dongle"), k)
        wifi_state = next(f for f in g["wifi"]["fields"] if f["name"] == "state")
        self.assertEqual(wifi_state["values"], ["idle", "searching", "joining", "connected", "failed"])
        self.assertEqual(wifi_state["swift"], "DongleWifiState")
        usb_state = next(f for f in g["usb"]["fields"] if f["name"] == "state")
        self.assertEqual(usb_state["values"], ["up", "down"])
        attempts = next(f for f in g["wifi"]["fields"] if f["name"] == "attempts")
        self.assertEqual(attempts["type"], "object")
        self.assertEqual([f["name"] for f in attempts["fields"]], ["used", "max"])
        err = next(f for f in g["relay"]["fields"] if f["name"] == "last_error")
        self.assertEqual(err["type"], "object")
        self.assertTrue(err["nullable"])
        self.assertEqual([f["name"] for f in err["fields"]], ["errno", "message", "count", "age_s"])
        for name in ("rssi_dbm", "channel"):
            f = next(f for f in g["wifi"]["fields"] if f["name"] == name)
            self.assertTrue(f.get("nullable"), name)

    def test_status_wifi_and_errors(self):
        s = self.load()
        self.assertEqual(s["status"]["groups"], ["device", "usb", "wifi", "relay", "system"])
        self.assertEqual(s["status"]["swift"], "DongleStatus")
        self.assertEqual(s["wifi_request"], {"ssid": "ssid", "password": "password"})
        self.assertEqual(s["wifi_reply"]["swift"], "DongleWifiReply")
        self.assertEqual(s["wifi_reply"]["fields"], ["ssid", "state"])
        self.assertEqual(s["errors"], ["bad_json", "missing_field", "unknown_field", "wrong_type",
                                       "bad_length", "bad_chars", "radio_refused", "too_small",
                                       "not_firmware", "write_failed", "busy", "internal"])


class TestDongleAgreesWithTheCar(unittest.TestCase):
    """The two schemas are separate files, and the relay is what couples them.

    contract/dongle-api.json's relay.http_port and relay.rt_port are not free numbers: they
    are the CAR's ports, which the dongle listens on so that CarHost.port, CarHost.rtPort and
    contract/car-api.json never have to move. The schema doc and the design note both assert
    the two must agree, and nothing checked it. Editing one file alone is a silent failure by
    construction — the relays parse nothing, so forwarding to a port the car does not serve
    produces a refused connection or a dead datagram, not a diagnosable error.
    """

    def setUp(self):
        with open(ROOT / "contract" / "dongle-api.json") as f:
            self.dongle = json.load(f)
        self.car = load()

    def test_relay_rt_port_is_the_cars_rt_port(self):
        self.assertEqual(self.dongle["relay"]["rt_port"], self.car["rt"]["port"],
                         "the dongle would relay the real-time channel to a port the car "
                         "does not listen on")

    def test_relay_http_port_is_the_cars_rest_port(self):
        # car-api.json carries no HTTP port: the car's http_server.c leaves
        # HTTPD_DEFAULT_CONFIG's 80 alone. Before the dongle cutover (Plan 5 Task 3) this
        # asserted CarHost.port's device branch carried that same number as its own literal;
        # now it takes the number from DongleContract.relayHttpPort instead of respelling it —
        # generated from this schema's relay.http_port, which is the tie back to the number
        # this test is named for. Checked as a whole line, not a substring: a substring search
        # would also match the symbol sitting in a comment, or on the simulator branch, without
        # telling the device branch's own assignment apart from either — exactly the trap
        # assertEmitsLine's docstring (below, in TestDongleEmitters) documents as having already
        # bitten a port assertion once. test_swift_exposes_the_same_vocabulary is what still
        # guards the constant's own value against contract/dongle-api.json; this test only
        # guards that CarHost.port still takes it from there. The line used to read
        # `directPort ?? DongleContract.relayHttpPort` while the bench escape hatch could
        # override it; the hatch is gone and the assignment is the constant alone.
        source = (ROOT / "app" / "AJMiddleCar" / "CarHost.swift").read_text()
        want = "    static let port: UInt16 = DongleContract.relayHttpPort"
        self.assertIn(want, source.splitlines(),
                      "CarHost.swift's device branch no longer takes its REST port from "
                      "DongleContract.relayHttpPort — the relay could again forward the "
                      "car's REST traffic to a port the app does not use, with nothing "
                      "here to catch it")

    def test_relay_http_port_is_the_number_the_car_actually_serves(self):
        """The value half of the tie, which the symbol assertion above cannot make.

        That one proves CarHost.port TAKES its number from DongleContract.relayHttpPort, and
        test_swift_exposes_the_same_vocabulary proves the constant carries what this schema
        says. Neither of them says the schema's number is the CAR's REST port. Between them the
        loop stayed open, and relay_tcp.c aims at that same constant for the car-side
        destination — so editing the one number moves both ends together: the app asks the
        dongle on a port the dongle really serves, the dongle forwards to a port the car does
        not, and every test in this file passes while the car is unreachable. That is the
        failure this test is named for, and it is the one it could no longer see.

        contract/car-api.json cannot close it — it carries no HTTP port at all — so the car's
        own firmware does. http_server.c takes HTTPD_DEFAULT_CONFIG()'s port and never assigns
        config.server_port, and that default is 80: the number this schema has to carry, and
        the assertion that fails the day either side of it moves.
        """
        httpd_default_port = 80          # esp_http_server's HTTPD_DEFAULT_CONFIG()
        car_http = (ROOT / "firmware" / "car" / "core" / "main" / "http_server.c").read_text()
        self.assertIn("HTTPD_DEFAULT_CONFIG()", car_http,
                      "the car's REST server no longer starts from HTTPD_DEFAULT_CONFIG, so "
                      "its port is no longer the default this test ties the relay to")
        self.assertNotIn("server_port", car_http,
                         "the car's REST server now sets its own port — contract/"
                         "dongle-api.json's relay.http_port has to move with it, or the relay "
                         "forwards the car's REST traffic into nothing")
        self.assertEqual(self.dongle["relay"]["http_port"], httpd_default_port,
                         "relay.http_port is not the port the car actually serves; the relay "
                         "would forward REST to a port nothing listens on")


class TestDongleEmitters(unittest.TestCase):
    def setUp(self):
        # No sys.path insertion here: the file already does it at module level, beside
        # its other mid-file imports, and a second one would be a second thing to keep true.
        import gen_dongle
        self.g = gen_dongle
        with open(ROOT / "contract" / "dongle-api.json") as f:
            self.s = json.load(f)

    def assertEmitsLine(self, line, out):
        """Assert `line` is one whole emitted line, not merely a substring of one.

        assertIn against the raw text is prefix matching, and every port assertion below has
        a longer sibling it is a prefix of: "…DONGLE_RELAY_HTTP_PORT 80" sits inside
        "…DONGLE_RELAY_HTTP_PORT 8080", and "…UInt16 = 80" inside "…UInt16 = 8080". That is
        not hypothetical — an emitter edited to put the dongle's own port (8080) in the
        relay's field still passed the test written to catch exactly that. Matching whole
        lines is what makes these assertions mean what they say.
        """
        self.assertIn(line, out.splitlines(),
                      f"no emitted line is exactly {line!r}")

    def test_c_header_is_pure_defines(self):
        out = self.g.emit_dongle_c(self.s)
        self.assertIn('#define DONGLE_DEVICE "ajdongle"', out)
        self.assertIn('#define DONGLE_HOST "192.168.7.1"', out)
        self.assertEmitsLine("#define DONGLE_PORT 8080", out)
        self.assertEmitsLine("#define DONGLE_RELAY_HTTP_PORT 80", out)
        self.assertEmitsLine("#define DONGLE_RELAY_RT_PORT 4210", out)
        self.assertIn("#define DONGLE_SSID_MAX 32", out)
        self.assertIn("#define DONGLE_PASS_MIN 8", out)
        # Pure means includable from net_cfg.h, which compiles with plain cc: no ESP-IDF,
        # no types, nothing but preprocessor text.
        for banned in ("#include", "esp_err_t", "typedef", "struct "):
            self.assertNotIn(banned, out)

    def test_c_header_carries_the_state_vocabulary(self):
        out = self.g.emit_dongle_c(self.s)
        self.assertIn('#define DONGLE_STATE_IDLE "idle"', out)
        self.assertIn('#define DONGLE_STATE_CONNECTED "connected"', out)

    def test_c_header_carries_the_usb_state_vocabulary(self):
        out = self.g.emit_dongle_c(self.s)
        self.assertIn('#define DONGLE_USB_STATE_UP "up"', out)
        self.assertIn('#define DONGLE_USB_STATE_DOWN "down"', out)

    def test_c_header_carries_the_paths(self):
        out = self.g.emit_dongle_c(self.s)
        self.assertIn('#define DONGLE_PATH_STATUS "/status"', out)
        self.assertIn('#define DONGLE_PATH_NET "/net"', out)
        self.assertIn('#define DONGLE_PATH_OTA "/ota"', out)

    def test_c_header_carries_the_status_and_net_keys(self):
        # DONGLE_KEY_IDF is the fix's regression test: status_fields gained "idf" because
        # status_api.c already puts "idf" in the /status body and had no macro for it.
        # DONGLE_KEY_DEVICE and DONGLE_NETKEY_SSID/PASSWORD are "at least one member of
        # each key group" — the drift check catches a whole-file regression here, but
        # nothing before this asserted an individual DONGLE_KEY_*/DONGLE_NETKEY_* name.
        out = self.g.emit_dongle_c(self.s)
        self.assertIn('#define DONGLE_KEY_DEVICE "device"', out)
        self.assertIn('#define DONGLE_KEY_IDF "idf"', out)
        self.assertIn('#define DONGLE_KEY_ROLLBACK "rollback"', out)
        self.assertIn('#define DONGLE_KEY_NET_SSID "ssid"', out)
        self.assertIn('#define DONGLE_NETKEY_SSID "ssid"', out)
        self.assertIn('#define DONGLE_NETKEY_PASSWORD "password"', out)

    def test_swift_exposes_the_same_vocabulary(self):
        out = self.g.emit_dongle_swift(self.s)
        self.assertIn('public static let device = "ajdongle"', out)
        self.assertIn('public static let host = "192.168.7.1"', out)
        self.assertEmitsLine("    public static let port: UInt16 = 8080", out)
        self.assertEmitsLine("    public static let relayHttpPort: UInt16 = 80", out)
        self.assertEmitsLine("    public static let relayRtPort: UInt16 = 4210", out)
        self.assertIn('public static let statusPath = "/status"', out)
        self.assertIn('public static let netPath = "/net"', out)
        self.assertIn('public static let otaPath = "/ota"', out)
        self.assertIn("public static let ssidMax = 32", out)
        self.assertIn(
            'public static let all = ["idle", "searching", "joining", "connected", "failed"]', out)

    def test_swift_exposes_the_usb_state(self):
        out = self.g.emit_dongle_swift(self.s)
        self.assertIn("public enum DongleUsbState {", out)
        self.assertIn('public static let up = "up"', out)
        self.assertIn('public static let down = "down"', out)
        self.assertIn('public static let all = ["up", "down"]', out)

    def test_swift_exposes_the_net_fields(self):
        out = self.g.emit_dongle_swift(self.s)
        self.assertIn('public static let ssidField = "ssid"', out)
        self.assertIn('public static let passwordField = "password"', out)
        self.assertIn('public static let configuredField = "configured"', out)

    def test_swift_exposes_the_status_keys(self):
        # Regression test: emit_dongle_swift once destructured net_fields and never
        # touched status_fields at all, so the C and Swift sides did not carry the same
        # vocabulary — the one property this task exists to establish. Every key
        # status_fields names must appear on the Swift side too, device through idf.
        out = self.g.emit_dongle_swift(self.s)
        self.assertIn("public enum DongleStatusKey {", out)
        self.assertIn('public static let device = "device"', out)
        self.assertIn('public static let fw = "fw"', out)
        self.assertIn('public static let idf = "idf"', out)
        self.assertIn('public static let usb = "usb"', out)
        self.assertIn('public static let rollback = "rollback"', out)
        self.assertIn('public static let net = "net"', out)
        self.assertIn('public static let netSsid = "ssid"', out)
        self.assertIn('public static let netState = "state"', out)
        self.assertIn('public static let netRssi = "rssi"', out)

    def test_both_emitters_are_deterministic(self):
        self.assertEqual(self.g.emit_dongle_c(self.s), self.g.emit_dongle_c(self.s))
        self.assertEqual(self.g.emit_dongle_swift(self.s), self.g.emit_dongle_swift(self.s))


if __name__ == "__main__":
    unittest.main()
