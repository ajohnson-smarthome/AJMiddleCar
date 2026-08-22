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
from rt_link import Impairment, RTLink                 # noqa: E402
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


class TestOwnedTraffic(Quiet):
    def test_a_command_drives_and_counts(self):
        rt, car, loop = link()
        send(rt, hello())
        for k in range(10):
            loop.t = k * 0.1
            send(rt, cmd(k + 1, 0.5, -0.25))
        self.assertEqual(car.command, (0.5, -0.25))
        self.assertEqual(car.ctl, CTL_RT)
        self.assertEqual(rt.rx_fps(loop.t), 10)

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
        self.assertEqual(frame["rx_fps"], 1)
        self.assertLessEqual(size, RT["max_datagram"])

    def test_pushed_seq_is_gapless(self):
        rt, car, loop = link()
        send(rt, hello())
        rt.transport.sent.clear()
        for _ in range(5):
            rt.push_telemetry(loop.t)
        seqs = [f[0]["seq"] for f in rt.transport.sent]
        self.assertEqual(seqs, list(range(1, 6)))


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


if __name__ == "__main__":
    unittest.main(verbosity=2)
