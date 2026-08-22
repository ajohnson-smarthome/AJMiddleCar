#!/usr/bin/env python3
"""Host tests for the mock's real-time channel. Stdlib only — no sockets, no event loop.

`rt_link.py` reaches the outside world through exactly three calls: `loop.time()`,
`loop.call_later()` and `transport.sendto()`. All three are supplied here, so adoption,
eviction, the sequence window, the goodbye, the two datagram caps and the telemetry push
are asserted by running the protocol rather than by remembering to drive a session by
hand. Before this file, every one of those lived in the gap between `test_state.py`
(state only) and `conformance.py` (REST only), and `tools/test-all.sh` said all green.
"""
import contextlib
import io
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from generated import PROTO, RT                        # noqa: E402
from rt_link import Impairment, REBOOT_QUIET_S, RTLink  # noqa: E402
from state import CTL_NONE, CTL_RT, CarState           # noqa: E402

APP = ("192.168.4.2", 50000)
OTHER = ("192.168.4.3", 50001)
DEADLINE_S = RT["watchdog_ms"] / 1000.0

HELLO = RT["hello_field"]
SEQ = RT["seq_field"]
BYE = RT["bye_field"]
T, Y = RT["throttle_field"], RT["yaw_field"]


class FakeLoop:
    """A clock that only moves when a test says so, and a call_later that runs inline."""

    def __init__(self):
        self.t = 0.0

    def time(self):
        return self.t

    def call_later(self, delay, fn, *args):
        fn(*args)


class Recorder:
    """The datagram transport, as a list."""

    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((json.loads(data), addr, len(data)))


class Quiet(unittest.TestCase):
    """The link narrates itself to stdout; a test run is not the place for it."""

    def setUp(self):
        quiet = contextlib.redirect_stdout(io.StringIO())
        quiet.__enter__()
        self.addCleanup(quiet.__exit__, None, None, None)


def link(**impair):
    loop = FakeLoop()
    car = CarState(now=0.0)
    rt = RTLink(car, Impairment(**impair), loop=loop)
    rt.connection_made(Recorder())
    return rt, car, loop


def send(rt, obj, addr=APP):
    """One datagram, exactly as it would go on the wire."""
    rt.datagram_received(json.dumps(obj, separators=(",", ":")).encode(), addr)


def hello(sid="7f3a91c2", proto=PROTO):
    return {RT["proto_field"]: proto, HELLO: sid}


def cmd(seq, t=0.0, y=0.0, **extra):
    return {SEQ: seq, T: t, Y: y, **extra}


