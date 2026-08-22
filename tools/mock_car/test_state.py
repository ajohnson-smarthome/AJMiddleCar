#!/usr/bin/env python3
"""Host tests for the mock car's state. Stdlib only — no aiohttp, no sockets, no sleeping.

The clock is an argument, so a watchdog deadline and a five-second retreat are tested by
passing the times they happen at. What is asserted here is behaviour the mock was missing
entirely until the UDP cutover: the watchdog, the retreat, the goodbye, and defaults that
match the car rather than the mock's own history.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from generated import CTL_VALUES, DOMAINS, RT, TELEMETRY_FIELDS   # noqa: E402
from state import (CTL_CALIB, CTL_NONE, CTL_OTA, CTL_RECOVER,     # noqa: E402
                   CTL_RT, CTL_SAFE, CarState, clamp_axis, number, parse_frame,
                   seq_is_newer, valid_seq, valid_sid)

DEADLINE_S = RT["watchdog_ms"] / 1000.0


def stream(car, t, y, start, count, hz=None):
    """Feed `count` frames at the contract's command rate, starting at `start`."""
    step = 1.0 / (hz or RT["command_hz"])
    now = start
    for _ in range(count):
        car.note_command(t, y, now)
        now += step
    return now - step        # the timestamp of the last frame


class TestSequence(unittest.TestCase):
    def test_newer_only(self):
        self.assertTrue(seq_is_newer(2, 1))
        self.assertFalse(seq_is_newer(1, 1))
        self.assertFalse(seq_is_newer(1, 2))

    def test_wraparound(self):
        top = 0xFFFFFFFF
        self.assertTrue(seq_is_newer(0, top))
        self.assertTrue(seq_is_newer(5, top - 2))
        self.assertFalse(seq_is_newer(top - 2, 5))

    def test_shape(self):
        self.assertTrue(valid_seq(0))
        self.assertTrue(valid_seq(0xFFFFFFFF))
        for bad in (True, -1, 0x100000000, 1.5, "7", None):
            self.assertFalse(valid_seq(bad), bad)


