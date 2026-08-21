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
    out.append(f"#define CFG_DOMAIN_COUNT {len(schema['domains'])}")
    return "\n".join(out)


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

    write(root / "firmware" / "p4" / "main" / "cfg_table.inc", emit_c(schema))

    return 0


if __name__ == "__main__":
    sys.exit(main())