class TestAdoption(Quiet):
    def test_a_hello_is_adopted_and_answered_with_identity(self):
        rt, car, _ = link()
        send(rt, hello())
        self.assertEqual(rt.owner, APP)
        self.assertEqual(rt.session, "7f3a91c2")
        reply, addr, _ = rt.transport.sent[-1]
        self.assertEqual(addr, APP)
        self.assertEqual(reply, {RT["proto_field"]: PROTO, HELLO: "7f3a91c2",
                                 RT["device_field"]: car.device, RT["fw_field"]: car.fw})

    def test_every_repeat_is_answered_but_only_a_change_re_adopts(self):
        rt, car, _ = link()
        send(rt, hello())
        send(rt, cmd(10, 0.5))
        send(rt, hello())                    # the app repeats until it hears one back
        self.assertEqual(len(rt.transport.sent), 2)
        self.assertEqual(rt.last_seq, 10, "a repeat of the live session is not a reset")

    def test_a_new_hello_evicts_the_old_owner(self):
        rt, car, _ = link()
        send(rt, hello())
        send(rt, cmd(10, 0.5))
        send(rt, hello("beef0001"), addr=OTHER)
        self.assertEqual(rt.owner, OTHER)
        self.assertIsNone(rt.last_seq, "the new session counts from wherever it likes")
        send(rt, cmd(1, 0.25), addr=OTHER)
        self.assertEqual(car.command, (0.25, 0.0))

    def test_a_hello_the_car_would_reject_changes_nothing(self):
        """The car's parser drops these whole: no adoption, no reply, no eviction.

        The mock binds 0.0.0.0, so one stray datagram on the LAN must not be able to
        evict the phone that is driving — and then have the same datagram be a no-op
        against the real car.
        """
        rt, car, _ = link()
        send(rt, hello())
        answered = len(rt.transport.sent)
        for bad in ({HELLO: {"a": 1}}, {HELLO: ""}, {HELLO: 1234},
                    {HELLO: "x" * 70}, {HELLO: 'a"b'}):
            send(rt, dict(hello(), **bad), addr=OTHER)
        self.assertEqual(rt.owner, APP)
        self.assertEqual(rt.session, "7f3a91c2")
        self.assertEqual(len(rt.transport.sent), answered, "no reply to a dropped frame")

    def test_a_reply_always_fits_a_datagram(self):
        rt, car, _ = link()
        car.fw = "v1.0+9000-dirty-with-a-long-branch-name"
        send(rt, hello("f" * 15))
        _, _, size = rt.transport.sent[-1]
        self.assertLessEqual(size, RT["max_datagram"])

    def test_a_foreign_proto_is_answered_but_not_adopted(self):
        rt, _, _ = link()
        send(rt, hello(proto=PROTO + 1))
        self.assertIsNone(rt.owner)
        self.assertEqual(rt.transport.sent[-1][0][RT["proto_field"]], PROTO,
                         "the reply names our version so the app can stop searching")

    def test_a_dead_sessions_hello_is_answered_but_not_adopted(self):
        """Rule 3: a network-duplicated hello of a session that already ended must
        not evict the live driver. Session 1 dies by bye; its delayed hello
        duplicate arrives while session 2 is driving."""
        rt, car, _ = link()
        send(rt, hello("dead0001"))
        send(rt, cmd(1, 0.5))
        send(rt, cmd(2, 0.0, 0.0, **{BYE: 1}))            # session 1 ends itself
        send(rt, hello("beef0002"), addr=OTHER)           # session 2 adopts
        send(rt, cmd(1, 0.7), addr=OTHER)
        answered = len(rt.transport.sent)
        send(rt, hello("dead0001"))                       # the delayed duplicate
        self.assertEqual(rt.owner, OTHER, "the live driver keeps the session")
        self.assertEqual(rt.session, "beef0002")
        self.assertEqual(len(rt.transport.sent), answered + 1,
                         "the dead hello is answered — a lost-reply retry must not hang")
        self.assertEqual(rt.transport.sent[-1][1], APP)
        send(rt, cmd(2, -0.3), addr=OTHER)
        self.assertEqual(car.command, (-0.3, 0.0), "the live stream still drives")

    def test_an_evicted_sid_is_dead_too(self):
        rt, car, _ = link()
        send(rt, hello("evict001"))
        send(rt, cmd(1, 0.5))
        send(rt, hello("beef0002"), addr=OTHER)           # evicts evict001
        send(rt, hello("evict001"))                       # a stale duplicate returns
        self.assertEqual(rt.owner, OTHER, "an evicted sid cannot re-adopt")

    def test_the_dead_ring_is_bounded(self):
        rt, car, _ = link()
        for i in range(6):              # each hello evicts the last: 5 dead sids recorded
            send(rt, hello(f"sid{i:05d}"))
        # The ring keeps the last 4 dead (sid00001..sid00004); sid00000 fell off.
        # A live session (sid00005) exists throughout, so refusal is in force.
        send(rt, hello("sid00000"), addr=OTHER)
        self.assertEqual(rt.session, "sid00000",
                         "a sid old enough to leave the ring may live again")
        send(rt, hello("sid00004"), addr=APP)             # still remembered
        self.assertEqual(rt.session, "sid00000", "and still refused while someone is live")

    def test_with_no_live_session_even_a_dead_sid_adopts(self):
        """Rule 3's refinement: refusing a dead sid when nobody is live would wedge
        a client whose session idled out mid-handshake, and adopting a stale
        duplicate displaces nobody — the phantom dies again by rule 4."""
        rt, car, _ = link()
        send(rt, hello("phoenix1"))
        send(rt, cmd(1, 0.5))
        send(rt, cmd(2, 0.0, 0.0, **{BYE: 1}))            # dies; nobody else arrives
        self.assertIsNone(rt.owner)
        send(rt, hello("phoenix1"))                       # the same sid returns
        self.assertEqual(rt.owner, APP, "with no live session, any hello adopts")
        self.assertEqual(rt.session, "phoenix1")


