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

from generated import (CALIBRATION, DOMAINS, GROUPS, PROTO, RT, STATUS_GROUPS,   # noqa: E402
                       TELEMETRY_GROUPS)
from state import (BATTERY_ABSENT, BUS_DOWN, BUS_OK, CHIP_ID, OWNER_CALIBRATION, OWNER_CONSOLE,   # noqa: E402
                   OWNER_IDLE, OWNER_RECOVERING, OWNER_REMOTE, OWNER_SAFE_STOP, OWNER_UPDATE,
                   RADIO_EXPECTED, RADIO_MISMATCH, RADIO_OK, RADIO_UNAVAILABLE, VIDEO_IDLE,
                   VIDEO_OFF, CarState, build_number, clamp_axis, image_refusal, number,
                   parse_frame, parse_image_version, seq_is_newer, valid_seq, valid_sid)

DEADLINE_S = RT["watchdog_ms"] / 1000.0
K, T = RT["keys"], RT["types"]


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
        for bad in (b'{"proto":2,"type":"hello","session":{"a":1}}',
                    b'{"proto":2,"type":"hello","session":1234}',
                    b'{"proto":2,"type":"hello","session":""}',
                    b'{"proto":2,"type":"hello","session":"' + b"x" * 70 + b'"}',
                    b'{"proto":2,"type":"hello"}',                        # no session
                    b'{"proto":2,"type":"hello","hello":"7f3a91c2"}'):    # a key the format does not have
            self.assertIsNone(parse_frame(bad), bad)
        good = parse_frame(b'{"proto":2,"type":"hello","session":"7f3a91c2"}')
        self.assertEqual(good, {"type": "hello", "proto": 2, "session": "7f3a91c2"})

    def test_a_command_needs_both_axes_and_real_numbers(self):
        self.assertEqual(
            parse_frame(b'{"proto":2,"type":"drive","seq":3,"throttle":0.5,"turn":-0.25}'),
            {"type": "drive", "proto": 2, "seq": 3, "throttle": 0.5, "turn": -0.25})
        for bad in (b'{"proto":2,"type":"drive","seq":3,"throttle":0.5}',
                    b'{"proto":2,"type":"drive","seq":3,"turn":0.5}',
                    b'{"proto":2,"type":"drive","seq":3,"throttle":true,"turn":false}',
                    b'{"proto":2,"type":"drive","seq":3,"throttle":"0.5","turn":"0"}',
                    b'{"proto":2,"type":"drive","seq":3,"throttle":0.5,"turn":null}'):
            self.assertIsNone(parse_frame(bad), bad)

    def test_a_broken_key_drops_the_whole_datagram(self):
        for bad in (b'{"proto":2,"type":"drive","seq":-1,"throttle":0,"turn":0}',
                    b'{"proto":2,"type":"drive","seq":1.5,"throttle":0,"turn":0}',
                    b'{"proto":2,"type":"drive","seq":true,"throttle":0,"turn":0}',
                    b'{"proto":"2","type":"hello","session":"abc"}',
                    b'{"proto":1.5,"type":"drive","seq":1,"throttle":0,"turn":0}'):
            self.assertIsNone(parse_frame(bad), bad)

    def test_nothing_to_execute_is_dropped(self):
        """No type, an unknown type, or a type missing the keys it requires."""
        no_type = (b'{"proto":2,"seq":1,"throttle":0,"turn":0}', b'{}', b'[]',
                   b'not json', b'"a string"', b'')
        unknown_type = (b'{"proto":2,"type":"telemetry","seq":1}',                # car->app only
                        b'{"proto":2,"type":"hello_ack","session":"a"}',          # car->app only
                        b'{"proto":2,"type":"drove","seq":1,"throttle":0,"turn":0}')
        missing_keys = (b'{"proto":2,"type":"hello"}',
                        b'{"proto":2,"type":"drive"}',
                        b'{"proto":2,"type":"drive","seq":1}',
                        b'{"proto":2,"type":"bye"}')
        for bad in no_type + unknown_type + missing_keys:
            self.assertIsNone(parse_frame(bad), bad)

    def test_a_bare_goodbye_is_a_goodbye(self):
        """`{"proto":2,"type":"bye","seq":n}` is a complete instruction on its own.
        Axes are tolerated (the app sends them alongside `bye`) but never required."""
        frame = parse_frame(b'{"proto":2,"type":"bye","seq":1}')
        self.assertEqual(frame, {"type": "bye", "proto": 2, "seq": 1})
        with_axes = parse_frame(b'{"proto":2,"type":"bye","seq":2,"throttle":0,"turn":0}')
        self.assertEqual(with_axes,
                         {"type": "bye", "proto": 2, "seq": 2, "throttle": 0.0, "turn": 0.0})

    def test_the_command_cap_is_what_bounds_an_inbound_datagram(self):
        # max_datagram sizes a receive buffer; max_command is what the car agrees to
        # act on, and telemetry needs the difference on the way out.
        self.assertLess(RT["max_command"], RT["max_datagram"])
        base = b'{"proto":2,"type":"drive","seq":1,"throttle":0,"turn":0,"z":""}'
        pad = RT["max_command"] - len(base)
        ok = b'{"proto":2,"type":"drive","seq":1,"throttle":0,"turn":0,"z":"%s"}' % (b"x" * pad)
        over = (b'{"proto":2,"type":"drive","seq":1,"throttle":0,"turn":0,"z":"%s"}'
                % (b"x" * (pad + 1)))
        self.assertEqual(len(ok), RT["max_command"])
        self.assertIsNotNone(parse_frame(ok))
        self.assertIsNone(parse_frame(over))

    def test_a_duplicate_key_drops_the_whole_datagram(self):
        """Rule 5: the car takes the first duplicate, json.loads keeps the last —
        the only shared answer is to drop the frame on both sides."""
        self.assertIsNone(parse_frame(
            b'{"proto":2,"type":"drive","seq":9,"throttle":0.5,"turn":0,"throttle":0.9}'))
        self.assertIsNone(parse_frame(
            b'{"proto":2,"type":"hello","session":"ab","proto":2}'))

    def test_the_shared_pinned_frames(self):
        """The spec's rule-6 table, in v2 spelling. The firmware host tests
        (test_control_proto.c) pin these same bytes with these same outcomes; a
        change here without a change there is wire drift."""
        dropped = [
            b'{"proto":2,"type":"drive","seq":7,"throttle":.5,"turn":0}',      # bare mantissa
            b'{"proto":2,"type":"drive","seq":8,"throttle":+1,"turn":0}',      # leading plus
            b'{"proto":2,"type":"drive","seq":01,"throttle":0,"turn":0}',      # leading zero
            b'{"proto":2,"type":"drive","seq":12,"throttle":0.5x,"turn":0}',   # trailing junk
            (b'{"proto":2,"type":"drive","seq":9,"throttle":0.5,"turn":0,'     # duplicate key
             b'"throttle":0.9}'),
            b'{"proto":1.5,"type":"drive","seq":1,"throttle":0,"turn":0}',     # fractional proto
        ]
        for frame in dropped:
            self.assertIsNone(parse_frame(frame), frame)
        # A nested "seq" inside a sub-object must not be read as the datagram's own.
        nested = parse_frame(
            b'{"proto":2,"type":"drive","seq":1,"throttle":0.1,"turn":0.2,"extra":{"seq":9}}')
        self.assertEqual(nested,
                         {"type": "drive", "proto": 2, "seq": 1, "throttle": 0.1, "turn": 0.2})
        self.assertEqual(parse_frame(b'{"proto":1,"type":"hello","session":"7f3a91c2"}'),
                         {"type": "hello", "proto": 1, "session": "7f3a91c2"})
        self.assertEqual(parse_frame(b'{"proto":2,"type":"hello","session":"7f3a91c2"}'),
                         {"type": "hello", "proto": 2, "session": "7f3a91c2"})

    def test_a_view_is_a_hello_shaped_datagram_with_an_optional_key(self):
        frame = parse_frame(b'{"proto":2,"type":"view","session":"7f3a91c2"}')
        self.assertEqual(frame["type"], "view")
        self.assertNotIn("key", frame)
        keyed = parse_frame(b'{"proto":2,"type":"view","session":"7f3a91c2","key":true}')
        self.assertIs(keyed["key"], True)
        for bad in (b'{"proto":2,"type":"view"}',
                    b'{"proto":2,"type":"view","session":"7f3a91c2","key":1}',
                    b'{"proto":2,"type":"view","session":"7f3a91c2","key":"true"}'):
            self.assertIsNone(parse_frame(bad), bad)