class TestWireShapes(unittest.TestCase):
    """The rules control_proto.c applies, which are stricter than JSON's."""

    def test_a_number_is_not_a_boolean_or_a_string(self):
        # float(True) is 1.0 and float("0.5") is 0.5, which is how a mock drives on
        # frames the car drops. The car's tokeniser never sees past the first quote.
        for bad in (True, False, "0.5", "1", None, [], {}):
            self.assertIsNone(number(bad), bad)
            self.assertIsNone(clamp_axis(bad), bad)
        self.assertEqual(number(0.5), 0.5)
        self.assertEqual(number(1), 1.0)

    def test_sid_matches_parse_sid(self):
        for good in ("7f3a91c2", "a", "0" * 15):
            self.assertTrue(valid_sid(good), good)
        for bad in ("", "0" * 16, "7f3a-91c2", 'a"b', "sid é", 1234, True, None,
                    {"a": 1}, ["a"]):
            self.assertFalse(valid_sid(bad), bad)

    def test_a_hello_the_car_would_reject_is_not_a_session(self):
        for bad in (b'{"proto":1,"hello":{"a":1}}', b'{"proto":1,"hello":1}',
                    b'{"proto":1,"hello":""}', b'{"proto":1,"hello":"' + b"x" * 70 + b'"}'):
            self.assertIsNone(parse_frame(bad), bad)
        good = parse_frame(b'{"proto":1,"hello":"7f3a91c2"}')
        self.assertEqual(good, {"proto": 1, "hello": "7f3a91c2"})

    def test_a_command_needs_both_axes_and_real_numbers(self):
        self.assertEqual(parse_frame(b'{"seq":3,"t":0.5,"y":-0.25}'),
                         {"seq": 3, "t": 0.5, "y": -0.25})
        for bad in (b'{"seq":3,"t":0.5}', b'{"seq":3,"y":0.5}',
                    b'{"seq":3,"t":true,"y":false}', b'{"seq":3,"t":"0.5","y":"0"}',
                    b'{"seq":3,"t":0.5,"y":null}'):
            self.assertIsNone(parse_frame(bad), bad)

    def test_a_broken_key_drops_the_whole_datagram(self):
        for bad in (b'{"seq":-1,"t":0,"y":0}', b'{"seq":1.5,"t":0,"y":0}',
                    b'{"seq":true,"t":0,"y":0}', b'{"proto":"1","hello":"abc"}',
                    b'{"seq":1,"t":0,"y":0,"bye":"yes"}'):
            self.assertIsNone(parse_frame(bad), bad)

    def test_nothing_to_act_on_is_dropped(self):
        # No hello, no axes, no goodbye. `{"bye":0}` says nothing and carries nothing
        # else, so it is in this list rather than in the one below.
        for bad in (b'{"seq":1}', b'{"seq":1,"bye":0}', b'{"seq":1,"bye":false}',
                    b'{}', b'[]', b'not json', b'"a string"', b''):
            self.assertIsNone(parse_frame(bad), bad)

    def test_a_bare_goodbye_is_a_goodbye(self):
        """`{"seq":n,"bye":1}` is a complete instruction, and the car acts on one.

        control_proto.c: `if (!f.has_hello && !f.has_ty && !f.bye) return -1;` — the
        goodbye counts on its own. The app happens to send `t`/`y` with its `bye`, but
        the mock's job is to answer what the car answers for the same bytes, not what
        today's client happens to send.
        """
        frame = parse_frame(b'{"seq":1,"bye":1}')
        self.assertIsNotNone(frame)
        self.assertIs(frame["bye"], True)
        self.assertNotIn("t", frame)

    def test_bye_is_a_goodbye_however_json_spells_yes(self):
        self.assertIs(parse_frame(b'{"seq":1,"t":0,"y":0,"bye":1}')["bye"], True)
        self.assertIs(parse_frame(b'{"seq":1,"t":0,"y":0,"bye":true}')["bye"], True)
        self.assertIs(parse_frame(b'{"seq":1,"t":0,"y":0,"bye":0}')["bye"], False)
        self.assertIs(parse_frame(b'{"seq":1,"t":0,"y":0,"bye":false}')["bye"], False)

    def test_the_command_cap_is_what_bounds_an_inbound_datagram(self):
        # max_datagram sizes a receive buffer; max_command is what the car agrees to
        # act on, and telemetry needs the difference on the way out.
        self.assertLess(RT["max_command"], RT["max_datagram"])
        pad = RT["max_command"] - len('{"seq":1,"t":0,"y":0,"z":""}')
        self.assertIsNotNone(parse_frame(b'{"seq":1,"t":0,"y":0,"z":"%s"}' % (b"x" * pad)))
        self.assertIsNone(parse_frame(b'{"seq":1,"t":0,"y":0,"z":"%s"}' % (b"x" * (pad + 1))))

    def test_a_duplicate_key_drops_the_whole_datagram(self):
        """Rule 5: the car takes the first duplicate, json.loads keeps the last —
        the only shared answer is to drop the frame on both sides."""
        self.assertIsNone(parse_frame(b'{"seq":9,"t":0.5,"y":0,"t":0.9}'))
        self.assertIsNone(parse_frame(b'{"proto":1,"hello":"ab","proto":1}'))

    def test_the_shared_pinned_frames(self):
        """The spec's rule-6 table, verbatim. The firmware host tests pin the same
        eight bytes with the same outcomes; a change here without a change there
        is wire drift."""
        dropped = [
            b'{"seq":5,"junk":{"t":0.9},"y":0.5}',   # no top-level t
            b'{"seq":7,"t":.5,"y":0}',               # bare . mantissa
            b'{"seq":8,"t":+1,"y":0}',               # leading +
            b'{"seq":9,"t":0.5,"y":0,"t":0.9}',      # duplicate key
            b'{"proto":1.5,"hello":"abcd1234"}',     # fractional proto
        ]
        for frame in dropped:
            self.assertIsNone(parse_frame(frame), frame)
        self.assertEqual(parse_frame(b'{"seq":10,"t":0.50,"y":-0.25}'),
                         {"seq": 10, "t": 0.50, "y": -0.25})
        self.assertEqual(parse_frame(b'{"proto":1,"hello":"7f3a91c2"}'),
                         {"proto": 1, "hello": "7f3a91c2"})
        self.assertEqual(parse_frame(b'{"proto":2,"hello":"7f3a91c2"}'),
                         {"proto": 2, "hello": "7f3a91c2"})


