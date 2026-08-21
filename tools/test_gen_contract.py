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

    def test_rt_constants(self):
        rt = load()["rt"]
        self.assertEqual(rt["port"], 4210)
        self.assertEqual(rt["max_datagram"], 96)
        self.assertEqual(rt["command_hz"], 10)
        self.assertEqual(rt["telemetry_hz"], 5)
        self.assertEqual(rt["watchdog_ms"], 300)

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
        src = "\n".join((main / n).read_text()
                        for n in ("wheel.h", "dims.h", "recovery.h", "ramp.c",
                                  "ramp_api.c", "trim_api.c"))
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
        for lo, hi in expected.values():
            self.assertIn(str(hi), src, f"{hi} is not in the firmware sources")


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

    def test_every_domain_appears_once(self):
        import gen_contract
        out = gen_contract.emit_c(load())
        for d in load()["domains"]:
            self.assertEqual(out.count(f'"{d["nvs_key"]}"'), 1, d["path"])


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

    def test_default_uses_the_schema_values(self):
        import gen_contract
        out = gen_contract.emit_swift(load())
        self.assertIn("Wheel(diameter_mm: 65, ppr: 11, gear_x100: 2100, quad: 4)", out)
        self.assertIn("Recover(enabled: true, window_ms: 5000)", out)


if __name__ == "__main__":
    unittest.main()
