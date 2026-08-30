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
        self.assertEqual(s["proto"], 1)
        self.assertEqual(s["device"], "ajmiddlecar")
        self.assertEqual(s["network"]["ssid"], "AJMiddleCar")
        self.assertEqual(s["network"]["host"], "192.168.4.1")

    def test_the_command_cap_is_below_the_datagram_cap(self):
        rt = load()["rt"]
        self.assertLess(rt["max_command"], rt["max_datagram"],
                        "a receive buffer sized from the command cap truncates telemetry")

    def test_rt_constants(self):
        rt = load()["rt"]
        self.assertEqual(rt["port"], 4210)
        self.assertEqual(rt["max_datagram"], 320)
        self.assertEqual(rt["command_hz"], 10)
        self.assertEqual(rt["telemetry_hz"], 5)
        self.assertEqual(rt["watchdog_ms"], 300)

    def test_session_idle(self):
        rt = load()["rt"]
        self.assertEqual(rt["session_idle_ms"], 10000)
        # Mortality must be far outside the watchdog's world: a slow trip is a
        # trip, not a death.
        self.assertGreater(rt["session_idle_ms"], rt["watchdog_ms"] * 10)

    def test_domains_are_unique(self):
        s = load()
        paths = [d["path"] for d in s["domains"]]
        keys = [d["nvs_key"] for d in s["domains"]]
        names = [d["swift"] for d in s["domains"]]
        self.assertEqual(len(paths), len(set(paths)))
        self.assertEqual(len(keys), len(set(keys)))
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(set(paths), {"/ramp", "/trim", "/recover", "/wheel", "/dims"})

    def test_every_field_is_well_formed(self):
        for d in load()["domains"]:
            self.assertTrue(d["fields"], f"{d['path']} has no fields")
            for f in d["fields"]:
                where = f"{d['path']}.{f['name']}"
                self.assertTrue(re.fullmatch(r"[a-z][a-z0-9_]*", f["name"]), where)
                self.assertIn(f["type"], ("int", "bool", "enum"), where)
                self.assertTrue(f["doc"].strip(), where)
                if f["type"] == "int":
                    self.assertLess(f["min"], f["max"], where)
                    self.assertGreaterEqual(f["default"], f["min"], where)
                    self.assertLessEqual(f["default"], f["max"], where)
                elif f["type"] == "enum":
                    self.assertIn(f["default"], f["values"], where)
                    self.assertEqual(len(f["values"]), len(set(f["values"])), where)
                else:
                    self.assertIsInstance(f["default"], bool, where)

    def test_ranges_match_the_firmware_today(self):
        """The schema must describe the firmware that exists, not one we imagined."""
        main = ROOT / "firmware" / "p4" / "main"
        # The five *_api.c files are gone: cfg_api.c drives all of them from the
        # generated table, so a range literal no longer appears in any handler. What
        # remains are the setters' own clamps, which are the C-side constants this
        # test exists to keep the schema honest against.
        #
        # Each domain's file is checked in isolation rather than as one concatenated
        # blob: a corpus-wide search let /recover window_ms's 1000 floor hide behind
        # /wheel ppr's unrelated 1000 ceiling in wheel.h, so a genuine floor drift in
        # recovery.h went undetected. Scoping per file closes that cross-domain
        # collision.
        file_for_path = {
            "/wheel": "wheel.h", "/dims": "dims.h", "/recover": "recovery.h",
            "/ramp": "ramp.c", "/trim": "car.c",
        }
        src_by_file = {n: (main / n).read_text() for n in set(file_for_path.values())}
        expected = {
            ("/wheel", "diameter_mm"): (20, 150), ("/wheel", "ppr"): (1, 1000),
            ("/wheel", "gear_x100"): (100, 30000),
            ("/dims", "track_mm"): (60, 300), ("/dims", "wheelbase_mm"): (90, 360),
            ("/recover", "window_ms"): (1000, 10000),
            ("/ramp", "ramp_ms"): (0, 2000), ("/trim", "trim_pct"): (-30, 30),
        }
        got = {}
        for d in load()["domains"]:
            for f in d["fields"]:
                if f["type"] == "int":
                    got[(d["path"], f["name"])] = (f["min"], f["max"])
        self.assertEqual(got, expected)
        for (path, name), (lo, hi) in expected.items():
            fname = file_for_path[path]
            src = src_by_file[fname]
            for bound in (lo, hi):
                # Word-bounded: "2000" must not be satisfied by "12000", and "30"
                # must not be found inside "-30". The min bounds were entirely
                # unchecked before — a firmware floor drifting from the schema is
                # exactly what this test exists to catch. Scoped to the one file that
                # owns this domain's clamps, so an identical literal in another
                # domain's file (e.g. wheel.h's WHEEL_PPR_MAX 1000) cannot mask a
                # drift here. Not bulletproof within a single file, though: 0 (the
                # /ramp floor) is common enough that another unrelated 0 in ramp.c
                # could still mask a real drift there — a residual, one-file risk.
                pat = rf"(?<![\w.-]){re.escape(str(bound))}(?![\w.])"
                self.assertRegex(src, pat,
                                 f"{path} {name}: bound {bound} not in {fname}")


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
            "firmware/p4/main/cfg_table.inc",
            "app/AJMiddleCar/Generated/CarAPI.swift",
            "tools/mock_car/generated.py",
            "firmware/s3/main/dongle_contract.inc",
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