class TestConfig(unittest.TestCase):
    def test_defaults_come_from_the_schema(self):
        car = CarState()
        for path, domain in DOMAINS.items():
            self.assertEqual(car.config[path], domain["defaults"], path)

    def test_recover_defaults_match_the_firmware(self):
        """The drift this mock existed with for months: off/3000 against the car's on/5000."""
        self.assertEqual(CarState().config["/recover"], {"enabled": True, "window_ms": 5000})

    def test_a_good_body_applies(self):
        car = CarState()
        ok, err = car.apply_config("/ramp", {"ramp_ms": 1200})
        self.assertTrue(ok)
        self.assertEqual(err, "")
        self.assertEqual(car.config["/ramp"], {"ramp_ms": 1200})

    def test_a_rejected_body_applies_nothing(self):
        car = CarState()
        before = dict(car.config["/wheel"])
        # Three good fields and one out of range: the record must survive intact.
        bad = dict(before)
        bad["diameter_mm"] = 9000
        ok, err = car.apply_config("/wheel", bad)
        self.assertFalse(ok)
        self.assertIn("diameter_mm", err)
        self.assertEqual(car.config["/wheel"], before)

    def test_a_missing_field_is_not_a_partial_write(self):
        car = CarState()
        before = dict(car.config["/dims"])
        ok, _ = car.apply_config("/dims", {"track_mm": 200})
        self.assertFalse(ok)
        self.assertEqual(car.config["/dims"], before)

    def test_unknown_endpoint(self):
        ok, err = CarState().apply_config("/nope", {})
        self.assertFalse(ok)
        self.assertIn("/nope", err)

    def test_field_of_names_the_offending_key(self):
        car = CarState()
        _, err = car.apply_config("/wheel", {"diameter_mm": 65, "ppr": 11,
                                             "gear_x100": 2100, "quad": 3})
        self.assertEqual(car.field_of("/wheel", err), "quad")
        _, err = car.apply_config("/dims", {"track_mm": 130})
        self.assertEqual(car.field_of("/dims", err), "wheelbase_mm")


class TestBreadcrumbs(unittest.TestCase):
    def test_history_is_bounded_by_the_window(self):
        car = CarState(now=0.0)
        window_s = car.config["/recover"]["window_ms"] / 1000.0
        stream(car, 0.5, 0.0, 0.0, int(window_s * RT["command_hz"]) * 2)
        self.assertLessEqual(car.history_len, window_s * RT["command_hz"] + 1)

    def test_shrinking_the_window_shortens_the_history(self):
        car = CarState(now=0.0)
        last = stream(car, 0.5, 0.0, 0.0, 60)          # 6 s at 10 Hz
        self.assertGreater(car.history_len, 20)
        ok, _ = car.apply_config("/recover", {"enabled": True, "window_ms": 1000})
        self.assertTrue(ok)
        car.note_command(0.5, 0.0, last + 0.1)
        self.assertLessEqual(car.history_len, 12)      # 1 s at 10 Hz, plus the new frame

    def test_a_refused_command_leaves_no_breadcrumb(self):
        """The retreat retraces where the wheels went, not what the app asked for."""
        car = CarState(now=0.0)
        car.begin_spin(0.0, 0, 1)                    # the wizard outranks the stream
        for k in range(3):
            car.note_command(0.9, 0.0, 0.05 + k * 0.1)
        self.assertEqual(car.history_len, 0)
        after = CarState.CALIB_HOLD_MS / 1000.0 + 0.05
        car.note_command(0.9, 0.0, after)            # the pulse has lapsed
        self.assertEqual(car.history_len, 1)

    def test_bye_clears_the_path_behind_it(self):
        """The empty history is what suppresses the retreat, not a sticky grant.

        The plan's "Session lifecycle" step 3: `any_motion()` over an empty history is
        false, so even a later trip stops instead of retracing. Leaving the samples in
        place and relying on a held SAFE grant instead is what locked OTA, the wizard
        and the console out of the car until an app reconnected.
        """
        car = CarState(now=0.0)
        last = stream(car, 0.9, 0.0, 0.0, 5)
        self.assertEqual(car.history_len, 5)
        self.assertTrue(car.note_bye(last + 0.05), "SAFE outranks everything; the stop lands")
        self.assertEqual(car.history_len, 0)

    def test_a_malformed_axis_changes_nothing(self):
        car = CarState(now=0.0)
        self.assertFalse(car.note_command(float("nan"), 0.0, 1.0))
        self.assertFalse(car.note_command(0.0, "left", 1.0))
        self.assertEqual(car.history_len, 0)
        self.assertIsNone(car.tick(1.0 + DEADLINE_S * 2))   # nothing armed the watchdog

    def test_axes_are_clamped_not_rejected(self):
        car = CarState(now=0.0)
        self.assertTrue(car.note_command(1.5, -9.0, 0.0))
        self.assertEqual(car.command, (1.0, -1.0))
        self.assertIsNone(clamp_axis(float("inf")))