class TestBuildNumber(unittest.TestCase):
    def test_the_number_after_plus(self):
        self.assertEqual(build_number("v1.0+9001"), 9001)
        self.assertEqual(build_number("v2.3+42"), 42)

    def test_a_git_suffix_is_read_past_not_rejected(self):
        """`v<semver>+<build>[-<n>-g<sha>[-dirty]]` is the contract's shape for any build
        not made from a tag. fw_build_number (device_json.h) reads the digits after the
        first `+` up to the first non-digit — 784 from `v1.0+784-dirty` — and
        `test_device_json.c` pins it; the mock used to demand the whole tail be digits
        and answer -1, which conformance then held the car to (AJM-108)."""
        self.assertEqual(build_number("v1.0+784-dirty"), 784)
        self.assertEqual(build_number("v1.0+9000-dirty-with-a-long-branch-name"), 9000)
        self.assertEqual(build_number("v1.2+9100-3-gabcdef-dirty"), 9100)

    def test_no_usable_number_is_minus_one(self):
        self.assertEqual(build_number("v1.0"), -1)
        self.assertEqual(build_number("v1.0+dirty"), -1)
        self.assertEqual(build_number("v1.0+"), -1)
        self.assertEqual(build_number("v1.0+x"), -1)
        self.assertEqual(build_number(""), -1)


class TestConfig(unittest.TestCase):
    def test_defaults_come_from_the_schema(self):
        car = CarState()
        for key, domain in DOMAINS.items():
            self.assertEqual(car.config[key], domain["defaults"], key)

    def test_recover_defaults_match_the_firmware(self):
        """The drift this mock existed with for months: off/3000 against the car's on/5000."""
        self.assertEqual(CarState().config["recovery"], {"enabled": True, "window_ms": 5000})

    def test_a_good_body_applies(self):
        car = CarState()
        ok, err = car.apply_config({"ramp": {"rise_ms": 400}})
        self.assertTrue(ok)
        self.assertIsNone(err)
        self.assertEqual(car.config["ramp"], {"rise_ms": 400})

    def test_a_rejected_body_names_the_field_and_changes_nothing(self):
        car = CarState()
        before = dict(car.config["ramp"])
        ok, err = car.apply_config({"ramp": {"rise_ms": 99999}})
        self.assertFalse(ok)
        self.assertEqual(err[0], "out_of_range")
        self.assertEqual(err[1], "ramp.rise_ms")
        self.assertEqual(car.config["ramp"], before)

    def test_gear_ratio_is_stored_as_the_scaled_integer(self):
        car = CarState()
        ok, _ = car.apply_config({"wheel": {"diameter_mm": 65, "encoder_ppr": 11,
                                            "gear_ratio": 9.0, "quadrature": 4}})
        self.assertTrue(ok)
        self.assertEqual(car.config["wheel"]["gear_ratio"], 900)
        self.assertEqual(car.config_wire()["wheel"]["gear_ratio"], 9.0)
        ok, _ = car.apply_config({"wheel": {"diameter_mm": 65, "encoder_ppr": 11,
                                            "gear_ratio": 9.125, "quadrature": 4}})
        self.assertTrue(ok)
        self.assertEqual(car.config["wheel"]["gear_ratio"], 913)

    def test_config_wire_round_trips_every_domain(self):
        car = CarState()
        wire = car.config_wire()
        self.assertEqual(set(wire), set(DOMAINS))
        for key, domain in DOMAINS.items():
            for f in domain["fields"]:
                want = domain["defaults"][f["name"]] / f["scale"] if f["type"] == "fixed" \
                    else domain["defaults"][f["name"]]
                self.assertEqual(wire[key][f["name"]], want, f"{key}.{f['name']}")

    def test_half_the_body_being_bad_applies_neither_domain(self):
        """Two domains, the second bad: the first must not have been applied either —
        the car validates the whole body before writing any of it."""
        car = CarState()
        before_ramp = dict(car.config["ramp"])
        before_trim = dict(car.config["trim"])
        ok, err = car.apply_config({"ramp": {"rise_ms": 500},
                                    "trim": {"balance_pct": 9999}})
        self.assertFalse(ok)
        self.assertEqual(err[0], "out_of_range")
        self.assertEqual(car.config["ramp"], before_ramp)
        self.assertEqual(car.config["trim"], before_trim)


