#!/usr/bin/env python3
"""The REST request matrix, run against the mock or against a real car.

    python3 tools/conformance.py http://127.0.0.1:8080
    python3 tools/conformance.py http://192.168.4.1

Every expectation comes from contract/car-api.json: the field sets, both ends of every
range, the value one past each end, the members of every enum. Nothing here is written
twice, so a schema change moves this suite with it — which is the point. The contract
used to exist in four hand-written places and be enforced in none, and they disagreed.

What it asserts for the five **config domains**, which are the endpoints the schema
describes and the only ones generated on both sides: the field set and its types,
`application/json` on every answer, both ends of every range accepted, one past each end
rejected, an enum refusing a value outside its set, a missing field rejected rather than
partially written, and a rejection carrying `{"error","field"}` — the envelope
`cfg_api.c` and the mock both emit.

`/calib*` and `/ota` are **asserted by status code only**, plus the JSON `calibrated`
flag that `GET /calib` returns on both. Their reply bodies are where the two
implementations genuinely differ: the firmware answers `ok` and `httpd_resp_send_err`
text, the mock answers JSON. The contract says nothing about either, so this suite
asserts the intersection rather than picking a winner — a suite only the mock can pass
would be worse than no suite, because the first real-car run would drown a reviewer in
failures that are not regressions. Unifying those bodies is a firmware change, and it
belongs in a plan, not smuggled in here.

It restores every config value it found. Three things it does anyway, unavoidably:
it POSTs each domain about ten times, and on a real car every accepted POST is an NVS
write; it spins a wheel once (`/calib/spin`), so put the car on a stand; and it never
POSTs a valid `/calib/save`, because a calibration cannot be put back.

Stdlib only — no venv needed to run it against a car.
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_car"))
from generated import DOMAINS, PROTO, RT, TELEMETRY_FIELDS   # noqa: E402

JSON_TYPES = {"int": int, "bool": bool, "str": str}
TIMEOUT_S = 10


class Unreachable(Exception):
    pass


class Conformance:
    def __init__(self, base, verbose=False):
        self.base = base.rstrip("/")
        self.verbose = verbose
        self.failures = []

    # ---- transport -------------------------------------------------------------

    def call(self, method, path, body=None, raw=None):
        """Returns (status, content_type, parsed-or-None, bytes)."""
        data = raw if raw is not None else (
            json.dumps(body).encode() if body is not None else None)
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
                status, headers, payload = r.status, r.headers, r.read()
        except urllib.error.HTTPError as e:
            status, headers, payload = e.code, e.headers, e.read()
        except (urllib.error.URLError, OSError) as e:
            raise Unreachable(f"{method} {path}: {e}") from e
        try:
            parsed = json.loads(payload)
        except ValueError:
            parsed = None
        if self.verbose:
            print(f"    {method} {path} {json.dumps(body) if body else ''} -> {status} "
                  f"{payload[:120].decode(errors='replace')}")
        return status, headers.get("Content-Type", ""), parsed, payload

    # ---- assertions ------------------------------------------------------------

    def check(self, ok, what):
        if not ok:
            self.failures.append(what)
            print(f"  FAIL  {what}")
        return ok

    def expect_json(self, where, status, ctype, parsed, want_status):
        ok = self.check(status == want_status, f"{where}: status {status}, want {want_status}")
        ok &= self.check(ctype.startswith("application/json"),
                         f"{where}: Content-Type {ctype!r}, want application/json")
        ok &= self.check(isinstance(parsed, dict), f"{where}: body is not a JSON object")
        return ok

    def expect_ok(self, where, path, body):
        status, ctype, parsed, _ = self.call("POST", path, body)
        if self.expect_json(where, status, ctype, parsed, 200):
            self.check(parsed.get("ok") is True, f"{where}: body {parsed}, want {{'ok':true}}")

    def expect_rejected(self, where, path, body, field=None, raw=None):
        """A config-domain rejection: 400 carrying `{"error","field"}`.

        Config only. `/calib*` and `/ota` reject with a bare text body on the car, and
        `expect_status` is what holds them to account.
        """
        status, ctype, parsed, _ = self.call("POST", path, body, raw=raw)
        if not self.expect_json(where, status, ctype, parsed, 400):
            return
        self.check(isinstance(parsed.get("error"), str) and parsed["error"] != "",
                   f"{where}: no 'error' in {parsed}")
        self.check("field" in parsed, f"{where}: no 'field' in {parsed}")
        if field is not None and isinstance(parsed.get("field"), str):
            self.check(parsed["field"] in (field, ""),
                       f"{where}: field {parsed['field']!r}, want {field!r} or ''")

    def expect_status(self, where, method, path, body=None, want=(400,), raw=None):
        """Status only — for the endpoints whose reply body the contract does not fix."""
        status, _, _, payload = self.call(method, path, body, raw=raw)
        self.check(status in want,
                   f"{where}: status {status}, want {' or '.join(map(str, want))}"
                   f" (body {payload[:80]!r})")
        return status

    def expect_config(self, where, path, want):
        status, ctype, parsed, _ = self.call("GET", path)
        if self.expect_json(where, status, ctype, parsed, 200):
            self.check(parsed == want, f"{where}: reads {parsed}, want {want}")

    # ---- the matrix ------------------------------------------------------------

    def status(self):
        print("/status")
        status, ctype, parsed, _ = self.call("GET", "/status")
        if not self.expect_json("/status", status, ctype, parsed, 200):
            return
        self.check(parsed.get(RT["proto_field"]) == PROTO,
                   f"/status: proto {parsed.get(RT['proto_field'])!r}, want {PROTO}")
        self.check(isinstance(parsed.get(RT["device_field"]), str)
                   and parsed[RT["device_field"]], "/status: device missing")
        self.check(isinstance(parsed.get(RT["fw_field"]), str) and parsed[RT["fw_field"]],
                   "/status: fw missing")
        for f in TELEMETRY_FIELDS:
            want = JSON_TYPES[f["type"]]
            got = parsed.get(f["name"])
            self.check(isinstance(got, want) and not (want is int and isinstance(got, bool)),
                       f"/status: {f['name']} is {got!r}, want {f['type']}")

    def identity(self):
        print("/")
        status, _, _, payload = self.call("GET", "/")
        self.check(status == 200, f"/: status {status}, want 200")
        self.check(bool(payload.strip()), "/: empty identity line")

    def unknown_path(self):
        print("/nosuchthing")
        status, _, _, _ = self.call("GET", "/nosuchthing")
        self.check(status == 404, f"/nosuchthing: status {status}, want 404")

    def domain(self, path, domain):
        print(path)
        fields = domain["fields"]
        status, ctype, original, _ = self.call("GET", path)
        if not self.expect_json(f"GET {path}", status, ctype, original, 200):
            return
        names = [f["name"] for f in fields]
        if not self.check(sorted(original) == sorted(names),
                          f"GET {path}: fields {sorted(original)}, want {sorted(names)}"):
            return
        for f in fields:
            want = JSON_TYPES["bool" if f["type"] == "bool" else "int"]
            got = original[f["name"]]
            self.check(isinstance(got, want) and not (want is int and isinstance(got, bool)),
                       f"GET {path}: {f['name']} is {got!r}, want {f['type']}")

        try:
            self.ranges(path, fields, original)
            self.body_shapes(path, fields, original)
        finally:
            # Whatever the matrix left behind, the car goes back to what it had.
            self.expect_ok(f"POST {path} (restore)", path, original)
            self.expect_config(f"GET {path} (restored)", path, original)

    def ranges(self, path, fields, original):
        for f in fields:
            name = f["name"]
            def body(v):
                b = dict(original)
                b[name] = v
                return b

            if f["type"] == "bool":
                for v in (True, False):
                    self.expect_ok(f"POST {path} {name}={v}", path, body(v))
                    self.expect_config(f"GET {path} after {name}={v}", path, body(v))
                # A JSON number is not a JSON boolean; cJSON_IsBool refuses it, and a
                # client that sends 1 must be told rather than quietly accepted.
                self.expect_rejected(f"POST {path} {name}=1 (not a bool)", path, body(1), name)
            elif f["type"] == "enum":
                for v in f["values"]:
                    self.expect_ok(f"POST {path} {name}={v}", path, body(v))
                    self.expect_config(f"GET {path} after {name}={v}", path, body(v))
                outside = max(f["values"]) + 1
                self.expect_rejected(f"POST {path} {name}={outside} (not in the set)",
                                     path, body(outside), name)
                self.expect_rejected(f"POST {path} {name}=0 (not in the set)",
                                     path, body(0), name)
            else:
                for v in (f["min"], f["max"]):
                    self.expect_ok(f"POST {path} {name}={v}", path, body(v))
                    self.expect_config(f"GET {path} after {name}={v}", path, body(v))
                edge = body(f["max"])            # a known-good record to check against
                for v in (f["min"] - 1, f["max"] + 1):
                    self.expect_rejected(f"POST {path} {name}={v} (out of range)",
                                         path, body(v), name)
                    self.expect_config(f"GET {path} after a rejected {name}={v}", path, edge)
                self.expect_rejected(f"POST {path} {name}=\"{f['max']}\" (a string)",
                                     path, body(str(f["max"])), name)

    def body_shapes(self, path, fields, original):
        self.expect_ok(f"POST {path} (whole record)", path, original)
        for f in fields:
            missing = {k: v for k, v in original.items() if k != f["name"]}
            self.expect_rejected(f"POST {path} without {f['name']}", path, missing, f["name"])
            # The rejection must not have written the fields it did like.
            self.expect_config(f"GET {path} after a body missing {f['name']}", path, original)
        self.expect_rejected(f"POST {path} (malformed JSON)", path, None, raw=b"{not json")
        self.expect_rejected(f"POST {path} (JSON array)", path, [1, 2])

    def calibration(self):
        """The wizard's three calls. Bodies are not asserted — see the module docstring.

        `GET /calib` is the exception: both sides answer `{"calibrated": <bool>}` as
        JSON, and the app reads it to decide whether to open the wizard at all, so it is
        held to that shape.
        """
        print("/calib")
        status, ctype, parsed, _ = self.call("GET", "/calib")
        if self.expect_json("GET /calib", status, ctype, parsed, 200):
            self.check(isinstance(parsed.get("calibrated"), bool),
                       f"GET /calib: calibrated is {parsed.get('calibrated')!r}, want a bool")

        self.expect_status("POST /calib/spin pair=9", "POST", "/calib/spin",
                           {"pair": 9, "dir": 1})
        self.expect_status("POST /calib/spin dir=7", "POST", "/calib/spin",
                           {"pair": 0, "dir": 7})
        self.expect_status("POST /calib/spin (empty body)", "POST", "/calib/spin", {})
        self.expect_status("POST /calib/spin (malformed JSON)", "POST", "/calib/spin",
                           None, raw=b"{not json")

        # A spin either turns a wheel (200) or is refused because something outranks the
        # wizard (409). Both are conformant; a 200 that did not spin is what the app's
        # calibration used to believe, and what the wizard must now be able to tell apart.
        # This is the one call in the suite that moves the car.
        self.expect_status("POST /calib/spin pair=0 dir=1", "POST", "/calib/spin",
                           {"pair": 0, "dir": 1}, want=(200, 409))

        # Only rejections: a valid table cannot be un-saved, and this suite restores
        # what it found.
        self.expect_status("POST /calib/save (three wheels)", "POST", "/calib/save",
                           {"wheels": [{"pair": p, "sign": 1} for p in range(3)]})
        self.expect_status("POST /calib/save (duplicate pairs)", "POST", "/calib/save",
                           {"wheels": [{"pair": 0, "sign": 1}] * 4})
        self.expect_status("POST /calib/save (sign 0)", "POST", "/calib/save",
                           {"wheels": [{"pair": p, "sign": 0} for p in range(4)]})
        self.expect_status("POST /calib/save (no wheels)", "POST", "/calib/save", {})

    def ota(self):
        print("/ota")
        # Deliberately tiny: a conformance run must never flash anything. Status only —
        # ota_api.c answers `httpd_resp_send_err`, which is text/html.
        self.expect_status("POST /ota (32 bytes)", "POST", "/ota", None,
                           raw=b"\xe9" + b"\x00" * 31)

    def run(self):
        self.status()
        self.identity()
        for path, domain in DOMAINS.items():
            self.domain(path, domain)
        self.calibration()
        self.ota()
        self.unknown_path()
        return self.failures


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("base_url", help="e.g. http://127.0.0.1:8080 or http://192.168.4.1")
    p.add_argument("-v", "--verbose", action="store_true", help="log every request")
    args = p.parse_args()

    suite = Conformance(args.base_url, args.verbose)
    print(f"conformance against {suite.base}")
    try:
        failures = suite.run()
    except Unreachable as e:
        print(f"unreachable: {e}")
        return 2
    if failures:
        print(f"\n{len(failures)} failure(s):")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\nconformance: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