class TestWatchdog(unittest.TestCase):
    def test_it_trips_once_and_then_stays_quiet(self):
        car = CarState(now=0.0)
        last = stream(car, 0.5, 0.0, 0.0, 20)
        self.assertIsNone(car.tick(last + DEADLINE_S))         # not yet past the deadline
        line = car.tick(last + DEADLINE_S + 0.01)
        self.assertIsNotNone(line)
        self.assertIn("retracing", line)
        self.assertEqual(car.wdt_trips, 1)
        for k in range(1, 20):
            car.tick(last + DEADLINE_S + 0.01 + k * 0.02)
        self.assertEqual(car.wdt_trips, 1, "a trip must not repeat while the link stays down")

    def test_traffic_re_arms_it(self):
        car = CarState(now=0.0)
        last = stream(car, 0.5, 0.0, 0.0, 20)
        car.tick(last + DEADLINE_S + 0.01)
        self.assertEqual(car.wdt_trips, 1)
        resumed = stream(car, 0.5, 0.0, last + 1.0, 20)
        car.tick(resumed + DEADLINE_S + 0.01)
        self.assertEqual(car.wdt_trips, 2)

    def test_silence_without_traffic_never_trips(self):
        car = CarState(now=0.0)
        for k in range(200):
            self.assertIsNone(car.tick(k * 0.05))
        self.assertEqual(car.wdt_trips, 0)


class TestRetreat(unittest.TestCase):
    def test_motion_retreats_in_reverse(self):
        car = CarState(now=0.0)
        last = stream(car, 0.6, -0.2, 0.0, 20)
        car.tick(last + DEADLINE_S + 0.01)
        self.assertTrue(car.retreating)
        self.assertEqual(car.ctl, CTL_RECOVER)
        self.assertEqual(car.command, (-0.6, 0.2))

    def test_a_still_history_stops_instead(self):
        car = CarState(now=0.0)
        last = stream(car, 0.01, -0.01, 0.0, 20)     # inside recovery.c's MOVE_EPS
        line = car.tick(last + DEADLINE_S + 0.01)
        self.assertIn("nothing to retrace", line)
        self.assertFalse(car.retreating)
        self.assertEqual(car.command, (0.0, 0.0))
        self.assertEqual(car.ctl, CTL_NONE)

    def test_disabled_recovery_stops_instead(self):
        car = CarState(now=0.0)
        car.apply_config("/recover", {"enabled": False, "window_ms": 5000})
        last = stream(car, 0.8, 0.0, 0.0, 20)
        line = car.tick(last + DEADLINE_S + 0.01)
        self.assertIn("auto-return off", line)
        self.assertFalse(car.retreating)
        self.assertEqual(car.command, (0.0, 0.0))
        self.assertEqual(car.wdt_trips, 1, "the trip still counts; only the response differs")

    def test_a_returning_frame_ends_the_retreat(self):
        car = CarState(now=0.0)
        last = stream(car, 0.6, 0.0, 0.0, 20)
        car.tick(last + DEADLINE_S + 0.01)
        self.assertTrue(car.retreating)
        car.note_command(0.4, 0.0, last + DEADLINE_S + 0.2)
        self.assertFalse(car.retreating)
        self.assertEqual(car.ctl, CTL_RT)
        self.assertEqual(car.command, (0.4, 0.0))

    def test_it_ends_on_its_own_when_the_history_runs_out(self):
        car = CarState(now=0.0)
        last = stream(car, 0.6, 0.0, 0.0, 20)        # 1.9 s of history
        car.tick(last + DEADLINE_S + 0.01)
        end, now = None, last + DEADLINE_S + 0.01
        for _ in range(400):
            now += 0.02
            end = car.tick(now) or end
        self.assertIsNotNone(end)
        self.assertIn("exhausted", end)
        self.assertFalse(car.retreating)
        self.assertEqual(car.command, (0.0, 0.0))
        self.assertEqual(car.ctl, CTL_NONE)


