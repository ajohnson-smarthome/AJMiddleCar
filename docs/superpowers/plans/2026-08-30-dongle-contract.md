# Dongle — one contract principle for both devices

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The values the app and the dongle must agree on — the address, the port, the identity string, the endpoint paths, the field names, the length bounds and the radio-state vocabulary — live in a schema and are generated into both sides, checked for drift by the same script that guards the car's contract.

**Architecture:** `tools/gen_contract.py` currently hardcodes one schema and four destination paths in `main()`, and `tools/check_contract.sh` holds a second copy of that list. This plan makes the routing a table the generator owns and the checker reads, then adds the dongle as a second entry with two emitters of its own. The car's four artifacts must regenerate byte-identical — that is the safety property, and it is testable.

**Tech Stack:** Python 3 (the generator and its tests), Bash (the drift check), C11 (the generated header and its consumers), Swift (the generated constants, consumed from Plan 4).

**Spec:** `docs/superpowers/specs/2026-08-30-dongle-api-design.md`. Note that this plan **amends** its "hand-written, not generated" section — see the amendment task.

## Global Constraints

- **The car's four generated artifacts must not change by one byte.** `cfg_table.inc`, `CarAPI.swift`, `generated.py` and `docs/protocol.md`'s spliced region are the regression surface for every step here.
- **The dongle still knows nothing about any car.** Its schema carries its own address, its own identity string and its own field names. No SSID, no password, no car device id.
- **`contract/dongle-api.json` does not reference `contract/car-api.json`,** and neither references the other. They are two vocabularies that happen to share a generator.
- The generated C header must be **pure**: `#define`s and nothing else, no ESP-IDF, because `net_cfg.h` includes it and `net_cfg` is host-tested with plain `cc`.
- `tools/test-all.sh` must stay green after every task.

## What this plan does and does not move

**Moves into the schema:** the vocabulary — names, numbers, paths, the state enum. The things two independent codebases must spell identically.

**Stays in `net_cfg`:** the rules — that a password is empty or 8–63 bytes, that control bytes are refused, that rendering escapes and never truncates. Those are behaviour, they are covered by 26 host tests, and the generator's value model could not express them anyway.

This split is the answer to "why did the two devices diverge". They did not, on the axis that matters: **neither side writes an agreed value as a literal**. What differs is that the car's config domains are five tables of ranged integers, which a generator can describe completely, and the dongle's one domain is two strings with a character-class rule, which it cannot.

## Why the car's generator cannot simply absorb the dongle

Worth stating once, because it is the question this plan exists to answer and the next reader will ask it too.

`contract/car-api.json`'s domain fields are `int`, `bool` or `enum` — all **numeric**. `cfg_table.inc` stores them as `int32_t` with `min`/`max`/`default`; `emit_swift` maps them to `Int` or `Bool`; the mock's validator compares numbers. And `cfg_api.c`'s comment names the load-bearing consequence: values move "as an array of int32 in the field order the generated table declares, so the generic handler never needs to know a domain's struct layout" — one handler serving five domains.

A string field cannot travel as an `int32`. Adding one would mean a new type across all four emitters and a rewrite of the generic handler that exists precisely because everything is numeric. That is a large change to the car to serve the dongle, and this plan does not make it.

## File Structure

| File | Responsibility |
|---|---|
| `tools/gen_contract.py` | Gains a `TARGETS` table — one entry per device, naming its schema and its artifacts — and a `--list-artifacts` mode the checker consumes. The car's emitters are untouched |
| `tools/check_contract.sh` | Stops holding its own copy of the artifact list; asks the generator |
| `contract/dongle-api.json` | The dongle's vocabulary: address, port, identity, paths, field names, bounds, the state enum |
| `tools/gen_dongle.py` | The dongle's two emitters, kept out of `gen_contract.py` so the car's file does not grow a second device's shapes |
| `firmware/s3/main/dongle_contract.inc` | Generated. Pure `#define`s |
| `app/AJMiddleCar/Generated/DongleAPI.swift` | Generated. No consumer until Plan 4, deliberately |
| `firmware/s3/main/{net_cfg.h,status_api.c,usb_net.h}` | Stop holding the values the schema now owns |
| `tools/test_gen_contract.py` | Tests for the routing table and the dongle's emitters |
| `docs/superpowers/specs/2026-08-30-dongle-api-design.md` | The "hand-written, not generated" section is amended to what is now true |

