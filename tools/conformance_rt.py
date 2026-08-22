#!/usr/bin/env python3
"""The real-time (UDP) conformance matrix, run against the mock or a real car.

    python3 tools/conformance_rt.py 127.0.0.1:4237
    python3 tools/conformance_rt.py 192.168.4.1:4210

What conformance.py is to REST, this is to the wire the app drives on: the hello
handshake, replies to repeats, the telemetry push and its schema, silence toward
a displaced socket, the goodbye, and the datagrams both sides must drop — spoken
as real datagrams against a real socket. The audit found the RT channel had no
cross-implementation check at all: the mock tested itself, the firmware tested
itself, and only REST was compared. This is the comparison.

Stdlib only — no venv needed against a car. The dropped frames come from the
rule-6 table in docs/superpowers/specs/2026-08-22-audit-fix-decisions.md, the
same table test_state.py and the firmware host tests pin.
"""
import argparse
import json
import os
import secrets
import socket
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_car"))
from generated import CTL_VALUES, PROTO, RT, TELEMETRY_FIELDS   # noqa: E402

JSON_TYPES = {"int": int, "bool": bool, "str": str}

DROPPED_FRAMES = [                       # rule 6: both sides drop these whole
    b'{"seq":5,"junk":{"t":0.9},"y":0.5}',
    b'{"seq":7,"t":.5,"y":0}',
    b'{"seq":8,"t":+1,"y":0}',
    b'{"seq":9,"t":0.5,"y":0,"t":0.9}',
    b'{"proto":1.5,"hello":"abcd1234"}',
    b'{"bye":1}',                        # a goodbye without a seq is dropped too
]


class Unreachable(Exception):
    pass


def enc(obj):
    return json.dumps(obj, separators=(",", ":")).encode()