class TestGoodbye(unittest.TestCase):
    def test_bye_stops_without_a_trip(self):
        """Stop, release SAFE, clear the path, disarm — the plan's four steps.

        `ctl` is `none` afterwards on purpose: SAFE is released immediately, because a
        sticky grant would also lock out OTA, the calibration wizard and the console
        until an app reconnected. What suppresses the retreat is the cleared history,
        asserted separately.
        """
        car = CarState(now=0.0)
        last = stream(car, 0.9, 0.0, 0.0, 20)
        car.note_bye(last + 0.05)
        self.assertEqual(car.command, (0.0, 0.0))
        self.assertEqual(car.ctl, CTL_NONE)
        self.assertEqual(car.history_len, 0)
        for k in range(50):
            self.assertIsNone(car.tick(last + 0.05 + k * 0.05))
        self.assertEqual(car.wdt_trips, 0)
        self.assertFalse(car.retreating)

    def test_a_goodbye_leaves_nothing_to_retrace(self):
        """A session that says goodbye and comes back must not reverse the old path.

        Without the clear, the 2 s of forward driving before the goodbye is still in the
        ring, and the next trip — a genuine one, in the *new* session — retraces it.
        """
        car = CarState(now=0.0)
        last = stream(car, 0.9, 0.0, 0.0, 20)
        car.note_bye(last + 0.05)
        car.adopt_session(last + 0.1)
        car.note_command(0.0, 0.0, last + 0.2)          # arms the watchdog, adds a still crumb
        line = car.tick(last + 0.2 + DEADLINE_S + 0.01)
        self.assertIn("nothing to retrace", line)
        self.assertFalse(car.retreating)
        self.assertEqual(car.command, (0.0, 0.0))

    def test_a_new_session_releases_the_stop(self):
        car = CarState(now=0.0)
        car.note_command(0.5, 0.0, 0.0)
        car.note_bye(0.1)
        car.adopt_session(0.2)
        self.assertEqual(car.ctl, CTL_NONE)
        car.note_command(0.3, 0.0, 0.3)
        self.assertEqual(car.ctl, CTL_RT)

    def test_adopting_forgets_the_previous_drivers_path(self):
        """Step 2 of adoption. A new session has no path to retrace, and retreating
        along the last driver's is worse than not retreating at all — this is the case a
        goodbye never happened in: the app was killed, the car retreated, and the next
        session inherits a ring full of someone else's driving."""
        car = CarState(now=0.0)
        stream(car, 0.9, 0.0, 0.0, 20)
        self.assertGreater(car.history_len, 10)
        car.adopt_session(5.0)
        self.assertEqual(car.history_len, 0)

    def test_adoption_leaves_the_watchdog_disarmed(self):
        """Step 4: it arms on the first accepted *command*, which is what it measures.

        Arming on the handshake trips a session whose first command is still in flight —
        the app repeats `hello` until it is answered and only then starts its send loop,
        so two lost replies at 10% loss are enough to spend the whole deadline in the
        handshake.
        """
        car = CarState(now=0.0)
        car.adopt_session(0.0)
        for k in range(100):                        # 5 s of silence after the hello
            self.assertIsNone(car.tick(k * 0.05))
        self.assertEqual(car.wdt_trips, 0)
        car.note_command(0.5, 0.0, 5.0)             # the first command arms it
        self.assertIsNotNone(car.tick(5.0 + DEADLINE_S + 0.01))
        self.assertEqual(car.wdt_trips, 1)


