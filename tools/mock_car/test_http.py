#!/usr/bin/env python3
"""Host tests for the mock's REST side, over a real aiohttp server on loopback.

`test_state.py` and `test_rtlink.py` cover everything that has no server attached. What is
left is the plumbing in `mock_car.py` that only shows through a socket — the simulated
reboot closing the door on REST, not just on UDP — and the REST sweep itself
(`tools/conformance.py`) run against this mock in-process, with every request it makes
recorded, so what it *sends* can be asserted and not only what it tolerates. Needs aiohttp,
so `tools/test-all.sh` runs this file with the conformance sweep, under the venv, and not
with the stdlib tests.

The two durations a flash spends — `OTA_SECONDS` and `REBOOT_QUIET_S` — are shortened
here so the file runs in a couple of seconds; their real values are hand-mirrored
against the app's stall guard and pinned where they are defined.
"""
import asyncio
import contextlib
import io
import json
import os
import sys
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(1, os.path.dirname(HERE))        # tools/, for the sweep itself

import aiohttp                                   # noqa: E402
from aiohttp import web                          # noqa: E402
from aiohttp.test_utils import TestClient, TestServer   # noqa: E402

import conformance                               # noqa: E402
import mock_car                                  # noqa: E402
import rt_link                                   # noqa: E402
import state                                     # noqa: E402
from generated import CALIBRATION, CONFIG_PATH, DOMAINS, ENDPOINTS, ENVELOPE   # noqa: E402
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