class TestOwnedTraffic(Quiet):
    def test_a_command_drives_and_counts(self):
        rt, car, loop = link()
        send(rt, hello())
        for k in range(10):
            loop.t = k * 0.1
            send(rt, cmd(k + 1, 0.5, -0.25))
        self.assertEqual(car.command, (0.5, -0.25))
        self.assertEqual(car.ctl, CTL_RT)
        # rx_fps is now a per-consumer delta (needs priming); the 1 s window count
        # this line used to read through it still exists, as _window_fps.
        self.assertEqual(rt._window_fps(loop.t), 10)

    def test_a_stranger_is_dropped_without_touching_anything(self):
        rt, car, _ = link()
        send(rt, hello())
        send(rt, cmd(5, 0.5))
        send(rt, cmd(9, -1.0), addr=OTHER)
        self.assertEqual(car.command, (0.5, 0.0))
        self.assertEqual(rt.last_seq, 5, "a stranger cannot burn a sequence number")
        self.assertEqual(rt.owner, APP)

    def test_a_replayed_or_reordered_seq_is_dropped(self):
        rt, car, _ = link()
        send(rt, hello())
        send(rt, cmd(5, 0.5))
        send(rt, cmd(5, -1.0))
        send(rt, cmd(4, -1.0))
        self.assertEqual(car.command, (0.5, 0.0))
        send(rt, cmd(6, -0.5))
        self.assertEqual(car.command, (-0.5, 0.0))

    def test_the_seq_window_wraps(self):
        rt, car, _ = link()
        send(rt, hello())
        send(rt, cmd(0xFFFFFFFF, 0.5))
        send(rt, cmd(1, -0.5))
        self.assertEqual(car.command, (-0.5, 0.0), "a session must survive 2^32 frames")

    def test_a_frame_the_car_drops_burns_nothing(self):
        """Parse before ownership, sequence or the watchdog — as control_proto.c does.

        A malformed axis used to advance `last_seq` here and not on the car, so the
        retransmitted good frame that followed was dropped as stale in the simulator
        only.
        """
        rt, car, _ = link()
        send(rt, hello())
        send(rt, cmd(5, 0.5))
        for bad in ({SEQ: 6, T: True, Y: False}, {SEQ: 6, T: "0.5", Y: "0"},
                    {SEQ: 6, T: 0.5}, {SEQ: 6}, {SEQ: 6, T: 0, Y: 0, BYE: "yes"}):
            send(rt, bad)
        rt.datagram_received(b"{not json", APP)
        rt.datagram_received(b"[1,2]", APP)
        self.assertEqual(rt.last_seq, 5)
        self.assertEqual(car.command, (0.5, 0.0))
        send(rt, cmd(6, -0.5))
        self.assertEqual(car.command, (-0.5, 0.0))

    def test_a_datagram_over_the_command_cap_is_dropped(self):
        rt, car, _ = link()
        send(rt, hello())
        send(rt, cmd(1, 0.5))
        pad = RT["max_command"] - len(json.dumps(cmd(2, 0.25, 0.0, pad=""),
                                                 separators=(",", ":")))
        send(rt, cmd(2, 0.25, 0.0, pad="x" * pad))
        self.assertEqual(car.command, (0.25, 0.0), "a datagram at the cap is accepted")
        send(rt, cmd(3, -1.0, 0.0, pad="x" * (pad + 1)))
        self.assertEqual(car.command, (0.25, 0.0), "one byte over is dropped")

    def test_a_command_without_a_seq_is_dropped(self):
        rt, car, _ = link()
        send(rt, hello())
        send(rt, {T: 0.9, Y: 0.0})
        self.assertEqual(car.command, (0.0, 0.0))