class TestActuatorOwnership(unittest.TestCase):
    def test_a_spin_outranks_a_live_stream(self):
        car = CarState(now=0.0)
        car.note_command(0.5, 0.0, 0.0)
        self.assertTrue(car.begin_spin(0.05, 2, 1))
        self.assertEqual(car.ctl, CTL_CALIB)
        car.note_command(0.5, 0.0, 0.1)      # the app keeps streaming through the pulse
        self.assertEqual(car.ctl, CTL_CALIB)
        self.assertEqual(car.command, (1.0, 0.0))

    def test_the_stream_still_feeds_the_watchdog_through_a_spin(self):
        """Ten presses of Spin during a live stream must leave wdt_trips unchanged."""
        car = CarState(now=0.0)
        now = 0.0
        for _ in range(10):
            car.begin_spin(now, 0, 1)
            for _ in range(10):              # 1 s of stream, longer than the pulse
                car.note_command(0.0, 0.0, now)
                car.tick(now)
                now += 1.0 / RT["command_hz"]
        self.assertEqual(car.wdt_trips, 0)

    def test_a_pulse_lapses_on_its_own(self):
        car = CarState(now=0.0)
        car.begin_spin(0.0, 1, 0)
        self.assertEqual(car.ctl, CTL_CALIB)
        car.tick(CarState.CALIB_HOLD_MS / 1000.0 + 0.01)
        self.assertEqual(car.ctl, CTL_NONE)

    def test_ota_refuses_a_spin(self):
        car = CarState(now=0.0)
        car.begin_ota(0.0)
        self.assertEqual(car.ctl, CTL_OTA)
        self.assertFalse(car.begin_spin(0.1, 0, 1))
        car.end_ota()
        self.assertTrue(car.begin_spin(0.2, 0, 1))

    def test_a_goodbye_leaves_the_wizard_free_to_spin(self):
        """SAFE is released immediately, so a backgrounded app cannot wedge the car.

        The plan's step 2, and the reason it is not a sticky grant: held SAFE outranks
        OTA, `calib` and the console, so an app that says goodbye and is then killed
        would leave the car unflashable and unbenchable until someone pulled the
        battery.
        """
        car = CarState(now=0.0)
        car.note_bye(0.0)
        self.assertEqual(car.ctl, CTL_NONE)
        self.assertTrue(car.begin_spin(0.1, 0, 1))
        self.assertEqual(car.ctl, CTL_CALIB)

    def test_a_goodbye_during_an_ota_leaves_the_flash_its_grant(self):
        """Rule 2: bye must not steal-and-release a sticky hold. Before this rule
        the SAFE grab displaced OTA and released to NONE, so anything could drive
        the motors for the rest of the flash — and the restart could land mid-drive."""
        car = CarState(now=0.0)
        stream(car, 0.5, 0.0, 0.0, 5)
        car.begin_ota(1.0)
        self.assertFalse(car.note_bye(1.1), "no stop was issued; the flash holds")
        self.assertEqual(car.ctl, CTL_OTA, "the goodbye did not touch the grant")
        self.assertEqual(car.history_len, 0, "the retreat is still suppressed")
        self.assertFalse(car.begin_spin(1.2, 0, 1), "still flashing; a spin is refused")
        for k in range(50):
            self.assertIsNone(car.tick(1.2 + k * 0.05))
        self.assertEqual(car.wdt_trips, 0, "the watchdog was still disarmed")
        car.end_ota()
        self.assertEqual(car.ctl, CTL_NONE, "not wedged: the flash's own end releases")
        self.assertTrue(car.begin_spin(5.0, 0, 1))

    def test_a_goodbye_during_a_spin_leaves_the_pulse_alone(self):
        car = CarState(now=0.0)
        self.assertTrue(car.begin_spin(0.0, 1, 1))
        self.assertFalse(car.note_bye(0.1))
        self.assertEqual(car.ctl, CTL_CALIB)
        self.assertEqual(car.command, (1.0, 0.0), "the pulse is not flattened")
        car.tick(CarState.CALIB_HOLD_MS / 1000.0 + 0.01)
        self.assertEqual(car.ctl, CTL_NONE, "the pulse lapses on its own schedule")
        self.assertEqual(car.command, (0.0, 0.0))

    def test_ota_bumps_the_build(self):
        car = CarState(fw="v1.0+9000", now=0.0)
        car.begin_ota(0.0)
        car.end_ota()
        self.assertEqual(car.fw, "v1.0+9001")

    def test_a_lapsed_grant_zeroes_the_command(self):
        """`ctl` and "the wheels are stopped" are one fact on the car; make them one here.

        link.c says it twice — link_release memsets the target, and the actuator task
        zeroes on a lapsed grant. Without it a calibration pulse that lapses leaves the
        mock at full throttle with `ctl` reporting nobody.
        """
        car = CarState(now=0.0)
        car.begin_spin(0.0, 1, 1)
        self.assertEqual(car.command, (1.0, 0.0))
        car.tick(5.0)
        self.assertEqual(car.ctl, CTL_NONE)
        self.assertEqual(car.command, (0.0, 0.0))

    def test_a_new_session_during_a_retreat_leaves_the_wheels_stopped(self):
        car = CarState(now=0.0)
        last = stream(car, 0.8, 0.0, 0.0, 20)
        car.tick(last + DEADLINE_S + 0.01)
        self.assertTrue(car.retreating)
        car.adopt_session(last + 1.0)
        self.assertEqual(car.ctl, CTL_NONE)
        self.assertEqual(car.command, (0.0, 0.0))

    def test_a_retrace_refused_by_the_actuator_does_not_drive(self):
        """recovery.c aborts the replay the first time car_drive is refused.

        Driving anyway is the "motors moving while this side reports they are held"
        case: an OTA owns the actuator, and the mock reverses the car under it.
        """
        car = CarState(now=0.0)
        last = stream(car, 0.8, 0.0, 0.0, 20)
        car.begin_ota(last + 0.01)
        line = car.tick(last + DEADLINE_S + 0.5)
        self.assertIsNone(line, "an OTA silences the watchdog entirely")

        # The real sequence: the app's send loop does not stop for an OTA, so the
        # stream keeps arriving (refused, but re-arming the watchdog) and then stops.
        car = CarState(now=0.0)
        last = stream(car, 0.8, 0.0, 0.0, 20)
        car.begin_ota(last + 0.01)
        now = stream(car, 0.8, 0.0, last + 0.1, 5)
        self.assertEqual(car.ctl, CTL_OTA)
        line = car.tick(now + DEADLINE_S + 0.01)
        self.assertIn("refused", line)
        self.assertFalse(car.retreating)
        self.assertEqual(car.ctl, CTL_OTA)
        self.assertEqual(car.command, (0.0, 0.0), "the flash holds the wheels at zero")

    def test_a_trip_with_auto_return_off_still_goes_through_the_arbiter(self):
        """recovery.c's disabled path is `car_stop(RECOVER)`, which OTA refuses."""
        car = CarState(now=0.0)
        car.apply_config("/recover", {"enabled": False, "window_ms": 5000})
        last = stream(car, 0.8, 0.0, 0.0, 20)
        car.begin_spin(last + 0.01, 0, 1)        # a pulse starts as the link dies
        now = stream(car, 0.8, 0.0, last + 0.05, 2)
        line = car.tick(now + DEADLINE_S + 0.01)
        self.assertIn("auto-return off", line)
        self.assertEqual(car.ctl, CTL_CALIB)
        self.assertEqual(car.command, (1.0, 0.0), "the pulse is not flattened by the trip")

    def test_a_spin_during_a_retreat_ends_it(self):
        car = CarState(now=0.0)
        last = stream(car, 0.8, 0.0, 0.0, 20)
        car.tick(last + DEADLINE_S + 0.01)
        self.assertTrue(car.retreating)
        self.assertTrue(car.begin_spin(last + DEADLINE_S + 0.02, 2, 1))
        self.assertFalse(car.retreating, "the retreat was outranked, so it is over")
        self.assertEqual(car.command, (1.0, 0.0))
        # The retreat's own deadline must not zero the pulse halfway through.
        car.tick(last + DEADLINE_S + 0.03)
        self.assertEqual(car.ctl, CTL_CALIB)
        self.assertEqual(car.command, (1.0, 0.0))

    def test_ota_silences_the_watchdog(self):
        car = CarState(now=0.0)
        last = stream(car, 0.5, 0.0, 0.0, 20)
        car.begin_ota(last + 0.01)
        self.assertIsNone(car.tick(last + 1.0))
        self.assertEqual(car.wdt_trips, 0)

    def test_end_spin_releases_the_pulse_and_only_the_pulse(self):
        """calib_api.c sleeps the pulse out and releases before replying, so the
        200 lands after the wheel has stopped — the wizard's next step assumes it."""
        car = CarState(now=0.0)
        self.assertTrue(car.begin_spin(0.0, 1, 1))
        car.end_spin()
        self.assertEqual(car.ctl, CTL_NONE)
        self.assertEqual(car.command, (0.0, 0.0), "the release is the stop")
        car.begin_ota(1.0)
        car.end_spin()
        self.assertEqual(car.ctl, CTL_OTA, "end_spin never releases someone else's grant")

    def test_ota_goes_through_the_arbiter(self):
        """ota_api.c takes the actuator through the checked car_stop(LINK_SRC_OTA)
        and answers 500 when refused; the mock used to seize it unconditionally,
        so the simulator never exhibited that 500."""
        car = CarState(now=0.0)
        # SAFE is only ever held transiently by note_bye, so stage it directly.
        self.assertTrue(car._take(CTL_SAFE, 0.0, None))
        self.assertFalse(car.begin_ota(0.1), "SAFE outranks a flash, as on the car")
        car._release(CTL_SAFE)
        self.assertTrue(car.begin_ota(0.2))
        fw = car.fw
        car.end_ota(flashed=False)
        self.assertEqual(car.ctl, CTL_NONE, "an aborted flash releases the grant")
        self.assertEqual(car.fw, fw, "and does not bump the build")