class TestBreadcrumbs(unittest.TestCase):
    def test_history_is_bounded_by_the_window(self):
        car = CarState(now=0.0)
        window_s = car.config["recovery"]["window_ms"] / 1000.0
        stream(car, 0.5, 0.0, 0.0, int(window_s * RT["command_hz"]) * 2)
        self.assertLessEqual(car.history_len, window_s * RT["command_hz"] + 1)

    def test_shrinking_the_window_shortens_the_history(self):
        car = CarState(now=0.0)
        last = stream(car, 0.5, 0.0, 0.0, 60)          # 6 s at 10 Hz
        self.assertGreater(car.history_len, 20)
        ok, _ = car.apply_config({"recovery": {"enabled": True, "window_ms": 1000}})
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
        self.assertEqual(car.ctl, OWNER_RECOVERING)
        self.assertEqual(car.command, (-0.6, 0.2))

    def test_a_still_history_stops_instead(self):
        car = CarState(now=0.0)
        last = stream(car, 0.01, -0.01, 0.0, 20)     # inside recovery.c's MOVE_EPS
        line = car.tick(last + DEADLINE_S + 0.01)
        self.assertIn("nothing to retrace", line)
        self.assertFalse(car.retreating)
        self.assertEqual(car.command, (0.0, 0.0))
        self.assertEqual(car.ctl, OWNER_IDLE)

    def test_disabled_recovery_stops_instead(self):
        car = CarState(now=0.0)
        car.apply_config({"recovery": {"enabled": False, "window_ms": 5000}})
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
        self.assertEqual(car.ctl, OWNER_REMOTE)
        self.assertEqual(car.command, (0.4, 0.0))

    def test_every_replay_segment_is_capped_not_just_the_tail(self):
        """recovery.h's RECOVER_SEG_MAX_MS caps EVERY segment, not only the newest.

        `recovery_seg_ms` is applied to each `dur` in retreat_task's loop, so dead air
        inside the path is never credited to the replay. This history has a 4.9 s
        stutter in the middle — a stream that froze and came back, which is exactly what
        a phone locking its screen mid-drive looks like. Capping only the tail (what the
        mock did while it mirrored a `TAIL_MS` that the firmware had already replaced)
        made the retrace 5.41 s long: 0.31 s of tail plus the whole 5.1 s span. Capped
        per segment it is 0.70 s — 0.25 tail + 0.1 + 0.25 (the stutter, capped) + 0.1 —
        which is the ground the car actually covered.
        """
        car = CarState(now=0.0)
        ok, _ = car.apply_config({"recovery": {"enabled": True, "window_ms": 8000}})
        self.assertTrue(ok)
        for ts in (0.0, 0.1, 5.0, 5.1):
            car.note_command(0.9, 0.0, ts)
        trip = 5.1 + DEADLINE_S + 0.01                   # t = 5.41
        self.assertIn("retracing", car.tick(trip))
        self.assertIsNone(car.tick(trip + 0.60), "0.70 s of replay is not over at 0.60")
        self.assertIn("exhausted", car.tick(trip + 0.75),
                      "the 4.9 s stutter is worth 250 ms of retrace, not 4.9 s of it")
        self.assertFalse(car.retreating)
        self.assertEqual(car.command, (0.0, 0.0))

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
        self.assertEqual(car.ctl, OWNER_IDLE)

    def test_a_trip_consumes_the_path_it_retraces(self):
        """recovery.c's `snapshot_consume`: the crumbs a trip takes are cleared with it.

        The retreat's own motion is never recorded, so a ring that survives the trip
        replays, on the next loss inside window_ms, ground the first retreat already
        covered — on top of it. The firmware fixed that in 3551f74; the mock kept
        evicting by window alone, so a second dropout reversed the whole 5 s.
        """
        car = CarState(now=0.0)
        last = stream(car, 0.5, 0.0, 0.0, 20)              # 2 s forward, then silence
        trip = last + DEADLINE_S + 0.01
        self.assertIn("retracing 20 samples", car.tick(trip))
        self.assertEqual(car.history_len, 0, "the path was consumed at the trip")
        now = trip
        while now < 4.4:                                   # 0.25 + 1.9 s of replay is over by 4.36
            now += 0.02
            end = car.tick(now)
            if end:
                self.assertIn("exhausted", end)
        self.assertFalse(car.retreating)
        # The driver is back at 4.5 s — inside the 5 s window of the first run — and
        # drives 1 s more before losing the link again.
        last = stream(car, 0.5, 0.0, 4.5, 10)
        trip = last + DEADLINE_S + 0.01
        self.assertIn("retracing 10 samples", car.tick(trip),
                      "only the ground driven after the first retreat")
        self.assertIsNone(car.tick(trip + 1.10), "0.25 + 0.9 s of replay is not over at 1.10")
        self.assertIn("exhausted", car.tick(trip + 1.20))

    def test_a_second_trip_retraces_nothing_the_first_already_did(self):
        car = CarState(now=0.0)
        last = stream(car, 0.5, 0.0, 0.0, 20)
        trip = last + DEADLINE_S + 0.01
        self.assertIn("retracing 20 samples", car.tick(trip))
        car.note_command(0.0, 0.0, trip + 0.1)             # the driver is back, holding still
        self.assertFalse(car.retreating)
        line = car.tick(trip + 0.1 + DEADLINE_S + 0.01)    # ... and drops out again
        self.assertIn("nothing to retrace", line)
        self.assertFalse(car.retreating)
        self.assertEqual(car.command, (0.0, 0.0))
        self.assertEqual(car.wdt_trips, 2)