class RTConformance:
    def __init__(self, host, port, verbose=False):
        self.addr = (host, port)
        self.verbose = verbose
        self.failures = []

    def check(self, ok, what):
        if not ok:
            self.failures.append(what)
            print(f"  FAIL  {what}")
        return ok

    def sock(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(0.2)
        return s

    def recv_frame(self, s, deadline_s):
        """The next parseable frame within deadline_s, or None."""
        end = time.monotonic() + deadline_s
        while time.monotonic() < end:
            try:
                data, _ = s.recvfrom(RT["max_datagram"])
            except socket.timeout:
                continue
            try:
                frame = json.loads(data)
            except ValueError:
                self.check(False, f"unparseable datagram from the car: {data[:60]!r}")
                continue
            if self.verbose:
                print(f"    <- {frame}")
            return frame
        return None

    def recv_matching(self, s, pred, deadline_s):
        end = time.monotonic() + deadline_s
        while time.monotonic() < end:
            f = self.recv_frame(s, end - time.monotonic())
            if f is not None and pred(f):
                return f
        return None

    def drain(self, s):
        while True:
            try:
                s.recvfrom(RT["max_datagram"])
            except socket.timeout:
                return

    def handshake(self, s, sid, proto=PROTO):
        """Send hello at the app's retry cadence until answered."""
        frame = {RT["proto_field"]: proto, RT["hello_field"]: sid}
        for _ in range(15):                                 # ~3 s at 5 Hz
            s.sendto(enc(frame), self.addr)
            reply = self.recv_matching(s, lambda f: RT["hello_field"] in f, 0.2)
            if reply is not None:
                return reply
        raise Unreachable(f"no hello reply from {self.addr[0]}:{self.addr[1]}")

    def run(self):
        sid = secrets.token_hex(4)
        s = self.sock()

        print("hello")
        reply = self.handshake(s, sid)
        self.check(reply.get(RT["proto_field"]) == PROTO,
                   f"hello reply proto {reply.get(RT['proto_field'])!r}, want {PROTO}")
        self.check(reply.get(RT["hello_field"]) == sid,
                   f"hello reply echoes {reply.get(RT['hello_field'])!r}, want {sid!r}")
        for key in (RT["device_field"], RT["fw_field"]):
            self.check(isinstance(reply.get(key), str) and reply[key],
                       f"hello reply {key} is {reply.get(key)!r}, want a nonempty string")
        again = self.handshake(s, sid)
        self.check(again.get(RT["hello_field"]) == sid,
                   "a repeated hello is answered (a lost reply must be recoverable)")

        print("wrong proto")
        s2 = self.sock()
        foreign = self.handshake(s2, secrets.token_hex(4), proto=PROTO + 1)
        self.check(foreign.get(RT["proto_field"]) == PROTO,
                   "a foreign proto is answered by name, so a client can stop searching")
        s2.close()

        print("telemetry")
        seq = 0
        frames = []
        end = time.monotonic() + 1.5
        next_send = 0.0
        while time.monotonic() < end:
            now = time.monotonic()
            if now >= next_send:
                seq += 1
                s.sendto(enc({RT["seq_field"]: seq, RT["throttle_field"]: 0.0,
                              RT["yaw_field"]: 0.0}), self.addr)
                next_send = now + 1.0 / RT["command_hz"]
            f = self.recv_frame(s, 0.05)
            if f is not None and "ctl" in f:
                frames.append(f)
        self.check(len(frames) >= 3,
                   f"telemetry: {len(frames)} frames in 1.5 s of streaming, want >= 3")
        if frames:
            f = frames[-1]
            names = [t["name"] for t in TELEMETRY_FIELDS]
            self.check(sorted(f) == sorted(names),
                       f"telemetry keys {sorted(f)}, want {sorted(names)}")
            for t in TELEMETRY_FIELDS:
                want = JSON_TYPES[t["type"]]
                got = f.get(t["name"])
                self.check(isinstance(got, want)
                           and not (want is int and isinstance(got, bool)),
                           f"telemetry {t['name']} is {got!r}, want {t['type']}")
            self.check(f.get("ctl") in CTL_VALUES,
                       f"telemetry ctl {f.get('ctl')!r} not in {CTL_VALUES}")

        print("dropped datagrams")
        pad = b"x" * (RT["max_command"] + 1 - len(b'{"seq":0,"t":0,"y":0,"p":""}'))
        oversized = b'{"seq":0,"t":0,"y":0,"p":"' + pad + b'"}'
        for bad in DROPPED_FRAMES + [oversized, enc({RT["seq_field"]: 1,
                                                     RT["throttle_field"]: 0.9,
                                                     RT["yaw_field"]: 0.0})]:
            s.sendto(bad, self.addr)                        # the last is a stale seq
        self.drain(s)
        stray = self.recv_matching(s, lambda f: f.get(RT["hello_field"]) == "abcd1234", 0.5)
        self.check(stray is None, "a malformed hello (proto:1.5) must not be answered")
        seq += 1
        s.sendto(enc({RT["seq_field"]: seq, RT["throttle_field"]: 0.0,
                      RT["yaw_field"]: 0.0}), self.addr)
        alive = self.recv_matching(s, lambda f: "ctl" in f, 1.0)
        self.check(alive is not None,
                   "the session survives the dropped datagrams and still pushes")

        print("eviction")
        s3 = self.sock()
        sid3 = secrets.token_hex(4)
        self.handshake(s3, sid3)
        self.drain(s)
        displaced = self.recv_matching(s, lambda f: "ctl" in f, 0.7)
        self.check(displaced is None,
                   "after an eviction the displaced socket hears nothing")
        moved = self.recv_matching(s3, lambda f: "ctl" in f, 1.0)
        self.check(moved is not None, "telemetry follows the new owner")

        print("bye")
        s3.sendto(enc({RT["seq_field"]: 1, RT["throttle_field"]: 0,
                       RT["yaw_field"]: 0, RT["bye_field"]: 1}), self.addr)
        time.sleep(0.3)
        self.drain(s3)
        after = self.recv_matching(s3, lambda f: "ctl" in f, 1.0)
        self.check(after is None, "telemetry stops after a goodbye")
        s.close()
        s3.close()
        return self.failures


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("target", help="host:port, e.g. 127.0.0.1:4237 or 192.168.4.1:4210")
    p.add_argument("-v", "--verbose", action="store_true", help="log every frame")
    args = p.parse_args()
    host, _, port = args.target.partition(":")
    suite = RTConformance(host, int(port or RT["port"]), args.verbose)
    print(f"rt conformance against udp://{host}:{suite.addr[1]}")
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
    print("\nrt conformance: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
