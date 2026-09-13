#!/usr/bin/env python3
"""Emitters shared by contract/car-api.json and contract/dongle-api.json.

A `group` is an object of named fields a device reports (`link`, `wifi`, …). A `state`
field is a word from a list; an `object` field is one level of nesting inside a group;
`nullable` means the value may be JSON null. The three languages get their names from
the same walk over the same schema, so a field renamed in one file is renamed in every
artifact or in none.
"""
import math
import pprint


def upper(name):
    return name.upper()


# Swift keywords that collide with a wire value used as an enum case (e.g. the error
# code "internal"). Escaped with backticks at every use site — declaration, dot syntax,
# and the case-name reference in a switch — so a generated enum compiles as itself.
_SWIFT_KEYWORDS = {
    "associatedtype", "class", "deinit", "enum", "extension", "fileprivate", "func",
    "import", "init", "inout", "internal", "let", "open", "operator", "private",
    "protocol", "public", "rethrows", "static", "struct", "subscript", "typealias",
    "var", "break", "case", "continue", "default", "defer", "do", "else",
    "fallthrough", "for", "guard", "if", "in", "repeat", "return", "switch", "where",
    "while", "as", "Any", "catch", "false", "is", "nil", "self", "Self", "throw",
    "throws", "true", "try",
}


def _swift_ident(name):
    return f"`{name}`" if name in _SWIFT_KEYWORDS else name


def lround(x):
    """C's lround: half away from zero. Python's round() is half-to-even, and a
    validator that disagrees with cfg_api.c about 9.005 is exactly the drift the
    generator exists to prevent."""
    return int(math.copysign(math.floor(abs(x) + 0.5), x))


# ---- C ---------------------------------------------------------------------------

def c_group_defines(schema, prefix):
    out = []
    for gname, g in schema["groups"].items():
        G = upper(gname)
        out.append(f'#define {prefix}KEY_GROUP_{G} "{gname}"')
        for f in g["fields"]:
            F = upper(f["name"])
            out.append(f'#define {prefix}KEY_{G}_{F} "{f["name"]}"')
            if f["type"] == "object":
                for sub in f["fields"]:
                    out.append(f'#define {prefix}KEY_{G}_{F}_{upper(sub["name"])} "{sub["name"]}"')
            if f["type"] == "state":
                for v in f["values"]:
                    out.append(f'#define {prefix}{G}_{F}_{upper(v)} "{v}"')
                out.append(f"#define {prefix}{G}_{F}_COUNT {len(f['values'])}")
        out.append("")
    return out


def c_envelope_defines(schema, prefix):
    e = schema["envelope"]
    return [
        f'#define {prefix}KEY_PROTO "{e["proto"]}"',
        f'#define {prefix}KEY_OK "{e["ok"]}"',
        f'#define {prefix}KEY_ERROR "{e["error"]}"',
        f'#define {prefix}KEY_ERROR_CODE "{e["code"]}"',
        f'#define {prefix}KEY_ERROR_MESSAGE "{e["message"]}"',
        f'#define {prefix}KEY_ERROR_FIELD "{e["field"]}"',
        "",
    ]


def c_error_defines(schema, prefix):
    return [f'#define {prefix}ERR_{upper(c)} "{c}"' for c in schema["errors"]] + [""]


def c_endpoint_defines(schema, prefix):
    return [f'#define {prefix}PATH_{upper(k)} "{v}"' for k, v in schema["endpoints"].items()] + [""]


# ---- Swift -----------------------------------------------------------------------

def swift_type(f):
    if f["type"] == "array":
        base = f["swift"]
    elif f["type"] in ("state", "object"):
        base = f["swift"]
    elif f["type"] == "fixed" or f["type"] == "number":
        base = "Double"
    else:
        base = {"int": "Int", "bool": "Bool", "str": "String", "enum": "Int"}[f["type"]]
    return base + ("?" if f.get("nullable") else "")