class TestGoodbye(unittest.TestCase):
    def test_bye_stops_without_a_trip(self):
        """Stop, release SAFE, clear the path, disarm — the plan's four steps.

        `ctl` is `idle` afterwards on purpose: SAFE is released immediately, because a
        sticky grant would also lock out OTA, the calibration wizard and the console
        until an app reconnected. What suppresses the retreat is the cleared history,
        asserted separately.
        """
        car = CarState(now=0.0)
        last = stream(car, 0.9, 0.0, 0.0, 20)
        car.note_bye(last + 0.05)
        self.assertEqual(car.command, (0.0, 0.0))
        self.assertEqual(car.ctl, OWNER_IDLE)
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
        self.assertEqual(car.ctl, OWNER_IDLE)
        car.note_command(0.3, 0.0, 0.3)
        self.assertEqual(car.ctl, OWNER_REMOTE)

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
        self.assertEqual(car.ctl, OWNER_CALIBRATION)
        car.note_command(0.5, 0.0, 0.1)      # the app keeps streaming through the pulse
        self.assertEqual(car.ctl, OWNER_CALIBRATION)
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
        self.assertEqual(car.ctl, OWNER_CALIBRATION)
        car.tick(CarState.CALIB_HOLD_MS / 1000.0 + 0.01)
        self.assertEqual(car.ctl, OWNER_IDLE)

    def test_ota_refuses_a_spin(self):
        car = CarState(now=0.0)
        car.begin_ota(0.0)
        self.assertEqual(car.ctl, OWNER_UPDATE)
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
        self.assertEqual(car.ctl, OWNER_IDLE)
        self.assertTrue(car.begin_spin(0.1, 0, 1))
        self.assertEqual(car.ctl, OWNER_CALIBRATION)

    def test_a_goodbye_during_an_ota_leaves_the_flash_its_grant(self):
        """Rule 2: bye must not steal-and-release a sticky hold. Before this rule
        the SAFE grab displaced OTA and released to IDLE, so anything could drive
        the motors for the rest of the flash — and the restart could land mid-drive."""
        car = CarState(now=0.0)
        stream(car, 0.5, 0.0, 0.0, 5)
        car.begin_ota(1.0)
        self.assertFalse(car.note_bye(1.1), "no stop was issued; the flash holds")
        self.assertEqual(car.ctl, OWNER_UPDATE, "the goodbye did not touch the grant")
        self.assertEqual(car.history_len, 0, "the retreat is still suppressed")
        self.assertFalse(car.begin_spin(1.2, 0, 1), "still flashing; a spin is refused")
        for k in range(50):
            self.assertIsNone(car.tick(1.2 + k * 0.05))
        self.assertEqual(car.wdt_trips, 0, "the watchdog was still disarmed")
        car.end_ota()
        self.assertEqual(car.ctl, OWNER_IDLE, "not wedged: the flash's own end releases")
        self.assertTrue(car.begin_spin(5.0, 0, 1))

    def test_a_goodbye_during_a_spin_leaves_the_pulse_alone(self):
        car = CarState(now=0.0)
        self.assertTrue(car.begin_spin(0.0, 1, 1))
        self.assertFalse(car.note_bye(0.1))
        self.assertEqual(car.ctl, OWNER_CALIBRATION)
        self.assertEqual(car.command, (1.0, 0.0), "the pulse is not flattened")
        car.tick(CarState.CALIB_HOLD_MS / 1000.0 + 0.01)
        self.assertEqual(car.ctl, OWNER_IDLE, "the pulse lapses on its own schedule")
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
        self.assertEqual(car.ctl, OWNER_IDLE)
        self.assertEqual(car.command, (0.0, 0.0))

    def test_a_new_session_during_a_retreat_leaves_the_wheels_stopped(self):
        car = CarState(now=0.0)
        last = stream(car, 0.8, 0.0, 0.0, 20)
        car.tick(last + DEADLINE_S + 0.01)
        self.assertTrue(car.retreating)
        car.adopt_session(last + 1.0)
        self.assertEqual(car.ctl, OWNER_IDLE)
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
        self.assertIsNotNone(line, "the flash does not disarm the watchdog")
        self.assertIn("refused", line)
        self.assertFalse(car.retreating)
        self.assertEqual(car.ctl, OWNER_UPDATE)

        # The usual sequence: the app's send loop does not stop for an OTA, so the
        # stream keeps arriving (refused, but re-arming the watchdog) and then stops.
        car = CarState(now=0.0)
        last = stream(car, 0.8, 0.0, 0.0, 20)
        car.begin_ota(last + 0.01)
        now = stream(car, 0.8, 0.0, last + 0.1, 5)
        self.assertEqual(car.ctl, OWNER_UPDATE)
        line = car.tick(now + DEADLINE_S + 0.01)
        self.assertIn("refused", line)
        self.assertFalse(car.retreating)
        self.assertEqual(car.ctl, OWNER_UPDATE)
        self.assertEqual(car.command, (0.0, 0.0), "the flash holds the wheels at zero")

    def test_a_trip_with_auto_return_off_still_goes_through_the_arbiter(self):
        """recovery.c's disabled path is `car_stop(RECOVER)`, which OTA refuses."""
        car = CarState(now=0.0)
        car.apply_config({"recovery": {"enabled": False, "window_ms": 5000}})
        last = stream(car, 0.8, 0.0, 0.0, 20)
        car.begin_spin(last + 0.01, 0, 1)        # a pulse starts as the link dies
        now = stream(car, 0.8, 0.0, last + 0.05, 2)
        line = car.tick(now + DEADLINE_S + 0.01)
        self.assertIn("auto-return off", line)
        self.assertEqual(car.ctl, OWNER_CALIBRATION)
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
        self.assertEqual(car.ctl, OWNER_CALIBRATION)
        self.assertEqual(car.command, (1.0, 0.0))

    def test_ota_leaves_the_watchdog_armed(self):
        """ota_api.c takes the actuator through car_stop(LINK_SRC_OTA) and touches
        nothing else: rt_link's silence check keeps running under the flash. A pult
        that stops streaming as the flash begins trips it after rt.watchdog_ms —
        `link.timeouts` grows by one, the retrace is refused by the sticky grant, and
        the wheels stay at the flash's zero. The mock used to disarm on begin_ota, so
        the same pult saw no timeout in the simulator and one on the car."""
        car = CarState(now=0.0)
        last = stream(car, 0.5, 0.0, 0.0, 20)
        car.begin_ota(last + 0.01)
        self.assertIsNone(car.tick(last + DEADLINE_S), "not yet past the deadline")
        line = car.tick(last + DEADLINE_S + 0.01)
        self.assertIsNotNone(line, "silence under a flash is still a trip")
        self.assertIn("refused", line)
        self.assertEqual(car.wdt_trips, 1)
        self.assertEqual(car.telemetry(0)["link"]["timeouts"], 1)
        self.assertEqual(car.ctl, OWNER_UPDATE, "the flash keeps its grant")
        self.assertFalse(car.retreating)
        self.assertEqual(car.command, (0.0, 0.0))
        self.assertEqual(car.history_len, 0, "the trip consumed the path even though refused")
        for k in range(1, 20):
            self.assertIsNone(car.tick(last + DEADLINE_S + 0.01 + k * 0.05))
        self.assertEqual(car.wdt_trips, 1, "one trip per silence, under a flash as anywhere")

    def test_end_spin_releases_the_pulse_and_only_the_pulse(self):
        """calib_api.c sleeps the pulse out and releases before replying, so the
        200 lands after the wheel has stopped — the wizard's next step assumes it."""
        car = CarState(now=0.0)
        self.assertTrue(car.begin_spin(0.0, 1, 1))
        car.end_spin()
        self.assertEqual(car.ctl, OWNER_IDLE)
        self.assertEqual(car.command, (0.0, 0.0), "the release is the stop")
        car.begin_ota(1.0)
        car.end_spin()
        self.assertEqual(car.ctl, OWNER_UPDATE, "end_spin never releases someone else's grant")

    def test_ota_goes_through_the_arbiter(self):
        """ota_api.c takes the actuator through the checked car_stop(LINK_SRC_OTA)
        and answers 409 when refused; the mock used to seize it unconditionally,
        so the simulator never exhibited that rejection."""
        car = CarState(now=0.0)
        # SAFE is only ever held transiently by note_bye, so stage it directly.
        self.assertTrue(car._take(OWNER_SAFE_STOP, 0.0, None))
        self.assertFalse(car.begin_ota(0.1), "SAFE outranks a flash, as on the car")
        car._release(OWNER_SAFE_STOP)
        self.assertTrue(car.begin_ota(0.2))
        fw = car.fw
        car.end_ota(flashed=False)
        self.assertEqual(car.ctl, OWNER_IDLE, "an aborted flash releases the grant")
        self.assertEqual(car.fw, fw, "and does not bump the build")


