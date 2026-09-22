#!/usr/bin/env python3
"""Host tests for the mock's REST side, over a real aiohttp server on loopback.

`test_state.py` and `test_rtlink.py` cover everything that has no server attached. What is
left is the plumbing in `mock_car.py` that only shows through a socket — the simulated
reboot closing the door on REST, not just on UDP; the body a handler refuses before
parsing it; what `/ota` makes of an image. Needs aiohttp, so `tools/test-all.sh` runs this
file with the conformance sweep, under the venv, and not with the stdlib tests.
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
from generated import CALIBRATION, CONFIG_PATH, DOMAINS, ENDPOINTS, STATUS_GROUPS   # noqa: E402
import state                                     # noqa: E402
from generated import CALIBRATION, CONFIG_PATH, DOMAINS, ENDPOINTS, ENVELOPE   # noqa: E402
from rt_link import Impairment, RTLink           # noqa: E402
from state import (BATTERY_ABSENT, BATTERY_OK, BUS_DOWN, BUS_OK, RADIO_EXPECTED,   # noqa: E402
                   RADIO_MISMATCH, RADIO_OK, RADIO_UNAVAILABLE, VIDEO_IDLE, VIDEO_OFF, CarState)
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

    async def test_no_version_is_404_until_the_first_accepted_image_then_the_document(self):
        """`--no-version` is the flag-day rehearsal: a car older than `/version` answers
        404 there until the first accepted image, which knows the endpoint. The flag used
        to live in the running application, and aiohttp freezes one once it starts — the
        first flash under it would have been a 500 (AJM-155)."""
        self.car.no_version = True
        await self.serve()
        resp = await self.client.get(ENDPOINTS["version"])
        self.assertEqual(resp.status, 404)
        await self.flash()
        await asyncio.sleep(QUIET_S + 0.3)
        doc = await self.version()
        self.assertEqual(set(doc), {"device", "fw", "build", "proto", "rolled_back"})
        self.assertEqual(doc["fw"], NEW_FW)

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

    async def test_a_foreign_key_is_named_before_a_missing_one(self):
        """`calib_api.c` walks the body's keys before it looks for `wheels`, so a body with
        a foreign key and no `wheels` names the foreign key; the mock answered
        `missing_field` `wheels` to it, while `/calibration/spin` already had the car's
        order (AJM-167)."""
        await self.serve()
        k = CALIBRATION["keys"]
        wheels = self.bodies()[1][1][k["wheels"]]
        for path, body, code, field in (
                (ENDPOINTS["calibration"], {"foo": 1}, "unknown_field", "foo"),
                (ENDPOINTS["calibration"], {}, "missing_field", k["wheels"]),
                (ENDPOINTS["calibration"], {k["wheels"]: wheels, "foo": 1}, "unknown_field", "foo"),
                (ENDPOINTS["spin"], {"foo": 1}, "unknown_field", "foo")):
            resp = await self.client.post(path, json=body)
            self.assertEqual(resp.status, 400, (path, body))
            err = (await resp.json())["error"]
            self.assertEqual((err["code"], err.get("field")), (code, field), (path, body))


class TestDegradationFlags(Served):
    """Each degradation flag, as the app would meet it over REST (AJM-116). The state's own
    rules are in test_state.py; here is the plumbing — the flag reaching `CarState`, the
    status code each refusal gets, and the two /status-only groups."""

    def good_table(self):
        k = CALIBRATION["keys"]
        return {k["wheels"]: [{k["corner"]: c, k["pair"]: i, k["inverted"]: False}
                              for i, c in enumerate(CALIBRATION["corners"])]}

    async def status(self):
        resp = await self.client.get(ENDPOINTS["status"])
        self.assertEqual(resp.status, 200)
        return await resp.json()

    async def expect_error(self, resp, status, code, field=None):
        self.assertEqual(resp.status, status)
        err = (await resp.json())["error"]
        self.assertEqual(err["code"], code)
        if field is None:
            self.assertNotIn("field", err)
        else:
            self.assertEqual(err["field"], field)

    def test_the_parser_defaults_every_flag_off_and_car_from_args_reads_each(self):
        """`car_from_args` is the one place the flags become state — so the mock
        test-all.sh starts, with no flags, is the healthy car."""
        off = mock_car.car_from_args(mock_car.parser().parse_args([]), now=0.0)
        groups = off.status_groups(0, STATUS_GROUPS)
        self.assertEqual(groups["motors"]["bus"], BUS_OK)
        self.assertEqual(groups["video"]["state"], VIDEO_IDLE)
        self.assertEqual(groups["radio"]["state"], RADIO_OK)
        self.assertIs(groups["storage"]["reset_at_boot"], False)
        self.assertEqual(off.write_fail, set())
        self.assertEqual((groups["battery"]["state"], groups["battery"]["soc_pct"]), (BATTERY_OK, 80))
        self.assertEqual(off.battery.drain_x, 1.0)
        args = mock_car.parser().parse_args(
            ["--bus", "down", "--camera", "off", "--radio", "unavailable", "--reset-at-boot",
             "--write-fail", "ramp", "--write-fail", "calibration", "--reboot-s", "0",
             "--battery", "absent"])
        on = mock_car.car_from_args(args, now=0.0)
        groups = on.status_groups(0, STATUS_GROUPS)
        self.assertEqual(groups["motors"]["bus"], BUS_DOWN)
        self.assertEqual(groups["video"]["state"], VIDEO_OFF)
        self.assertEqual(groups["radio"]["state"], RADIO_UNAVAILABLE)
        self.assertIs(groups["storage"]["reset_at_boot"], True)
        self.assertEqual(on.write_fail, {"ramp", "calibration"})
        self.assertEqual(args.reboot_s, 0.0)
        self.assertEqual(groups["battery"]["state"], BATTERY_ABSENT)
        self.assertEqual(set(mock_car.degradations(args)),
                         {"bus down", "camera off", "radio unavailable", "reset-at-boot",
                          "write-fail ramp", "write-fail calibration", "reboot 0 s", "battery absent"})
        pack = mock_car.car_from_args(
            mock_car.parser().parse_args(["--battery-soc", "25", "--battery-drain-x", "600"]), now=0.0)
        self.assertEqual(pack.status_groups(0, STATUS_GROUPS)["battery"]["soc_pct"], 25)
        self.assertEqual(pack.battery.drain_x, 600.0)
        self.assertIsNone(mock_car.parser().parse_args([]).reboot_s,
                          "unset: the link falls back to rt_link.REBOOT_QUIET_S at reboot time")

    def test_the_parser_refuses_a_word_the_contract_does_not_have(self):
        for bad in (["--bus", "broken"], ["--radio", "missing"], ["--camera", "no"],
                    ["--write-fail", "wheels"], ["--reboot-s", "-1"], ["--battery", "low"],
                    ["--battery-soc", "101"], ["--battery-soc", "-1"], ["--battery-drain-x", "-1"]):
            with self.assertRaises(SystemExit, msg=bad), contextlib.redirect_stderr(io.StringIO()):
                mock_car.parser().parse_args(bad)

    async def test_status_carries_radio_and_storage_in_the_contracts_order(self):
        await self.serve()
        doc = await self.status()
        self.assertEqual(list(doc), ["proto"] + STATUS_GROUPS)
        self.assertEqual(doc["radio"], {"fw": RADIO_EXPECTED, "expected": RADIO_EXPECTED,
                                        "state": RADIO_OK})
        self.assertEqual(doc["storage"], {"reset_at_boot": False})

    async def test_bus_down_is_in_status_and_a_spin_is_409_busy(self):
        self.car = CarState(fw=OLD_FW, now=asyncio.get_running_loop().time(), bus_ok=False)
        self.link = RTLink(self.car, Impairment())
        await self.serve()
        self.assertEqual((await self.status())["motors"]["bus"], BUS_DOWN)
        k = CALIBRATION["keys"]
        resp = await self.client.post(ENDPOINTS["spin"], json={k["pair"]: 0, k["direction"]: CALIBRATION["directions"][0]})
        await self.expect_error(resp, 409, "busy")
        self.assertEqual((await resp.json())["error"]["message"], "motor bus down")
        # ...and the car is still updatable: the flash takes the actuator and goes through.
        await self.flash()

    async def test_radio_unavailable_is_null_fw_and_mismatch_is_another_version(self):
        self.car = CarState(fw=OLD_FW, now=asyncio.get_running_loop().time(), radio=RADIO_UNAVAILABLE)
        self.link = RTLink(self.car, Impairment())
        await self.serve()
        radio = (await self.status())["radio"]
        self.assertEqual(radio, {"fw": None, "expected": RADIO_EXPECTED, "state": RADIO_UNAVAILABLE})
        await self.client.close()
        self.car = CarState(fw=OLD_FW, now=asyncio.get_running_loop().time(), radio=RADIO_MISMATCH)
        self.link = RTLink(self.car, Impairment())
        await self.serve()
        radio = (await self.status())["radio"]
        self.assertEqual(radio["state"], RADIO_MISMATCH)
        self.assertNotEqual(radio["fw"], radio["expected"])

    async def test_reset_at_boot_shows_in_status_over_default_config(self):
        self.car = CarState(fw=OLD_FW, now=asyncio.get_running_loop().time(), nvs_wiped=True)
        self.link = RTLink(self.car, Impairment())
        await self.serve()
        doc = await self.status()
        self.assertIs(doc["storage"]["reset_at_boot"], True)
        self.assertIs(doc["motors"]["calibrated"], False)
        resp = await self.client.get(CONFIG_PATH)
        cfg = await resp.json()
        defaults = CarState(now=0.0).config_wire()
        self.assertEqual({d: cfg[d] for d in DOMAINS}, defaults)

    async def test_write_fail_ramp_is_one_500_and_the_next_get_shows_the_old_value(self):
        self.car = CarState(fw=OLD_FW, now=asyncio.get_running_loop().time(), write_fail=["ramp"])
        self.link = RTLink(self.car, Impairment())
        await self.serve()
        before = (await (await self.client.get(CONFIG_PATH)).json())["ramp"]
        body = {"ramp": {"rise_ms": before["rise_ms"] + 100}}
        resp = await self.client.post(CONFIG_PATH, json=body)
        await self.expect_error(resp, 500, "write_failed", field="ramp")
        self.assertEqual((await (await self.client.get(CONFIG_PATH)).json())["ramp"], before)
        resp = await self.client.post(CONFIG_PATH, json=body)
        self.assertEqual(resp.status, 200)
        self.assertEqual((await resp.json())["ramp"], body["ramp"])

    async def test_write_fail_calibration_is_one_500_with_field_wheels(self):
        self.car = CarState(fw=OLD_FW, now=asyncio.get_running_loop().time(), write_fail=["calibration"])
        self.link = RTLink(self.car, Impairment())
        await self.serve()
        k = CALIBRATION["keys"]
        resp = await self.client.post(ENDPOINTS["calibration"], json=self.good_table())
        await self.expect_error(resp, 500, "write_failed", field=k["wheels"])
        doc = await (await self.client.get(ENDPOINTS["calibration"])).json()
        self.assertIs(doc[k["calibrated"]], False)
        self.assertEqual(doc[k["wheels"]], [])
        resp = await self.client.post(ENDPOINTS["calibration"], json=self.good_table())
        self.assertEqual(resp.status, 200)
        self.assertIs((await resp.json())[k["calibrated"]], True)

    async def test_reboot_s_zero_leaves_rest_answering_right_after_a_flash(self):
        self.link = RTLink(self.car, Impairment(), reboot_s=0.0)
        await self.serve()
        await self.flash()
        self.assertEqual((await self.version())["fw"], NEW_FW)
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
    # Every warning is an error: aiohttp announces what its next major release refuses
    # (state written into a started application, string app keys) as a warning first.
    unittest.main(verbosity=2, warnings="error")