---

### Task 1: The generator routes from a table, and the checker reads it

No new device yet. This task changes only *how* the existing four artifacts are addressed, and its whole point is that they come out identical.

**Files:**
- Modify: `tools/gen_contract.py`
- Modify: `tools/check_contract.sh`
- Modify: `tools/test_gen_contract.py`

**Interfaces:**
- Produces: `TARGETS`, a list of dicts, each `{"name": str, "schema": Path, "artifacts": [(relative_path, emitter_callable)], "spliced": [(relative_path, emitter_callable, begin_marker, end_marker)]}`. Task 2 appends the dongle's entry.
- Produces: `python3 tools/gen_contract.py --list-artifacts`, printing one repo-relative path per line — every artifact that is written whole. Spliced files are **not** listed, because they are compared by region rather than by file.

- [ ] **Step 1: Write the failing test for the listing**

Add to `tools/test_gen_contract.py`:

```python
class TestArtifactListing(unittest.TestCase):
    """The generator owns the list of what it writes; check_contract.sh asks rather
    than keeping a second copy that has to be edited in step."""

    def test_list_artifacts_names_every_whole_file(self):
        out = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "gen_contract.py"), "--list-artifacts"],
            capture_output=True, text=True, check=True).stdout.split()
        self.assertIn("firmware/p4/main/cfg_table.inc", out)
        self.assertIn("app/AJMiddleCar/Generated/CarAPI.swift", out)
        self.assertIn("tools/mock_car/generated.py", out)

    def test_list_artifacts_excludes_spliced_files(self):
        # docs/protocol.md is spliced into hand-written prose and is compared by
        # region, so a caller that diffs whole files must not be handed it.
        out = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "gen_contract.py"), "--list-artifacts"],
            capture_output=True, text=True, check=True).stdout.split()
        self.assertNotIn("docs/protocol.md", out)
```

**Where this class goes matters.** `tools/test_gen_contract.py` imports `json`, `pathlib`, `re`
and `unittest` at the top, but `subprocess`, `sys`, `filecmp` and `tempfile` only at **line ~120**,
mid-file, just above the determinism tests. A class placed above that block fails with `NameError`
rather than a test failure. Put this class **after** that import block, and add no imports — every
name it needs is already there.

- [ ] **Step 2: Run it and watch it fail**

```bash
python3 tools/test_gen_contract.py TestArtifactListing -v
```

Expected: failure — `--list-artifacts` is not a recognised argument, so the subprocess exits non-zero and `check=True` raises. Quote the message in your report.

- [ ] **Step 3: Introduce the routing table**

In `tools/gen_contract.py`, above `main()`:

```python
# One entry per device. The car's four artifacts and the dongle's two are addressed the
# same way, so adding a device is a table entry rather than another pair of literals in
# main() — and check_contract.sh reads this list instead of keeping its own copy, which
# was a second thing to remember to edit.
#
# `artifacts` are written whole and diffed whole. `spliced` are written into a marked
# region of a hand-written file and diffed by region; the two cannot be checked the same
# way, which is why they are separate lists rather than one with a flag.
TARGETS = [
    {
        "name": "car",
        "schema": SCHEMA,
        "artifacts": [
            ("firmware/p4/main/cfg_table.inc", emit_c),
            ("app/AJMiddleCar/Generated/CarAPI.swift", emit_swift),
            ("tools/mock_car/generated.py", emit_python),
        ],
        "spliced": [
            ("docs/protocol.md", emit_doc, MARK_BEGIN, MARK_END),
        ],
    },
]
```

