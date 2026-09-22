#!/usr/bin/env python3
"""Host test: the constants the mock mirrors by hand agree with their sources. Stdlib only.

Everything on the wire reaches the mock through `generated.py`, so a schema change moves
the mock with the generator. A handful of the car's behaviour constants have no key in
contract/car-api.json — the stationary threshold and the per-segment cap of the retreat,
the identification pulse, the service tick, the sid length, the ring of dead sids, the OTA
floor, the video sender's ring and step — and the mock carries copies; the reboot gap has to outlast a number the app keeps.
`test_state.py` and `test_rtlink.py` pin the copies, and nothing pinned their equality to
the source: a change in the firmware left `tools/test-all.sh` green while the mock went on
impersonating the previous car (AJM-121). This file reads each source with a regular
expression — the way `tools/test_gen_contract.py` reads the domain bounds — and compares.
A mismatch fails with the constant's name and both values.
"""
import os
import pathlib
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rt_link                                                       # noqa: E402
import video                                                         # noqa: E402
from rt_link import Impairment, RTLink                                # noqa: E402
from state import SID_MAX_CHARS, Battery, CarState                    # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
MAIN = ROOT / "firmware" / "car" / "core" / "main"
APP = ROOT / "app" / "AJMiddleCar"


def rel(path):
    return str(path.relative_to(ROOT))


def literal(path, pattern, cast):
    """The one number `pattern` captures in `path`, without any C suffix. Exactly one match:
    a constant defined twice, or not at all, is a source the test no longer reads."""
    hits = re.findall(pattern, path.read_text(), re.M)
    if len(hits) != 1:
        raise AssertionError(f"{rel(path)}: expected one match for {pattern!r}, found {len(hits)}")
    return cast(hits[0])


def define(name, path, cast):
    """`#define NAME <number>[u|f]` in a firmware header or source."""
    return literal(path, rf"^#define\s+{re.escape(name)}\s+([0-9.]+)[uUfF]?\b", cast)


class Mirrors(unittest.TestCase):
    def mirror(self, name, copy, source, where):
        self.assertEqual(copy, source,
                         f"{name}: the mock's copy is {copy!r}, {where} says {source!r}")

    def test_stationary_threshold(self):
        """Below MOVE_EPS a breadcrumb is stationary and is not retraced — here and there."""
        src = define("MOVE_EPS", MAIN / "recovery.c", float)
        self.mirror("MOVE_EPS", CarState.MOVE_EPS, src, "recovery.c MOVE_EPS")

    def test_retreat_segment_cap(self):
        """One replay segment lasts at most RECOVER_SEG_MAX_MS, however long the gap between
        two breadcrumbs was."""
        src = define("RECOVER_SEG_MAX_MS", MAIN / "recovery.h", int)
        self.mirror("SEG_MAX_MS", CarState.SEG_MAX_MS, src, "recovery.h RECOVER_SEG_MAX_MS")

    def test_identification_pulse(self):
        """The spin's pulse is LINK_HOLD_CALIB_MS — the grant's lifetime and the delay before
        the reply — and the wizard in the simulator must wait exactly as long as it does on
        the bench. conformance.py's floor for the spin's latency derives from this copy."""
        src = define("LINK_HOLD_CALIB_MS", MAIN / "link.h", int)
        self.mirror("CALIB_HOLD_MS", CarState.CALIB_HOLD_MS, src, "link.h LINK_HOLD_CALIB_MS")
        # The macro the mock mirrors is the one the pulse actually waits on.
        self.assertRegex((MAIN / "calib_api.c").read_text(),
                         r"vTaskDelay\(pdMS_TO_TICKS\(LINK_HOLD_CALIB_MS\)\)",
                         "calib_api.c: the pulse no longer waits LINK_HOLD_CALIB_MS")

    def test_service_tick(self):
        """The watchdog is checked, and telemetry counted, on the receive timeout's beat."""
        src = define("TICK_MS", MAIN / "rt_link.c", int)
        self.mirror("TICK_S", rt_link.TICK_S, src / 1000.0, "rt_link.c TICK_MS / 1000")

    def test_sid_length(self):
        """The longest sid the parser accepts: CONTROL_SID_MAX with room for the NUL."""
        src = define("CONTROL_SID_MAX", MAIN / "control_proto.h", int)
        self.mirror("SID_MAX_CHARS", SID_MAX_CHARS, src - 1, "control_proto.h CONTROL_SID_MAX - 1")

    def test_dead_sid_ring(self):
        """How many ended sessions the car remembers, so a replayed handshake of one of them
        cannot evict the live driver."""

        class Still:
            def time(self):
                return 0.0

        ring = RTLink(CarState(now=0.0), Impairment(), loop=Still()).dead_sids.maxlen
        src = define("RT_DEAD_SIDS", MAIN / "rt_link.h", int)
        self.mirror("dead_sids maxlen", ring, src, "rt_link.h RT_DEAD_SIDS")

    def test_ota_floor(self):
        """The smallest body /ota accepts. mock_car.py imports aiohttp, so its copy is read
        the same way the firmware's is."""
        copy = literal(HERE / "mock_car.py", r"^OTA_MIN_BYTES\s*=\s*(\d+)\b", int)
        src = literal(MAIN / "ota_api.c", r"content_len\s*<\s*(\d+)\b", int)
        self.mirror("OTA_MIN_BYTES", copy, src, "ota_api.c content_len <")

    def test_video_sender(self):
        """The mock's video sender is the car's: a ring of RING_SLOTS encoded frames, drained
        one chunk every SEND_PERIOD_US — or the keyframe pacing leg judges the mock by a
        step the car no longer keeps (AJM-168)."""
        for name in ("RING_SLOTS", "SEND_PERIOD_US"):
            src = define(name, MAIN / "video_link.c", int)
            self.mirror(name, getattr(video, name), src, f"video_link.c {name}")

    def test_pack_constants(self):
        """The pack is not on the wire, so the contract has no key for it: the mock's model
        drains at the firmware's capacity and says `low` at the firmware's two thresholds
        (battery_pack.h), or the bar in the simulator tells a story the car's does not."""
        for name, copy in (("BATTERY_CAPACITY_MAH", Battery.CAPACITY_MAH),
                           ("BATTERY_LOW_PCT", Battery.LOW_PCT),
                           ("BATTERY_LOW_CLEAR_PCT", Battery.LOW_CLEAR_PCT)):
            src = define(name, MAIN / "battery_pack.h", int)
            self.mirror(name, copy, src, f"battery_pack.h {name}")

    def test_reboot_gap_outlasts_the_app_stall(self):
        """The mock's simulated reboot must exceed the app's stall timeout, or the app never
        tears the session down, never re-hellos, and never learns the new fw. Not equality:
        the gap is the mock's own number, and the app's is its floor."""
        hits = [(p, m) for p in sorted(APP.glob("*.swift"))
                for m in re.findall(r"stallTimeout\s*:\s*TimeInterval\s*=\s*([0-9.]+)\b",
                                    p.read_text())]
        self.assertEqual(len(hits), 1,
                         f"expected one stallTimeout in {rel(APP)}, found {[rel(p) for p, _ in hits]}")
        path, stall = hits[0]
        self.assertGreater(rt_link.REBOOT_QUIET_S, float(stall),
                           f"REBOOT_QUIET_S: the mock's gap is {rt_link.REBOOT_QUIET_S!r} s, "
                           f"{rel(path)} stallTimeout is {float(stall)!r} s — the gap must be longer")


if __name__ == "__main__":
    unittest.main()