class TestGoodbye(Quiet):
    def test_bye_stops_the_car_and_drops_ownership(self):
        rt, car, loop = link()
        send(rt, hello())
        loop.t = 1.0
        send(rt, cmd(1, 0.9))
        loop.t = 1.1
        send(rt, cmd(2, 0.0, 0.0, **{BYE: 1}))
        self.assertEqual(car.command, (0.0, 0.0))
        self.assertIsNone(rt.owner)
        self.assertIsNone(rt.last_seq)
        # Silence after a goodbye is not silence that means the driver is out of range.
        loop.t = 2.0
        self.assertIsNone(rt.tick(loop.t))
        self.assertEqual(car.wdt_trips, 0)
        # SAFE is released with the stop, not held: a goodbye must not lock OTA, the
        # wizard and the console out of the car until an app reconnects. What suppresses
        # the retreat is the cleared history.
        self.assertEqual(car.ctl, CTL_NONE)
        self.assertEqual(car.history_len, 0)

    def test_a_bare_goodbye_is_acted_on(self):
        """`{"seq":n,"bye":1}` with no axes — the car acts on it, so this must too."""
        rt, car, loop = link()
        send(rt, hello())
        send(rt, cmd(1, 0.9))
        rt.datagram_received(json.dumps({SEQ: 2, BYE: 1}).encode(), APP)
        self.assertEqual(car.command, (0.0, 0.0))
        self.assertIsNone(rt.owner, "a bare goodbye still drops ownership")

    def test_a_goodbye_without_a_seq_is_dropped(self):
        """Every app->car datagram except `hello` carries `seq`, goodbye included.

        Accepting one would be a frame that bypasses replay protection: a recorded
        goodbye replayed later would stop a session that never sent it.
        """
        rt, car, _ = link()
        send(rt, hello())
        send(rt, cmd(1, 0.9))
        rt.datagram_received(json.dumps({BYE: 1}).encode(), APP)
        self.assertEqual(car.command, (0.9, 0.0))
        self.assertEqual(rt.owner, APP)

    def test_after_a_goodbye_the_old_peer_is_a_stranger(self):
        rt, car, _ = link()
        send(rt, hello())
        send(rt, cmd(1, 0.9))
        send(rt, cmd(2, 0.0, 0.0, **{BYE: True}))
        send(rt, cmd(3, 0.9))
        self.assertEqual(car.command, (0.0, 0.0), "ownership is not resumable")
        send(rt, hello("beef0002"))
        send(rt, cmd(1, 0.4))
        self.assertEqual(car.command, (0.4, 0.0))


