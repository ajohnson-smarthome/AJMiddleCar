#!/usr/bin/env python3
"""Emit every expression of the app<->car contract from one schema.

Source of truth: contract/car-api.json. Everything this writes carries a header
saying so and must never be hand-edited; tools/check_contract.sh fails a build
where the committed output and a fresh run disagree.
"""
import argparse
import json
import pathlib
import pprint
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "contract" / "car-api.json"

# Invoked as `python3 tools/gen_contract.py` from the repo root: Python already puts the
# script's own directory (tools/) at sys.path[0] for a direct run, but this file is also
# imported as a plain module (e.g. by test_gen_contract.py), where that auto-insertion
# does not happen. Insert it explicitly, the same way test_gen_contract.py already does
# for its own directory, so `import gen_dongle` is not invocation-dependent.
sys.path.insert(0, str(ROOT / "tools"))
from gen_dongle import emit_dongle_c, emit_dongle_swift

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


def _allowed_symbol(domain, f):
    return f"CFG_{domain['nvs_key'].upper()}_{f['name'].upper()}_ALLOWED"


def _c_field(domain, f):
    if f["type"] == "int":
        return f'{{ "{f["name"]}", CFG_INT, {f["min"]}, {f["max"]}, {f["default"]}, NULL, 0 }}'
    if f["type"] == "bool":
        return f'{{ "{f["name"]}", CFG_BOOL, 0, 1, {1 if f["default"] else 0}, NULL, 0 }}'
    sym = _allowed_symbol(domain, f)
    return (f'{{ "{f["name"]}", CFG_ENUM, {min(f["values"])}, {max(f["values"])}, '
            f'{f["default"]}, {sym}, {len(f["values"])} }}')


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
    rt = schema["rt"]
    out.append(f'#define RT_PORT {rt["port"]}')
    out.append(f'#define RT_MAX_DATAGRAM {rt["max_datagram"]}')
    out.append(f'#define RT_MAX_COMMAND {rt["max_command"]}')
    for k in ("hello", "seq", "bye", "proto", "device", "fw", "throttle", "yaw"):
        out.append(f'#define RT_KEY_{k.upper()} "{rt[k + "_field"]}"')
    for i, v in enumerate(schema["ctl_values"]):
        out.append(f'#define CTL_{v.upper()} "{v}"')
    out.append(f'#define CTL_COUNT {len(schema["ctl_values"])}')
    out.append(f'#define RT_COMMAND_HZ {rt["command_hz"]}')
    out.append(f'#define RT_TELEMETRY_HZ {rt["telemetry_hz"]}')
    out.append(f'#define RT_WATCHDOG_MS {rt["watchdog_ms"]}')
    out.append(f'#define RT_SESSION_IDLE_MS {rt["session_idle_ms"]}')
    out.append(f'#define RT_PROTO {schema["proto"]}')
    out.append("")
    out.append(f"#define CFG_DOMAIN_COUNT {len(schema['domains'])}")
    widest = max(len(d["fields"]) for d in schema["domains"])
    out.append(f"#define CFG_MAX_FIELDS {widest}")
    return "\n".join(out)


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
           f"    public static let maxCommand = {rt['max_command']}",
           f'    public static let protoField = "{rt["proto_field"]}"',
           f'    public static let deviceField = "{rt["device_field"]}"',
           f'    public static let fwField = "{rt["fw_field"]}"',
           f'    public static let throttleField = "{rt["throttle_field"]}"',
           f'    public static let yawField = "{rt["yaw_field"]}"',
           f"    public static let commandHz = {rt['command_hz']}",
           f"    public static let telemetryHz = {rt['telemetry_hz']}",
           f"    public static let watchdogMs = {rt['watchdog_ms']}",
           f"    public static let sessionIdleMs = {rt['session_idle_ms']}",
           f'    public static let helloField = "{rt["hello_field"]}"',
           f'    public static let seqField = "{rt["seq_field"]}"',
           f'    public static let byeField = "{rt["bye_field"]}"',
           "}", "",
           "/// The fields the car sends in every telemetry datagram.",
           "public enum TelemetryKey {"]
    for f in schema["telemetry"]["fields"]:
        camel = f["name"].split("_")[0] + "".join(w.title() for w in f["name"].split("_")[1:])
        out.append(f'    /// {f["doc"]}')
        out.append(f'    public static let {camel} = "{f["name"]}"')
    out += ["}", ""]
    out.append("/// The values the car reports in telemetry's `ctl` field.")
    out.append("public enum CtlOwner {")
    for v in schema["ctl_values"]:
        out.append(f'    public static let {v} = "{v}"')
    joined = ", ".join(f'"{v}"' for v in schema["ctl_values"])
    out.append(f"    public static let all = [{joined}]")
    out += ["}", ""]
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


VALIDATE_SRC = '''

def validate(path, body):
    """Return (True, "") or (False, reason). Mirrors the firmware exactly."""
    domain = DOMAINS.get(path)
    if domain is None:
        return False, f"unknown endpoint {path}"
    for f in domain["fields"]:
        name = f["name"]
        if name not in body:
            return False, f"missing {name}"
        v = body[name]
        if f["type"] == "bool":
            if not isinstance(v, bool):
                return False, f"{name} must be a boolean"
            continue
        # bool is a subclass of int in Python, so {"ramp_ms": true} would sneak
        # past a plain isinstance check. The firmware's cJSON_IsNumber does not
        # accept a JSON boolean, so neither does this.
        if isinstance(v, bool) or not isinstance(v, int):
            return False, f"{name} must be an integer"
        if f["type"] == "enum":
            if v not in f["values"]:
                return False, f"{name} must be one of {f['values']}"
        elif not (f["min"] <= v <= f["max"]):
            return False, f"{name} must be {f['min']}..{f['max']}"
    return True, ""
'''


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
        f"TELEMETRY_FIELDS = {schema['telemetry']['fields']!r}",
        f"CTL_VALUES = {schema['ctl_values']!r}",
        "",
        "# Name-keyed, like C's CTL_RT and Swift's CtlOwner.rt. Position in",
        "# CTL_VALUES is still rank; these names free callers from the unpack.",
        *[f"CTL_{v.upper()} = {v!r}" for v in schema["ctl_values"]],
        "",
        # pformat, not json.dumps: this file is a Python module, and JSON writes
        # `true` where Python needs `True`. sort_dicts=False keeps it deterministic.
        f"DOMAINS = {pprint.pformat(body, indent=4, sort_dicts=False, width=96)}",
        VALIDATE_SRC.rstrip("\n"),
        "",
    ])


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
]


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


if __name__ == "__main__":
    sys.exit(main())
