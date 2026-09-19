#!/usr/bin/env python3
"""Host tests for the mock's REST side, over a real aiohttp server on loopback.

`test_state.py` and `test_rtlink.py` cover everything that has no server attached. What is
left is the plumbing in `mock_car.py` that only shows through a socket — the simulated
reboot closing the door on REST, not just on UDP; the body a handler refuses before
parsing it; what `/ota` makes of an image. Needs aiohttp, so `tools/test-all.sh` runs this
file with the conformance sweep, under the venv, and not with the stdlib tests.

The two durations a flash spends — `OTA_SECONDS` and `REBOOT_QUIET_S` — are shortened
here so the file runs in a couple of seconds; their real values are hand-mirrored
against the app's stall guard and pinned where they are defined.
"""
import asyncio
import json
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import aiohttp                                   # noqa: E402
from aiohttp.test_utils import TestClient, TestServer   # noqa: E402

import mock_car                                  # noqa: E402
import rt_link                                   # noqa: E402
from generated import CALIBRATION, CONFIG_PATH, ENDPOINTS   # noqa: E402
from rt_link import Impairment, RTLink           # noqa: E402
from state import CarState                       # noqa: E402
from test_state import synthetic_image           # noqa: E402

OLD_FW = "v1.0+9000"
NEW_FW = "v1.1+9100"
QUIET_S = 0.6       # the reboot window, shortened; 4.0 s for real (rt_link.REBOOT_QUIET_S)


class Served(unittest.IsolatedAsyncioTestCase):
    """One mock, served on a free loopback port, torn down after each test."""

    async def asyncSetUp(self):
        self.enterContext(mock.patch.object(mock_car, "OTA_SECONDS", 0.05))
        self.enterContext(mock.patch.object(rt_link, "REBOOT_QUIET_S", QUIET_S))
        self.car = CarState(fw=OLD_FW, now=asyncio.get_running_loop().time())
        self.link = RTLink(self.car, Impairment())
        self.client = None

    async def serve(self, **flags):
        app = mock_car.build_app(self.car, self.link, **flags)
        self.client = TestClient(TestServer(app))
        await self.client.start_server()

    async def asyncTearDown(self):
        if self.client is not None:
            await self.client.close()

    async def flash(self, version=NEW_FW):
        resp = await self.client.post(ENDPOINTS["ota"], data=synthetic_image(version.encode()))
        self.assertEqual(resp.status, 200)
        self.assertIs((await resp.json())["ok"], True)

    async def version(self):
        resp = await self.client.get(ENDPOINTS["version"])
        self.assertEqual(resp.status, 200)
        return await resp.json()

    async def expect_refused(self, path, raw, code):
        """A 400 with the contract's envelope and no `field`: the body as a whole is at
        fault, which is how `tools/conformance.py` judges the car for the same bodies."""
        resp = await self.client.post(path, data=raw)
        self.assertEqual(resp.status, 400, (path, raw[:40]))
        err = (await resp.json())["error"]
        self.assertEqual(err["code"], code, (path, raw[:40]))
        self.assertNotIn("field", err, (path, raw[:40]))


class TestReboot(Served):
    async def test_rest_is_unreachable_for_the_quiet_window_then_answers_the_new_fw(self):
        """`car/ota-and-rollback`: the reply goes out, then the car reboots — REST and
        both UDP channels are silent until the AP is back. The mock's reboot used to
        silence UDP alone, so the app's reboot guard (FirmwareFlow.swift), which arms on
        `/version` NOT answering, never armed in the simulator (AJM-103)."""
        await self.serve()
        await self.flash()
        for path in (ENDPOINTS["version"], ENDPOINTS["status"], ENDPOINTS["root"]):
            with self.assertRaises(aiohttp.ClientConnectionError, msg=path):
                await self.client.get(path)
        await asyncio.sleep(QUIET_S + 0.3)
        doc = await self.version()
        self.assertEqual(doc["fw"], NEW_FW)
        self.assertIs(doc["rolled_back"], False)

    async def test_rollback_is_the_old_fw_after_the_same_silence(self):
        """`--rollback` is the rehearsal of "flashed, did not survive its first boot".
        The app's detector calls it a rollback only as "went away, came back on the SAME
        fw" — so the silence has to happen first, or the rehearsal runs its 60 s window
        out and reports success (AJM-103)."""
        await self.serve(rollback_mode=True)
        await self.flash()
        with self.assertRaises(aiohttp.ClientConnectionError):
            await self.client.get(ENDPOINTS["version"])
        await asyncio.sleep(QUIET_S + 0.3)
        doc = await self.version()
        self.assertEqual(doc["fw"], OLD_FW)
        self.assertIs(doc["rolled_back"], True)

    async def test_a_request_queued_behind_the_flash_goes_down_with_the_reboot(self):
        """The car serves REST from one httpd task: a poll that arrives mid-flash waits
        behind the upload, and the reboot drops it unanswered. The reboot check sits
        under the same lock here so the mock's answer is the same — not a 200 slipped
        in between the flash and the silence."""
        self.enterContext(mock.patch.object(mock_car, "OTA_SECONDS", 0.4))
        await self.serve()
        flashing = asyncio.ensure_future(self.flash())
        await asyncio.sleep(0.1)                 # inside the flash, behind the lock
        with self.assertRaises(aiohttp.ClientConnectionError):
            await self.client.get(ENDPOINTS["status"])
        await flashing