class TestCalibration(unittest.TestCase):
    def _wheels(self, pairs=(0, 1, 2, 3), inverted=(False, True, False, True)):
        corners = CALIBRATION["corners"]
        return [{"corner": c, "pair": p, "inverted": inv}
                for c, p, inv in zip(corners, pairs, inverted)]

    def test_a_valid_table_is_accepted(self):
        car = CarState()
        self.assertFalse(car.calibrated)
        ok, err = car.save_calibration(self._wheels())
        self.assertTrue(ok)
        self.assertIsNone(err)
        self.assertTrue(car.calibrated)

    def test_calibration_table_is_reported_fl_fr_rl_rr(self):
        car = CarState()
        car.save_calibration(
            self._wheels(pairs=(2, 0, 3, 1), inverted=(True, False, False, True)))
        table = car.calibration_table()
        self.assertEqual([w["corner"] for w in table], CALIBRATION["corners"])
        self.assertEqual([w["pair"] for w in table], [2, 0, 3, 1])
        self.assertEqual([w["inverted"] for w in table], [True, False, False, True])

    def test_calibration_table_is_empty_until_saved(self):
        self.assertEqual(CarState().calibration_table(), [])

    def test_a_repeated_corner_is_refused(self):
        car = CarState()
        wheels = self._wheels()
        wheels[2] = dict(wheels[0])          # front_left again, at index 2
        ok, err = car.save_calibration(wheels)
        self.assertFalse(ok)
        self.assertEqual(err[0], "not_allowed")
        self.assertEqual(err[1], "wheels[2]")
        self.assertFalse(car.calibrated)

    def test_an_unknown_corner_is_refused(self):
        car = CarState()
        wheels = self._wheels()
        wheels[0]["corner"] = "middle"
        ok, err = car.save_calibration(wheels)
        self.assertFalse(ok)
        self.assertEqual(err[0], "not_allowed")
        self.assertFalse(car.calibrated)

    def test_a_repeated_pair_is_refused(self):
        car = CarState()
        wheels = self._wheels(pairs=(0, 0, 1, 2))
        ok, err = car.save_calibration(wheels)
        self.assertFalse(ok)
        self.assertEqual(err[0], "not_allowed")
        self.assertFalse(car.calibrated)

    def test_a_pair_out_of_range_is_refused(self):
        car = CarState()
        wheels = self._wheels(pairs=(0, 1, 2, 9))
        ok, err = car.save_calibration(wheels)
        self.assertFalse(ok)
        self.assertEqual(err[0], "out_of_range")

    def test_an_unknown_wheel_field_is_refused(self):
        car = CarState()
        wheels = self._wheels()
        wheels[0]["extra"] = True
        ok, err = car.save_calibration(wheels)
        self.assertFalse(ok)
        self.assertEqual(err[0], "unknown_field")

    def test_wrong_shapes_are_refused(self):
        car = CarState()
        rest = self._wheels()[1:]
        for wheels in (
            [],
            self._wheels()[:3],
            "wheels",
            [{"corner": "front_left", "pair": "0", "inverted": False}] + rest,   # string pair
            [{"corner": "front_left", "pair": True, "inverted": False}] + rest,  # bool pair
            [{"corner": "front_left", "pair": 0.5, "inverted": False}] + rest,   # fractional pair
            [{"corner": "front_left", "pair": 0, "inverted": "no"}] + rest,      # non-bool inverted
            [{"corner": 1, "pair": 0, "inverted": False}] + rest,               # non-string corner
            [{"corner": "front_left", "pair": 10**400, "inverted": False}] + rest,     # huge integer
            [{"corner": "front_left", "pair": float("inf"), "inverted": False}] + rest,  # non-finite
        ):
            ok, err = car.save_calibration(wheels)
            self.assertFalse(ok, wheels)
            self.assertIsNotNone(err)
        self.assertFalse(car.calibrated)

    def test_integral_floats_are_numbers(self):
        """cJSON sees 1.0 as a number with valueint 1; so does the car."""
        car = CarState()
        wheels = [{"corner": c, "pair": float(p), "inverted": bool(p % 2)}
                  for c, p in zip(CALIBRATION["corners"], range(4))]
        ok, _ = car.save_calibration(wheels)
        self.assertTrue(ok)