def swift_state_enum(name, values, doc):
    """An enum with the contract's cases plus unknown(String): a firmware that grows a
    word must not make the app fail to decode a document it otherwise understands."""
    cases = ", ".join("." + _swift_ident(v) for v in values)
    lines = [f"/// {doc}", f"public enum {name}: Equatable, Sendable, Codable {{"]
    lines += [f"    case {_swift_ident(v)}" for v in values]
    lines += [
        "    case unknown(String)",
        "    public var rawValue: String {",
        "        switch self {",
    ]
    lines += [f'        case .{_swift_ident(v)}: return "{v}"' for v in values]
    lines += [
        "        case .unknown(let raw): return raw",
        "        }",
        "    }",
        "    public init(rawValue: String) {",
        "        switch rawValue {",
    ]
    lines += [f'        case "{v}": self = .{_swift_ident(v)}' for v in values]
    lines += [
        "        default: self = .unknown(rawValue)",
        "        }",
        "    }",
        "    public init(from decoder: Decoder) throws {",
        "        self.init(rawValue: try decoder.singleValueContainer().decode(String.self))",
        "    }",
        "    public func encode(to encoder: Encoder) throws {",
        "        var c = encoder.singleValueContainer()",
        "        try c.encode(rawValue)",
        "    }",
        f"    public static let all: [{name}] = [{cases}]",
        "}",
        "",
    ]
    return lines


def swift_struct(name, fields, doc):
    """Property names ARE the wire names — snake_case, no CodingKeys — the convention the
    generated config structs already follow. Synthesised Codable then needs no mapping."""
    lines = [f"/// {doc}", f"public struct {name}: Codable, Equatable, Sendable {{"]
    for f in fields:
        lines.append(f'    /// {f["doc"]}')
        lines.append(f'    public var {f["name"]}: {swift_type(f)}')
    args = ", ".join(f'{f["name"]}: {swift_type(f)}' for f in fields)
    assigns = "; ".join(f'self.{f["name"]} = {f["name"]}' for f in fields)
    lines.append(f"    public init({args}) {{ {assigns} }}")
    lines += ["}", ""]
    return lines


def swift_groups(schema):
    """Every state enum, every object struct, every group struct — nested types first,
    so a file that reads top to bottom meets each name before it is used."""
    out = []
    for g in schema["groups"].values():
        for f in g["fields"]:
            if f["type"] == "state":
                out += swift_state_enum(f["swift"], f["values"], f["doc"])
            if f["type"] == "object":
                for sub in f["fields"]:
                    if sub["type"] == "state":
                        out += swift_state_enum(sub["swift"], sub["values"], sub["doc"])
                out += swift_struct(f["swift"], f["fields"], f["doc"])
        out += swift_struct(g["swift"], g["fields"], g["doc"])
    return out


def swift_document(name, schema, groups, doc, extra_fields=()):
    """A struct made of groups: proto first, then any extras (telemetry's seq), then one
    property per group named exactly as the group is on the wire."""
    fields = [{"name": schema["envelope"]["proto"], "type": "int",
               "doc": "the protocol version the device speaks"}]
    fields += list(extra_fields)
    for gname in groups:
        g = schema["groups"][gname]
        fields.append({"name": gname, "type": "object", "swift": g["swift"], "doc": g["doc"]})
    return swift_struct(name, fields, doc)


def swift_error_envelope(name, code_enum, schema):
    e = schema["envelope"]
    return [
        "/// The error envelope every endpoint answers with, on 400 / 409 / 500.",
        f"public struct {name}: Codable, Equatable, Sendable {{",
        f"    public var {e['proto']}: Int",
        f"    public var {e['error']}: Body",
        "    public struct Body: Codable, Equatable, Sendable {",
        f"        public var {e['code']}: {code_enum}",
        f"        public var {e['message']}: String",
        f"        public var {e['field']}: String?",
        f"        public init({e['code']}: {code_enum}, {e['message']}: String, {e['field']}: String?) {{ "
        f"self.{e['code']} = {e['code']}; self.{e['message']} = {e['message']}; self.{e['field']} = {e['field']} }}",
        "    }",
        f"    public init({e['proto']}: Int, {e['error']}: Body) {{ self.{e['proto']} = {e['proto']}; self.{e['error']} = {e['error']} }}",
        "}",
        "",
    ]


# ---- Python ----------------------------------------------------------------------

def py_common(schema):
    """The tables both mocks and both conformance tools read."""
    return [
        f"PROTO = {schema['proto']}",
        f"DEVICE = {schema['device']!r}",
        f"ENVELOPE = {schema['envelope']!r}",
        f"ENDPOINTS = {schema['endpoints']!r}",
        f"ERRORS = {schema['errors']!r}",
        f"STATUS_GROUPS = {schema['status']['groups']!r}",
        # pformat, not json.dumps: this is a Python module, and JSON writes `true`
        # where Python needs `True`. sort_dicts=False keeps it deterministic.
        f"GROUPS = {pprint.pformat(schema['groups'], indent=4, sort_dicts=False, width=96)}",
    ]
