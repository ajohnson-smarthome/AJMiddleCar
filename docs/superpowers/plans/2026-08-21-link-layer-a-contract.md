# Link Layer — Plan A: hygiene and the generated contract

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make HEAD build again, remove the bench diagnostics, and replace the four hand-written copies of the app↔car contract with one JSON schema and a generator that emits all four.

**Architecture:** `contract/car-api.json` is the source of truth. `tools/gen_contract.py` reads it and writes four artefacts: a C descriptor table for the firmware's future generic config handler, Swift `Codable` structs with range constants, a Python table for the mock, and a marked region inside `docs/protocol.md`. `tools/check_contract.sh` regenerates into a temp directory and diffs, so drift fails a check instead of shipping. The generator has no third-party dependencies and its tests use stdlib `unittest`, matching how the rest of the project host-tests pure code.

**Tech Stack:** Python 3 (stdlib only), C99 (host-compiled with `cc` for the table test), Swift (`swiftc`, host), Markdown.

**Spec:** `docs/superpowers/specs/2026-08-21-link-layer-rearchitecture.md`

## Global Constraints

- Protocol version is `1`. It appears as `"proto": 1` in `/status` and in the RT `hello` reply.
- Real-time channel: UDP port **4210**, max datagram **96 bytes**, command **10 Hz**, telemetry **5 Hz**, watchdog deadline **300 ms**.
- Device id is `ajmiddlecar`. SSID `AJMiddleCar`, WPA2, password `drive1234`, gateway `192.168.4.1`.
- Config endpoints **reject** out-of-range values with `400`. They do not clamp.
- Every REST response is `application/json`.
- The generator must be **deterministic**: same input bytes produce identical output bytes.
- Generated files carry a header naming their source and are never hand-edited.
- iOS deployment target is **26.0**.
- No new third-party dependencies anywhere in this plan.

## File Structure

| File | Responsibility |
|---|---|
| `contract/car-api.json` | The single source of truth: proto version, RT channel constants, five config domains with fields, types, ranges and defaults |
| `tools/gen_contract.py` | Reads the schema, writes the four artefacts. One emitter function per artefact |
| `tools/test_gen_contract.py` | stdlib `unittest` over the schema and each emitter, plus a guard asserting the schema matches the firmware's current constants |
| `tools/check_contract.sh` | Regenerate to a temp dir, diff against committed, exit non-zero on drift |
| `firmware/p4/main/cfg_contract.h` | Hand-written types the generated table uses (`cfg_field_t`, `cfg_domain_t`). Not generated |
| `firmware/p4/main/cfg_table.inc` | **Generated.** The descriptor table |
| `firmware/p4/test/test_cfg_table.c` | Host test that the generated table compiles and carries the right ranges |
| `app/AJMiddleCar/Generated/CarAPI.swift` | **Generated.** `Codable` structs, range constants, defaults |
| `app/tests/test_carapi.swift` | Host test over the generated Swift |
| `tools/mock_car/generated.py` | **Generated.** Domain table and a validator for the mock |
| `docs/protocol.md` | Hand-written prose with one generated region between markers |

Nothing in this plan changes runtime behaviour on either side. The generated artefacts are committed but not yet consumed — `cfg_api.c` starts using `cfg_table.inc` in Plan B, and the app starts using `CarAPI.swift` in Plan D.

---

### Task 1: Make HEAD build and remove the bench diagnostics

The four transport files added on 2026-08-21 are untracked while tracked files call them, so a clean clone does not compile. The same session left diagnostics marked `NOT FOR COMMIT` running in the 10 Hz hot path and in a priority-22 timer callback.

**Files:**
- Add to git: `app/AJMiddleCar/CarNet.swift`, `app/AJMiddleCar/CarHTTP.swift`, `app/AJMiddleCar/HTTPParse.swift`, `app/AJMiddleCarTests/HTTPParseTests.swift`
- Delete: `app/AJMiddleCar/DiagProbe.swift`
- Modify: `app/AJMiddleCar/AJMiddleCarApp.swift`, `app/AJMiddleCar/CarConnection.swift`, `app/AJMiddleCar/AppFlow.swift`, `firmware/p4/main/telemetry.c`, `app/project.yml`

**Interfaces:**
- Consumes: nothing.
- Produces: a tree where `git stash -u && git stash pop` round-trips cleanly and no file contains the string `NOT FOR COMMIT`.

- [ ] **Step 1: Confirm the problem before touching anything**

```bash
cd ~/VSCode/esp32-p4-car
git status --short
grep -rn "NOT FOR COMMIT" app firmware | cat
```

Expected: four `??` transport files, several `M` files, and four or more `NOT FOR COMMIT` hits.

- [ ] **Step 2: Remove `DiagProbe` and its call site**

Delete `app/AJMiddleCar/DiagProbe.swift`.

In `app/AJMiddleCar/AJMiddleCarApp.swift`, the `.task` modifier currently reads:

```swift
.task { DiagProbe.run(host: "192.168.4.1", port: 80); conn.onTelemetry = { status.apply($0) }; await flow.startupCheck() }
```

Replace with:

```swift
.task { conn.onTelemetry = { status.apply($0) }; await flow.startupCheck() }
```

In the same file, delete the whole body of `tryCarConnected`'s first line:

```swift
NSLog("DIAG app: tryCarConnected fw=%@ online=%@", status.fw ?? "nil", status.online ? "true" : "false")
```

- [ ] **Step 3: Remove the 10 Hz diagnostic from the send path**

In `app/AJMiddleCar/CarConnection.swift`, `tick()` starts with:

```swift
    private nonisolated func tick() {
        // ==== TEMPORARY BENCH DIAGNOSTIC — NOT FOR COMMIT ====
        outbox.countTick()
        guard let c = liveConnection() else { return }
```

Replace those four lines with:

```swift
    private nonisolated func tick() {
        guard let c = liveConnection() else { return }
```

Then delete from the `Outbox` class the entire block starting at the `// ==== TEMPORARY BENCH DIAGNOSTIC` comment through the closing brace of `countTick()` — the `ticks`, `since` and `countTick` members. `Outbox` keeps only `set`, `get`, `setConnection`, `connection` and its `lock`.