class TestCalibration(unittest.TestCase):
    def test_a_valid_table_is_accepted(self):
        car = CarState()
        self.assertFalse(car.calibrated)
        wheels = [{"pair": p, "sign": s} for p, s in zip((0, 1, 2, 3), (1, -1, 1, -1))]
        self.assertTrue(car.save_calibration(wheels))
        self.assertTrue(car.calibrated)

    def test_invalid_tables_are_refused(self):
        car = CarState()
        for wheels in ([],
                       [{"pair": 0, "sign": 1}] * 4,                     # not unique
                       [{"pair": p, "sign": 0} for p in range(4)],       # sign not ±1
                       [{"pair": p} for p in range(4)],                  # missing key
                       "wheels"):
            self.assertFalse(car.save_calibration(wheels), wheels)
        self.assertFalse(car.calibrated)


class TestOwnershipVocabulary(unittest.TestCase):
    def test_ctl_names_come_from_the_schema(self):
        from state import PRIORITY
        self.assertEqual(list(PRIORITY), CTL_VALUES)

    def test_every_reported_owner_is_in_the_vocabulary(self):
        car = CarState(now=0.0)
        seen = {car.ctl}
        car.note_command(0.5, 0.0, 0.0)
        seen.add(car.ctl)
        car.begin_spin(0.1, 0, 1)
        seen.add(car.ctl)
        car.begin_ota(1.0)
        seen.add(car.ctl)
        car.end_ota()
        seen.add(car.ctl)
        last = stream(car, 0.8, 0.0, 2.0, 20)
        car.tick(last + DEADLINE_S + 0.01)
        seen.add(car.ctl)
        self.assertTrue(seen <= set(CTL_VALUES), seen)
        self.assertIn(CTL_RECOVER, seen)


