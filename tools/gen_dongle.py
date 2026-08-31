#!/usr/bin/env python3
"""Emitters for contract/dongle-api.json.

Separate from gen_contract.py so the car's file does not grow a second device's shapes.
The two schemas share a generator's plumbing and nothing else — neither references the
other, and the dongle's rules (lengths, the character class, escaping) live in
firmware/s3/main/net_cfg.{c,h} where they are host-tested rather than here where they
would only be described.
"""

# tools/gen_dongle.py itself has no __main__ and isn't executable — it's a pair of
# emitters gen_contract.py imports. The banner names the entry point someone would
# actually run to regenerate, not the file the rule they'd hand-edited lives in.
BANNER = "generated from contract/dongle-api.json by tools/gen_contract.py - do not edit"


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
        f"#define DONGLE_RELAY_HTTP_PORT {schema['relay']['http_port']}",
        f"#define DONGLE_RELAY_RT_PORT {schema['relay']['rt_port']}",
        "",
        f'#define DONGLE_PATH_STATUS "{e["status"]}"',
        f'#define DONGLE_PATH_NET "{e["net"]}"',
        f'#define DONGLE_PATH_OTA "{e["ota"]}"',
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
    lines.append("")
    for state in schema["usb_states"]:
        lines.append(f'#define DONGLE_USB_STATE_{state.upper()} "{state}"')
    lines += ["", "#endif /* DONGLE_CONTRACT_INC */", ""]
    return "\n".join(lines)


def emit_dongle_swift(schema):
    """The app's half. No consumer until the app-side plan — generated now so that plan
    adds an import rather than a second hand-maintained copy of every name."""
    n, b, e = schema["network"], schema["bounds"], schema["endpoints"]
    sf, nf = schema["status_fields"], schema["net_fields"]
    states = ", ".join(f'"{s}"' for s in schema["net_states"])
    usb_states = ", ".join(f'"{s}"' for s in schema["usb_states"])

    lines = [
        f"// {BANNER}",
        "",
        "public enum DongleContract {",
        f'    public static let device = "{schema["device"]}"',
        f'    public static let host = "{n["host"]}"',
        f"    public static let port: UInt16 = {n['port']}",
        f"    public static let relayHttpPort: UInt16 = {schema['relay']['http_port']}",
        f"    public static let relayRtPort: UInt16 = {schema['relay']['rt_port']}",
        "",
        f'    public static let statusPath = "{e["status"]}"',
        f'    public static let netPath = "{e["net"]}"',
        f'    public static let otaPath = "{e["ota"]}"',
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
        "/// The keys `/status` uses. Named here so the app never spells one as a literal.",
        "public enum DongleStatusKey {",
    ]
    for key, value in sf.items():
        # Same camelCasing TelemetryKey uses in gen_contract.py's emit_swift: the
        # schema's snake_case key becomes the Swift identifier, the schema's value
        # (the actual wire name) becomes the string literal — the two can differ, as
        # they do for net_ssid -> "ssid" nested under the body's "net" object.
        camel = key.split("_")[0] + "".join(w.title() for w in key.split("_")[1:])
        lines.append(f'    public static let {camel} = "{value}"')
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
        "/// What the dongle's USB link is doing, as `/status` reports it.",
        "public enum DongleUsbState {",
    ]
    for state in schema["usb_states"]:
        lines.append(f'    public static let {state} = "{state}"')
    lines += [
        f"    public static let all = [{usb_states}]",
        "}",
        "",
    ]
    return "\n".join(lines)
