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

The dropped-frames check does not just look for aliveness (some push arriving):
a mock that quietly accepts one of the seven bad datagrams still looks "alive" to
a check that only wants any push at all. It reads the car's own rx_fps instead —
0 means nothing landed, non-zero means one was silently accepted — the same
signal a real fleet's telemetry would surface.

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
from generated import CTL_VALUES, DEVICE, PROTO, RT, TELEMETRY_FIELDS   # noqa: E402

JSON_TYPES = {"int": int, "bool": bool, "str": str}

# Two literals worth naming once: the contract has no field-name constant for
# either (RT has no "ctl" key, and "abcd1234" is just this tool's own probe
# sid), but each is used from more than one place below and a typo in one
# copy silently un-tests the thing it was checking.
CTL_KEY = "ctl"                      # TELEMETRY_FIELDS' name for the actuator's owner
BAD_HELLO_SID = "abcd1234"           # the sid the malformed hello below carries

DROPPED_FRAMES = [                       # rule 6: both sides drop these whole
    b'{"seq":5,"junk":{"t":0.9},"y":0.5}',
    b'{"seq":7,"t":.5,"y":0}',
    b'{"seq":8,"t":+1,"y":0}',
    b'{"seq":9,"t":0.5,"y":0,"t":0.9}',
    ('{"proto":1.5,"hello":"%s"}' % BAD_HELLO_SID).encode(),
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
        """The first frame matching pred within deadline_s — everything else read
        along the way is silently discarded. Fine when only one thing in the
        window is interesting; wrong when a later check needs a frame this one
        would otherwise have thrown away (see scan_window)."""
        end = time.monotonic() + deadline_s
        while time.monotonic() < end:
            f = self.recv_frame(s, end - time.monotonic())
            if f is not None and pred(f):
                return f
        return None

    def scan_window(self, s, deadline_s):
        """Every frame that arrives within deadline_s, in order, nothing discarded.

        recv_matching (and drain) throw away whatever does not match as they
        search — fine for a single question, but two independent questions asked
        as separate blind searches over overlapping time can each eat the one
        frame the other needed. Telemetry keeps pushing on its own ~200 ms
        cadence throughout, so a "search for A, then search for B" over the same
        stretch of wire risks A's search discarding the specific push B's
        search was counting on. One shared scan avoids the race.
        """
        end = time.monotonic() + deadline_s
        out = []
        while time.monotonic() < end:
            f = self.recv_frame(s, end - time.monotonic())
            if f is not None:
                out.append(f)
        return out

    def drain(self, s, deadline_s=0.5):
        """Discard whatever arrives for up to deadline_s, total — not "until
        nothing new shows up", which never returns against a live push stream."""
        end = time.monotonic() + deadline_s
        while time.monotonic() < end:
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
        self.check(reply.get(RT["device_field"]) == DEVICE,
                   f"hello reply device is {reply.get(RT['device_field'])!r}, "
                   f"want {DEVICE!r}")
        again = self.handshake(s, sid)
        self.check(again.get(RT["hello_field"]) == sid,
                   "a repeated hello is answered (a lost reply must be recoverable)")

        print("wrong proto")
        s2 = self.sock()
        try:
            foreign = self.handshake(s2, secrets.token_hex(4), proto=PROTO + 1)
        except Unreachable:
            # Reachability was already proven above (the first handshake worked),
            # so a timeout here is a specific defect in this car's handling of a
            # foreign-proto hello, not "the target is unreachable" — a named FAIL,
            # not exit 2.
            self.check(False, "a foreign-proto hello gets no reply at all (it "
                               "must be answered by name, just not adopted)")
        else:
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
            if f is not None and CTL_KEY in f:
                frames.append(f)
        self.check(len(frames) >= 3,
                   f"telemetry: {len(frames)} frames in 1.5 s of streaming, want >= 3")
        if frames:
            f = frames[-1]
            names = [t["name"] for t in TELEMETRY_FIELDS]
            self.check(sorted(f) == sorted(names),
                       f"telemetry keys {sorted(f)}, want {sorted(names)}")
            for t in TELEMETRY_FIELDS:
                want = JSON_TYPES.get(t["type"])
                if want is None:
                    self.check(False, f"telemetry field {t['name']} has type "
                                       f"{t['type']!r}, which this tool has no check for")
                    continue
                got = f.get(t["name"])
                self.check(isinstance(got, want)
                           and not (want is int and isinstance(got, bool)),
                           f"telemetry {t['name']} is {got!r}, want {t['type']}")
            self.check(f.get(CTL_KEY) in CTL_VALUES,
                       f"telemetry {CTL_KEY} {f.get(CTL_KEY)!r} not in {CTL_VALUES}")

        print("dropped datagrams (rejection is measured via rx_fps, not just aliveness)")
        # Quiesce: the telemetry loop above already stopped sending. Read one
        # push before doing anything else, so the backlog left by the streaming
        # tail is flushed and the pushes we inspect below are ones generated
        # after this point, not stale carryover from the loop.
        flushed = self.recv_matching(s, lambda f: CTL_KEY in f, 1.0)
        self.check(flushed is not None, "a push still arrives once streaming stops")

        pad = b"x" * (RT["max_command"] + 1 - len(b'{"seq":0,"t":0,"y":0,"p":""}'))
        oversized = b'{"seq":0,"t":0,"y":0,"p":"' + pad + b'"}'
        # The stale/replayed seq: not a new bad-frame shape, but the same "must
        # be dropped whole" rule applied to a network-delayed duplicate. t is
        # zeroed — on a car with broken replay protection this would otherwise
        # be a live 90% throttle command issued by a conformance tool.
        stale_frame = enc({RT["seq_field"]: 1, RT["throttle_field"]: 0.0,
                            RT["yaw_field"]: 0.0})
        bad_batch = DROPPED_FRAMES + [oversized, stale_frame]
        for bad in bad_batch:
            s.sendto(bad, self.addr)

        # One shared scan answers both questions below. Running them as two
        # separate blind searches (recv_matching, then drain, then another
        # recv_matching) would have each one discard frames the other needed:
        # a search for the stray hello-reply throws away every push it passes
        # over, including the one whose rx_fps would prove a bad frame got in.
        seen = self.scan_window(s, 0.6)
        stray = next((f for f in seen if f.get(RT["hello_field"]) == BAD_HELLO_SID), None)
        self.check(stray is None, "a malformed hello (proto:1.5) must not be answered")
        pushes = [f for f in seen if CTL_KEY in f]
        self.check(bool(pushes),
                   "the session survives the dropped datagrams and still pushes")
        bad_fps = [f.get("rx_fps") for f in pushes if f.get("rx_fps")]
        self.check(not bad_fps,
                   f"rx_fps after the {len(bad_batch)} bad datagrams was "
                   f"{bad_fps or [0]}, want all 0 — a lax car accepted one or more "
                   f"of them")

        print("accepted at the cap")
        # The other side of the same boundary: a *valid* command padded to
        # exactly max_command bytes must be admitted, not just the over-cap one
        # rejected above.
        seq += 1
        prefix = enc({RT["seq_field"]: seq, RT["throttle_field"]: 0.0,
                      RT["yaw_field"]: 0.0, "p": ""})
        valid_padded = enc({RT["seq_field"]: seq, RT["throttle_field"]: 0.0,
                            RT["yaw_field"]: 0.0,
                            "p": "x" * (RT["max_command"] - len(prefix))})
        self.check(len(valid_padded) == RT["max_command"],
                   f"the padded probe is {len(valid_padded)} bytes, want exactly "
                   f"{RT['max_command']}")
        s.sendto(valid_padded, self.addr)
        # A single recv_matching for "the next push" is not safe here: the
        # server's push tick is on its own 200 ms clock, so the one datagram we
        # just sent can land either side of the next tick's window depending on
        # timing alone, with no defect involved. Scan a few ticks' worth and
        # require the bump to show up on any one of them, the same tolerance
        # the bad-batch check above needs for the opposite reason.
        after_pushes = [f for f in self.scan_window(s, 0.6) if CTL_KEY in f]
        self.check(bool(after_pushes),
                   "telemetry keeps flowing after a max-size valid command")
        if after_pushes:
            seen_fps = [f.get("rx_fps") for f in after_pushes]
            self.check(any(seen_fps),
                       f"rx_fps after a valid {RT['max_command']}-byte command was "
                       f"{seen_fps}, want at least one > 0 — the cap must admit "
                       f"exactly max_command bytes, not reject it too")

        print("eviction")
        s3 = self.sock()
        sid3 = secrets.token_hex(4)
        try:
            self.handshake(s3, sid3)
        except Unreachable:
            # Same reasoning as the foreign-proto handshake above: the target is
            # known reachable, so this is a named FAIL, not exit 2.
            self.check(False, "a second session's hello gets no reply at all "
                               "(eviction and bye cannot be exercised without it)")
        else:
            self.drain(s, 0.5)
            displaced = self.recv_matching(s, lambda f: CTL_KEY in f, 0.7)
            self.check(displaced is None,
                       "after an eviction the displaced socket hears nothing")
            moved = self.recv_matching(s3, lambda f: CTL_KEY in f, 1.0)
            self.check(moved is not None, "telemetry follows the new owner")

            print("bye")
            s3.sendto(enc({RT["seq_field"]: 1, RT["throttle_field"]: 0,
                           RT["yaw_field"]: 0, RT["bye_field"]: 1}), self.addr)
            time.sleep(0.3)
            self.drain(s3, 0.5)
            after = self.recv_matching(s3, lambda f: CTL_KEY in f, 1.0)
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