class TestOta(Served):
    async def test_another_boards_image_is_not_firmware_and_the_fw_stays(self):
        """`car/ota-and-rollback`: an image that fails verification as a whole after the
        write is `not_firmware` — slot unassigned, running image untouched, no reboot. The
        dongle's `ajdongle.bin` is such an image: 0xE9, chip_id 9 (ESP32-S3), a descriptor
        carrying the release tag. The mock checked byte 0 alone, so a wrong release asset
        "updated the car" in the simulator and even showed the right build (AJM-105)."""
        await self.serve()
        dongle = synthetic_image(b"v9.9+7777-dongle", chip_id=0x0009)
        await self.expect_refused(ENDPOINTS["ota"], dongle, "not_firmware")
        # Refused means no reboot: REST answers at once, still on the old firmware...
        self.assertEqual((await self.version())["fw"], OLD_FW)
        # ...and the sticky grant is released — the next image goes through.
        await self.flash()

    async def test_an_image_without_a_descriptor_is_not_firmware(self):
        """0xE9 and zeros: the image magic alone used to "flash" and bump the build."""
        await self.serve()
        await self.expect_refused(ENDPOINTS["ota"], b"\xe9" + b"\x00" * 8191, "not_firmware")
        self.assertEqual((await self.version())["fw"], OLD_FW)


class TestBodyLimits(Served):
    """The car reads each JSON body into a fixed buffer and refuses one that does not fit
    with its NUL — `api_util.c` `api_read_body`, `bad_json` with no field — 512 bytes for
    `/config` and `/calibration`, 96 for `/calibration/spin`; and a body that is not an
    object is `bad_json` on all three. The mock took 17 MB everywhere, and answered
    `missing_field` `wheels` to `[]` on `/calibration` (AJM-113)."""

    async def asyncSetUp(self):
        await super().asyncSetUp()
        # a 95-byte spin body is accepted and pulses; not for 600 ms here
        self.enterContext(mock.patch.object(CarState, "CALIB_HOLD_MS", 1))

    def padded(self, body, size):
        """`body` as compact JSON, widened with trailing spaces to exactly `size` bytes —
        the same document, so the limit is the only thing that can refuse it."""
        raw = json.dumps(body, separators=(",", ":")).encode()
        self.assertLessEqual(len(raw), size)
        return raw + b" " * (size - len(raw))

    def bodies(self):
        k = CALIBRATION["keys"]
        wheels = [{k["corner"]: c, k["pair"]: i, k["inverted"]: False}
                  for i, c in enumerate(CALIBRATION["corners"])]
        return ((CONFIG_PATH, {"ramp": self.car.config_wire()["ramp"]}, mock_car.BODY_MAX_CONFIG),
                (ENDPOINTS["calibration"], {k["wheels"]: wheels}, mock_car.BODY_MAX_CALIBRATION),
                (ENDPOINTS["spin"], {k["pair"]: 0, k["direction"]: CALIBRATION["directions"][0]},
                 mock_car.BODY_MAX_SPIN))

    async def test_one_byte_under_the_limit_is_read_and_the_limit_itself_is_not(self):
        await self.serve()
        for path, body, limit in self.bodies():
            resp = await self.client.post(path, data=self.padded(body, limit - 1))
            self.assertEqual(resp.status, 200, (path, limit - 1))
            await self.expect_refused(path, self.padded(body, limit), "bad_json")

    async def test_a_body_that_is_not_an_object_is_bad_json_on_every_endpoint(self):
        await self.serve()
        for path, _, _ in self.bodies():
            for raw in (b"[]", b"5", b'"wheels"', b""):
                await self.expect_refused(path, raw, "bad_json")


if __name__ == "__main__":
    unittest.main(verbosity=2)