- [ ] **Step 4: Rewrite `main()` to walk it**

```python
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--schema", default=None,
                    help="override the schema of a single-target run (tests use this)")
    ap.add_argument("--out-dir", default=None,
                    help="write every artefact under this root instead of in place")
    ap.add_argument("--list-artifacts", action="store_true",
                    help="print the repo-relative path of every whole-file artefact and exit")
    args = ap.parse_args(argv)

    if args.list_artifacts:
        for target in TARGETS:
            for rel, _ in target["artifacts"]:
                print(rel)
        return 0

    root = pathlib.Path(args.out_dir) if args.out_dir else ROOT

    for target in TARGETS:
        # --schema overrides only a single-target run; with several targets it would be
        # ambiguous which one it names, and silently applying it to the first is the kind
        # of helpfulness that hides a mistake.
        schema_path = args.schema if (args.schema and len(TARGETS) == 1) else target["schema"]
        if args.schema and len(TARGETS) > 1:
            raise SystemExit("--schema is ambiguous with more than one target; edit TARGETS instead")
        schema = load_schema(schema_path)

        for rel, emitter in target["artifacts"]:
            write(root / rel, emitter(schema))

        for rel, emitter, begin, end in target["spliced"]:
            path = root / rel
            if args.out_dir:
                write(path, begin + "\n" + emitter(schema) + "\n" + end)
            else:
                write(path, splice(path.read_text(), emitter(schema)))

    return 0
```

Note what this preserves: `--out-dir` still writes the spliced file fresh with its markers rather than splicing, which is what the drift check depends on.

- [ ] **Step 5: Run the new tests and the whole generator suite**

```bash
python3 tools/test_gen_contract.py -v
```

Expected: all pass, including `TestDeterminism`, which runs the generator twice into temp dirs and compares byte-for-byte.

- [ ] **Step 6: Prove the car's artifacts did not move**

This is the task's real deliverable:

```bash
bash tools/check_contract.sh
```

Expected: `contract: no drift`. If anything differs, the routing rewrite changed an output and must be corrected before going further — the car's generated files are not this plan's to touch.

- [ ] **Step 7: Make the checker read the list**

In `tools/check_contract.sh`, replace the hardcoded `for rel in ...` list with the generator's own:

```bash
python3 "$ROOT/tools/gen_contract.py" --out-dir "$TMP"

status=0
# The artefact list comes from the generator: it was duplicated here, and a fifth
# artefact meant editing two files in step or silently checking only some of them.
while IFS= read -r rel; do
    if ! diff -u "$ROOT/$rel" "$TMP/$rel" > /dev/null 2>&1; then
        echo "DRIFT: $rel differs from a fresh generation" >&2
        diff -u "$ROOT/$rel" "$TMP/$rel" >&2 || true
        status=1
    fi
done < <(python3 "$ROOT/tools/gen_contract.py" --list-artifacts)
```

Leave the spliced-region comparison below it exactly as it is.

- [ ] **Step 8: Run the whole suite**

```bash
tools/test-all.sh
```

Expected: `== all green ==`, with `contract: no drift` in the output.

- [ ] **Step 9: Commit**

```bash
git add tools/gen_contract.py tools/check_contract.sh tools/test_gen_contract.py
git commit -m "refactor(contract): route generated artefacts from one table both tools read"
```

---

### Task 2: The dongle's schema and its two emitters

**Files:**
- Create: `contract/dongle-api.json`
- Create: `tools/gen_dongle.py`
- Modify: `tools/gen_contract.py`
- Modify: `tools/test_gen_contract.py`
- Create (generated): `firmware/s3/main/dongle_contract.inc`
- Create (generated): `app/AJMiddleCar/Generated/DongleAPI.swift`

