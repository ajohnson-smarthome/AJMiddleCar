#!/usr/bin/env python3
"""Host tests for the mock's REST side, over a real aiohttp server on loopback.

`test_state.py` and `test_rtlink.py` cover everything that has no server attached. What is
left is the plumbing in `mock_car.py` that only shows through a socket — the simulated
reboot closing the door on REST, not just on UDP. Needs aiohttp, so `tools/test-all.sh`
runs this file with the conformance sweep, under the venv, and not with the stdlib tests.

The two durations a flash spends — `OTA_SECONDS` and `REBOOT_QUIET_S` — are shortened
here so the file runs in a couple of seconds; their real values are hand-mirrored
against the app's stall guard and pinned where they are defined.
"""
import asyncio
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import aiohttp                                   # noqa: E402
from aiohttp.test_utils import TestClient, TestServer   # noqa: E402

import mock_car                                  # noqa: E402
import rt_link                                   # noqa: E402
from generated import ENDPOINTS                  # noqa: E402
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
