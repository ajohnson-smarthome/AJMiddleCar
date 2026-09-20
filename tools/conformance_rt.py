#!/usr/bin/env python3
"""The real-time (UDP) conformance matrix, run against the mock or a real car.

    python3 tools/conformance_rt.py 127.0.0.1:4237
    python3 tools/conformance_rt.py 192.168.7.1:4210     # the car, through the dongle's relay

What conformance.py is to REST, this is to the wire the app drives on: the hello
handshake, replies to repeats, the telemetry push and its schema, silence toward
a displaced socket, the goodbye, and the datagrams both sides must drop — spoken
as real datagrams against a real socket. The audit found the RT channel had no
cross-implementation check at all: the mock tested itself, the firmware tested
itself, and only REST was compared. This is the comparison.

It also asks what happens after the app goes quiet — the part of the channel a
driver depends on most and no cross-implementation check used to read: the
control watchdog (silence after zeros: `link.timeouts` +1, `motors.owner` idle,
the session alive without a new hello), a repeated hello leaving the sequence
gate alone, and the retreat (a second at 30 % throttle, then silence:
`recovering` in the first frame, `idle` when the path is retraced). Every legend
is safe for a car on a stand: the retreat legend is the only one that moves the
wheels, for one second forward and the same path back. Session mortality
(`rt.session_idle_ms` of silence) costs ~13 s and runs only with `--slow`.

The dropped-frames check does not just look for aliveness (some push arriving):
a mock that quietly accepts one of the bad datagrams still looks "alive" to a
check that only wants any push at all. It reads the car's own `link.rx_hz`
instead — 0 means nothing landed, non-zero means one was silently accepted — the
same signal a real fleet's telemetry would surface. The rule-6 shapes are
numbered fresh against the live seq counter, not with small fixed literals: a
literal that is already stale gets dropped by the (both-implementations-shared)
replay gate regardless of whether the shape itself would have been rejected, so
a lax parser's real defect never gets exercised — see rule6_seq_frames below.

Stdlib only — no venv needed against a car. The dropped frames mirror the rule-6
table `test_state.py::TestWireShapes.test_the_shared_pinned_frames` and the
firmware's `test_control_proto.c` pin, in v2 spelling.
"""
import argparse
import json
import os
import secrets
import socket
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_car"))
from generated import DEVICE, GROUPS, PROTO, RT, TELEMETRY_GROUPS   # noqa: E402

K = RT["keys"]
TYPES = RT["types"]
_OWNER_FIELD = next(f for f in GROUPS["motors"]["fields"] if f["name"] == "owner")
OWNER_VALUES = _OWNER_FIELD["values"]
# The three words the watchdog and retreat legends read, by position: the list is
# link_src_t's order on all three sides, and tools/mock_car/state.py unpacks it the
# same way. A rename in the schema moves these with it; a reorder breaks both.
OWNER_IDLE, OWNER_RECOVERING, OWNER_REMOTE = (OWNER_VALUES[0], OWNER_VALUES[1],
                                              OWNER_VALUES[3])
# The pack's words, the same way: `absent` is the last of the three, and the one
# whose meaning the schema spells out — every number of the group null.
_BATTERY_FIELD = next(f for f in GROUPS["battery"]["fields"] if f["name"] == "state")
BATTERY_VALUES = _BATTERY_FIELD["values"]
BATTERY_ABSENT = BATTERY_VALUES[2]
BATTERY_NUMBERS = [f["name"] for f in GROUPS["battery"]["fields"] if f["type"] == "int"]

# A literal worth naming once: the contract has no field-name constant for a
# malformed-hello probe's own sid — it is just this tool's own probe value, used
# from more than one place below, and a typo in one copy would silently un-test
# the thing it was checking.
BAD_HELLO_SID = "abcd1234"

# How long one receive waits for the car when nothing bounds it tighter: one
# telemetry period, so a silent car costs a scan one missed push, not a hang.
SOCK_TIMEOUT_S = 1.0 / RT["telemetry_hz"]