class TestCEmitter(unittest.TestCase):
    def test_table_carries_names_ranges_and_defaults(self):
        import gen_contract
        out = gen_contract.emit_c(load())
        self.assertIn(gen_contract.BANNER, out)
        self.assertIn('{ "diameter_mm", CFG_INT, 20, 150, 65, NULL, 0 }', out)
        self.assertIn('{ "trim_pct", CFG_INT, -30, 30, 0, NULL, 0 }', out)
        self.assertIn('{ "enabled", CFG_BOOL, 0, 1, 1, NULL, 0 }', out)
        self.assertIn("static const int32_t CFG_WHEEL_QUAD_ALLOWED[] = { 1, 2, 4 };", out)
        self.assertIn('{ "quad", CFG_ENUM, 1, 4, 4, CFG_WHEEL_QUAD_ALLOWED, 3 }', out)
        self.assertIn("#define CFG_DOMAIN_COUNT 5", out)
        self.assertIn("#define CFG_MAX_FIELDS 4", out)   # /wheel is the widest
        # The real-time channel's constants reach C from the same schema the app and
        # the mock read, so the port and the deadline cannot drift between the three.
        self.assertIn("#define RT_PORT 4210", out)
        self.assertIn("#define RT_WATCHDOG_MS 300", out)
        self.assertIn("#define RT_SESSION_IDLE_MS 10000", out)
        # Two caps, deliberately different: the car accepts at most RT_MAX_COMMAND, but a
        # telemetry frame is 119-156 bytes, so a receive buffer sized from the command cap
        # would truncate every one of them.
        self.assertIn("#define RT_MAX_COMMAND 96", out)
        self.assertIn("#define RT_MAX_DATAGRAM 320", out)
        self.assertIn('#define RT_KEY_THROTTLE "t"', out)
        self.assertIn('#define RT_KEY_BYE "bye"', out)
        self.assertIn('#define CTL_RECOVER "recover"', out)
        self.assertIn("#define RT_PROTO 1", out)

    def test_every_domain_appears_once(self):
        import gen_contract
        out = gen_contract.emit_c(load())
        for d in load()["domains"]:
            # Match the domain row's shape, not the bare key: the ctl vocabulary also
            # contains the word "recover", and counting substrings caught that instead.
            row = f'{{ "{d["path"]}", "{d["nvs_key"]}", '
            self.assertEqual(out.count(row), 1, d["path"])


class TestSwiftEmitter(unittest.TestCase):
    def test_structs_and_constants(self):
        import gen_contract
        out = gen_contract.emit_swift(load())
        self.assertIn(gen_contract.BANNER, out)
        self.assertIn("public struct Wheel: Codable, Equatable, Sendable {", out)
        self.assertIn("public var diameter_mm: Int", out)
        self.assertIn("public var enabled: Bool", out)
        # Inside a `public extension` the members are already public; an explicit
        # `public` there is a redundant modifier, so the emitter omits it.
        self.assertIn("    static let diameter_mmRange: ClosedRange<Int> = 20...150", out)
        self.assertIn("    static let quadAllowed: [Int] = [1, 2, 4]", out)
        self.assertIn('    static let path = "/wheel"', out)
        self.assertIn("public static let rtPort: UInt16 = 4210", out)
        self.assertIn("public static let proto = 1", out)
        self.assertIn('public static let seqField = "seq"', out)
        self.assertIn('public static let byeField = "bye"', out)
        self.assertIn("public static let sessionIdleMs = 10000", out)
        self.assertIn("public enum TelemetryKey {", out)
        self.assertIn('public static let rxFps = "rx_fps"', out)
        self.assertIn('public static let busOk = "bus_ok"', out)
        self.assertIn("public enum CtlOwner {", out)
        self.assertIn('public static let recover = "recover"', out)
        self.assertIn('public static let throttleField = "t"', out)

    def test_default_uses_the_schema_values(self):
        import gen_contract
        out = gen_contract.emit_swift(load())
        self.assertIn("Wheel(diameter_mm: 65, ppr: 11, gear_x100: 900, quad: 4)", out)
        self.assertIn("Recover(enabled: true, window_ms: 5000)", out)


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
    def setUp(self):
        with open(ROOT / "contract" / "dongle-api.json") as f:
            self.s = json.load(f)

    def test_identity_and_address_are_pinned(self):
        self.assertEqual(self.s["device"], "ajdongle")
        self.assertEqual(self.s["network"]["host"], "192.168.7.1")
        # 8080, not 80: the car keeps its native ports and the dongle takes the odd one.
        self.assertEqual(self.s["network"]["port"], 8080)

    def test_bounds_are_wpa2s(self):
        b = self.s["bounds"]
        self.assertEqual((b["ssid_min"], b["ssid_max"]), (1, 32))
        self.assertEqual((b["pass_min"], b["pass_max"]), (8, 63))

    def test_it_names_no_car(self):
        # The dongle's schema must not acquire a car's SSID, password or device id —
        # the constraint the whole firmware is built around.
        blob = json.dumps(self.s).lower()
        for forbidden in ("ajmiddlecar", "drive1234", "192.168.4."):
            self.assertNotIn(forbidden, blob)

    def test_state_vocabulary_is_the_documented_one(self):
        self.assertEqual(self.s["net_states"], ["idle", "joining", "connected", "failed"])

    def test_usb_state_vocabulary_is_the_documented_one(self):
        # usb had a key (status_fields) but no enumerated values until this fix — status_api.c
        # hardcoded "up" with nothing here to check it against.
        self.assertEqual(self.s["usb_states"], ["up"])


