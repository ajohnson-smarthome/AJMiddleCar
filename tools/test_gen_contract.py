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


if __name__ == "__main__":
    unittest.main()