class TestOwnershipVocabulary(unittest.TestCase):
    def test_owner_names_come_from_the_schema(self):
        from state import PRIORITY
        self.assertEqual(list(PRIORITY), GROUPS["motors"]["fields"][2]["values"])

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
        self.assertTrue(seen <= set(GROUPS["motors"]["fields"][2]["values"]), seen)
        self.assertIn(OWNER_RECOVERING, seen)

    def test_the_symbols_spell_the_wire_values(self):
        self.assertEqual((OWNER_IDLE, OWNER_RECOVERING, OWNER_CONSOLE, OWNER_REMOTE,
                          OWNER_UPDATE, OWNER_SAFE_STOP),
                         ("idle", "recovering", "console", "remote", "update", "safe_stop"))


class TestTelemetry(unittest.TestCase):
    def test_it_carries_the_envelope_and_exactly_the_schema_groups(self):
        car = CarState(now=0.0)
        frame = car.telemetry(10)
        self.assertEqual(frame["proto"], PROTO)
        self.assertEqual(frame["type"], T["telemetry"])
        self.assertIn("seq", frame)
        self.assertEqual(set(frame) - {"proto", "type", "seq"}, set(TELEMETRY_GROUPS))
        for g in TELEMETRY_GROUPS:
            names = [f["name"] for f in GROUPS[g]["fields"]]
            self.assertEqual(list(frame[g]), names, g)

    def test_status_groups_are_the_same_four_without_the_envelope(self):
        car = CarState(now=0.0)
        groups = car.status_groups(10)
        self.assertEqual(set(groups), set(TELEMETRY_GROUPS))
        for key in ("proto", "type", "seq"):
            self.assertNotIn(key, groups)

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
        self.assertEqual(car.telemetry(10)["motors"]["owner"], OWNER_REMOTE)
        car.tick(last + DEADLINE_S + 0.01)
        frame = car.telemetry(0)
        self.assertEqual(frame["motors"]["owner"], OWNER_RECOVERING)
        self.assertEqual(frame["link"]["timeouts"], 1)
        car.set_bus_ok(False)
        self.assertEqual(car.telemetry(0)["motors"]["bus"], "down")

    def test_rssi_zero_is_null_on_the_wire(self):
        car = CarState(now=0.0)
        car.rssi = 0
        self.assertIsNone(car.telemetry(10)["link"]["rssi_dbm"])
        car.rssi = -58
        self.assertEqual(car.telemetry(10)["link"]["rssi_dbm"], -58)

    def test_uptime_follows_the_clock_it_was_given(self):
        car = CarState(now=100.0)
        car.tick(142.0)
        self.assertEqual(car.telemetry(0)["system"]["uptime_s"], 42)