class TestDongleEmitters(unittest.TestCase):
    def setUp(self):
        # No sys.path insertion here: the file already does it at module level, beside
        # its other mid-file imports, and a second one would be a second thing to keep true.
        import gen_dongle
        self.g = gen_dongle
        with open(ROOT / "contract" / "dongle-api.json") as f:
            self.s = json.load(f)

    def test_c_header_is_pure_defines(self):
        out = self.g.emit_dongle_c(self.s)
        self.assertIn('#define DONGLE_DEVICE "ajdongle"', out)
        self.assertIn('#define DONGLE_HOST "192.168.7.1"', out)
        self.assertIn("#define DONGLE_PORT 8080", out)
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

    def test_c_header_carries_the_paths(self):
        out = self.g.emit_dongle_c(self.s)
        self.assertIn('#define DONGLE_PATH_STATUS "/status"', out)
        self.assertIn('#define DONGLE_PATH_NET "/net"', out)

    def test_c_header_carries_the_status_and_net_keys(self):
        # DONGLE_KEY_IDF is the fix's regression test: status_fields gained "idf" because
        # status_api.c already puts "idf" in the /status body and had no macro for it.
        # DONGLE_KEY_DEVICE and DONGLE_NETKEY_SSID/PASSWORD are "at least one member of
        # each key group" — the drift check catches a whole-file regression here, but
        # nothing before this asserted an individual DONGLE_KEY_*/DONGLE_NETKEY_* name.
        out = self.g.emit_dongle_c(self.s)
        self.assertIn('#define DONGLE_KEY_DEVICE "device"', out)
        self.assertIn('#define DONGLE_KEY_IDF "idf"', out)
        self.assertIn('#define DONGLE_KEY_NET_SSID "ssid"', out)
        self.assertIn('#define DONGLE_NETKEY_SSID "ssid"', out)
        self.assertIn('#define DONGLE_NETKEY_PASSWORD "password"', out)

    def test_swift_exposes_the_same_vocabulary(self):
        out = self.g.emit_dongle_swift(self.s)
        self.assertIn('public static let device = "ajdongle"', out)
        self.assertIn('public static let host = "192.168.7.1"', out)
        self.assertIn("public static let port: UInt16 = 8080", out)
        self.assertIn('public static let statusPath = "/status"', out)
        self.assertIn('public static let netPath = "/net"', out)
        self.assertIn("public static let ssidMax = 32", out)
        self.assertIn('public static let all = ["idle", "joining", "connected", "failed"]', out)

    def test_swift_exposes_the_usb_state(self):
        out = self.g.emit_dongle_swift(self.s)
        self.assertIn("public enum DongleUsbState {", out)
        self.assertIn('public static let up = "up"', out)
        self.assertIn('public static let all = ["up"]', out)

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
        self.assertIn('public static let net = "net"', out)
        self.assertIn('public static let netSsid = "ssid"', out)
        self.assertIn('public static let netState = "state"', out)
        self.assertIn('public static let netRssi = "rssi"', out)

    def test_both_emitters_are_deterministic(self):
        self.assertEqual(self.g.emit_dongle_c(self.s), self.g.emit_dongle_c(self.s))
        self.assertEqual(self.g.emit_dongle_swift(self.s), self.g.emit_dongle_swift(self.s))


if __name__ == "__main__":
    unittest.main()