**Interfaces:**
- Consumes from Task 1: the `TARGETS` table and its shape.
- Produces: `emit_dongle_c(schema) -> str` and `emit_dongle_swift(schema) -> str`, both pure functions of the schema dict, both importable from `tools/gen_dongle.py`.
- Produces the two generated files. Task 3 consumes the `.inc`; the Swift file has no consumer until Plan 4.

- [ ] **Step 1: Write the schema**

`contract/dongle-api.json`:

```json
{
  "device": "ajdongle",
  "doc": "The vocabulary the app and the dongle must spell identically. Rules live in firmware/s3/main/net_cfg.{c,h}, which is host-tested; this file carries only names, numbers and paths.",
  "network": {
    "host": "192.168.7.1",
    "port": 8080,
    "doc": "The dongle's own address on the USB wire, and the port it serves. Port 80 and the real-time port are reserved for the car, forwarded through untouched."
  },
  "endpoints": {
    "status": "/status",
    "net": "/net"
  },
  "status_fields": {
    "device": "device",
    "fw": "fw",
    "usb": "usb",
    "net": "net",
    "net_ssid": "ssid",
    "net_state": "state",
    "net_rssi": "rssi"
  },
  "net_fields": {
    "ssid": "ssid",
    "password": "password",
    "configured": "configured"
  },
  "bounds": {
    "ssid_min": 1,
    "ssid_max": 32,
    "pass_min": 8,
    "pass_max": 63,
    "doc": "WPA2's limits, not ours. A password is empty or pass_min..pass_max; net_cfg enforces that, and the character-class rule it also enforces has no expression here."
  },
  "net_states": ["idle", "joining", "connected", "failed"]
}
```

- [ ] **Step 2: Write the failing tests**

Add to `tools/test_gen_contract.py`:

```python
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

    def test_swift_exposes_the_same_vocabulary(self):
        out = self.g.emit_dongle_swift(self.s)
        self.assertIn('public static let device = "ajdongle"', out)
        self.assertIn('public static let host = "192.168.7.1"', out)
        self.assertIn("public static let port: UInt16 = 8080", out)
        self.assertIn('public static let statusPath = "/status"', out)
        self.assertIn('public static let netPath = "/net"', out)
        self.assertIn("public static let ssidMax = 32", out)
        self.assertIn('public static let all = ["idle", "joining", "connected", "failed"]', out)

    def test_both_emitters_are_deterministic(self):
        self.assertEqual(self.g.emit_dongle_c(self.s), self.g.emit_dongle_c(self.s))
        self.assertEqual(self.g.emit_dongle_swift(self.s), self.g.emit_dongle_swift(self.s))
```

Both classes go **after** the mid-file import block described in Task 1, for the same reason.
`json` is imported at the top and `sys.path` already carries `tools/`, so neither class adds an
import.

- [ ] **Step 3: Run them and watch them fail**

```bash
python3 tools/test_gen_contract.py TestDongleSchema TestDongleEmitters -v
```

Expected: `TestDongleSchema` fails on the missing file, `TestDongleEmitters` on the missing module. Quote both.

- [ ] **Step 4: Write the emitters**

`tools/gen_dongle.py`:

```python
#!/usr/bin/env python3
"""Emitters for contract/dongle-api.json.

Separate from gen_contract.py so the car's file does not grow a second device's shapes.
The two schemas share a generator's plumbing and nothing else — neither references the
other, and the dongle's rules (lengths, the character class, escaping) live in
firmware/s3/main/net_cfg.{c,h} where they are host-tested rather than here where they
would only be described.
"""

BANNER = "generated from contract/dongle-api.json by tools/gen_dongle.py - do not edit"


def emit_dongle_c(schema):
    """A pure C header: preprocessor text only.

    net_cfg.h includes this, and net_cfg compiles on the host with plain `cc` under
    -Wall -Wextra -Werror. Anything here that needed a type or a header would break that,
    which is why this emitter produces #defines and nothing else.
    """
    n, b, e = schema["network"], schema["bounds"], schema["endpoints"]
    sf, nf = schema["status_fields"], schema["net_fields"]

    lines = [
        f"/* {BANNER} */",
        "",
        "#ifndef DONGLE_CONTRACT_INC",
        "#define DONGLE_CONTRACT_INC",
        "",
        f'#define DONGLE_DEVICE "{schema["device"]}"',
        f'#define DONGLE_HOST "{n["host"]}"',
        f"#define DONGLE_PORT {n['port']}",
        "",
        f'#define DONGLE_PATH_STATUS "{e["status"]}"',
        f'#define DONGLE_PATH_NET "{e["net"]}"',
        "",
        f"#define DONGLE_SSID_MIN {b['ssid_min']}",
        f"#define DONGLE_SSID_MAX {b['ssid_max']}",
        f"#define DONGLE_PASS_MIN {b['pass_min']}",
        f"#define DONGLE_PASS_MAX {b['pass_max']}",
        "",
    ]
    for key, value in sf.items():
        lines.append(f'#define DONGLE_KEY_{key.upper()} "{value}"')
    lines.append("")
    for key, value in nf.items():
        lines.append(f'#define DONGLE_NETKEY_{key.upper()} "{value}"')
    lines.append("")
    for state in schema["net_states"]:
        lines.append(f'#define DONGLE_STATE_{state.upper()} "{state}"')
    lines += ["", "#endif /* DONGLE_CONTRACT_INC */", ""]
    return "\n".join(lines)


def emit_dongle_swift(schema):
    """The app's half. No consumer until the app-side plan — generated now so that plan
    adds an import rather than a second hand-maintained copy of every name."""
    n, b, e = schema["network"], schema["bounds"], schema["endpoints"]
    nf = schema["net_fields"]
    states = ", ".join(f'"{s}"' for s in schema["net_states"])

    lines = [
        f"// {BANNER}",
        "",
        "public enum DongleContract {",
        f'    public static let device = "{schema["device"]}"',
        f'    public static let host = "{n["host"]}"',
        f"    public static let port: UInt16 = {n['port']}",
        "",
        f'    public static let statusPath = "{e["status"]}"',
        f'    public static let netPath = "{e["net"]}"',
        "",
        f"    public static let ssidMin = {b['ssid_min']}",
        f"    public static let ssidMax = {b['ssid_max']}",
        f"    public static let passMin = {b['pass_min']}",
        f"    public static let passMax = {b['pass_max']}",
        "",
    ]
    for key, value in nf.items():
        lines.append(f'    public static let {key}Field = "{value}"')
    lines += [
        "}",
        "",
        "/// What the dongle's radio is doing, as `/status` reports it.",
        "public enum DongleNetState {",
    ]
    for state in schema["net_states"]:
        lines.append(f'    public static let {state} = "{state}"')
    lines += [
        f"    public static let all = [{states}]",
        "}",
        "",
    ]
    return "\n".join(lines)
```

- [ ] **Step 5: Add the dongle to the routing table**

In `tools/gen_contract.py`, beside the existing import block:

```python
from gen_dongle import emit_dongle_c, emit_dongle_swift
```

If `tools/` is not already on the path for a direct script run, import it the way the file already reaches its own helpers — check how `gen_contract.py` is invoked (`python3 tools/gen_contract.py` from the repo root) and match it; a `sys.path` insertion of the script's own directory is acceptable and should carry a one-line comment saying why.

Then append to `TARGETS`:

```python
    {
        "name": "dongle",
        "schema": ROOT / "contract" / "dongle-api.json",
        "artifacts": [
            ("firmware/s3/main/dongle_contract.inc", emit_dongle_c),
            ("app/AJMiddleCar/Generated/DongleAPI.swift", emit_dongle_swift),
        ],
        # No spliced documentation: the dongle's endpoints are described in its spec as
        # prose, and a generated table would duplicate rather than replace it.
        "spliced": [],
    },
```