class TestWatchdog(Quiet):
    def test_a_trip_keeps_the_sequence_gate(self):
        """Rule 1 of the 2026-08-22 audit-fix spec: the gate survives a trip.

        Clearing it opened the window the audit confirmed: one network-delayed
        duplicate of a pre-dropout command was accepted as the resumed stream,
        drove the car at stale stick values and aborted the retreat. A genuinely
        resuming same-session stream carries monotonically newer seqs and passes
        the kept gate; only adopt and bye reset it.
        """
        rt, car, loop = link()
        send(rt, hello())
        loop.t = 1.0
        send(rt, cmd(500, 0.9))
        loop.t = 1.0 + DEADLINE_S + 0.05
        self.assertIsNotNone(rt.tick(loop.t))
        self.assertEqual(car.wdt_trips, 1)
        self.assertEqual(rt.last_seq, 500, "the gate survives the trip")
        # Channel ownership survives too: a stream that comes back is the same
        # session, and the app would otherwise be ignored until it said hello.
        self.assertEqual(rt.owner, APP)
        send(rt, cmd(499, -0.5))                 # the delayed pre-dropout duplicate
        self.assertEqual(car.command, (-0.9, 0.0), "a stale frame cannot drive the car; the retreat continues")
        send(rt, cmd(501, -0.5))                 # the genuinely resumed stream
        self.assertEqual(car.command, (-0.5, 0.0), "the resumed stream is heard")

    def test_a_handshake_that_goes_quiet_does_not_trip(self):
        """Adoption leaves the watchdog disarmed; the first command arms it.

        The app repeats `hello` until it is answered and starts its send loop only then,
        so at 10% loss a handshake can easily outlast the deadline. Tripping there would
        run the retreat before a single command had arrived.
        """
        rt, car, loop = link()
        send(rt, hello())
        for k in range(1, 101):                        # 5 s of hello-only silence
            loop.t = k * 0.05
            self.assertIsNone(rt.tick(loop.t))
        self.assertEqual(car.wdt_trips, 0)
        send(rt, cmd(1, 0.9))
        loop.t += DEADLINE_S + 0.05
        self.assertIsNotNone(rt.tick(loop.t), "the first command arms it")
        self.assertEqual(car.wdt_trips, 1)

    def test_a_tripped_session_expires_past_the_idle_window(self):
        """Amended rule 4: sessions are mortal, anchored at their LAST ACTIVITY —
        the last accepted command — not at the trip. Without this the car pushed
        telemetry to a vanished owner forever, and the kept seq gate (rule 1)
        would hold state for a peer that will never return. At exactly
        session_idle_ms the session is still alive; strictly past it, it ends."""
        idle_s = RT["session_idle_ms"] / 1000.0
        rt, car, loop = link()
        loop.t = 1.0
        send(rt, hello("mortal01"))
        send(rt, cmd(10, 0.5))                                # last activity: t = 1.0
        loop.t = 1.0 + DEADLINE_S + 0.05
        self.assertIsNotNone(rt.tick(loop.t))                 # the trip disarms
        loop.t = 1.0 + idle_s                                 # exactly the window
        rt.tick(loop.t)
        self.assertEqual(rt.owner, APP, "at exactly idle_ms the session still lives")
        rt.transport.sent.clear()
        rt.push_telemetry(loop.t)
        self.assertEqual(len(rt.transport.sent), 1, "telemetry still flows")
        loop.t = 1.0 + idle_s + 0.05                          # strictly past it
        rt.tick(loop.t)
        self.assertIsNone(rt.owner, "the session expired, watchdog_ms earlier than trip+idle")
        self.assertIn("mortal01", rt.dead_sids)
        rt.transport.sent.clear()
        rt.push_telemetry(loop.t)
        self.assertEqual(rt.transport.sent, [], "no more telemetry to a dead peer")
        send(rt, hello("fresh002"))
        self.assertEqual(rt.owner, APP, "a fresh hello starts over")

    def test_activity_moves_the_expiry_deadline(self):
        idle_s = RT["session_idle_ms"] / 1000.0
        rt, car, loop = link()
        loop.t = 1.0
        send(rt, hello("mortal02"))
        send(rt, cmd(10, 0.5))                                # activity at 1.0
        loop.t = 1.0 + DEADLINE_S + 0.05
        rt.tick(loop.t)                                       # trip
        loop.t = 5.0
        send(rt, cmd(11, 0.5))                                # activity moves to 5.0
        loop.t = 1.0 + idle_s + 0.05                          # past the OLD deadline
        rt.tick(loop.t)                                       # (trips again — fine)
        self.assertEqual(rt.owner, APP, "the resumed command moved the deadline")
        loop.t = 5.0 + idle_s + 0.05                          # past the new one
        rt.tick(loop.t)
        self.assertIsNone(rt.owner, "which then passes in its own time")

    def test_a_hello_only_session_expires_too(self):
        """A session that never commanded has its adoption as its last activity —
        the watchdog never armed, so the idle clock runs from the handshake. The
        firmware behaves the same; without this the phantom lives forever."""
        idle_s = RT["session_idle_ms"] / 1000.0
        rt, car, loop = link()
        loop.t = 1.0
        send(rt, hello("ghost001"))
        loop.t = 1.0 + idle_s
        rt.tick(loop.t)
        self.assertEqual(rt.owner, APP, "at exactly the window it still lives")
        loop.t = 1.0 + idle_s + 0.05
        rt.tick(loop.t)
        self.assertIsNone(rt.owner, "a handshake that never commanded ages out")
        self.assertIn("ghost001", rt.dead_sids)

    def test_expiry_aborts_a_retreat_still_in_flight(self):
        """Rule 4's amendment: expiry forgets the path too, exactly as the firmware's
        rt_glue_idle calls recovery_forget() — whose liveness bump is what aborts
        recovery.c's retreat_task mid-replay. Without it a dead driver's retreat kept
        retracing after the session that started it was already gone.

        Timed so the retrace genuinely outlives the idle deadline rather than merely
        outliving the trip, and shaped so it still does under the per-segment cap
        (`CarState.SEG_MAX_MS`, recovery.h's RECOVER_SEG_MAX_MS): a long retrace has
        to be built from *many* short segments, because no single one can be credited
        with more than 250 ms. Two samples 9.5 s apart — the shape this test used
        while the mock capped only its newest segment — now collapse to 0.5 s:
        a capped tail plus one capped gap, and expiry would never find a retrace
        left to abort.

        The timeline, and why every number is where it is:

          t=0.3 … 9.9   49 commands, one every 200 ms (48 gaps, each inside the cap,
                        so each is credited in full: 48 × 0.2 s = 9.6 s)
          t=9.9         last activity — the anchor for BOTH clocks below
          t=10.25       the trip (last command + watchdog 300 ms + 50 ms). `_trip`
                        evicts breadcrumbs older than window_ms (10 s, the contract's
                        max) first, and the oldest sample is 9.95 s old here, so all
                        49 survive and the retrace replays the whole path.
          retrace       9.6 s of gaps + a 250 ms tail (capped from the 350 ms of
                        silence) = 9.85 s → still running until t=20.10
          t=19.95       the idle clock, anchored at t=9.9, runs out (10 s + 50 ms) —
                        150 ms before the retrace would have exhausted on its own,
                        which is the whole point: the branch under test is expiry
                        killing a live retrace, not a retrace ending by itself.
        """
        idle_s = RT["session_idle_ms"] / 1000.0
        rt, car, loop = link()
        ok, _ = car.apply_config("/recover", {"enabled": True, "window_ms": 10000})
        self.assertTrue(ok)
        loop.t = 0.0
        send(rt, hello("longtrip"))
        seq, loop.t = 1, 0.3
        while loop.t <= 9.9 + 1e-9:                           # 49 samples, 200 ms apart
            send(rt, cmd(seq, 0.9, 0.0))
            seq, loop.t = seq + 1, round(loop.t + 0.2, 10)
        self.assertEqual(seq - 1, 49, "48 gaps of 200 ms, each credited under the cap")
        loop.t = 9.9 + DEADLINE_S + 0.05                      # t = 10.25: the trip
        self.assertIsNotNone(rt.tick(loop.t), "the trip starts the retrace")
        self.assertTrue(car.retreating, "a wide window keeps the retrace running")
        self.assertEqual(car.history_len, 49, "the oldest sample survives the trip's own eviction")
        loop.t = 9.9 + idle_s + 0.05                          # t = 19.95: past idle
        line = rt.tick(loop.t)
        self.assertIsNone(line, "not a natural exhaustion — the retrace was still due to run")
        self.assertIsNone(rt.owner, "the session itself expired")
        self.assertIn("longtrip", rt.dead_sids)
        self.assertFalse(car.retreating, "a dead driver's retrace must not keep replaying")
        self.assertEqual(car.history_len, 0, "the path is forgotten, not just the session")
        self.assertEqual(car.command, (0.0, 0.0), "the released grant zeroes the actuator")