- [ ] **Step 4: Remove the `DIAG flow` logging**

In `app/AJMiddleCar/AppFlow.swift`, delete every line matching `NSLog("DIAG flow:` — there are four, at the start of `startupCheck`, after the reachability probe, on the `latestRelease` failure, on success, and before `phase = .connectToCar`. Delete each `NSLog(...)` line only; leave the surrounding control flow untouched.

- [ ] **Step 5: Remove the firmware rate diagnostic**

In `firmware/p4/main/telemetry.c`, `telemetry_gather` contains:

```c
    // ==== TEMPORARY BENCH DIAGNOSTIC — NOT FOR COMMIT ====
    {
        static int64_t last_shout;
        int64_t now = esp_timer_get_time();
        if (now - last_shout > 1000000) {
            last_shout = now;
            ESP_LOGW("RATE", "car receives %d control frames/s", out->ws_fps);
        }
    }
```

Delete the comment and the whole block.

- [ ] **Step 6: Raise the deployment target**

In `app/project.yml`, change:

```yaml
  deploymentTarget:
    iOS: "16.0"
```

to:

```yaml
  deploymentTarget:
    iOS: "26.0"
```

- [ ] **Step 7: Verify nothing is left and the app still compiles**

```bash
cd ~/VSCode/esp32-p4-car
grep -rn "NOT FOR COMMIT\|DiagProbe\|countTick\|DIAG " app firmware | cat
cd app && xcodegen generate && \
  xcodebuild build -scheme AJMiddleCar \
    -destination 'platform=iOS Simulator,name=iPhone 17' \
    -derivedDataPath /tmp/ddata-middle 2>&1 | tail -5
```

Expected: the grep prints nothing, and `xcodebuild` ends with `BUILD SUCCEEDED`.

If `xcodebuild` fails because no iOS 26 simulator runtime is installed, note the exact error and stop — that is a real environment gap, not something to work around by reverting the deployment target.

- [ ] **Step 8: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add app/AJMiddleCar/CarNet.swift app/AJMiddleCar/CarHTTP.swift \
        app/AJMiddleCar/HTTPParse.swift app/AJMiddleCarTests/HTTPParseTests.swift
git add -A app/AJMiddleCar/AJMiddleCarApp.swift app/AJMiddleCar/CarConnection.swift \
           app/AJMiddleCar/AppFlow.swift app/AJMiddleCar/DiagProbe.swift \
           app/project.yml firmware/p4/main/telemetry.c
git commit -m "chore: commit the pinned-networking transport, drop the bench diagnostics

The Wi-Fi-pinned transport landed as four untracked files while tracked callers
referenced them, so a clean clone did not build. XcodeGen globs the directory,
which is why the project file never showed it.

The same session left diagnostics marked NOT FOR COMMIT running for real: a
lock-taking counter with two Date allocations on every one of 36,000 control
ticks an hour, a launch-time probe that leaked two NWPathMonitors and hardcoded
192.168.4.1 even in simulator builds, and a WARN-level log emitted from a
priority-22 timer callback on the car.