- [ ] **Step 6: Generate, and run the tests**

```bash
python3 tools/gen_contract.py
python3 tools/test_gen_contract.py -v
```

Expected: both new files appear, all tests pass. Then confirm the car is still untouched:

```bash
git diff --stat firmware/p4 app/AJMiddleCar/Generated/CarAPI.swift tools/mock_car/generated.py docs/protocol.md
```

Expected: **empty**. A single changed byte in the car's artifacts means Task 1's routing rewrite was not faithful.

- [ ] **Step 7: Confirm the drift check now covers the dongle**

```bash
bash tools/check_contract.sh
```

Expected: `contract: no drift`. Then prove it actually watches the new files — edit one generated file by hand, re-run, see it caught, and restore it:

```bash
printf '\n#define TAMPER 1\n' >> firmware/s3/main/dongle_contract.inc
bash tools/check_contract.sh || echo "caught, as it should be"
git checkout firmware/s3/main/dongle_contract.inc
bash tools/check_contract.sh
```

Expected: the middle run reports `DRIFT: firmware/s3/main/dongle_contract.inc` and exits non-zero; the last reports no drift. Record both in your report — a drift check that does not catch a deliberate edit is worse than none.

- [ ] **Step 8: Run the whole suite and commit**

```bash
tools/test-all.sh
git add contract/dongle-api.json tools/gen_dongle.py tools/gen_contract.py \
        tools/test_gen_contract.py firmware/s3/main/dongle_contract.inc \
        app/AJMiddleCar/Generated/DongleAPI.swift
git commit -m "feat(contract): the dongle's vocabulary becomes generated, and drift-checked"
```

---

### Task 3: The firmware stops holding the values

**Files:**
- Modify: `firmware/s3/main/net_cfg.h`
- Modify: `firmware/s3/main/status_api.c`
- Modify: `firmware/s3/main/usb_net.h`
- Modify: `firmware/s3/test/Makefile`
- Modify: `docs/superpowers/specs/2026-08-30-dongle-api-design.md`

**Interfaces:**
- Consumes from Task 2: `firmware/s3/main/dongle_contract.inc` and its `DONGLE_*` names.

- [ ] **Step 1: Have `net_cfg.h` take its bounds from the contract**

Replace the four literal bound macros with the generated ones, keeping every comment that explains *why* those numbers are what they are:

```c
#include "dongle_contract.inc"

/* WPA2's limits, not ours, and now the contract's: the same four numbers reach the app
 * through app/AJMiddleCar/Generated/DongleAPI.swift, so neither side writes them as a
 * literal and check_contract.sh fails a tree where they disagree.
 *
 * The bounds are named here rather than used directly so the rest of this header reads
 * as it did — and so a reader sees at a glance which numbers are contractual. */
#define NET_SSID_MAX   DONGLE_SSID_MAX
#define NET_PASS_MIN   DONGLE_PASS_MIN
#define NET_PASS_MAX   DONGLE_PASS_MAX
```

Keep `NET_SSID_MIN` if the file defines one; if the minimum is written as a bare `1` in `net_cfg.c`'s check, replace that with `DONGLE_SSID_MIN` and say so in your report.

The struct's array sizes stay expressed in terms of `NET_SSID_MAX` / `NET_PASS_MAX`, so they follow automatically.

- [ ] **Step 2: Have `status_api.c` take the identity, port and state from the contract**

Three literals move:

```c
    cfg.server_port = DONGLE_PORT;
```

the `"ajdongle"` in the body format string becomes `DONGLE_DEVICE` — which means the format string can no longer carry it inline, so add it as an argument:

