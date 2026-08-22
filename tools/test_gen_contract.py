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
        self.assertIn("Wheel(diameter_mm: 65, ppr: 11, gear_x100: 2100, quad: 4)", out)
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


class TestDriftCheck(unittest.TestCase):
    def test_check_script_passes_on_a_clean_tree(self):
        r = subprocess.run(["bash", str(ROOT / "tools" / "check_contract.sh")],
                           capture_output=True, text=True, cwd=str(ROOT))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