class TestTelemetry(unittest.TestCase):
    def test_it_carries_exactly_the_schema_fields(self):
        car = CarState(now=0.0)
        frame = car.telemetry(10)
        self.assertEqual(list(frame), [f["name"] for f in TELEMETRY_FIELDS])
        types = {"int": int, "bool": bool, "str": str}
        for f in TELEMETRY_FIELDS:
            self.assertIsInstance(frame[f["name"]], types[f["type"]], f["name"])

    def test_seq_is_monotonic(self):
        car = CarState(now=0.0)
        seqs = [car.telemetry(10)["seq"] for _ in range(5)]
        self.assertEqual(seqs, sorted(set(seqs)))

    def test_only_a_push_advances_the_pushed_seq(self):
        """A /status poll must not make the 5 Hz stream skip a number.

        telemetry.c keeps a counter per consumer for exactly this reason: the app
        measures loss by the gaps in `seq`, and a second reader perturbing the count
        turns a 1 Hz poll into one dropped frame a second.
        """
        car = CarState(now=0.0)
        self.assertEqual(car.telemetry(10)["seq"], 1)
        for _ in range(3):
            self.assertEqual(car.telemetry(10, bump=False)["seq"], 1)
        self.assertEqual(car.telemetry(10)["seq"], 2)

    def test_it_reports_the_live_state(self):
        car = CarState(now=0.0)
        last = stream(car, 0.7, 0.0, 0.0, 20)
        self.assertEqual(car.telemetry(10)["ctl"], CTL_RT)
        car.tick(last + DEADLINE_S + 0.01)
        frame = car.telemetry(0)
        self.assertEqual(frame["ctl"], CTL_RECOVER)
        self.assertEqual(frame["wdt_trips"], 1)
        car.set_bus_ok(False)
        self.assertFalse(car.telemetry(0)["bus_ok"])

    def test_uptime_follows_the_clock_it_was_given(self):
        car = CarState(now=100.0)
        car.tick(142.0)
        self.assertEqual(car.telemetry(0)["uptime_s"], 42)


if __name__ == "__main__":
    unittest.main(verbosity=2)
