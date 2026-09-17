#!/usr/bin/env python3
"""The REST request matrix, run against the mock or against a real car.

    python3 tools/conformance.py http://127.0.0.1:8080
    python3 tools/conformance.py http://192.168.7.1      # the car, through the dongle's relay
    python3 tools/conformance.py --write-calibration http://127.0.0.1:8080

Every expectation comes from contract/car-api.json: the field sets, both ends of every
range, the value one past each end, the members of every enum. Nothing here is written
twice, so a schema change moves this suite with it — which is the point. The contract
used to exist in four hand-written places and be enforced in none, and they disagreed.

What it asserts for `/status` and for **/config** (v2's single endpoint over the five
config domains): the field set and its types, `application/json` on every answer, both
ends of every range accepted, one past each end rejected, an enum refusing a value
outside its set, a fractional value where a `fixed` field expects one accepted and
rounded, a missing field rejected rather than partially written, and a rejection
carrying `{"proto","error":{"code","message"[,"field"]}}` — the envelope `cfg_api.c` and
the mock both emit. `field` is checked to be present only when a field, not the whole
body, is at fault — its absence otherwise is asserted too, not merely tolerated.

`/calibration*` and `/ota` are asserted against the same envelope. The spin's 200 is also
held to land only after the pulse, the timing calib_api.c documents the wizard as
assuming.

It restores every config value it found. Three things it does anyway, unavoidably:
it POSTs each domain about ten times, and on a real car every accepted POST is an NVS
write; it spins a wheel once (`/calibration/spin`), so put the car on a stand; and the
two rejected `/ota` bodies stop the motors and briefly take the actuator on a real car.

The one check it does NOT run by default is POSTing a valid `/calibration` table: a
calibration is not a range that can be dialled back to what it was, so overwriting a
car's saved one on every conformance run would be a surprise, not a convenience. That
check is skipped and a `skipped: ...` line is printed instead. Pass `--write-calibration`
to run it anyway; when passed, the table `GET /calibration` reported before the run is
POSTed back afterwards, in a `finally`, but only if the car was already `calibrated: true`
— there is nothing to restore a car that had none.

Stdlib only — no venv needed to run it against a car.
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_car"))
from generated import (CALIBRATION, CONFIG_PATH, DEVICE, DOMAINS, ENDPOINTS,   # noqa: E402
                       ENVELOPE, GROUPS, PROTO, STATUS_GROUPS, VERSION_FIELDS, lround)
from state import CarState, build_number   # noqa: E402

TIMEOUT_S = 10


class Unreachable(Exception):
    pass


class Conformance:
    def __init__(self, base, verbose=False, write_calibration=False):
        self.base = base.rstrip("/")
        self.verbose = verbose
        self.write_calibration = write_calibration
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
        if ok:
            ok &= self.check(parsed.get(ENVELOPE["proto"]) == PROTO,
                             f"{where}: proto {parsed.get(ENVELOPE['proto'])!r}, want {PROTO}")
        return ok

    def expect_ok(self, where, path, body):
        status, ctype, parsed, _ = self.call("POST", path, body)
        self.expect_json(where, status, ctype, parsed, 200)
        return parsed

    def expect_rejected(self, where, path, body, code, field=None, status=400, raw=None):
        """A rejection carrying the `{"proto","error":{"code","message"[,"field"]}}`
        envelope. `field` absent from the reply must mean `field is None` here — a
        fault with the body as a whole must not silently omit the check."""
        got, ctype, parsed, _ = self.call("POST", path, body, raw=raw)
        if not self.expect_json(where, got, ctype, parsed, status):
            return
        err = parsed.get(ENVELOPE["error"])
        if not self.check(isinstance(err, dict), f"{where}: no {ENVELOPE['error']!r} object in {parsed}"):
            return
        self.check(isinstance(err.get(ENVELOPE["code"]), str) and err[ENVELOPE["code"]] != "",
                   f"{where}: error.code {err.get(ENVELOPE['code'])!r}, want a nonempty string")
        self.check(err.get(ENVELOPE["code"]) == code,
                   f"{where}: error.code {err.get(ENVELOPE['code'])!r}, want {code!r}")
        self.check(isinstance(err.get(ENVELOPE["message"]), str) and err[ENVELOPE["message"]] != "",
                   f"{where}: no error.message in {err}")
        has_field = ENVELOPE["field"] in err
        if field is None:
            self.check(not has_field,
                       f"{where}: error.field {err.get(ENVELOPE['field'])!r} present, want absent")
        else:
            self.check(has_field and err[ENVELOPE["field"]] == field,
                       f"{where}: error.field {err.get(ENVELOPE['field'])!r}, want {field!r}")

    def wire_type_ok(self, value, f):
        """Is `value` the shape /config reports for a schema field of type `f['type']`?
        `fixed` is a decimal on the wire; `enum` and `int` are plain integers."""
        if f["type"] == "bool":
            return isinstance(value, bool)
        if f["type"] == "fixed":
            return isinstance(value, (int, float)) and not isinstance(value, bool)
        return isinstance(value, int) and not isinstance(value, bool)

    def status_field_ok(self, value, f):
        if f.get("nullable") and value is None:
            return True
        t = f["type"]
        if t == "int":
            return isinstance(value, int) and not isinstance(value, bool)
        if t == "bool":
            return isinstance(value, bool)
        if t == "str":
            return isinstance(value, str)
        if t == "state":
            return value in f["values"]
        return False

    # ---- /status, / -------------------------------------------------------------

    def status(self):
        print("/status")
        status, ctype, parsed, _ = self.call("GET", "/status")
        if not self.expect_json("/status", status, ctype, parsed, 200):
            return
        want_keys = ["proto"] + STATUS_GROUPS
        self.check(sorted(parsed) == sorted(want_keys),
                   f"/status: top-level keys {sorted(parsed)}, want {sorted(want_keys)}")
        for g in STATUS_GROUPS:
            group = parsed.get(g)
            if not self.check(isinstance(group, dict), f"/status.{g} is {group!r}, want an object"):
                continue
            fields = GROUPS[g]["fields"]
            names = [f["name"] for f in fields]
            self.check(sorted(group) == sorted(names),
                       f"/status.{g}: fields {sorted(group)}, want {sorted(names)}")
            for f in fields:
                v = group.get(f["name"])
                self.check(self.status_field_ok(v, f),
                           f"/status.{g}.{f['name']} is {v!r}, want {f['type']}"
                           + (" or null" if f.get("nullable") else ""))

    def version(self):
        print("/version")
        status, ctype, parsed, _ = self.call("GET", "/version")
        if not self.expect_json("/version", status, ctype, parsed, 200):
            return
        names = [f["name"] for f in VERSION_FIELDS]
        self.check(list(parsed) == names,
                   f"/version: keys {list(parsed)}, want exactly {names} in this order")
        for f in VERSION_FIELDS:
            v = parsed.get(f["name"])
            self.check(self.status_field_ok(v, f), f"/version.{f['name']} is {v!r}, want {f['type']}")
        self.check(parsed.get("device") == DEVICE, f"/version.device {parsed.get('device')!r}, want {DEVICE!r}")
        self.check(parsed.get("build") == build_number(parsed.get("fw", "")),
                   f"/version.build {parsed.get('build')!r}, want build_number({parsed.get('fw')!r})")
        self.check(parsed.get("proto") == PROTO, f"/version.proto {parsed.get('proto')!r}, want {PROTO}")

    def identity(self):
        print("/")
        _, _, ver, _ = self.call("GET", "/version")
        device = (ver or {}).get("device", "")
        status, _, _, payload = self.call("GET", "/")
        self.check(status == 200, f"/: status {status}, want 200")
        line = payload.decode(errors="replace").strip()
        self.check(bool(line), "/: empty identity line")
        self.check(bool(device) and line.split()[:1] == [device],
                   f"/: {line!r} does not lead with the device id {device!r}")

    def unknown_path(self):
        print("/nosuchthing")
        status, _, _, _ = self.call("GET", "/nosuchthing")
        self.check(status == 404, f"/nosuchthing: status {status}, want 404")

    # ---- /config ------------------------------------------------------------

    def get_config(self, where):
        status, ctype, parsed, _ = self.call("GET", CONFIG_PATH)
        if self.expect_json(where, status, ctype, parsed, 200):
            return parsed
        return None

    def expect_domain_ok(self, where, key, record):
        parsed = self.expect_ok(where, CONFIG_PATH, {key: record})
        if isinstance(parsed, dict):
            self.check(parsed.get(key) == record,
                       f"{where}: config[{key}] {parsed.get(key)}, want {record}")

    def expect_domain_get(self, where, key, want):
        parsed = self.get_config(where)
        if parsed is not None:
            self.check(parsed.get(key) == want, f"{where}: config[{key}] {parsed.get(key)}, want {want}")

    def config_top(self):
        print(CONFIG_PATH)
        parsed = self.get_config(f"GET {CONFIG_PATH}")
        if parsed is None:
            return
        self.check(sorted(parsed) == sorted(["proto"] + list(DOMAINS)),
                   f"GET {CONFIG_PATH}: top-level keys {sorted(parsed)}, "
                   f"want {sorted(['proto'] + list(DOMAINS))}")

    def domain(self, key, domain):
        print(f"{CONFIG_PATH}[{key}]")
        parsed = self.get_config(f"GET {CONFIG_PATH}")
        if parsed is None:
            return
        fields = domain["fields"]
        names = [f["name"] for f in fields]
        original = parsed.get(key)
        if not self.check(isinstance(original, dict) and sorted(original) == sorted(names),
                          f"GET {CONFIG_PATH}.{key}: fields "
                          f"{sorted(original) if isinstance(original, dict) else original}, "
                          f"want {sorted(names)}"):
            return
        for f in fields:
            v = original[f["name"]]
            self.check(self.wire_type_ok(v, f),
                       f"GET {CONFIG_PATH}.{key}.{f['name']} is {v!r}, want {f['type']}")

        try:
            self.ranges(key, fields, original)
            self.field_shapes(key, fields, original)
        finally:
            # Whatever the matrix left behind, the car goes back to what it had.
            self.expect_domain_ok(f"POST {CONFIG_PATH} {key} (restore)", key, original)
            self.expect_domain_get(f"GET {CONFIG_PATH} {key} (restored)", key, original)

    def ranges(self, key, fields, original):
        for f in fields:
            name = f["name"]
            where = f"{key}.{name}"

            def body(v, _name=name):
                b = dict(original)
                b[_name] = v
                return b

            if f["type"] == "bool":
                for v in (True, False):
                    self.expect_domain_ok(f"POST {CONFIG_PATH} {where}={v}", key, body(v))
                    self.expect_domain_get(f"GET {CONFIG_PATH} after {where}={v}", key, body(v))
                # A JSON number is not a JSON boolean; cJSON_IsBool refuses it, and a
                # client that sends 1 must be told rather than quietly accepted.
                self.expect_rejected(f"POST {CONFIG_PATH} {where}=1 (not a bool)",
                                     CONFIG_PATH, {key: body(1)}, "wrong_type", where)
            elif f["type"] == "enum":
                for v in f["values"]:
                    self.expect_domain_ok(f"POST {CONFIG_PATH} {where}={v}", key, body(v))
                    self.expect_domain_get(f"GET {CONFIG_PATH} after {where}={v}", key, body(v))
                outside = max(f["values"]) + 1
                self.expect_rejected(f"POST {CONFIG_PATH} {where}={outside} (not in the set)",
                                     CONFIG_PATH, {key: body(outside)}, "not_allowed", where)
                self.expect_rejected(f"POST {CONFIG_PATH} {where}=0 (not in the set)",
                                     CONFIG_PATH, {key: body(0)}, "not_allowed", where)
            elif f["type"] == "fixed":
                scale = f["scale"]
                lo, hi = f["min"] / scale, f["max"] / scale
                for v in (lo, hi):
                    self.expect_domain_ok(f"POST {CONFIG_PATH} {where}={v}", key, body(v))
                    self.expect_domain_get(f"GET {CONFIG_PATH} after {where}={v}", key, body(v))
                edge = body(hi)
                # The smallest wire delta guaranteed to round to at least one internal
                # unit past the boundary: 0.5/scale rounds to the next integer either
                # way (lround is half-away-from-zero), plus a hair against float error.
                slack = 0.5 / scale + 0.0001
                over, under = hi + slack, lo - slack
                self.expect_rejected(f"POST {CONFIG_PATH} {where}={over} (out of range)",
                                     CONFIG_PATH, {key: body(over)}, "out_of_range", where)
                self.expect_domain_get(f"GET {CONFIG_PATH} after a rejected {where}={over}",
                                      key, edge)
                self.expect_rejected(f"POST {CONFIG_PATH} {where}={under} (out of range)",
                                     CONFIG_PATH, {key: body(under)}, "out_of_range", where)
                self.expect_domain_get(f"GET {CONFIG_PATH} after a rejected {where}={under}",
                                      key, edge)
                # A JSON boolean is not a JSON number; cJSON_IsNumber refuses it outright,
                # before the field's own type (fixed vs. plain int) is even consulted.
                self.expect_rejected(f"POST {CONFIG_PATH} {where}=true (a bool)",
                                     CONFIG_PATH, {key: body(True)}, "wrong_type", where)
                self.expect_domain_get(f"GET {CONFIG_PATH} after {where}=true (a bool)",
                                      key, edge)
                # A fraction between two internal integers still rounds and is accepted —
                # the mock and the car must agree which way (lround: half away from zero).
                # The POST answers with the ROUNDED value, not the one that was sent.
                frac = f["default"] / scale + 0.125
                rounded = lround(frac * scale) / scale
                parsed = self.expect_ok(f"POST {CONFIG_PATH} {where}={frac} (rounds)",
                                        CONFIG_PATH, {key: body(frac)})
                if isinstance(parsed, dict):
                    self.check(parsed.get(key) == body(rounded),
                               f"POST {CONFIG_PATH} {where}={frac}: config[{key}] "
                               f"{parsed.get(key)}, want {body(rounded)}")
                self.expect_domain_get(f"GET {CONFIG_PATH} after {where}={frac}", key, body(rounded))
            else:  # plain int
                for v in (f["min"], f["max"]):
                    self.expect_domain_ok(f"POST {CONFIG_PATH} {where}={v}", key, body(v))
                    self.expect_domain_get(f"GET {CONFIG_PATH} after {where}={v}", key, body(v))
                edge = body(f["max"])            # a known-good record to check against
                for v in (f["min"] - 1, f["max"] + 1):
                    self.expect_rejected(f"POST {CONFIG_PATH} {where}={v} (out of range)",
                                         CONFIG_PATH, {key: body(v)}, "out_of_range", where)
                    self.expect_domain_get(f"GET {CONFIG_PATH} after a rejected {where}={v}",
                                          key, edge)
                self.expect_rejected(f"POST {CONFIG_PATH} {where}=\"{f['max']}\" (a string)",
                                     CONFIG_PATH, {key: body(str(f["max"]))}, "wrong_type", where)
                # A JSON boolean is not a JSON number; cJSON_IsNumber refuses it outright.
                self.expect_rejected(f"POST {CONFIG_PATH} {where}=true (a bool)",
                                     CONFIG_PATH, {key: body(True)}, "wrong_type", where)
                self.expect_domain_get(f"GET {CONFIG_PATH} after {where}=true (a bool)",
                                      key, edge)
                # Rule 7: both sides reject a fraction where an integer is expected.
                self.expect_rejected(f"POST {CONFIG_PATH} {where}={f['min']}.5 (a fraction)",
                                     CONFIG_PATH, {key: body(f["min"] + 0.5)}, "wrong_type", where)
                self.expect_domain_get(f"GET {CONFIG_PATH} after {where}={f['min']}.5 (a fraction)",
                                      key, edge)

    def field_shapes(self, key, fields, original):
        self.expect_domain_ok(f"POST {CONFIG_PATH} {key} (whole record)", key, original)
        for f in fields:
            missing = {k: v for k, v in original.items() if k != f["name"]}
            self.expect_rejected(f"POST {CONFIG_PATH} {key} without {f['name']}", CONFIG_PATH,
                                 {key: missing}, "missing_field", f"{key}.{f['name']}")
            # The rejection must not have written the fields it did like.
            self.expect_domain_get(f"GET {CONFIG_PATH} after a body missing {f['name']}",
                                  key, original)
        extra = dict(original, bogus_field=1)
        self.expect_rejected(f"POST {CONFIG_PATH} {key} with an unknown field", CONFIG_PATH,
                             {key: extra}, "unknown_field", f"{key}.bogus_field")
        self.expect_domain_get(f"GET {CONFIG_PATH} after an unknown field in {key}", key, original)

    def config_body_shapes(self):
        print(f"{CONFIG_PATH} (whole-body shapes)")
        original = self.get_config(f"GET {CONFIG_PATH}")
        if original is None:
            return
        self.expect_rejected("POST /config (unknown domain)", CONFIG_PATH,
                             {"nope": {}}, "unknown_field", "nope")
        self.expect_rejected("POST /config (empty body)", CONFIG_PATH, {}, "missing_field")
        self.expect_rejected("POST /config (malformed JSON)", CONFIG_PATH, None,
                             "bad_json", raw=b"{not json")
        self.expect_rejected("POST /config (JSON array)", CONFIG_PATH, [1, 2], "bad_json")
        # Two domains, the second bad: the car validates the whole body before writing
        # any of it, so the first (good) domain must not have been applied either.
        bad_body = {"ramp": original["ramp"],
                   "trim": dict(original["trim"], balance_pct=9999)}
        self.expect_rejected("POST /config (half the body bad)", CONFIG_PATH, bad_body,
                             "out_of_range", "trim.balance_pct")
        parsed = self.get_config(f"GET {CONFIG_PATH} (after half-bad POST)")
        if parsed is not None:
            self.check(parsed == original,
                       f"GET {CONFIG_PATH}: {parsed}, want unchanged {original}")

    # ---- /calibration* ------------------------------------------------------

    def _wheels(self, pairs=(0, 1, 2, 3), inverted=(False, True, False, True)):
        k = CALIBRATION["keys"]
        return [{k["corner"]: c, k["pair"]: p, k["inverted"]: inv}
                for c, p, inv in zip(CALIBRATION["corners"], pairs, inverted)]

    def calibration(self):
        print(ENDPOINTS["calibration"])
        k = CALIBRATION["keys"]
        status, ctype, before, _ = self.call("GET", ENDPOINTS["calibration"])
        if self.expect_json("GET /calibration", status, ctype, before, 200):
            calibrated, wheels = before.get(k["calibrated"]), before.get(k["wheels"])
            self.check(isinstance(calibrated, bool),
                       f"GET /calibration: {k['calibrated']} is {calibrated!r}, want a bool")
            self.check(isinstance(wheels, list),
                       f"GET /calibration: {k['wheels']} is {wheels!r}, want a list")
            if calibrated and isinstance(wheels, list):
                corners = [w.get(k["corner"]) for w in wheels]
                self.check(corners == CALIBRATION["corners"],
                           f"GET /calibration: corners {corners}, want {CALIBRATION['corners']}")
        else:
            before = None

        self.expect_rejected("POST /calibration/spin pair=9", ENDPOINTS["spin"],
                             {k["pair"]: 9, k["direction"]: "forward"},
                             "out_of_range", k["pair"])
        self.expect_rejected("POST /calibration/spin with an unknown field", ENDPOINTS["spin"],
                             {k["pair"]: 0, k["direction"]: "forward", "x": 1},
                             "unknown_field", "x")
        self.expect_rejected("POST /calibration/spin direction=up", ENDPOINTS["spin"],
                             {k["pair"]: 0, k["direction"]: "up"},
                             "not_allowed", k["direction"])
        self.expect_rejected("POST /calibration/spin (empty body)", ENDPOINTS["spin"],
                             {}, "missing_field", k["pair"])
        self.expect_rejected("POST /calibration/spin (malformed JSON)", ENDPOINTS["spin"],
                             None, "bad_json", raw=b"{not json")

        # The one call that moves the car. A 200 must land AFTER the pulse:
        # calib_api.c sleeps it out so the wizard's "which wheel turned?" prompt
        # appears with the wheel already stopped, and a client paced against an
        # instant reply mispaces on hardware.
        t0 = time.monotonic()
        status, ctype, parsed, _ = self.call("POST", ENDPOINTS["spin"],
                                             {k["pair"]: 0, k["direction"]: "forward"})
        elapsed = time.monotonic() - t0
        if self.expect_json("POST /calibration/spin", status, ctype, parsed, 200):
            self.check(parsed.get(ENVELOPE["ok"]) is True,
                       f"spin: body {parsed}, want {{'ok':true}}")
            floor = CarState.CALIB_HOLD_MS / 1000.0 * 0.8
            self.check(elapsed >= floor,
                       f"spin: 200 in {elapsed:.2f}s, want >= {floor:.2f}s (after the pulse)")

        self.expect_rejected("POST /calibration (three wheels)", ENDPOINTS["calibration"],
                             {k["wheels"]: self._wheels()[:3]}, "wrong_type", k["wheels"])
        dup_corner = self._wheels()
        dup_corner[2] = dict(dup_corner[0])
        self.expect_rejected("POST /calibration (a corner repeated)", ENDPOINTS["calibration"],
                             {k["wheels"]: dup_corner}, "not_allowed", f"{k['wheels']}[2]")
        dup_pair = self._wheels(pairs=(0, 0, 1, 2))
        self.expect_rejected("POST /calibration (a pair repeated)", ENDPOINTS["calibration"],
                             {k["wheels"]: dup_pair}, "not_allowed", k["wheels"])
        str_pair = self._wheels()
        str_pair[0] = dict(str_pair[0], **{k["pair"]: str(str_pair[0][k["pair"]])})
        self.expect_rejected("POST /calibration (pair as a string)", ENDPOINTS["calibration"],
                             {k["wheels"]: str_pair}, "wrong_type", f"{k['wheels']}[0]")
        self.expect_rejected("POST /calibration (no wheels)", ENDPOINTS["calibration"],
                             {}, "missing_field", k["wheels"])

        # The one call that saves, and a saved calibration cannot be un-saved — so this
        # is opt-in (see the module docstring), and restores whatever was there before.
        if not self.write_calibration:
            print("  skipped: POST /calibration (a good table) — pass --write-calibration "
                  "to run it; it overwrites the car's saved calibration")
            return
        try:
            good = self._wheels()
            status, ctype, parsed, _ = self.call("POST", ENDPOINTS["calibration"],
                                                 {k["wheels"]: good})
            if self.expect_json("POST /calibration (good table)", status, ctype, parsed, 200):
                self.check(parsed.get(k["calibrated"]) is True,
                           f"POST /calibration: {k['calibrated']} {parsed.get(k['calibrated'])!r}, want True")
                self.check(parsed.get(k["wheels"]) == good,
                           f"POST /calibration: {k['wheels']} {parsed.get(k['wheels'])}, want {good}")
            status, ctype, parsed, _ = self.call("GET", ENDPOINTS["calibration"])
            if self.expect_json("GET /calibration (after save)", status, ctype, parsed, 200):
                self.check(parsed.get(k["wheels"]) == good,
                           f"GET /calibration: {k['wheels']} {parsed.get(k['wheels'])}, want {good}")
        finally:
            if isinstance(before, dict) and before.get(k["calibrated"]) \
                    and isinstance(before.get(k["wheels"]), list):
                self.call("POST", ENDPOINTS["calibration"], {k["wheels"]: before[k["wheels"]]})
                print(f"  restored the calibration table {ENDPOINTS['calibration']} reported "
                      f"before this run")

    def ota(self):
        print(ENDPOINTS["ota"])
        # Neither body flashes anything: the first is under the size floor, the
        # second fails the ESP image magic on the first write. On a real car both
        # do stop the motors and briefly take the actuator, like the spin above.
        self.expect_rejected("POST /ota (32 bytes)", ENDPOINTS["ota"], None,
                             "too_small", raw=b"\xe9" + b"\x00" * 31)
        self.expect_rejected("POST /ota (4 KB, bad magic)", ENDPOINTS["ota"], None,
                             "not_firmware", raw=b"\x00" * 4096)

    def run(self):
        self.status()
        self.version()
        self.identity()
        self.config_top()
        for key, domain in DOMAINS.items():
            self.domain(key, domain)
        self.config_body_shapes()
        self.calibration()
        self.ota()
        self.unknown_path()
        return self.failures


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("base_url", help="e.g. http://127.0.0.1:8080 (the mock) or http://192.168.7.1 (the car, through the dongle)")
    p.add_argument("-v", "--verbose", action="store_true", help="log every request")
    p.add_argument("--write-calibration", action="store_true",
                   help="also POST a valid /calibration table; skipped by default because a "
                        "calibration cannot be un-set. Restores whatever GET /calibration "
                        "reported before the run, if the car was already calibrated")
    args = p.parse_args()

    suite = Conformance(args.base_url, args.verbose, write_calibration=args.write_calibration)
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