```c
    int n = snprintf(body, sizeof(body),
                     "{\"" DONGLE_KEY_DEVICE "\":\"" DONGLE_DEVICE "\","
                     "\"fw\":\"%s\",\"idf\":\"%s\",\"usb\":\"up\","
                     "\"net\":{\"ssid\":\"%s\",\"state\":\"" DONGLE_STATE_IDLE "\",\"rssi\":0}}",
                     app->version, app->idf_ver, ssid_esc);
```

String-literal concatenation keeps this a compile-time constant, so the buffer arithmetic does not change. Verify that claim by rebuilding and comparing the binary size to before — a jump would mean something became a runtime format.

Include the header, and update the boot log line so the port comes from the same place:

```c
    ESP_LOGI(TAG, "http://%s:%d" DONGLE_PATH_STATUS, USB_NET_ADDR, DONGLE_PORT);
```

- [ ] **Step 3: Have `usb_net.h` take the address from the contract**

```c
#define USB_NET_ADDR DONGLE_HOST
```

keeping the existing comment about why the subnet was chosen. The netmask stays local — it is not something the app needs to agree on.

- [ ] **Step 4: Teach the host test where the header is**

`firmware/s3/test/Makefile` already has `-I../main`, which is where the generated `.inc` lands, so no change should be needed. **Verify** rather than assume:

```bash
make -C firmware/s3/test clean && make -C firmware/s3/test run
```

Expected: `test_net_cfg: all passed`. If the include is not found, add the path and say so.

- [ ] **Step 5: Build the firmware**

```bash
source tools/env-p4.sh && cd firmware/s3 && idf.py build
```

Expected: clean, no warnings. Record the binary size and compare it to the previous build — the string-concatenation claim in step 2 predicts no meaningful change.

- [ ] **Step 6: Amend the spec**

`docs/superpowers/specs/2026-08-30-dongle-api-design.md` says the dongle's API is hand-written and argues the generator would cost more than it saves. That is now half true and half wrong, and the file is the binding authority — leaving it stale is worse than the original inaccuracy.

Replace the section **"The dongle's API is hand-written, not generated"** with the split this plan actually implements. Cover, in the file's own voice:

- The vocabulary — names, numbers, paths, the state list — **is** generated, into a C header and a Swift enum, and `check_contract.sh` fails a tree where the two disagree. Neither side writes an agreed value as a literal, which is the same principle the car lives under.
- The rules — lengths, the character class, escaping, refusing rather than truncating — stay in `net_cfg`, host-tested. The generator's value model is numeric (`int`, `bool`, `enum`, all travelling as `int32` through one generic handler) and cannot express a string field with a character-class rule without a change to the car that this plan does not make.
- Keep the existing precedent sentence about six of the car's eleven endpoints being hand-written; it is still true and still relevant.

- [ ] **Step 7: Run everything**

```bash
tools/test-all.sh
```

Expected: `== all green ==` with `contract: no drift`.

- [ ] **Step 8: Commit**

```bash
git add firmware/s3 docs/superpowers/specs/2026-08-30-dongle-api-design.md
git commit -m "feat(s3): the firmware reads its contract instead of restating it"
```

---

## Verification on hardware

Deliberately not a task here. This branch changes where constants come from, not what they are, so its bench evidence is the same run Plan 2 is waiting for: flash once with both branches merged, then `firmware/s3/verify-on-host.sh`.

One thing that run should confirm beyond Plan 2's own list: `/status` still answers on **8080** and still names itself `ajdongle`. If the generated header disagreed with what the firmware previously hardcoded, that is where it shows — and it is the reason step 2 of Task 3 asks for the binary size before and after rather than trusting that a macro substitution changed nothing.

## After this plan

Plan 4 writes the app's dongle client. It imports `DongleContract` and `DongleNetState` rather than hand-copying names, and `check_contract.sh` is what keeps the import honest. That is the whole reason the Swift file is generated now, with no consumer — the same reasoning that laid down OTA partitions before there was OTA code.