class TestDegradation(unittest.TestCase):
    """The states a car can boot into and the app has to show (AJM-116): each is a flag on
    the mock, off by default, and each shows wherever the car shows it — `car/status-and-
    version`, `car/actuator-arbiter`, `car/video-stream`, `car/config`, `car/calibration`."""

    def good_table(self):
        k = CALIBRATION["keys"]
        return [{k["corner"]: c, k["pair"]: i, k["inverted"]: False}
                for i, c in enumerate(CALIBRATION["corners"])]

    def test_every_degradation_is_off_by_default(self):
        car = CarState(now=0.0)
        groups = car.status_groups(0, STATUS_GROUPS)
        self.assertEqual(groups["motors"]["bus"], BUS_OK)
        self.assertEqual(groups["video"]["state"], VIDEO_IDLE)
        self.assertEqual(groups["radio"], {"fw": RADIO_EXPECTED, "expected": RADIO_EXPECTED,
                                           "state": RADIO_OK})
        self.assertIs(groups["storage"]["reset_at_boot"], False)
        self.assertEqual(car.write_fail, set())

    def test_status_walks_all_seven_groups_in_the_contracts_order(self):
        """`radio` and `storage` are /status-only; built by walking the schema like the
        other five, so a field added to the contract cannot go missing on the wire."""
        groups = CarState(now=0.0).status_groups(0, STATUS_GROUPS)
        self.assertEqual(list(groups), STATUS_GROUPS)
        for g in STATUS_GROUPS:
            self.assertEqual(list(groups[g]), [f["name"] for f in GROUPS[g]["fields"]], g)

    def test_battery_is_absent_with_null_numbers_in_telemetry_and_status(self):
        """`car/battery-monitor` → «Монитора нет — absent, числа null»: the group is on
        the wire in both places, last, and reads the same in each. No pack model yet — the
        monitor's absence is the one state the wire has for a car with nothing to report."""
        car = CarState(now=0.0)
        absent = {"voltage_mv": None, "current_ma": None, "power_mw": None, "soc_pct": None,
                  "state": BATTERY_ABSENT}
        self.assertEqual(car.telemetry(0)["battery"], absent)
        self.assertEqual(car.status_groups(0, STATUS_GROUPS)["battery"], absent)
        self.assertEqual(list(car.telemetry(0))[-1], "battery")
        self.assertEqual(list(car.status_groups(0, STATUS_GROUPS))[-1], "battery")

    def test_bus_down_is_the_same_word_in_telemetry_and_status(self):
        car = CarState(now=0.0, bus_ok=False)
        self.assertEqual(car.telemetry(0)["motors"]["bus"], BUS_DOWN)
        self.assertEqual(car.status_groups(0, STATUS_GROUPS)["motors"]["bus"], BUS_DOWN)

    def test_bus_down_takes_commands_but_nothing_moves_and_nothing_is_retraced(self):
        """`car/actuator-arbiter`: the arbiter accepts, the wheels do not turn. The stream
        still arms the watchdog; the breadcrumbs — the path the car drove — stay empty, so
        the trip stops instead of retracing ground never covered."""
        car = CarState(now=0.0, bus_ok=False)
        last = stream(car, 0.7, 0.2, 0.0, 10)
        self.assertEqual(car.ctl, OWNER_REMOTE, "the arbiter granted the stream")
        self.assertTrue(car.armed)
        self.assertEqual(car.command, (0.0, 0.0), "nothing reaches the wheels")
        self.assertEqual(car.history_len, 0, "no breadcrumb for a command that moved nothing")
        line = car.tick(last + DEADLINE_S + 0.01)
        self.assertIn("stopped", line)
        self.assertFalse(car.retreating)
        self.assertEqual(car.ctl, OWNER_IDLE)
        self.assertEqual(car.wdt_trips, 1)

    def test_bus_down_refuses_a_spin_without_a_grant(self):
        """calib_spin.h asks the bus BEFORE the arbiter: a down bus is a 409 with no grant
        taken, no hold, nothing to release — the wizard must not advance on a wheel that
        did not turn (AJM-100)."""
        car = CarState(now=0.0, bus_ok=False)
        self.assertFalse(car.begin_spin(0.0, 0, 1))
        self.assertEqual(car.ctl, OWNER_IDLE)
        self.assertEqual(car.command, (0.0, 0.0))

    def test_bus_down_still_takes_a_flash(self):
        """`car/status-and-version`: a car with `bus: down` stays reachable and updatable."""
        car = CarState(now=0.0, bus_ok=False)
        self.assertTrue(car.begin_ota(0.0))
        self.assertEqual(car.ctl, OWNER_UPDATE)

    def test_camera_off_reports_off_with_zero_counters(self):
        car = CarState(now=0.0, camera=False)
        video = car.telemetry(0)["video"]
        self.assertEqual(video["state"], VIDEO_OFF)
        self.assertEqual((video["fps"], video["kbps"], video["dropped"]), (0, 0, 0))

    def test_radio_words_follow_status_api(self):
        """status_api.c's radio_state_word: no answer is `unavailable` with `fw` null — the
        only case it is null; another version is `mismatch` with that version in `fw`."""
        radio = CarState(now=0.0, radio=RADIO_MISMATCH).status_groups(0, STATUS_GROUPS)["radio"]
        self.assertEqual(radio["state"], RADIO_MISMATCH)
        self.assertEqual(radio["expected"], RADIO_EXPECTED)
        self.assertIsNotNone(radio["fw"])
        self.assertNotEqual(radio["fw"], radio["expected"])
        radio = CarState(now=0.0, radio=RADIO_UNAVAILABLE).status_groups(0, STATUS_GROUPS)["radio"]
        self.assertEqual(radio["state"], RADIO_UNAVAILABLE)
        self.assertIsNone(radio["fw"])
        self.assertEqual(radio["expected"], RADIO_EXPECTED)

    def test_reset_at_boot_is_true_over_the_contracts_defaults(self):
        """`car/status-and-version`: the one signal that the settings and the calibration
        were wiped — every domain at its default, `motors.calibrated` false."""
        car = CarState(now=0.0, nvs_wiped=True)
        groups = car.status_groups(0, STATUS_GROUPS)
        self.assertIs(groups["storage"]["reset_at_boot"], True)
        self.assertIs(groups["motors"]["calibrated"], False)
        self.assertEqual(car.config, {key: d["defaults"] for key, d in DOMAINS.items()})
        self.assertEqual(car.calibration_table(), [])

    def test_write_fail_refuses_the_domains_first_write_once_and_rolls_it_back(self):
        """`car/config` → «Отказ применить или сохранить — честный»: 500 `write_failed`
        with the domain's key, the domain back at its previous values; the next write goes
        through, so a retry in the app is not refused forever."""
        car = CarState(now=0.0, write_fail=["ramp"])
        before = dict(car.config["ramp"])
        self.assertEqual(car.apply_config({"ramp": {"rise_ms": before["rise_ms"] + 100}}),
                         (False, ("write_failed", "ramp", "could not persist")))
        self.assertEqual(car.config["ramp"], before, "rolled back")
        self.assertEqual(car.write_fail, set(), "one refusal, consumed")
        self.assertEqual(car.apply_config({"ramp": {"rise_ms": before["rise_ms"] + 100}}),
                         (True, None))
        self.assertEqual(car.config["ramp"]["rise_ms"], before["rise_ms"] + 100)

    def test_write_fail_leaves_the_domains_before_it_applied(self):
        """cfg_api.c applies domain by domain in the contract's order: a 500 on the second
        does not undo the first — the next GET is the only way to learn what stuck."""
        car = CarState(now=0.0, write_fail=["video"])
        video = dict(car.config_wire()["video"], bitrate_kbps=1000)
        ok, err = car.apply_config({"video": video, "ramp": {"rise_ms": 50}})
        self.assertFalse(ok)
        self.assertEqual(err[1], "video")
        self.assertEqual(car.config["ramp"]["rise_ms"], 50, "ramp, earlier in order, stays applied")
        self.assertEqual(car.config["video"], DOMAINS["video"]["defaults"], "video rolled back")

    def test_write_fail_is_not_spent_on_an_unchanged_write(self):
        """The car's NVS skips a write whose value is already stored (cfg_json.c), so it
        cannot fail; the arm waits for a write that changes something."""
        car = CarState(now=0.0, write_fail=["ramp"])
        self.assertEqual(car.apply_config({"ramp": car.config_wire()["ramp"]}), (True, None))
        self.assertEqual(car.write_fail, {"ramp"})

    def test_write_fail_calibration_refuses_the_first_table_once_and_keeps_the_old(self):
        """`car/calibration` → «Хранилище отказало»: 500 `write_failed`, `field` wheels;
        the table the car drives by and `motors.calibrated` unchanged."""
        car = CarState(now=0.0, write_fail=["calibration"])
        self.assertEqual(car.save_calibration(self.good_table()),
                         (False, ("write_failed", CALIBRATION["keys"]["wheels"], "could not persist")))
        self.assertFalse(car.calibrated)
        self.assertEqual(car.calibration_table(), [])
        self.assertEqual(car.save_calibration(self.good_table()), (True, None))
        self.assertTrue(car.calibrated)

    def test_write_fail_calibration_is_not_spent_on_the_same_table(self):
        """calibration.c: saving the table already stored does not rewrite the flash."""
        car = CarState(now=0.0)
        self.assertEqual(car.save_calibration(self.good_table()), (True, None))
        car.write_fail.add("calibration")
        self.assertEqual(car.save_calibration(self.good_table()), (True, None))
        self.assertEqual(car.write_fail, {"calibration"})