class TestTelemetry(Quiet):
    def test_nothing_is_pushed_without_a_session(self):
        rt, _, loop = link()
        rt.push_telemetry(loop.t)
        self.assertEqual(rt.transport.sent, [])

    def test_a_push_goes_to_the_owner_and_carries_the_live_state(self):
        rt, car, loop = link()
        send(rt, hello())
        send(rt, cmd(1, 0.5))
        rt.transport.sent.clear()
        rt.push_telemetry(loop.t)
        frame, addr, size = rt.transport.sent[-1]
        self.assertEqual(addr, APP)
        self.assertEqual(frame["ctl"], CTL_RT)
        # first read of the push consumer — fps_now answers 0 until it has a delta
        self.assertEqual(frame["rx_fps"], 0)
        self.assertLessEqual(size, RT["max_datagram"])

    def test_pushed_seq_is_gapless(self):
        rt, car, loop = link()
        send(rt, hello())
        rt.transport.sent.clear()
        for _ in range(5):
            rt.push_telemetry(loop.t)
        seqs = [f[0]["seq"] for f in rt.transport.sent]
        self.assertEqual(seqs, list(range(1, 6)))


class TestRxFps(Quiet):
    """rx_fps semantics = firmware's fps_now (telemetry.c): per-consumer delta."""

    def stream(self, rt, loop, start, count, first_seq=1):
        for k in range(count):
            loop.t = start + k * 0.1
            send(rt, cmd(first_seq + k, 0.5))

    def test_the_first_status_read_is_zero(self):
        rt, _, loop = link()
        send(rt, hello())
        self.stream(rt, loop, 1.0, 10)
        self.assertEqual(rt.rx_fps(loop.t, "status"), 0,
                         "no previous poll to delta against — the car answers 0")

    def test_a_prompt_second_read_measures_the_stream(self):
        rt, _, loop = link()
        send(rt, hello())
        rt.rx_fps(1.0, "status")                       # primes the accumulator
        self.stream(rt, loop, 1.0, 10)                 # 10 frames over 0.9 s
        self.assertEqual(rt.rx_fps(2.0, "status"), 10)

    def test_a_gap_of_ten_seconds_reads_zero(self):
        rt, _, loop = link()
        send(rt, hello())
        rt.rx_fps(1.0, "status")
        self.stream(rt, loop, 1.0, 10)
        self.assertEqual(rt.rx_fps(12.0, "status"), 0, "a stale delta is not a rate")
        self.stream(rt, loop, 12.0, 10, first_seq=11)
        self.assertEqual(rt.rx_fps(13.0, "status"), 10, "and the next prompt read works")

    def test_status_reads_do_not_perturb_the_push_consumer(self):
        rt, _, loop = link()
        send(rt, hello())
        rt.rx_fps(1.0, "push")
        self.stream(rt, loop, 1.0, 10)
        rt.rx_fps(1.5, "status")
        rt.rx_fps(1.7, "status")
        self.assertEqual(rt.rx_fps(2.0, "push"), 10,
                         "each consumer deltas against its own last read")