# rule 6: both sides drop these whole, and neither carries a usable seq — a hello
# is never subject to the owned-traffic seq gate at all, and a bye missing its
# seq entirely is exactly the shape under test.
DROPPED_FRAMES = [
    ('{"proto":1.5,"type":"hello","session":"%s"}' % BAD_HELLO_SID).encode(),
    ('{"proto":%d,"type":"bye"}' % PROTO).encode(),          # a goodbye without a seq
]


def rule6_seq_frames(fresh_seqs):
    """The rule-6 malformed `drive` shapes that carry a seq — numbered fresh, not
    with small fixed literals.

    A literal is always stale by the time the batch is sent (the telemetry loop
    above has already pushed the live counter well past single digits), so on a
    mock whose actual defect is a lax *parser*, the frame still gets dropped —
    just at the seq gate, for the wrong reason, before the parser is ever
    exercised. Freshly numbered, a frame a lax parser wrongly accepts also clears
    the (unrelated, correctly functioning) replay gate, reaches note_command, and
    shows up in rx_hz — which is what the caller below is checking.
    """
    a, b, c, d = fresh_seqs
    return [
        # has "turn" but not the top-level "throttle" the axis pair requires —
        # the nested one must not be read as the datagram's own.
        ('{"proto":%d,"type":"drive","seq":%d,"junk":{"throttle":0.9},"turn":0.5}'
         % (PROTO, a)).encode(),
        ('{"proto":%d,"type":"drive","seq":%d,"throttle":.5,"turn":0}'
         % (PROTO, b)).encode(),                              # bare mantissa
        ('{"proto":%d,"type":"drive","seq":%d,"throttle":+1,"turn":0}'
         % (PROTO, c)).encode(),                               # leading plus
        ('{"proto":%d,"type":"drive","seq":%d,"throttle":0.5,"turn":0,"throttle":0.9}'
         % (PROTO, d)).encode(),                                # duplicate key
    ]


class Unreachable(Exception):
    pass


def enc(obj):
    return json.dumps(obj, separators=(",", ":")).encode()


def is_telemetry(f):
    return isinstance(f, dict) and f.get(K["type"]) == TYPES["telemetry"]


def owner_of(f):
    return (f.get("motors") or {}).get("owner")


def rx_hz_of(f):
    return (f.get("link") or {}).get("rx_hz")


def battery_of(f):
    return f.get("battery") or {}


def timeouts_of(f):
    return (f.get("link") or {}).get("timeouts")


def is_count(v):
    return isinstance(v, int) and not isinstance(v, bool)


def padded_frame(seq_value, total_len):
    """A valid drive frame, padded via an ignored "p" key to exactly total_len bytes.

    Not a fixed byte template: seq_value's digit count depends on how far the
    live counter has moved by the time this is called, so the padding has to
    be sized against the actual encoded prefix rather than a guess baked in
    at import time.
    """
    obj = {K["proto"]: PROTO, K["type"]: TYPES["drive"], K["seq"]: seq_value,
           K["throttle"]: 0, K["turn"]: 0, "p": ""}
    fill = total_len - len(enc(obj))
    assert fill >= 0, f"{total_len} bytes is too small to hold seq {seq_value}"
    obj["p"] = "x" * fill
    return enc(obj)


