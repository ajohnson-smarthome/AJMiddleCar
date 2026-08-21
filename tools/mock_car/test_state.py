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

from generated import DOMAINS, RT, TELEMETRY_FIELDS   # noqa: E402
from state import CarState, clamp_axis, seq_is_newer, valid_seq   # noqa: E402

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

    def test_bye_leaves_the_car_at_rest_in_the_history(self):
        car = CarState(now=0.0)
        last = stream(car, 0.9, 0.0, 0.0, 5)
        self.assertEqual(car.history_len, 5)
        car.note_bye(last + 0.05)
        self.assertEqual(car.history_len, 6, "the goodbye is a breadcrumb of its own")

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
        self.assertEqual(car.ctl, "recover")
        self.assertEqual(car.command, (-0.6, 0.2))

    def test_a_still_history_stops_instead(self):
        car = CarState(now=0.0)
        last = stream(car, 0.01, -0.01, 0.0, 20)     # inside recovery.c's MOVE_EPS
        line = car.tick(last + DEADLINE_S + 0.01)
        self.assertIn("nothing to retrace", line)
        self.assertFalse(car.retreating)
        self.assertEqual(car.command, (0.0, 0.0))
        self.assertEqual(car.ctl, "none")

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
        self.assertEqual(car.ctl, "rt")
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
        self.assertEqual(car.ctl, "none")


class TestGoodbye(unittest.TestCase):
    def test_bye_stops_without_a_trip(self):
        car = CarState(now=0.0)
        last = stream(car, 0.9, 0.0, 0.0, 20)
        car.note_bye(last + 0.05)
        self.assertEqual(car.command, (0.0, 0.0))
        self.assertEqual(car.ctl, "safe")
        for k in range(50):
            self.assertIsNone(car.tick(last + 0.05 + k * 0.05))
        self.assertEqual(car.wdt_trips, 0)
        self.assertFalse(car.retreating)

    def test_a_new_session_releases_the_stop(self):
        car = CarState(now=0.0)
        car.note_command(0.5, 0.0, 0.0)
        car.note_bye(0.1)
        car.adopt_session(0.2)
        self.assertEqual(car.ctl, "none")
        car.note_command(0.3, 0.0, 0.3)
        self.assertEqual(car.ctl, "rt")


class TestActuatorOwnership(unittest.TestCase):
    def test_a_spin_outranks_a_live_stream(self):
        car = CarState(now=0.0)
        car.note_command(0.5, 0.0, 0.0)
        self.assertTrue(car.begin_spin(0.05, 2, 1))
        self.assertEqual(car.ctl, "calib")
        car.note_command(0.5, 0.0, 0.1)      # the app keeps streaming through the pulse
        self.assertEqual(car.ctl, "calib")
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
        self.assertEqual(car.ctl, "calib")
        car.tick(CarState.CALIB_HOLD_MS / 1000.0 + 0.01)
        self.assertEqual(car.ctl, "none")

    def test_ota_and_safe_refuse_a_spin(self):
        car = CarState(now=0.0)
        car.begin_ota(0.0)
        self.assertEqual(car.ctl, "ota")
        self.assertFalse(car.begin_spin(0.1, 0, 1))
        car.end_ota()
        self.assertTrue(car.begin_spin(0.2, 0, 1))

        car = CarState(now=0.0)
        car.note_bye(0.0)
        self.assertFalse(car.begin_spin(0.1, 0, 1))

    def test_ota_bumps_the_build(self):
        car = CarState(fw="v1.0+9000", now=0.0)
        car.begin_ota(0.0)
        car.end_ota()
        self.assertEqual(car.fw, "v1.0+9001")

    def test_ota_silences_the_watchdog(self):
        car = CarState(now=0.0)
        last = stream(car, 0.5, 0.0, 0.0, 20)
        car.begin_ota(last + 0.01)
        self.assertIsNone(car.tick(last + 1.0))
        self.assertEqual(car.wdt_trips, 0)


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

    def test_it_reports_the_live_state(self):
        car = CarState(now=0.0)
        last = stream(car, 0.7, 0.0, 0.0, 20)
        self.assertEqual(car.telemetry(10)["ctl"], "rt")
        car.tick(last + DEADLINE_S + 0.01)
        frame = car.telemetry(0)
        self.assertEqual(frame["ctl"], "recover")
        self.assertEqual(frame["wdt_trips"], 1)
        car.set_bus_ok(False)
        self.assertFalse(car.telemetry(0)["bus_ok"])

    def test_uptime_follows_the_clock_it_was_given(self):
        car = CarState(now=100.0)
        car.tick(142.0)
        self.assertEqual(car.telemetry(0)["uptime_s"], 42)


if __name__ == "__main__":
    unittest.main(verbosity=2)