class TestImpairment(Quiet):
    def test_total_loss_is_a_car_that_never_hears(self):
        rt, car, _ = link(loss_pct=100.0)
        send(rt, hello())
        self.assertIsNone(rt.owner)
        self.assertEqual(rt.transport.sent, [])

    def test_a_stall_services_nothing_and_pushes_nothing(self):
        rt, car, loop = link(stall_ms=1000.0)
        loop.t = 100.0
        send(rt, hello())                    # the stall's cycle starts at this datagram
        self.assertIsNone(rt.owner)
        loop.t = 101.5                       # past the stall, inside the period
        send(rt, hello())
        self.assertEqual(rt.owner, APP)
        rt.transport.sent.clear()
        loop.t = 105.5                       # the next stall
        rt.push_telemetry(loop.t)
        self.assertEqual(rt.transport.sent, [])

    def test_the_rx_loss_pattern_replays_from_the_seed(self):
        def run():
            rt, _, _ = link(loss_pct=40.0, seed=7)
            send(rt, hello())
            return [rt.owner is not None] + [
                (send(rt, cmd(k + 1, 0.1 * k)), rt.last_seq)[1] for k in range(20)]
        self.assertEqual(run(), run())


class TestReboot(Quiet):
    def test_a_simulated_reboot_is_deaf_mute_and_forgets_the_session(self):
        """After a real flash the car reboots: telemetry gaps past the app's 3 s
        stall, the session dies, and the reconnect's hello reply carries the new
        fw. The mock's OTA used to keep the session alive through the flash, so
        the app's success detector could never fire in the simulator."""
        rt, car, loop = link()
        send(rt, hello("flasher1"))
        send(rt, cmd(1, 0.2))
        car.begin_ota(0.0)
        car.end_ota()                             # bumps car.fw
        rt.simulate_reboot(10.0)
        self.assertIsNone(rt.owner)
        self.assertIn("flasher1", rt.dead_sids)
        rt.transport.sent.clear()
        loop.t = 11.0                             # inside the quiet window
        send(rt, hello("fresh003"))
        self.assertIsNone(rt.owner, "a rebooting car hears nothing")
        self.assertEqual(rt.transport.sent, [])
        rt.push_telemetry(loop.t)
        self.assertEqual(rt.transport.sent, [], "and says nothing")
        loop.t = 10.0 + REBOOT_QUIET_S + 0.1
        send(rt, hello("fresh003"))
        self.assertEqual(rt.owner, APP, "the reconnect adopts")
        self.assertEqual(rt.transport.sent[-1][0][RT["fw_field"]], car.fw,
                         "and the hello reply carries the bumped fw")


class TestTwoPhones(Quiet):
    def test_last_hello_wins_and_nobody_is_told(self):
        """Rule 11: the audit flagged silent hijack and the ~3 s two-phone
        ownership oscillation; the spec defers the eviction notice and pins the
        behaviour instead. If a notice is ever added, this is the test to rewrite."""
        rt, car, _ = link()
        send(rt, hello("phoneaaa"), addr=APP)
        send(rt, cmd(1, 0.5), addr=APP)
        sent_before = len(rt.transport.sent)
        send(rt, hello("phonebbb"), addr=OTHER)      # B silently steals the session
        self.assertEqual(rt.owner, OTHER)
        to_a = [d for d in rt.transport.sent[sent_before:] if d[1] == APP]
        self.assertEqual(to_a, [], "the displaced phone is told nothing")
        send(rt, cmd(2, 0.9), addr=APP)              # A keeps streaming, unheard
        self.assertEqual(car.command, (0.5, 0.0), "A's frames no longer land")
        self.assertEqual(car.history_len, 0, "adoption wiped A's breadcrumbs")
        send(rt, hello("phoneaa2"), addr=APP)        # A's stall guard re-hellos
        self.assertEqual(rt.owner, APP, "and the theft works both ways, forever")


if __name__ == "__main__":
    unittest.main(verbosity=2)