def synthetic_image(version=b"v9.9+123", magic=0xABCD5432, first=0xE9, size=4096, chip_id=CHIP_ID):
    """The least image the mock's /ota flashes: 0xE9 header with this board's chip_id at
    its offset 12, esp_app_desc_t at 32 (image header 24 + segment header 8), magic word
    first, version[32] at its offset 16. `chip_id=0x0009` is the dongle's (ESP32-S3)."""
    img = bytearray(size)
    img[0] = first
    img[12:14] = chip_id.to_bytes(2, "little")
    img[32:36] = magic.to_bytes(4, "little")
    img[48:48 + len(version)] = version
    return bytes(img)


class TestImageVersion(unittest.TestCase):
    def test_a_real_layout_yields_its_version(self):
        self.assertEqual(parse_image_version(synthetic_image()), "v9.9+123")

    def test_garbage_yields_none(self):
        """A 0xE9 blob without the app-desc magic is not an app image. /ota refuses it
        before asking (`image_refusal`); this is the parser's own answer."""
        self.assertIsNone(parse_image_version(b"\xe9" + b"\x00" * 8191))
        self.assertIsNone(parse_image_version(b"\x00" * 8192))
        self.assertIsNone(parse_image_version(b"\xe9short"))

    def test_end_ota_prefers_the_parsed_version(self):
        car = CarState(now=0.0)
        car.begin_ota(0.0)
        car.end_ota(version="v2.0+700")
        self.assertEqual(car.fw, "v2.0+700")

    def test_end_ota_without_a_version_still_bumps(self):
        car = CarState(now=0.0)
        old = car.fw
        car.begin_ota(0.0)
        car.end_ota()
        self.assertNotEqual(car.fw, old)

    def test_rollback_and_nvs_wiped_default_false(self):
        car = CarState(now=0.0)
        self.assertFalse(car.rollback)
        self.assertFalse(car.nvs_wiped)


class TestImageRefusal(unittest.TestCase):
    """What POST /ota refuses as `not_firmware`, decided from the header — the checks
    esp_ota_write and esp_ota_end make of it on the car (`car/ota-and-rollback`: an image
    that fails verification as a whole is `not_firmware`, the running image untouched)."""

    def test_this_boards_image_is_not_refused(self):
        self.assertIsNone(image_refusal(synthetic_image()))

    def test_another_boards_chip_id_is_refused(self):
        """The dongle's image: 0xE9, chip_id 9 (ESP32-S3), a descriptor carrying the same
        release tag. The mock used to flash it and report the tag (AJM-105); the car's
        esp_ota_end fails it as a whole — "image invalid", not "not an ESP image"."""
        dongle = synthetic_image(b"v9.9+7777-dongle", chip_id=0x0009)
        self.assertEqual(image_refusal(dongle), "image invalid")

    def test_a_missing_app_descriptor_is_refused(self):
        """Right chip, no descriptor magic at segment 0: esp_image_verify fails it too."""
        self.assertEqual(image_refusal(synthetic_image(magic=0)), "image invalid")
        self.assertEqual(image_refusal(b"\xe9" + b"\x00" * 8191), "image invalid")

    def test_the_wrong_first_byte_is_not_an_esp_image(self):
        """esp_ota_write's first-block check, the one refusal the mock always had."""
        self.assertEqual(image_refusal(b"\x00" * 4096), "not an ESP image")
        self.assertEqual(image_refusal(synthetic_image(first=0xE8)), "not an ESP image")


if __name__ == "__main__":
    unittest.main(verbosity=2)
