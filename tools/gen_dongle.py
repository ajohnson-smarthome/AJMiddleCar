#!/usr/bin/env python3
"""Emitters for contract/dongle-api.json.

Separate from gen_contract.py so the car's file does not grow a second device's shapes.
The two schemas share gen_common's plumbing and nothing else — neither references the
other, and the dongle's rules (lengths, the character class, escaping) live in
firmware/dongle/main/net_cfg.{c,h} where they are host-tested rather than here.
"""
from gen_common import (c_group_defines, c_envelope_defines, c_error_defines,
                        c_endpoint_defines, swift_groups, swift_document, swift_state_enum,
                        swift_struct, swift_error_envelope)

BANNER = "generated from contract/dongle-api.json by tools/gen_contract.py - do not edit"


def emit_dongle_c(schema):
    """A pure C header: preprocessor text only. net_cfg.h includes this and compiles on
    the host with plain cc under -Wall -Wextra -Werror."""
    n, b = schema["network"], schema["bounds"]
    lines = [
        f"/* {BANNER} */",
        "",
        "#ifndef DONGLE_CONTRACT_INC",
        "#define DONGLE_CONTRACT_INC",
        "",
        f"#define DONGLE_PROTO {schema['proto']}",
        f'#define DONGLE_DEVICE "{schema["device"]}"',
        f'#define DONGLE_HOST "{n["host"]}"',
        f"#define DONGLE_PORT {n['port']}",
        f"#define DONGLE_RELAY_HTTP_PORT {schema['relay']['http_port']}",
        f"#define DONGLE_RELAY_RT_PORT {schema['relay']['rt_port']}",
        f"#define DONGLE_RELAY_VIDEO_PORT {schema['relay']['video_port']}",
        f"#define DONGLE_RELAY_VIDEO_MAX_KBPS {schema['relay']['video_max_kbps']}",
        "",
        f"#define DONGLE_SSID_MIN {b['ssid_min']}",
        f"#define DONGLE_SSID_MAX {b['ssid_max']}",
        f"#define DONGLE_PASS_MIN {b['pass_min']}",
        f"#define DONGLE_PASS_MAX {b['pass_max']}",
        "",
    ]
    lines += c_endpoint_defines(schema, "DONGLE_")
    lines += c_envelope_defines(schema, "DONGLE_")
    lines += c_group_defines(schema, "DONGLE_")
    for k, v in schema["wifi_request"].items():
        lines.append(f'#define DONGLE_WIFI_REQ_{k.upper()} "{v}"')
    lines.append("")
    lines += c_error_defines(schema, "DONGLE_")
    lines += ["#endif /* DONGLE_CONTRACT_INC */", ""]
    return "\n".join(lines)


def emit_dongle_swift(schema):
    n, b, e = schema["network"], schema["bounds"], schema["endpoints"]
    lines = [
        f"// {BANNER}",
        "",
        "public enum DongleContract {",
        f"    public static let proto = {schema['proto']}",
        f'    public static let device = "{schema["device"]}"',
        f'    public static let host = "{n["host"]}"',
        f"    public static let port: UInt16 = {n['port']}",
        f"    public static let relayHttpPort: UInt16 = {schema['relay']['http_port']}",
        f"    public static let relayRtPort: UInt16 = {schema['relay']['rt_port']}",
        f"    public static let relayVideoPort: UInt16 = {schema['relay']['video_port']}",
        f"    public static let relayVideoMaxKbps = {schema['relay']['video_max_kbps']}",
        "",
    ]
    for k, v in e.items():
        lines.append(f'    public static let {k}Path = "{v}"')
    lines += [
        "",
        f"    public static let ssidMin = {b['ssid_min']}",
        f"    public static let ssidMax = {b['ssid_max']}",
        f"    public static let passMin = {b['pass_min']}",
        f"    public static let passMax = {b['pass_max']}",
        "",
    ]
    for k, v in schema["wifi_request"].items():
        lines.append(f'    public static let {k}Field = "{v}"')
    lines += ["}", ""]
    lines += swift_groups(schema)
    lines += swift_document(schema["status"]["swift"], schema, schema["status"]["groups"],
                            schema["status"]["doc"])
    wifi_fields = {f["name"]: f for f in schema["groups"]["wifi"]["fields"]}
    reply = schema["wifi_reply"]
    lines += swift_struct(reply["swift"],
                          [{"name": schema["envelope"]["proto"], "type": "int",
                            "doc": "the protocol version the device speaks"}] +
                          [wifi_fields[name] for name in reply["fields"]],
                          reply["doc"])
    lines += swift_state_enum("DongleErrorCode", schema["errors"],
                              "The code inside an error envelope.")
    lines += swift_error_envelope("DongleAPIError", "DongleErrorCode", schema)
    return "\n".join(lines)