class TestRestSweepLegends(Served):
    """`tools/conformance.py` against this mock, in-process, every request recorded.

    J8 (AJM-119) found a set of bodies the car has an exact answer for that the sweep
    never SENT — a domain that is not an object, `proto` echoed back in a `/config` body,
    the shapes of a `wheels[i]` record, a good table out of corner order, a second spin
    in reverse, `/status.link.rx_hz` with no stream — so a mock diverging on any of them
    passed `test-all` regardless. Each is asserted here as sent; the sweep's own checks
    judge the answers, and the sweep has to come back green, since the mock mirrors
    `cfg_api.c` and `calib_api.c` on every one of them.
    """

    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.requests = []          # (method, path, parsed body or None), in order

    async def serve(self, **flags):
        app = mock_car.build_app(self.car, self.link, **flags)

        @web.middleware
        async def recording(request, handler):
            raw = await request.read()        # cached: the handler's request.json() reads the same bytes
            try:
                body = json.loads(raw) if raw else None
            except ValueError:
                body = None
            self.requests.append((request.method, request.path, body))
            return await handler(request)

        app.middlewares.append(recording)
        self.client = TestClient(TestServer(app))
        await self.client.start_server()

    async def sweep(self):
        """The whole matrix, --write-calibration, off the loop's thread: urllib blocks,
        and the server it is talking to lives on this loop. Its narration is swallowed —
        the verdict is the returned failure list."""
        suite = conformance.Conformance(f"http://{self.client.host}:{self.client.port}",
                                        write_calibration=True)

        def run():
            with contextlib.redirect_stdout(io.StringIO()):
                return suite.run()
        return await asyncio.to_thread(run)

    def sent(self, method, path, legend, pred):
        hits = [b for m, p, b in self.requests if m == method and p == path and pred(b)]
        self.assertTrue(hits, f"the sweep never sent {legend} to {method} {path}")

    async def test_the_sweep_sends_every_legend_of_ajm_119_and_the_mock_passes_it(self):
        await self.serve()
        failures = await self.sweep()
        self.assertEqual(failures, [])

        k, corners, pairs = CALIBRATION["keys"], CALIBRATION["corners"], CALIBRATION["pairs"]
        record_keys = {k["corner"], k["pair"], k["inverted"]}

        def wheels(b):
            ws = b.get(k["wheels"]) if isinstance(b, dict) else None
            return ws if isinstance(ws, list) else None

        def a_record(b, pred):
            ws = wheels(b)
            return ws is not None and any(pred(w) for w in ws)

        def a_dict_record(b, pred):
            return a_record(b, lambda w: isinstance(w, dict) and pred(w))

        def whole(v):
            return isinstance(v, int) and not isinstance(v, bool)

        def good_record(w):
            return isinstance(w, dict) and set(w) == record_keys \
                and w[k["corner"]] in corners and whole(w[k["pair"]]) \
                and 0 <= w[k["pair"]] < pairs and isinstance(w[k["inverted"]], bool)

        def good_table(b):
            ws = wheels(b)
            return ws is not None and len(ws) == pairs and all(good_record(w) for w in ws) \
                and len({w[k["corner"]] for w in ws}) == pairs \
                and {w[k["pair"]] for w in ws} == set(range(pairs))

        self.sent("POST", CONFIG_PATH, "a domain that is not an object",
                  lambda b: isinstance(b, dict)
                  and any(d in b and not isinstance(b[d], dict) for d in DOMAINS))
        self.sent("POST", CONFIG_PATH, "proto in the body",
                  lambda b: isinstance(b, dict) and ENVELOPE["proto"] in b)

        cal = ENDPOINTS["calibration"]
        self.sent("POST", cal, "a record that is not an object",
                  lambda b: a_record(b, lambda w: not isinstance(w, dict)))
        self.sent("POST", cal, "inverted that is not a boolean",
                  lambda b: a_dict_record(b, lambda w: k["inverted"] in w
                                          and not isinstance(w[k["inverted"]], bool)))
        self.sent("POST", cal, "corner that is not a string",
                  lambda b: a_dict_record(b, lambda w: k["corner"] in w
                                          and not isinstance(w[k["corner"]], str)))
        self.sent("POST", cal, "an unknown key inside a record",
                  lambda b: a_dict_record(b, lambda w: bool(set(w) - record_keys)))
        self.sent("POST", cal, "an unknown key beside wheels",
                  lambda b: wheels(b) is not None and bool(set(b) - {k["wheels"]}))
        self.sent("POST", cal, "pair out of range inside a record",
                  lambda b: a_dict_record(b, lambda w: whole(w.get(k["pair"]))
                                          and not 0 <= w[k["pair"]] < pairs))
        self.sent("POST", cal, "a corner outside the contract's list",
                  lambda b: a_dict_record(b, lambda w: isinstance(w.get(k["corner"]), str)
                                          and w[k["corner"]] not in corners))
        self.sent("POST", cal, "a good table out of corner order",
                  lambda b: good_table(b) and [w[k["corner"]] for w in wheels(b)] != corners)

        self.sent("POST", ENDPOINTS["spin"], "a spin in reverse",
                  lambda b: isinstance(b, dict)
                  and b.get(k["direction"]) == CALIBRATION["directions"][1])

        polls = [(m, p) for m, p, _ in self.requests]
        self.assertTrue(any(a == b == ("GET", ENDPOINTS["status"]) for a, b in zip(polls, polls[1:])),
                        "the sweep never polled /status twice in a row")

    async def test_a_mock_that_diverges_on_one_legend_fails_the_sweep(self):
        """The point of sending them: a mock answering `bad_json` without `field` where
        the car answers `wrong_type` with the domain's key — the kind of shortcut J8 was
        looking for — now fails the sweep instead of passing `test-all`."""
        real = state.validate_config

        def sloppy(body):
            if isinstance(body, dict) and any(d in body and not isinstance(body[d], dict)
                                              for d in DOMAINS):
                return False, ("bad_json", "", "expected an object")
            return real(body)

        self.enterContext(mock.patch.object(state, "validate_config", sloppy))
        await self.serve()
        failures = await self.sweep()
        self.assertTrue(any("wrong_type" in f for f in failures), failures)


if __name__ == "__main__":
    unittest.main(verbosity=2)