Deployment target moves to iOS 26, which the re-architecture assumes.
"
```

---

### Task 2: The contract schema

**Files:**
- Create: `contract/car-api.json`
- Create: `tools/test_gen_contract.py`

**Interfaces:**
- Produces: a schema whose top level is `{proto, device, network, rt, domains}`; `domains` is a list of `{path, nvs_key, swift, doc, fields}`; each field is `{name, type, doc}` plus `min`/`max`/`default` for `int`, `default` for `bool`, `values`/`default` for `enum`.

The ranges and defaults below are copied from the firmware as it stands today — `wheel.h:8-13`, `wheel.c:10-12`, `dims.h:8-11`, `dims.c:9`, `ramp.c:15-16`, `ramp_api.c:29`, `trim_api.c:29`, `car.c:31`, `recovery.h:9-10`, `recovery.c:25-26`. Task 2's last test asserts that correspondence, so the schema cannot silently invent a range.

Note one deliberate correction, recorded in the spec: `/recover` defaults are `enabled=true, window_ms=5000` — the firmware's values. `docs/protocol.md` says `3000` and the mock says `off/3000`; both are wrong and this schema is what fixes them.

- [ ] **Step 1: Write the failing test**

Create `tools/test_gen_contract.py`:

```python
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
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
cd ~/VSCode/esp32-p4-car && python3 tools/test_gen_contract.py -v
```

Expected: every test errors with `FileNotFoundError: .../contract/car-api.json`.

- [ ] **Step 3: Write the schema**

Create `contract/car-api.json`:

```json
{
  "proto": 1,
  "device": "ajmiddlecar",
  "network": {
    "ssid": "AJMiddleCar",
    "password": "drive1234",
    "host": "192.168.4.1"
  },
  "rt": {
    "port": 4210,
    "max_datagram": 96,
    "command_hz": 10,
    "telemetry_hz": 5,
    "watchdog_ms": 300
  },
  "domains": [
    {
      "path": "/ramp",
      "nvs_key": "ramp",
      "swift": "Ramp",
      "doc": "Slew-rate limit on acceleration. Rise is bounded, fall is instant, so stopping is never delayed.",
      "fields": [
        {
          "name": "ramp_ms",
          "type": "int",
          "min": 0,
          "max": 2000,
          "default": 300,
          "doc": "time from zero to full scale in ms; 0 disables the ramp"
        }
      ]
    },
    {
      "path": "/trim",
      "nvs_key": "trim",
      "swift": "Trim",
      "doc": "Straight-line correction. Positive slows the left side, negative slows the right; it only ever attenuates.",
      "fields": [
        {
          "name": "trim_pct",
          "type": "int",
          "min": -30,
          "max": 30,
          "default": 0,
          "doc": "percentage by which the faster side is slowed"
        }
      ]
    },
    {
      "path": "/recover",
      "nvs_key": "recover",
      "swift": "Recover",
      "doc": "Reverse-replay retreat on unexpected link loss. A deliberate goodbye suppresses it.",
      "fields": [
        {
          "name": "enabled",
          "type": "bool",
          "default": true,
          "doc": "retrace on unexpected silence; when false the car stops instead"
        },
        {
          "name": "window_ms",
          "type": "int",
          "min": 1000,
          "max": 10000,
          "default": 5000,
          "doc": "how far back the breadcrumb history reaches"
        }
      ]
    },
    {
      "path": "/wheel",
      "nvs_key": "wheel",
      "swift": "Wheel",
      "doc": "Wheel and encoder geometry. Stored on the car; speed is not yet computed from it.",
      "fields": [
        {
          "name": "diameter_mm",
          "type": "int",
          "min": 20,
          "max": 150,
          "default": 65,
          "doc": "wheel diameter in mm"
        },
        {
          "name": "ppr",
          "type": "int",
          "min": 1,
          "max": 1000,
          "default": 11,
          "doc": "encoder pulses per motor-shaft revolution, one channel"
        },
        {
          "name": "gear_x100",
          "type": "int",
          "min": 100,
          "max": 30000,
          "default": 2100,
          "doc": "gear ratio times 100; 1:21 is 2100"
        },
        {
          "name": "quad",
          "type": "enum",
          "values": [1, 2, 4],
          "default": 4,
          "doc": "quadrature edge multiplier"
        }
      ]
    },
    {
      "path": "/dims",
      "nvs_key": "dims",
      "swift": "Dims",
      "doc": "Distances between wheel centres. The track feeds the app's manoeuvre geometry.",
      "fields": [
        {
          "name": "track_mm",
          "type": "int",
          "min": 60,
          "max": 300,
          "default": 130,
          "doc": "lateral distance between left and right wheel centres"
        },
        {
          "name": "wheelbase_mm",
          "type": "int",
          "min": 90,
          "max": 360,
          "default": 210,
          "doc": "longitudinal distance between front and rear wheel centres"
        }
      ]
    }
  ]
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd ~/VSCode/esp32-p4-car && python3 tools/test_gen_contract.py -v
```

Expected: `Ran 5 tests` and `OK`.

- [ ] **Step 5: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add contract/car-api.json tools/test_gen_contract.py
git commit -m "feat(contract): the schema, with ranges asserted against the firmware

Five config domains, their fields, ranges and defaults, plus the real-time
channel constants and the protocol version. A test reads the firmware's own
headers and fails if the schema claims a range the C does not carry, so the
schema cannot drift away from the code it describes at birth.

One correction rides along: /recover's real defaults are enabled=true and
window_ms=5000. docs/protocol.md said 3000 and the mock said off/3000. Both
were wrong, and a developer working against the mock would conclude a fresh
car stops on link loss when it actually reverses for five seconds.
"
```

---

### Task 3: The generator, and the doc it writes

The doc emitter comes first because its output is the easiest to eyeball, which makes the generator's shape obvious before three more emitters hang off it.

**Files:**
- Create: `tools/gen_contract.py`
- Modify: `tools/test_gen_contract.py`
- Modify: `docs/protocol.md`

**Interfaces:**
- Consumes: `contract/car-api.json` from Task 2.
- Produces: `load_schema(path=SCHEMA) -> dict`; `emit_doc(schema) -> str`; `MARK_BEGIN = "<!-- generated:endpoints -->"`, `MARK_END = "<!-- /generated:endpoints -->"`; `splice(existing: str, block: str) -> str`; `main(argv) -> int` writing every artefact, accepting `--out-dir` to redirect all four outputs under a directory root.

- [ ] **Step 1: Write the failing tests**

Append to `tools/test_gen_contract.py`, above the `if __name__` block:

```python
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
            import filecmp
            cmp = filecmp.dircmp(a, b)
            self.assertEqual(cmp.diff_files, [])
            self.assertEqual(cmp.left_only, [])
            self.assertEqual(cmp.right_only, [])
```

- [ ] **Step 2: Run them to verify they fail**

```bash
cd ~/VSCode/esp32-p4-car && python3 tools/test_gen_contract.py -v
```

Expected: `ModuleNotFoundError: No module named 'gen_contract'`.

- [ ] **Step 3: Write the generator with its doc emitter**

Create `tools/gen_contract.py`:

```python
#!/usr/bin/env python3
"""Emit every expression of the app<->car contract from one schema.

Source of truth: contract/car-api.json. Everything this writes carries a header
saying so and must never be hand-edited; tools/check_contract.sh fails a build
where the committed output and a fresh run disagree.
"""
import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "contract" / "car-api.json"

MARK_BEGIN = "<!-- generated:endpoints -->"
MARK_END = "<!-- /generated:endpoints -->"

BANNER = "generated from contract/car-api.json by tools/gen_contract.py - do not edit"


def load_schema(path=SCHEMA):
    return json.loads(pathlib.Path(path).read_text())


def field_range(f):
    """Human-readable range for one field, escaped for a Markdown table cell."""
    if f["type"] == "int":
        return f"{f['min']}..{f['max']}"
    if f["type"] == "enum":
        return " \\| ".join(str(v) for v in f["values"])
    return "true \\| false"


def emit_doc(schema):
    lines = [
        "| Endpoint | GET returns | POST body | Ranges |",
        "|---|---|---|---|",
    ]
    for d in schema["domains"]:
        shape = ", ".join(f'"{f["name"]}":…' for f in d["fields"])
        ranges = "<br>".join(f"`{f['name']}` {field_range(f)}" for f in d["fields"])
        lines.append(f"| `{d['path']}` | `{{{shape}}}` | same | {ranges} |")
    return "\n".join(lines)


def splice(existing, block):
    i = existing.find(MARK_BEGIN)
    j = existing.find(MARK_END)
    if i < 0 or j < 0 or j < i:
        raise ValueError(f"markers {MARK_BEGIN} / {MARK_END} not found in the document")
    return existing[: i + len(MARK_BEGIN)] + "\n" + block + "\n" + existing[j:]


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text if text.endswith("\n") else text + "\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--schema", default=str(SCHEMA))
    ap.add_argument("--out-dir", default=None,
                    help="write every artefact under this root instead of in place")
    args = ap.parse_args(argv)

    schema = load_schema(args.schema)
    root = pathlib.Path(args.out_dir) if args.out_dir else ROOT

    doc_path = root / "docs" / "protocol.md"
    if args.out_dir:
        write(doc_path, MARK_BEGIN + "\n" + emit_doc(schema) + "\n" + MARK_END)
    else:
        write(doc_path, splice(doc_path.read_text(), emit_doc(schema)))

    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Put the markers into the doc**

In `docs/protocol.md`, find the endpoint table under `## Configuration — REST`. It begins with the line `| Endpoint | GET returns | POST body | Range |` and ends with the `/dims` row. Replace that whole table — header, separator and all seven rows — with:

```markdown
<!-- generated:endpoints -->
<!-- /generated:endpoints -->
```

Leave every surrounding paragraph, including the `/calib` prose and the "What the values mean" section, exactly as it is. The `/calib` rows leave the table because they are not table-driven config domains; the prose that describes them already stands on its own.

- [ ] **Step 5: Run the generator and the tests**

```bash
cd ~/VSCode/esp32-p4-car
python3 tools/gen_contract.py
python3 tools/test_gen_contract.py -v
git diff --stat docs/protocol.md
```

Expected: `Ran 8 tests` and `OK`, and `docs/protocol.md` shows a table between the markers with five rows.

- [ ] **Step 6: Read the generated table**

```bash
cd ~/VSCode/esp32-p4-car && sed -n '/generated:endpoints/,/\/generated:endpoints/p' docs/protocol.md
```

Confirm by eye that `/recover` reads `window_ms 1000..10000` and that `quad` renders as `1 | 2 | 4` rather than as a broken table cell.

- [ ] **Step 7: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add tools/gen_contract.py tools/test_gen_contract.py docs/protocol.md
git commit -m "feat(contract): generator, and the doc table it now owns

docs/protocol.md keeps its prose and gives up its endpoint table to a marked
region the generator rewrites. The splice refuses a document without markers
rather than appending, so a doc that loses them fails loudly.

A determinism test runs the generator twice into temp directories and compares
byte for byte, because a generator whose output depends on dict ordering makes
the drift check it is meant to enable useless.
"
```

---

### Task 4: The C descriptor table

**Files:**
- Create: `firmware/p4/main/cfg_contract.h`
- Create: `firmware/p4/main/cfg_table.inc` (generated)
- Create: `firmware/p4/test/test_cfg_table.c`
- Modify: `tools/gen_contract.py`, `tools/test_gen_contract.py`, `firmware/p4/test/Makefile`

**Interfaces:**
- Consumes: `load_schema`, `write`, `BANNER` from Task 3.
- Produces: `emit_c(schema) -> str`. C side: `cfg_type_t {CFG_INT, CFG_BOOL, CFG_ENUM}`; `cfg_field_t {const char *name; cfg_type_t type; int32_t min, max, def; const int32_t *allowed; uint8_t n_allowed;}`; `cfg_domain_t {const char *path; const char *nvs_key; const cfg_field_t *fields; uint8_t n_fields;}`; and in the generated include, `static const cfg_domain_t CFG_DOMAINS[]` with `CFG_DOMAIN_COUNT`.

The table carries data only. Binding a domain to its `*_get`/`*_set` functions stays hand-written in `cfg_api.c` in Plan B, because that binding is the one part not derivable from the schema.

- [ ] **Step 1: Write the failing tests**

Append to `tools/test_gen_contract.py`:

```python
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
```

And a C host test, `firmware/p4/test/test_cfg_table.c`:

```c
/* The generated table must compile as plain C and carry the schema's numbers. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cfg_contract.h"
#include "cfg_table.inc"

static const cfg_field_t *find(const char *path, const char *name) {
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        if (strcmp(CFG_DOMAINS[i].path, path) != 0) continue;
        for (int f = 0; f < CFG_DOMAINS[i].n_fields; f++) {
            if (strcmp(CFG_DOMAINS[i].fields[f].name, name) == 0) {
                return &CFG_DOMAINS[i].fields[f];
            }
        }
    }
    return NULL;
}

int main(void) {
    assert(CFG_DOMAIN_COUNT == 5);

    const cfg_field_t *d = find("/wheel", "diameter_mm");
    assert(d && d->type == CFG_INT && d->min == 20 && d->max == 150 && d->def == 65);

    const cfg_field_t *q = find("/wheel", "quad");
    assert(q && q->type == CFG_ENUM && q->n_allowed == 3);
    assert(q->allowed[0] == 1 && q->allowed[1] == 2 && q->allowed[2] == 4);

    const cfg_field_t *e = find("/recover", "enabled");
    assert(e && e->type == CFG_BOOL && e->def == 1);

    const cfg_field_t *w = find("/recover", "window_ms");
    assert(w && w->min == 1000 && w->max == 10000 && w->def == 5000);

    const cfg_field_t *t = find("/trim", "trim_pct");
    assert(t && t->min == -30 && t->max == 30 && t->def == 0);

    assert(find("/wheel", "nonexistent") == NULL);

    /* Every domain must name a distinct NVS key: two domains sharing one key
       would silently overwrite each other's stored config. */
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        for (int j = i + 1; j < CFG_DOMAIN_COUNT; j++) {
            assert(strcmp(CFG_DOMAINS[i].nvs_key, CFG_DOMAINS[j].nvs_key) != 0);
        }
    }

    printf("test_cfg_table: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run them to verify they fail**

```bash
cd ~/VSCode/esp32-p4-car && python3 tools/test_gen_contract.py -v 2>&1 | tail -20
```

Expected: `AttributeError: module 'gen_contract' has no attribute 'emit_c'`.

- [ ] **Step 3: Write the hand-written C types**

Create `firmware/p4/main/cfg_contract.h`:

```c
#ifndef CFG_CONTRACT_H
#define CFG_CONTRACT_H

#include <stdint.h>

/* Types for the generated config descriptor table (cfg_table.inc).
   This header is hand-written; the table that uses it is not. */

typedef enum { CFG_INT, CFG_BOOL, CFG_ENUM } cfg_type_t;

typedef struct {
    const char    *name;
    cfg_type_t     type;
    int32_t        min;         /* for CFG_BOOL: 0..1; for CFG_ENUM: the value bounds */
    int32_t        max;
    int32_t        def;
    const int32_t *allowed;     /* NULL unless CFG_ENUM */
    uint8_t        n_allowed;
} cfg_field_t;

typedef struct {
    const char        *path;
    const char        *nvs_key;
    const cfg_field_t *fields;
    uint8_t            n_fields;
} cfg_domain_t;

#endif /* CFG_CONTRACT_H */
```

- [ ] **Step 4: Add the C emitter**

In `tools/gen_contract.py`, add above `main`:

```python
def _c_field(domain, f):
    if f["type"] == "int":
        return f'{{ "{f["name"]}", CFG_INT, {f["min"]}, {f["max"]}, {f["default"]}, NULL, 0 }}'
    if f["type"] == "bool":
        return f'{{ "{f["name"]}", CFG_BOOL, 0, 1, {1 if f["default"] else 0}, NULL, 0 }}'
    sym = _allowed_symbol(domain, f)
    return (f'{{ "{f["name"]}", CFG_ENUM, {min(f["values"])}, {max(f["values"])}, '
            f'{f["default"]}, {sym}, {len(f["values"])} }}')


def _allowed_symbol(domain, f):
    return f"CFG_{domain['nvs_key'].upper()}_{f['name'].upper()}_ALLOWED"


def emit_c(schema):
    out = [f"/* {BANNER} */", "", '#include "cfg_contract.h"', ""]
    for d in schema["domains"]:
        for f in d["fields"]:
            if f["type"] == "enum":
                vals = ", ".join(str(v) for v in f["values"])
                out.append(f"static const int32_t {_allowed_symbol(d, f)}[] = {{ {vals} }};")
    out.append("")
    for d in schema["domains"]:
        name = f"CFG_{d['nvs_key'].upper()}_FIELDS"
        out.append(f"static const cfg_field_t {name}[] = {{")
        for f in d["fields"]:
            out.append("    " + _c_field(d, f) + ",")
        out.append("};")
    out.append("")
    out.append("static const cfg_domain_t CFG_DOMAINS[] = {")
    for d in schema["domains"]:
        name = f"CFG_{d['nvs_key'].upper()}_FIELDS"
        out.append(f'    {{ "{d["path"]}", "{d["nvs_key"]}", {name}, '
                   f'{len(d["fields"])} }},')
    out.append("};")
    out.append("")
    out.append(f"#define CFG_DOMAIN_COUNT {len(schema['domains'])}")
    return "\n".join(out)
```

and inside `main`, after the doc is written:

```python
    write(root / "firmware" / "p4" / "main" / "cfg_table.inc", emit_c(schema))
```

- [ ] **Step 5: Generate, then wire the C test into the host Makefile**

```bash
cd ~/VSCode/esp32-p4-car && python3 tools/gen_contract.py && head -20 firmware/p4/main/cfg_table.inc
```

Then open `firmware/p4/test/Makefile` and read how the existing host tests are declared. Add `test_cfg_table` to the same list the other tests use, following the file's existing pattern exactly — it already compiles sources from `../main` with plain `cc`, so the new test needs the same include path and no new flags.

- [ ] **Step 6: Run the host tests**

```bash
cd ~/VSCode/esp32-p4-car/firmware/p4/test && make run
```

Expected: the existing tests still pass and `test_cfg_table: OK` appears.

- [ ] **Step 7: Run the Python tests**

```bash
cd ~/VSCode/esp32-p4-car && python3 tools/test_gen_contract.py -v
```

Expected: `Ran 10 tests` and `OK`.

- [ ] **Step 8: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add firmware/p4/main/cfg_contract.h firmware/p4/main/cfg_table.inc \
        firmware/p4/test/test_cfg_table.c firmware/p4/test/Makefile \
        tools/gen_contract.py tools/test_gen_contract.py
git commit -m "feat(contract): generated C descriptor table, host-tested

The table carries data only — names, types, ranges, defaults and the NVS key.
Binding a domain to its getter and setter stays hand-written, because that is
the one part the schema cannot describe.

The host test compiles the generated include with plain cc and asserts the
numbers, including that no two domains share an NVS key: that mistake would
have one domain silently overwrite another's stored config, and it is exactly
the kind of thing a hand-copied table eventually does.
"
```

---

### Task 5: The Swift structs

**Files:**
- Create: `app/AJMiddleCar/Generated/CarAPI.swift` (generated)
- Create: `app/tests/test_carapi.swift`
- Modify: `tools/gen_contract.py`, `tools/test_gen_contract.py`

**Interfaces:**
- Consumes: `load_schema`, `write`, `BANNER`.
- Produces: `emit_swift(schema) -> str`. Swift side, per domain: `struct Ramp: Codable, Equatable, Sendable { var ramp_ms: Int }` with `static let `default`: Ramp`, `static let path: String`, and one `static let <field>Range: ClosedRange<Int>` per int field or `static let <field>Allowed: [Int]` per enum field. Plus `enum CarContract { static let proto = 1; static let device = "ajmiddlecar"; static let rtPort: UInt16 = 4210; static let maxDatagram = 96; static let commandHz = 10; static let telemetryHz = 5; static let watchdogMs = 300; static let ssid = "AJMiddleCar"; static let host = "192.168.4.1" }`.

Wire names are kept verbatim as Swift property names so no `CodingKeys` are needed. That is deliberate: a hand-written `CodingKeys` block is exactly the kind of thing that drifts.

- [ ] **Step 1: Write the failing tests**

Append to `tools/test_gen_contract.py`:

```python
class TestSwiftEmitter(unittest.TestCase):
    def test_structs_and_constants(self):
        import gen_contract
        out = gen_contract.emit_swift(load())
        self.assertIn(gen_contract.BANNER, out)
        self.assertIn("public struct Wheel: Codable, Equatable, Sendable {", out)
        self.assertIn("public var diameter_mm: Int", out)
        self.assertIn("public var enabled: Bool", out)
        self.assertIn("public static let diameter_mmRange: ClosedRange<Int> = 20...150", out)
        self.assertIn("public static let quadAllowed: [Int] = [1, 2, 4]", out)
        self.assertIn('public static let path = "/wheel"', out)
        self.assertIn("public static let rtPort: UInt16 = 4210", out)
        self.assertIn("public static let proto = 1", out)

    def test_default_uses_the_schema_values(self):
        import gen_contract
        out = gen_contract.emit_swift(load())
        self.assertIn("Wheel(diameter_mm: 65, ppr: 11, gear_x100: 2100, quad: 4)", out)
        self.assertIn("Recover(enabled: true, window_ms: 5000)", out)
```

And a Swift host test, `app/tests/test_carapi.swift`:

```swift
// Host test for the generated contract. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// Defaults match the schema.
check(Wheel.default == Wheel(diameter_mm: 65, ppr: 11, gear_x100: 2100, quad: 4),
      "Wheel.default")
check(Recover.default == Recover(enabled: true, window_ms: 5000), "Recover.default")
check(Dims.default == Dims(track_mm: 130, wheelbase_mm: 210), "Dims.default")
check(Ramp.default == Ramp(ramp_ms: 300), "Ramp.default")
check(Trim.default == Trim(trim_pct: 0), "Trim.default")

// Ranges are the firmware's.
check(Wheel.diameter_mmRange == 20...150, "diameter range")
check(Trim.trim_pctRange == -30...30, "trim range")
check(Recover.window_msRange == 1000...10000, "window range")
check(Wheel.quadAllowed == [1, 2, 4], "quad allowed")

// Round-trips over the wire names the car actually sends.
let json = #"{"diameter_mm":70,"ppr":12,"gear_x100":960,"quad":2}"#
let decoded = try! JSONDecoder().decode(Wheel.self, from: Data(json.utf8))
check(decoded.diameter_mm == 70 && decoded.gear_x100 == 960, "decode")
let reencoded = try! JSONEncoder().encode(decoded)
let back = try! JSONDecoder().decode(Wheel.self, from: reencoded)
check(back == decoded, "round trip")

// Contract constants.
check(CarContract.proto == 1, "proto")
check(CarContract.device == "ajmiddlecar", "device")
check(CarContract.rtPort == 4210, "rt port")
check(CarContract.watchdogMs == 300, "watchdog")

if failures == 0 { print("test_carapi: OK") } else { exit(1) }
```

- [ ] **Step 2: Run them to verify they fail**

```bash
cd ~/VSCode/esp32-p4-car && python3 tools/test_gen_contract.py -v 2>&1 | tail -20
```

Expected: `AttributeError: module 'gen_contract' has no attribute 'emit_swift'`.

- [ ] **Step 3: Add the Swift emitter**

In `tools/gen_contract.py`, add above `main`:

```python
def _swift_type(f):
    return "Bool" if f["type"] == "bool" else "Int"


def _swift_literal(f):
    return ("true" if f["default"] else "false") if f["type"] == "bool" else str(f["default"])


def emit_swift(schema):
    rt, net = schema["rt"], schema["network"]
    out = [f"// {BANNER}", "", "import Foundation", "",
           "public enum CarContract {",
           f"    public static let proto = {schema['proto']}",
           f'    public static let device = "{schema["device"]}"',
           f'    public static let ssid = "{net["ssid"]}"',
           f'    public static let password = "{net["password"]}"',
           f'    public static let host = "{net["host"]}"',
           f"    public static let rtPort: UInt16 = {rt['port']}",
           f"    public static let maxDatagram = {rt['max_datagram']}",
           f"    public static let commandHz = {rt['command_hz']}",
           f"    public static let telemetryHz = {rt['telemetry_hz']}",
           f"    public static let watchdogMs = {rt['watchdog_ms']}",
           "}", ""]
    for d in schema["domains"]:
        n = d["swift"]
        out.append(f"/// {d['doc']}")
        out.append(f"public struct {n}: Codable, Equatable, Sendable {{")
        for f in d["fields"]:
            out.append(f"    /// {f['doc']}")
            out.append(f"    public var {f['name']}: {_swift_type(f)}")
        args = ", ".join(f"{f['name']}: {_swift_type(f)}" for f in d["fields"])
        assigns = "; ".join(f"self.{f['name']} = {f['name']}" for f in d["fields"])
        out.append(f"    public init({args}) {{ {assigns} }}")
        out.append("}")
        out.append("")
        out.append(f"public extension {n} {{")
        out.append(f'    static let path = "{d["path"]}"')
        lit = ", ".join(f"{f['name']}: {_swift_literal(f)}" for f in d["fields"])
        out.append(f"    static let `default` = {n}({lit})")
        for f in d["fields"]:
            if f["type"] == "int":
                out.append(f"    static let {f['name']}Range: ClosedRange<Int> "
                           f"= {f['min']}...{f['max']}")
            elif f["type"] == "enum":
                vals = ", ".join(str(v) for v in f["values"])
                out.append(f"    static let {f['name']}Allowed: [Int] = [{vals}]")
        out.append("}")
        out.append("")
    return "\n".join(out)
```

and inside `main`, after the C table:

```python
    write(root / "app" / "AJMiddleCar" / "Generated" / "CarAPI.swift", emit_swift(schema))
```

- [ ] **Step 4: Generate and run the Swift host test**

```bash
cd ~/VSCode/esp32-p4-car
python3 tools/gen_contract.py
swiftc -o /tmp/test_carapi app/AJMiddleCar/Generated/CarAPI.swift app/tests/test_carapi.swift \
  && /tmp/test_carapi
```

Expected: `test_carapi: OK`.

- [ ] **Step 5: Run the Python tests**

```bash
cd ~/VSCode/esp32-p4-car && python3 tools/test_gen_contract.py -v
```

Expected: `Ran 12 tests` and `OK`.

- [ ] **Step 6: Confirm the app still builds with the generated file in the target**

`app/project.yml` lists `sources: - AJMiddleCar`, so `Generated/` is picked up by the glob and needs no project change.

```bash
cd ~/VSCode/esp32-p4-car/app && xcodegen generate && \
  xcodebuild build -scheme AJMiddleCar \
    -destination 'platform=iOS Simulator,name=iPhone 17' \
    -derivedDataPath /tmp/ddata-middle 2>&1 | tail -5
```

Expected: `BUILD SUCCEEDED`. Nothing consumes the generated types yet; this only proves they compile inside the target.

- [ ] **Step 7: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add app/AJMiddleCar/Generated/CarAPI.swift app/tests/test_carapi.swift \
        tools/gen_contract.py tools/test_gen_contract.py
git commit -m "feat(contract): generated Swift structs, host-tested with swiftc

Wire names are kept verbatim as property names so no CodingKeys block exists to
drift. Each domain gets its defaults and its ranges as constants, which is what
lets ConfigStore tell 'not read yet' from 'the car said 65' in Plan D — the
distinction whose absence currently lets a failed GET be POSTed back as if it
were the car's own configuration.
"
```

---

### Task 6: The mock's table, and the drift check

**Files:**
- Create: `tools/mock_car/generated.py` (generated)
- Create: `tools/check_contract.sh`
- Modify: `tools/gen_contract.py`, `tools/test_gen_contract.py`

**Interfaces:**
- Consumes: `load_schema`, `write`, `BANNER`.
- Produces: `emit_python(schema) -> str`. Python side: `PROTO`, `DEVICE`, `RT` dict, and `DOMAINS: dict[str, dict]` where each entry is `{"key": str, "defaults": dict, "fields": list}`; plus `validate(path, body) -> tuple[bool, str]` returning `(True, "")` or `(False, reason)`.

`validate` is the mock's whole reason for existing in this plan: today the mock accepts things the car rejects, so the app is developed against a more permissive car than the real one.

- [ ] **Step 1: Write the failing tests**

Append to `tools/test_gen_contract.py`:

```python
class TestPythonEmitter(unittest.TestCase):
    def setUp(self):
        import gen_contract
        ns = {}
        exec(gen_contract.emit_python(load()), ns)
        self.ns = ns

    def test_table(self):
        self.assertEqual(self.ns["PROTO"], 1)
        self.assertEqual(self.ns["DEVICE"], "ajmiddlecar")
        self.assertEqual(self.ns["RT"]["port"], 4210)
        self.assertEqual(set(self.ns["DOMAINS"]),
                         {"/ramp", "/trim", "/recover", "/wheel", "/dims"})
        self.assertEqual(self.ns["DOMAINS"]["/recover"]["defaults"],
                         {"enabled": True, "window_ms": 5000})

    def test_validate_accepts_the_defaults(self):
        v = self.ns["validate"]
        for path, d in self.ns["DOMAINS"].items():
            ok, why = v(path, dict(d["defaults"]))
            self.assertTrue(ok, f"{path}: {why}")

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
```

- [ ] **Step 2: Run them to verify they fail**

```bash
cd ~/VSCode/esp32-p4-car && python3 tools/test_gen_contract.py -v 2>&1 | tail -20
```

Expected: `AttributeError: module 'gen_contract' has no attribute 'emit_python'`.

- [ ] **Step 3: Add the Python emitter**

In `tools/gen_contract.py`, add above `main`:

```python
def emit_python(schema):
    body = {
        d["path"]: {
            "key": d["nvs_key"],
            "defaults": {f["name"]: f["default"] for f in d["fields"]},
            "fields": d["fields"],
        }
        for d in schema["domains"]
    }
    return "\n".join([
        f"# {BANNER}",
        "",
        f"PROTO = {schema['proto']}",
        f"DEVICE = {schema['device']!r}",
        f"NETWORK = {schema['network']!r}",
        f"RT = {schema['rt']!r}",
        "",
        f"DOMAINS = {json.dumps(body, indent=4, sort_keys=False)}",
        "",
        "",
        "def validate(path, body):",
        '    """Return (True, "") or (False, reason). Mirrors the firmware exactly."""',
        "    domain = DOMAINS.get(path)",
        "    if domain is None:",
        '        return False, f"unknown endpoint {path}"',
        "    for f in domain[\"fields\"]:",
        "        name = f[\"name\"]",
        "        if name not in body:",
        '            return False, f"missing {name}"',
        "        v = body[name]",
        "        if f[\"type\"] == \"bool\":",
        "            if not isinstance(v, bool):",
        '                return False, f"{name} must be a boolean"',
        "            continue",
        "        if isinstance(v, bool) or not isinstance(v, int):",
        '            return False, f"{name} must be an integer"',
        "        if f[\"type\"] == \"enum\":",
        "            if v not in f[\"values\"]:",
        '                return False, f"{name} must be one of {f[\'values\']}"',
        "        elif not (f[\"min\"] <= v <= f[\"max\"]):",
        '            return False, f"{name} must be {f[\'min\']}..{f[\'max\']}"',
        '    return True, ""',
        "",
    ])
```

and inside `main`, after the Swift file:

```python
    write(root / "tools" / "mock_car" / "generated.py", emit_python(schema))
```

- [ ] **Step 4: Write the drift check**

Create `tools/check_contract.sh`:

```bash
#!/usr/bin/env bash
# Fail if any generated artefact differs from a fresh run of the generator.
# The contract lives in contract/car-api.json; nothing it produces is hand-edited.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

python3 "$ROOT/tools/gen_contract.py" --out-dir "$TMP"

status=0
for rel in firmware/p4/main/cfg_table.inc \
           app/AJMiddleCar/Generated/CarAPI.swift \
           tools/mock_car/generated.py; do
    if ! diff -u "$ROOT/$rel" "$TMP/$rel" > /dev/null 2>&1; then
        echo "DRIFT: $rel differs from a fresh generation" >&2
        diff -u "$ROOT/$rel" "$TMP/$rel" >&2 || true
        status=1
    fi
done

# docs/protocol.md is spliced into hand-written prose, so compare only the region.
region() { sed -n '/generated:endpoints/,/\/generated:endpoints/p' "$1"; }
if ! diff -u <(region "$ROOT/docs/protocol.md") <(region "$TMP/docs/protocol.md") > /dev/null 2>&1; then
    echo "DRIFT: docs/protocol.md generated region differs" >&2
    diff -u <(region "$ROOT/docs/protocol.md") <(region "$TMP/docs/protocol.md") >&2 || true
    status=1
fi

if [ "$status" -eq 0 ]; then echo "contract: no drift"; fi
exit "$status"
```

```bash
chmod +x ~/VSCode/esp32-p4-car/tools/check_contract.sh
```

- [ ] **Step 5: Generate and run everything**

```bash
cd ~/VSCode/esp32-p4-car
python3 tools/gen_contract.py
python3 tools/test_gen_contract.py -v
bash tools/check_contract.sh
```

Expected: `Ran 20 tests`, `OK`, then `contract: no drift`.

- [ ] **Step 6: Prove the drift check actually catches drift**

```bash
cd ~/VSCode/esp32-p4-car
printf '\n// hand-edited\n' >> app/AJMiddleCar/Generated/CarAPI.swift
bash tools/check_contract.sh; echo "exit=$?"
git checkout app/AJMiddleCar/Generated/CarAPI.swift
bash tools/check_contract.sh
```

Expected: the first run prints `DRIFT: app/AJMiddleCar/Generated/CarAPI.swift ...` and `exit=1`; after the checkout it prints `contract: no drift`. A check that cannot fail is not a check.

- [ ] **Step 7: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add tools/mock_car/generated.py tools/check_contract.sh \
        tools/gen_contract.py tools/test_gen_contract.py
git commit -m "feat(contract): generated mock table with a validator, plus the drift check

The mock now has the firmware's ranges rather than its own opinion. That matters
more than it sounds: development runs against the mock, and the mock has been
the more permissive of the two, so the app has been written against a car that
accepts things the real one rejects.

validate() refuses a bool where an int belongs, because in Python True == 1 and
a naive isinstance check lets {'ramp_ms': true} through.

check_contract.sh regenerates into a temp dir and diffs. The plan's own step
proves it fails on a hand-edit before it is trusted.
"
```

---

### Task 7: Wire the checks into the project's test entry points

The generator is only load-bearing if a normal test run exercises it. Right now the project has two entry points: `firmware/p4/test/make run` for C, and ad-hoc `swiftc` invocations for Swift.

**Files:**
- Create: `tools/test-all.sh`
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: everything above.
- Produces: `tools/test-all.sh` exiting non-zero if any host test or the drift check fails.

- [ ] **Step 1: Write the script**

Create `tools/test-all.sh`:

```bash
#!/usr/bin/env bash
# Every host test in the project, plus the contract drift check. No hardware,
# no simulator, no ESP-IDF — this is what runs before every commit.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "== contract =="
python3 tools/test_gen_contract.py
bash tools/check_contract.sh

echo "== firmware host tests =="
make -C firmware/p4/test run

echo "== swift host tests =="
swiftc -o /tmp/test_carapi app/AJMiddleCar/Generated/CarAPI.swift app/tests/test_carapi.swift
/tmp/test_carapi

echo "== all green =="
```

```bash
chmod +x ~/VSCode/esp32-p4-car/tools/test-all.sh
```

- [ ] **Step 2: Run it**

```bash
cd ~/VSCode/esp32-p4-car && bash tools/test-all.sh
```

Expected: it ends with `== all green ==`. If the Swift step fails because other Swift host tests exist and are not listed, add them here in the same shape rather than leaving them outside the entry point.

- [ ] **Step 3: Document it**

In `CLAUDE.md`, the `## Build` section currently documents host tests as:

```markdown
**Host tests** (pure modules, no ESP-IDF):

```bash
cd firmware/p4/test && make run
```
```

Replace that block with:

```markdown
**Host tests** — everything that runs without hardware, a simulator or ESP-IDF:

```bash
tools/test-all.sh
```

That covers the contract (schema, generator, drift), the firmware's pure modules and
the app's pure Swift. `make -C firmware/p4/test run` still works on its own for the C half.
```

Then add a new section after `## Layout`:

```markdown
## The contract

`contract/car-api.json` is the source of truth for everything both sides agree on: the
protocol version, the real-time channel's constants, and the five config domains with
their ranges and defaults. `tools/gen_contract.py` emits all four expressions of it —
the firmware's descriptor table, the app's Swift structs, the mock's table and validator,
and the endpoint table inside `docs/protocol.md`.

Never hand-edit a generated file. Change the schema and re-run the generator;
`tools/check_contract.sh` fails a tree where the two disagree.
```

- [ ] **Step 4: Verify the documented command is the one that works**

```bash
cd ~/VSCode/esp32-p4-car && tools/test-all.sh
```

Expected: `== all green ==`. Run it as written in the doc, not with `bash` in front, to prove the executable bit is set.

- [ ] **Step 5: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add tools/test-all.sh CLAUDE.md
git commit -m "chore: one host-test entry point, and document the contract

A generator nobody runs is a second source of truth with extra steps. test-all.sh
puts the drift check next to the tests that already ran, so divergence surfaces
in the same command that catches a broken mixer.
"
```

---

## Self-Review

**Spec coverage.** This plan implements the spec's "The contract is generated" section in full (schema, four artefacts, CI check) and the delivery table's step 0 and step A. It deliberately does **not** implement: the UDP channel, `link.c`, the actuator-safety fixes, `cfg_api.c` consuming the table, or any iOS module — those are Plans B, C and D. The `/calib`, `/status` and `/ota` endpoints are intentionally absent from the schema's `domains`: they are not table-driven config and their prose in `docs/protocol.md` is untouched. Plan B revisits `/status` when it gains `"proto":1`.

**Placeholders.** None. Every code step carries the actual content.

**Type consistency.** `emit_doc`/`emit_c`/`emit_swift`/`emit_python` and `load_schema`/`write`/`splice`/`BANNER` are named identically wherever they appear. `cfg_field_t` and `cfg_domain_t` member names match between `cfg_contract.h` (Task 4 Step 3), the emitter (Task 4 Step 4) and the C test (Task 4 Step 1). Swift `Wheel`/`Ramp`/`Trim`/`Recover`/`Dims` match the schema's `swift` keys.

**Known gap, deliberate.** Task 1 Step 7 may fail if no iOS 26 simulator runtime is installed — the repo's current SDK is `iphonesimulator26.2` with runtime 26.3, so this should hold, but it is the one environment assumption in the plan and the step says to stop rather than work around it.