class RTConformance:
    def __init__(self, host, port, verbose=False, slow=False):
        self.addr = (host, port)
        self.verbose = verbose
        self.slow = slow
        self.failures = []

    def check(self, ok, what):
        if not ok:
            self.failures.append(what)
            print(f"  FAIL  {what}")
        return ok

    def field_type_ok(self, value, f):
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

    def sock(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(SOCK_TIMEOUT_S)
        return s

    def recv_frame(self, s, deadline_s):
        """The next parseable frame within deadline_s, or None.

        The socket's timeout is bounded to what is left of the deadline while this
        runs, and restored after: a caller streaming at rt.command_hz waits for a
        push only until its next send is due, not for the socket's full
        SOCK_TIMEOUT_S — which used to hold the stream to half the contract's rate,
        and a retreat legend's path to half its crumbs.
        """
        end = time.monotonic() + deadline_s
        try:
            while True:
                left = end - time.monotonic()
                if left <= 0:
                    return None
                s.settimeout(min(SOCK_TIMEOUT_S, left))
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
        finally:
            s.settimeout(SOCK_TIMEOUT_S)

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
        frame the other needed. Telemetry keeps pushing on its own cadence
        throughout, so a "search for A, then search for B" over the same
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
        frame = {K["proto"]: proto, K["type"]: TYPES["hello"], K["session"]: sid}
        for _ in range(15):                                 # ~3 s at 5 Hz
            s.sendto(enc(frame), self.addr)
            reply = self.recv_matching(s, lambda f: f.get(K["type"]) == TYPES["hello_ack"], 0.2)
            if reply is not None:
                return reply
        raise Unreachable(f"no hello reply from {self.addr[0]}:{self.addr[1]}")

    def drive(self, seq, throttle=0.0):
        return enc({K["proto"]: PROTO, K["type"]: TYPES["drive"], K["seq"]: seq,
                    K["throttle"]: throttle, K["turn"]: 0.0})

    def stream(self, s, seq, seconds, throttle=0.0):
        """Hold one command on the wire at rt.command_hz for `seconds`, numbering
        from seq + 1 — the way the app streams the held stick — and return (the
        telemetry frames heard meanwhile, the last seq sent).

        Every legend that needs the watchdog armed, or a breadcrumb path worth
        retracing, goes through here, so the axes this tool ever drives are
        visible in one place: zeros everywhere except the retreat legend's one
        second at 0.3.
        """
        frames = []
        end = time.monotonic() + seconds
        next_send = 0.0
        while time.monotonic() < end:
            now = time.monotonic()
            if now >= next_send:
                seq += 1
                s.sendto(self.drive(seq, throttle), self.addr)
                next_send = now + 1.0 / RT["command_hz"]
            # Listen exactly until the next send is due, so the cadence is the
            # contract's and not a function of when pushes happen to arrive.
            f = self.recv_frame(s, min(next_send, end) - time.monotonic())
            if f is not None and is_telemetry(f):
                frames.append(f)
        return frames, seq

    def run(self):
        sid = secrets.token_hex(4)
        s = self.sock()

        print("hello")
        reply = self.handshake(s, sid)
        self.check(reply.get(K["proto"]) == PROTO,
                   f"hello reply proto {reply.get(K['proto'])!r}, want {PROTO}")
        self.check(reply.get(K["session"]) == sid,
                   f"hello reply echoes session {reply.get(K['session'])!r}, want {sid!r}")
        device = reply.get("device")
        if self.check(isinstance(device, dict), f"hello reply device is {device!r}, want an object"):
            names = [f["name"] for f in GROUPS["device"]["fields"]]
            self.check(sorted(device) == sorted(names),
                       f"hello reply device keys {sorted(device)}, want {sorted(names)}")
            for f in GROUPS["device"]["fields"]:
                v = device.get(f["name"])
                self.check(self.field_type_ok(v, f),
                           f"hello reply device.{f['name']} is {v!r}, want {f['type']}")
            self.check(device.get("id") == DEVICE,
                       f"hello reply device.id {device.get('id')!r}, want {DEVICE!r}")
        again = self.handshake(s, sid)
        self.check(again.get(K["session"]) == sid,
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
            self.check(foreign.get(K["proto"]) == PROTO,
                       "a foreign proto is answered by name, so a client can stop searching")
            # Not adopted: a session that was never opened must never be pushed to.
            s2.sendto(enc({K["proto"]: PROTO, K["type"]: TYPES["drive"], K["seq"]: 1,
                          K["throttle"]: 0.9, K["turn"]: 0.0}), self.addr)
            never = self.recv_matching(s2, is_telemetry, 0.7)
            self.check(never is None, "a foreign-proto hello must not have opened a session")
        s2.close()

        print("drive requires our own proto")
        # New in v2: proto is carried but not judged by the parser — only the link
        # judges it, and it must reject a drive missing it or speaking a foreign one,
        # even from the adopted owner's own socket. Measured via link.rx_hz, not just
        # aliveness, so a lax car that quietly accepts one cannot pass by accident.
        primed = self.recv_matching(s, is_telemetry, 1.0)
        self.check(primed is not None, "a push arrives before the no-proto probe")
        no_proto = enc({K["type"]: TYPES["drive"], K["seq"]: 900001,
                        K["throttle"]: 0.9, K["turn"]: 0.0})
        foreign_proto = enc({K["proto"]: 1, K["type"]: TYPES["drive"], K["seq"]: 900002,
                            K["throttle"]: 0.9, K["turn"]: 0.0})
        s.sendto(no_proto, self.addr)
        s.sendto(foreign_proto, self.addr)
        seen = [f for f in self.scan_window(s, 1.0) if is_telemetry(f)]
        self.check(bool(seen), "telemetry keeps flowing after the no-/foreign-proto probes")
        bad_rx = [rx_hz_of(f) for f in seen if rx_hz_of(f)]
        self.check(not bad_rx, f"link.rx_hz after a no-/foreign-proto drive was "
                               f"{bad_rx or [0]}, want all 0 — the car accepted one")

        print("telemetry")
        frames, seq = self.stream(s, 0, 1.5)
        self.check(len(frames) >= 3,
                   f"telemetry: {len(frames)} frames in 1.5 s of streaming, want >= 3")
        if frames:
            seqs = [fr.get(K["seq"]) for fr in frames]
            self.check(all(isinstance(sq, int) and not isinstance(sq, bool) for sq in seqs),
                       f"telemetry seqs {seqs}, want every one an int")
            # The client's whole loss-detection story is "a gap in seq means a drop" —
            # a repeat or a rewind must never happen, on any network.
            self.check(all(b > a for a, b in zip(seqs, seqs[1:])),
                       f"telemetry seq is not strictly increasing: {seqs}")
            # Consecutive (no gaps at all) only makes sense where nothing between this
            # tool and the target can drop a datagram — loopback against the mock. Over
            # real WiFi a lost push is not a defect, so this half is skipped there.
            if self.addr[0] in ("127.0.0.1", "::1", "localhost"):
                self.check(seqs == list(range(seqs[0], seqs[0] + len(seqs))),
                           f"telemetry seq has gaps on loopback: {seqs}, want consecutive "
                           f"integers from {seqs[0]}")
            f = frames[-1]
            self.check(isinstance(f.get(K["seq"]), int) and not isinstance(f.get(K["seq"]), bool),
                       f"telemetry seq is {f.get(K['seq'])!r}, want an int")
            for g in TELEMETRY_GROUPS:
                group = f.get(g)
                if not self.check(isinstance(group, dict), f"telemetry.{g} is {group!r}, want an object"):
                    continue
                fields = GROUPS[g]["fields"]
                names = [gf["name"] for gf in fields]
                self.check(sorted(group) == sorted(names),
                           f"telemetry.{g} keys {sorted(group)}, want {sorted(names)}")
                for gf in fields:
                    v = group.get(gf["name"])
                    self.check(self.field_type_ok(v, gf),
                               f"telemetry.{g}.{gf['name']} is {v!r}, want {gf['type']}")
            self.check(owner_of(f) in OWNER_VALUES,
                       f"telemetry motors.owner {owner_of(f)!r} not in {OWNER_VALUES}")
            # The pack (`car/battery-monitor`): the type walk above only says an int or a
            # null is in each slot. What the badge draws from has meaning past that — the
            # percent is a percent, and a monitor that is not there has no numbers at all.
            battery = battery_of(f)
            self.check(battery.get("state") in BATTERY_VALUES,
                       f"telemetry battery.state {battery.get('state')!r} not in {BATTERY_VALUES}")
            soc = battery.get("soc_pct")
            self.check(soc is None or (is_count(soc) and 0 <= soc <= 100),
                       f"telemetry battery.soc_pct is {soc!r}, want 0..100 or null")
            if battery.get("state") == BATTERY_ABSENT:
                nulls = {k: battery.get(k) for k in BATTERY_NUMBERS}
                self.check(all(v is None for v in nulls.values()),
                           f"telemetry battery is {BATTERY_ABSENT!r} but carries {nulls}, want every number null")

        print("watchdog (silence after zeros: link.timeouts +1, idle, the session lives on)")
        # The stream above just stopped, and every quiet stretch of this tool used
        # to trip the car's watchdog unread. Read it: silence past rt.watchdog_ms
        # after an accepted command is a loss, counted once in link.timeouts, and
        # the path it would retrace is all zeros — so the honest answer is a stop,
        # not a retreat, and the actuator falls to idle. The session is not over:
        # telemetry keeps coming, and the stream resumes on the same session with
        # fresh seqs — no new hello — which is what a driver walking back into
        # range relies on. All of it stationary on the bench.
        before = timeouts_of(frames[-1]) if frames else None
        if not self.check(is_count(before),
                          f"link.timeouts while streaming is {before!r}, want a count"):
            before = None
        if frames:
            self.check(owner_of(frames[-1]) == OWNER_REMOTE,
                       f"motors.owner while the zero stream is held is "
                       f"{owner_of(frames[-1])!r}, want {OWNER_REMOTE!r}")
        # Two half-second windows rather than one: the trip lands in the first
        # (rt.watchdog_ms plus a push period), so the second is proof on its own
        # that the session outlives it — one frame there is one frame after.
        early = [f for f in self.scan_window(s, 0.5) if is_telemetry(f)]
        late = [f for f in self.scan_window(s, 0.5) if is_telemetry(f)]
        self.check(bool(early), "telemetry keeps coming while the tool is silent")
        self.check(bool(late), "telemetry keeps coming past rt.watchdog_ms of silence — "
                               "a watchdog trip is not the end of the session")
        quiet = early + late
        if quiet and before is not None:
            trips = [timeouts_of(f) for f in quiet]
            self.check(before + 1 in trips,
                       f"link.timeouts within 1 s of silence was {trips}, want "
                       f"{before + 1} — the watchdog must trip at rt.watchdog_ms")
            self.check(all(t in (before, before + 1) for t in trips),
                       f"link.timeouts within 1 s of silence was {trips}, want no more "
                       f"than one trip — a trip disarms until the stream resumes")
            owners = [owner_of(f) for f in quiet if timeouts_of(f) == before + 1]
            self.check(all(o == OWNER_IDLE for o in owners),
                       f"motors.owner after the trip was {owners}, want all "
                       f"{OWNER_IDLE!r} — a path of zeros stops, it does not retrace")
        resumed, seq = self.stream(s, seq, 0.6)
        self.check(bool(resumed), "telemetry keeps coming when the stream resumes")
        if resumed:
            rx = [rx_hz_of(f) for f in resumed]
            self.check(any(rx), f"link.rx_hz after the stream resumed without a hello was "
                                f"{rx}, want > 0 — the trip must not end the session")
            self.check(owner_of(resumed[-1]) == OWNER_REMOTE,
                       f"motors.owner once the stream resumed is {owner_of(resumed[-1])!r}, "
                       f"want {OWNER_REMOTE!r} — the driver is back")

        print("repeat hello keeps the gate")
        # The session is live and was just driving. A hello with the same sid from
        # the same socket is what the app sends when a reply was lost; answered, it
        # must change nothing — a replay of the last accepted seq stays refused.
        # The fresh seq right after is accepted, so the zero is the gate's verdict
        # and not a dead session's silence. Quiesced first, for a fixed half
        # second rather than until a gap: the push counting the resumed stream's
        # last commands may still be in flight, and over WiFi it may be late.
        self.scan_window(s, 0.5)
        try:
            again = self.handshake(s, sid)
        except Unreachable:
            # Reachability is long proven: a named FAIL, not exit 2, as with the
            # foreign-proto handshake above.
            self.check(False, "the owner's repeated hello mid-session gets no reply")
        else:
            self.check(again.get(K["session"]) == sid,
                       "the owner's repeated hello mid-session is answered")
        s.sendto(self.drive(seq), self.addr)       # the last accepted seq, replayed
        seen = [f for f in self.scan_window(s, 0.6) if is_telemetry(f)]
        self.check(bool(seen), "telemetry keeps flowing after a repeated hello")
        bad_rx = [rx_hz_of(f) for f in seen if rx_hz_of(f)]
        self.check(not bad_rx,
                   f"link.rx_hz after replaying seq {seq} behind a repeated hello was "
                   f"{bad_rx or [0]}, want 0 — the repeat reset the sequence gate")
        if seen and before is not None:
            # This silence is the second one since the stream first resumed: the
            # resumed stream re-armed the watchdog, or the counter would sit at +1.
            self.check(timeouts_of(seen[-1]) == before + 2,
                       f"link.timeouts after the resumed stream went quiet is "
                       f"{timeouts_of(seen[-1])}, want {before + 2} — a resumed stream "
                       f"must re-arm the watchdog without a new hello")
        seq += 1
        for _ in range(3):                         # as the cap probe below: WiFi may lose one
            s.sendto(self.drive(seq), self.addr)
        after_fresh = [rx_hz_of(f) for f in self.scan_window(s, 0.6) if is_telemetry(f)]
        self.check(any(after_fresh),
                   f"link.rx_hz after a fresh seq behind the repeated hello was "
                   f"{after_fresh}, want > 0 — the session is alive, so the zero above "
                   f"was the gate's doing")

        print("retreat (1 s at throttle 0.3, then silence: recovering, then idle)")
        # The one legend that moves the wheels, and by design the only one: a
        # second of 30 % throttle is the path, and the retreat that follows is its
        # reverse, segment for segment — a short roll forward and the same roll
        # back on the bench, never a drive longer than the second it was given.
        # The earlier silences consumed their own (motionless) paths at their
        # trips, so what this trip retraces is exactly these ten crumbs.
        moving, seq = self.stream(s, seq, 1.0, throttle=0.3)
        self.check(bool(moving), "telemetry arrives while the 0.3 stream is held")
        before_move = timeouts_of(moving[-1]) if moving else None
        if is_count(before_move):
            # The trip at rt.watchdog_ms, ~1.15 s of retrace (ten 100 ms segments
            # and the capped newest one), then the stop — all inside 2.5 s.
            silent = [f for f in self.scan_window(s, 2.5) if is_telemetry(f)]
            tripped = [f for f in silent if timeouts_of(f) == before_move + 1]
            owners = [owner_of(f) for f in tripped]
            if self.check(bool(tripped),
                          f"link.timeouts after the 0.3 stream went quiet was "
                          f"{[timeouts_of(f) for f in silent]}, want {before_move + 1}"):
                # The first frame after the trip names the retreat. The car gets one
                # push of grace: its retreat task sits one priority below rt_task,
                # so a push that shares the trip's tick leaves before the first
                # reverse step lands — that frame is idle (the RT grant is revoked
                # at the trip), and the next one is recovering. The mock trips and
                # retreats in one tick.
                self.check(owners[:1] == [OWNER_RECOVERING]
                           or owners[:2] == [OWNER_IDLE, OWNER_RECOVERING],
                           f"motors.owner in the first frames after the trip was "
                           f"{owners[:2]}, want {OWNER_RECOVERING!r} — a path with motion "
                           f"is retraced, and telemetry names the retreat")
                self.check(OWNER_REMOTE not in owners,
                           f"motors.owner during the silence was {owners}, want no "
                           f"{OWNER_REMOTE!r} — the trip revokes the driver's grant")
            if OWNER_RECOVERING in owners:
                rest = owners[owners.index(OWNER_RECOVERING):]
                if self.check(OWNER_IDLE in rest,
                              f"motors.owner after the trip was {owners}, want {OWNER_IDLE!r} "
                              f"within 2.5 s — a 1 s path retraces in about 1.15 s"):
                    stopped = rest[rest.index(OWNER_IDLE):]
                    self.check(all(o == OWNER_IDLE for o in stopped),
                               f"motors.owner after the retrace ended was {stopped}, want all "
                               f"{OWNER_IDLE!r} — one retreat per loss")

        print("dropped datagrams (rejection is measured via link.rx_hz, not just aliveness)")
        # Quiesce: the retreat legend above ended in silence, and the car is idle.
        # Read one push before doing anything else, so the pushes we inspect
        # below are ones generated after this point, not stale carryover.
        flushed = self.recv_matching(s, is_telemetry, 1.0)
        self.check(flushed is not None, "a push still arrives once streaming stops")

        # The rule-6 shapes, numbered fresh against the live counter (see
        # rule6_seq_frames' docstring) — only those that carry a seq need it;
        # the malformed hello and the seq-less bye are unaffected either way.
        seq_frames = rule6_seq_frames([seq + 10, seq + 11, seq + 12, seq + 13])
        oversized = padded_frame(seq + 14, RT["max_command"] + 1)
        # The stale/replayed seq is the one frame that is *supposed* to be low —
        # this is what a network-delayed duplicate of the telemetry loop's own
        # traffic looks like, and it is the replay gate specifically under test
        # here, not the parser. throttle is zeroed: on a car with broken replay
        # protection this would otherwise be a live 90% throttle command issued
        # by a conformance tool.
        stale_frame = enc({K["proto"]: PROTO, K["type"]: TYPES["drive"], K["seq"]: 1,
                            K["throttle"]: 0.0, K["turn"]: 0.0})
        bad_batch = DROPPED_FRAMES + seq_frames + [oversized, stale_frame]
        for bad in bad_batch:
            s.sendto(bad, self.addr)

        # One shared scan answers both questions below. Running them as two
        # separate blind searches (recv_matching, then drain, then another
        # recv_matching) would have each one discard frames the other needed:
        # a search for the stray hello-reply throws away every push it passes
        # over, including the one whose rx_hz would prove a bad frame got in.
        # 1.0 s (not the tighter 0.6 s used elsewhere below) because this scan
        # also carries the "does the session even survive" check — on real
        # WiFi a lost push or two must not read as a dead session.
        seen = self.scan_window(s, 1.0)
        stray = next((f for f in seen if f.get(K["type"]) == TYPES["hello_ack"]
                     and f.get(K["session"]) == BAD_HELLO_SID), None)
        self.check(stray is None, "a malformed hello (proto:1.5) must not be answered")
        pushes = [f for f in seen if is_telemetry(f)]
        self.check(bool(pushes),
                   "the session survives the dropped datagrams and still pushes")
        bad_rx = []
        for f in pushes:
            rx = rx_hz_of(f)
            if rx is None:
                # A push missing link.rx_hz entirely must not read the same as a
                # push reporting 0 — an omitting car would otherwise pass this
                # check by accident rather than by actually rejecting the batch.
                self.check(False, f"a push after the bad batch is missing link.rx_hz: "
                                   f"{sorted(f)}")
                continue
            if rx:
                bad_rx.append(rx)
        self.check(not bad_rx,
                   f"link.rx_hz after the {len(bad_batch)} bad datagrams was "
                   f"{bad_rx or [0]}, want all 0 — a lax car accepted one or more "
                   f"of them")

        print("accepted at the cap")
        # The other side of the same boundary: a *valid* command padded to
        # exactly max_command bytes must be admitted, not just the over-cap one
        # rejected above. seq jumps clear of the whole seq+10..seq+14 range the
        # batch above just used, so a lax car cannot confuse this probe with one
        # of those (the padded probe used to reuse seq+1, which collided with
        # the batch's literal seq 9 on exactly such a car).
        seq += 20
        valid_padded = padded_frame(seq, RT["max_command"])
        self.check(len(valid_padded) == RT["max_command"],
                   f"the padded probe is {len(valid_padded)} bytes, want exactly "
                   f"{RT['max_command']}")
        # Sent 3 times, same seq: on real WiFi a single lost datagram would
        # otherwise read as a cap defect that was never actually exercised.
        # Duplicates land as harmless replay-drops after the first acceptance
        # (same seq is never "newer"), so acceptance is still measured once.
        for _ in range(3):
            s.sendto(valid_padded, self.addr)
        # A single recv_matching for "the next push" is not safe here: the
        # server's push tick is on its own clock, so the datagrams we just sent
        # can land either side of the next tick's window depending on timing
        # alone, with no defect involved. Scan a few ticks' worth and require
        # the bump to show up on any one of them, the same tolerance the
        # bad-batch check above needs for the opposite reason.
        after_pushes = [f for f in self.scan_window(s, 0.6) if is_telemetry(f)]
        self.check(bool(after_pushes),
                   "telemetry keeps flowing after a max-size valid command")
        if after_pushes:
            seen_rx = [rx_hz_of(f) for f in after_pushes]
            self.check(any(seen_rx),
                       f"link.rx_hz after a valid {RT['max_command']}-byte command was "
                       f"{seen_rx}, want at least one > 0 — the cap must admit "
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
            displaced = self.recv_matching(s, is_telemetry, 0.7)
            self.check(displaced is None,
                       "after an eviction the displaced socket hears nothing")
            moved = self.recv_matching(s3, is_telemetry, 1.0)
            self.check(moved is not None, "telemetry follows the new owner")

            print("bye")
            s3.sendto(enc({K["proto"]: PROTO, K["type"]: TYPES["bye"], K["seq"]: 1}),
                      self.addr)
            time.sleep(0.3)
            self.drain(s3, 0.5)
            after = self.recv_matching(s3, is_telemetry, 1.0)
            self.check(after is None, "telemetry stops after a goodbye")
        s.close()
        s3.close()

        idle_s = RT["session_idle_ms"] / 1000.0
        if not self.slow:
            print(f"  skipped: session idle ({idle_s:g} s of silence ends the session) — "
                  f"pass --slow to run it; it adds ~{idle_s + 3:.0f} s")
            return self.failures

        print(f"session idle (--slow: {idle_s:g} s of silence ends the session)")
        # A fresh session: hello, half a second of zeros so the watchdog is armed
        # (the driver-vanished-after-a-trip case, not the never-commanded one),
        # then nothing. The trip disarms at rt.watchdog_ms; mortality counts from
        # the last accepted command, and at rt.session_idle_ms the session ends:
        # the pushes stop, a drive gets nothing, and the same sid — dead, but with
        # no live session to protect — is adopted again. Nothing moves.
        s5 = self.sock()
        sid5 = secrets.token_hex(4)
        try:
            self.handshake(s5, sid5)
        except Unreachable:
            self.check(False, "a fresh session's hello gets no reply at all "
                              "(session idle cannot be exercised without it)")
            s5.close()
            return self.failures
        _, seq5 = self.stream(s5, 0, 0.5)
        quiet_from = time.monotonic()               # the last command went out just before
        last_push = None
        end = quiet_from + idle_s + 1.5
        while time.monotonic() < end:
            f = self.recv_frame(s5, end - time.monotonic())
            if f is not None and is_telemetry(f):
                last_push = time.monotonic() - quiet_from
        if self.check(last_push is not None, "telemetry ran on after the stream stopped"):
            # Not later: a session that outlives the deadline pushes to a vanished
            # address and keeps a stale datagram's window open. Not earlier: a
            # session ended before it means a driver mid-stall loses the car for
            # nothing. One push period of slack past the deadline; a second before
            # it, since WiFi may lose the final push.
            self.check(idle_s - 1.0 <= last_push <= idle_s + 0.5,
                       f"the last push came {last_push:.2f} s after the last command, "
                       f"want about {idle_s:g} s — rt.session_idle_ms ends the session, "
                       f"not earlier and not later")
        seq5 += 1
        for _ in range(3):
            s5.sendto(self.drive(seq5), self.addr)
        revived = self.recv_matching(s5, is_telemetry, 1.0)
        self.check(revived is None, "after the session idled out a drive is dropped and "
                                    "nothing is pushed — the next driver says hello")
        try:
            back = self.handshake(s5, sid5)
        except Unreachable:
            self.check(False, "a hello with the dead session's sid gets no reply at all")
        else:
            self.check(back.get(K["session"]) == sid5,
                       "a hello with the dead session's sid is answered")
            readopted = self.recv_matching(s5, is_telemetry, 1.0)
            self.check(readopted is not None,
                       "with no live session, the dead sid's hello is adopted again — "
                       "telemetry resumes")
            s5.sendto(enc({K["proto"]: PROTO, K["type"]: TYPES["bye"], K["seq"]: 1}),
                      self.addr)
        s5.close()
        return self.failures


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("target", help="host:port, e.g. 127.0.0.1:4237 (the mock) or 192.168.7.1:4210 (the car, through the dongle)")
    p.add_argument("-v", "--verbose", action="store_true", help="log every frame")
    p.add_argument("--slow", action="store_true",
                   help="also wait out rt.session_idle_ms of silence to see the session "
                        "end (~13 s more); skipped by default so test-all.sh stays short")
    args = p.parse_args()
    host, _, port = args.target.partition(":")
    suite = RTConformance(host, int(port or RT["port"]), args.verbose, slow=args.slow)
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
