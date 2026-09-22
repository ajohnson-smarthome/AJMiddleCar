#!/usr/bin/env python3
"""AJMiddleCar, mocked: the real-time UDP channel and the REST API.

Two servers over one `CarState` (state.py, where all the behaviour is and where it is
tested):

  * a UDP endpoint on the contract's real-time port, speaking hello / drive / bye and
    pushing telemetry to whoever owns the session;
  * the aiohttp REST server: /status, /config, /calibration*, /ota.

It binds `0.0.0.0` by default, not loopback, so that a simulator on this Mac can be pointed
at the Mac's LAN address (`-carHost`) and the conformance tools can be run from another
machine. A device build cannot be pointed here at all: on a phone the app addresses the
dongle, and only the dongle — the direct path it once had is gone. What a device exercises
that a simulator does not — App Transport Security, local-network privacy, interface
pinning — is exercised against the real dongle and the real car, or not at all.

Impairment flags are seeded from `--seed`, never from the clock, so the *inbound* loss
pattern replays exactly for a client that behaves the same way. Outbound loss rides a
second stream whose position depends on how many telemetry pushes preceded the session,
so it is repeatable only for a run driven the same way from the same moment.

    python3 mock_car.py                       # LAN, contract ports
    python3 mock_car.py --loss-pct 10         # verify a dropped datagram costs one tick
    python3 mock_car.py --host 127.0.0.1      # simulator only
    python3 mock_car.py --bus down            # a car whose PWM boards never came up

The degradation flags — `--bus down`, `--camera off`, `--radio mismatch|unavailable`,
`--reset-at-boot`, `--write-fail <domain|calibration>`, `--reboot-s N`, `--battery absent`
— are states the car can boot into and the app has to show (AJM-116). Each is a field of
`CarState` that the places already reporting it read, all off by default; `car_from_args`
is where the flags become state. The pack itself is a model (`state.Battery`): `--battery-soc N`
is where it starts and `--battery-drain-x N` how much faster than life it drains, for a
bar someone can watch melt.
"""
import argparse
import asyncio
import json
import math
import os
import socket
import sys

from aiohttp import web

from generated import (CALIBRATION, CONFIG_PATH, DEVICE, DOMAINS, ENDPOINTS, ENVELOPE,
                       PROTO, RT, STATUS_GROUPS, VIDEO)
from rt_link import REBOOT_QUIET_S, Impairment, RTLink, service_loop
from state import (BATTERY_ABSENT, BATTERY_OK, BUS_DOWN, BUS_OK, RADIO_MISMATCH, RADIO_OK,
                   RADIO_UNAVAILABLE, Battery, CarState, build_number, image_refusal,
                   parse_image_version)
from video import VideoLink

# A flash is the one REST call that takes real time; the mock spends it so a client's
# progress UI has something to show.
OTA_SECONDS = 2.0
OTA_MIN_BYTES = 4096       # the firmware refuses to erase a slot for anything smaller

# The car reads each JSON body into a fixed buffer on the httpd task's stack and refuses
# one that does not fit with its NUL: api_util.c's api_read_body returns -1 for
# `content_len >= sizeof buf`, and every handler answers that with `bad_json` and no
# `field`. The three sizes are not in the contract on any side — the buffer is each
# handler's own — so they are mirrored here by hand, like SID_MAX_CHARS, and read as
# "a body of this many bytes is refused". The app's compact bodies sit well inside; a
# pretty-printed one would otherwise learn the limit from the car alone (AJM-113).
BODY_MAX_CONFIG = 512        # cfg_api.c cfg_post: char body[512]
BODY_MAX_CALIBRATION = 512   # calib_api.c calib_save: char b[512]
BODY_MAX_SPIN = 96           # calib_api.c calib_spin: char b[96]


def is_private(addr):
    """RFC 1918 — the ranges a Mac and a simulator share on a home network."""
    if addr.startswith("10.") or addr.startswith("192.168."):
        return True
    if addr.startswith("172."):
        second = addr.split(".")[1] if addr.count(".") >= 2 else "0"
        return second.isdigit() and 16 <= int(second) <= 31
    return False


def lan_address():
    """The address another machine on this network can reach.

    Two sources, because neither alone is reliable: asking the routing table which
    interface would carry traffic to a public address (no packet is sent) answers with the
    VPN tunnel when one is up, and `gethostbyname(gethostname())` answers 127.0.0.1 often
    enough to be useless. A private address is preferred over whatever the route named,
    since that is the one a simulator or a second Mac on the same Wi-Fi can dial.
    """
    candidates = []
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        candidates.append(s.getsockname()[0])
    except OSError:
        pass
    finally:
        s.close()
    try:
        candidates += [i[4][0] for i in
                       socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)]
    except socket.gaierror:
        pass
    for addr in candidates:
        if is_private(addr):
            return addr
    return candidates[0] if candidates else "127.0.0.1"


# ---- REST ----------------------------------------------------------------------

def reply(members, status=200):
    """Every JSON the car emits starts with proto."""
    return web.json_response({ENVELOPE["proto"]: PROTO, **members}, status=status)


def json_error(status, code, message, field=""):
    """The car's rejection envelope. `field` is omitted when the body as a whole is at fault."""
    err = {ENVELOPE["code"]: code, ENVELOPE["message"]: message}
    if field:
        err[ENVELOPE["field"]] = field
    return web.json_response({ENVELOPE["proto"]: PROTO, ENVELOPE["error"]: err}, status=status)


def reply_refusal(err):
    """A (code, field, message) the state refused a body with, as the car answers it —
    the status follows the code (`car/status-and-version`): 500 for `write_failed`,
    which the state raises only under `--write-fail`, 400 for everything it validates."""
    code, field, message = err
    return json_error(500 if code == "write_failed" else 400, code, message, field)


async def read_object(request, limit):
    """The body as the car's JSON handlers see it, or the rejection they answer instead.

    Returns (body, None) or (None, response). The three refusals every one of cfg_api.c's
    and calib_api.c's handlers makes before looking at a single key, in their order and
    with their envelope — `bad_json`, no `field`: a body that is missing or would not fit
    the handler's buffer (`limit`, see BODY_MAX_*), one that does not parse, and one that
    parses to anything but an object. The mock's own limit was aiohttp's 17 MB, and
    `/calibration` answered `[]` with `missing_field` `wheels` (AJM-113).
    """
    data = await request.read()
    if not data or len(data) >= limit:
        return None, json_error(400, "bad_json", "body missing or too long")
    try:
        body = json.loads(data)
    except ValueError:
        return None, json_error(400, "bad_json", "malformed JSON")
    if not isinstance(body, dict):
        return None, json_error(400, "bad_json", "expected a JSON object")
    return body, None


@web.middleware
async def one_at_a_time(request, handler):
    """The firmware serves REST from a single httpd task, so requests queue behind
    each other — including behind the whole of an OTA upload and flash. A mock
    that answers /status mid-flash teaches a client the car can do that; the car
    holds its one task from the first body byte to the reboot."""
    async with request.app["lock"]:
        return await handler(request)


@web.middleware
async def rebooting(request, handler):
    """A car that just took a flash is rebooting, and nothing of it answers until the
    AP is back — REST included. For the same window the real-time channel is deaf and
    mute (`RTLink.simulate_reboot`), a request here is dropped before a byte of a reply,
    whether its connection was just accepted or held open from before the flash: the
    app's reboot guard (FirmwareFlow.swift) arms on `/version` *not* answering, and only
    then can "came back on the same fw" read as a rollback. A mock that answered
    `/version` straight through the pause never armed it, so `--rollback` — the rehearsal
    of exactly that screen — ran its 60 s window out and reported success instead.

    Listed after `one_at_a_time`, so the check runs under the lock: a poll that arrived
    mid-flash and waited behind the upload goes down with the reboot, as it does behind
    the car's single httpd task, rather than being answered in the gap between the two.
    """
    if request.app["link"].rebooting(asyncio.get_running_loop().time()):
        # Close the connection now and let aiohttp find it closed when it goes to write
        # the reply: that is its "premature client disconnection" path, logged at debug
        # and nothing else. Raising here instead would be an "Error handling request"
        # in the mock's log, and a 500 is not silence.
        request.protocol.force_close()
        return web.Response()
    return await handler(request)


async def cfg_get(request):
    return reply(request.app["car"].config_wire())


async def cfg_post(request):
    car = request.app["car"]
    body, refused = await read_object(request, BODY_MAX_CONFIG)
    if refused is not None:
        return refused
    ok, err = car.apply_config(body)
    if not ok:
        return reply_refusal(err)
    print(f"{CONFIG_PATH}: {car.config_wire()}")
    return reply(car.config_wire())


async def status(request):
    car, link = request.app["car"], request.app["link"]
    now = asyncio.get_running_loop().time()
    # All six of the schema's groups in its order — telemetry's four plus `radio` and
    # `storage`, the /status-only diagnostics, walked from the same schema by the state.
    return reply(car.status_groups(link.rx_fps(now, "status"), STATUS_GROUPS))


async def version(request):
    """GET /version — the frozen five-field document, raw (it spells its own proto)."""
    car = request.app["car"]
    if request.app["no_version"]:
        # A board older than the endpoint: a 404 with any body, exactly like an unmatched
        # path — the app's `VersionReply.of` maps any 404 here to `.absent`, not a decode.
        return json_error(404, "not_found", "no such path")
    return web.json_response({
        "device": car.device,
        "fw": car.fw,
        "build": build_number(car.fw),
        "proto": PROTO,
        "rolled_back": car.rollback,
    })


async def calib_get(request):
    car = request.app["car"]
    k = CALIBRATION["keys"]
    return reply({k["calibrated"]: car.calibrated, k["wheels"]: car.calibration_table()})


async def calib_spin(request):
    car = request.app["car"]
    k = CALIBRATION["keys"]
    body, refused = await read_object(request, BODY_MAX_SPIN)
    if refused is not None:
        return refused
    for key in body:
        if key not in (k["pair"], k["direction"]):
            return json_error(400, "unknown_field", "no such field", key)
    for key in (k["pair"], k["direction"]):
        if key not in body:
            return json_error(400, "missing_field", "required", key)
    pair, direction = body[k["pair"]], body[k["direction"]]
    if isinstance(pair, bool) or not isinstance(pair, (int, float)):
        return json_error(400, "wrong_type", "expected an integer", k["pair"])
    # request.json() accepts Infinity, NaN and integers of any size; int(pair) raises
    # OverflowError on an infinity or an oversized Python int and ValueError on NaN, and
    # math.isfinite raises that same OverflowError on the oversized int too. A 400 is the
    # answer either way, not a 500 from an uncaught exception.
    try:
        whole = math.isfinite(pair) and float(pair) == int(pair)
    except (OverflowError, ValueError):
        whole = False
    if not whole:
        return json_error(400, "wrong_type", "expected an integer", k["pair"])
    if not isinstance(direction, str):
        return json_error(400, "wrong_type", "expected a word", k["direction"])
    if not 0 <= int(pair) < CALIBRATION["pairs"]:
        return json_error(400, "out_of_range", "pair 0..3", k["pair"])
    if direction not in CALIBRATION["directions"]:
        return json_error(400, "not_allowed", "forward or reverse", k["direction"])
    forward = direction == CALIBRATION["directions"][0]
    now = asyncio.get_running_loop().time()
    if not car.begin_spin(now, int(pair), 1 if forward else 0):
        # The request is fine; the actuator is taken — or the bus is down and nothing
        # would reach the wheel. Both are 409 busy without a field (calib_spin.h): the
        # wizard must not advance either way — the wheel did not turn, and four blind
        # taps produce a table nothing can reject. Only the message tells the log which.
        return json_error(409, "busy", "actuator busy" if car.bus_ok else "motor bus down")
    print(f"calib: spin pair={int(pair)} {direction}")
    # The firmware's order (calib_api.c): sleep the pulse out, release, then answer.
    # The reply lands after the wheel has stopped — the wizard's next step assumes
    # it — and the app lock is held throughout, as the single httpd task is.
    await asyncio.sleep(CarState.CALIB_HOLD_MS / 1000.0)
    car.end_spin()
    return reply({ENVELOPE["ok"]: True})


async def calib_save(request):
    car = request.app["car"]
    k = CALIBRATION["keys"]
    body, refused = await read_object(request, BODY_MAX_CALIBRATION)
    if refused is not None:
        return refused
    # The firmware's order (calib_api.c): a foreign key is named before a missing one.
    for key in body:
        if key != k["wheels"]:
            return json_error(400, "unknown_field", "no such field", key)
    if k["wheels"] not in body:
        return json_error(400, "missing_field", "required", k["wheels"])
    ok, err = car.save_calibration(body[k["wheels"]])
    if not ok:
        return reply_refusal(err)
    print(f"calib: saved {car.calibration_table()}")
    return reply({k["calibrated"]: car.calibrated, k["wheels"]: car.calibration_table()})


async def ota(request):
    car, link = request.app["car"], request.app["link"]
    now = asyncio.get_running_loop().time()
    # The car stops the motors and takes the sticky grant before reading a single
    # body byte (car_stop(LINK_SRC_OTA) is ota_api.c's first statement), and
    # answers 409 when something outranks the flash.
    if not car.begin_ota(now):
        return json_error(409, "busy", "actuator busy")
    try:
        data = await request.read()
    except Exception:
        # A client that aborts mid-upload (aiohttp sets the payload exception on
        # connection loss) must not leave OWNER_UPDATE held forever — ota_api.c's own
        # recv-error path is esp_ota_abort + link_release_must + a rejection.
        # hold_s is None (sticky), so nothing else times this grant out.
        car.end_ota(flashed=False)
        raise
    if len(data) < OTA_MIN_BYTES:
        car.end_ota(flashed=False)
        return json_error(400, "too_small", "image too small")
    refusal = image_refusal(data)
    if refusal:
        # esp_ota_write checks the image magic on the first block, esp_ota_end verifies
        # the written image as a whole — chip_id and the app descriptor among the rest —
        # and ota_api.c answers either with `not_firmware`, the slot unassigned and the
        # running image untouched. After the whole body, as here: the car has read it
        # all before esp_ota_end can refuse it. Byte 0 alone used to be the check, so
        # any 4 KB blob — the dongle's image included — flashed and bumped fw: the exact
        # wrong-release-asset path the app could never rehearse (AJM-105).
        car.end_ota(flashed=False)
        print(f"ota: {len(data)} bytes refused — {refusal}")
        return json_error(400, "not_firmware", refusal)
    prev_fw = car.fw
    print(f"ota: {len(data)} bytes — motors stopped, flashing")
    await asyncio.sleep(OTA_SECONDS)
    car.end_ota(version=parse_image_version(data))
    if request.app["rollback_mode"]:
        # Rehearsal: the flashed image "fails its first boot" — the car comes back on
        # the previous firmware with the rollback flag up, exactly what the app's
        # detector must learn to call a FAILURE (decision 5).
        car.fw = prev_fw
        car.rollback = True
        print(f"ota: 'rolled back' — reporting {car.fw}, rolled_back:true")
    else:
        # The new image knows /version: from here the board answers it.
        request.app["no_version"] = False
        print(f"ota: done, now running {car.fw} — 'rebooting'")
    link.simulate_reboot(asyncio.get_running_loop().time())
    return reply({ENVELOPE["ok"]: True})


async def root(request):
    """The car serves a one-line identity here; there is no web UI."""
    car = request.app["car"]
    return web.Response(text=f"{car.device} {car.fw}\n")


def build_app(car, link, rollback_mode=False, no_version=False):
    # aiohttp's default client_max_size is 1 MB. A real image is already ~0.75 MB
    # (firmware/car/core/build/ajmiddlecar.bin) and growing, so the default would 413 a
    # legitimate upload — and, without the read() guard above, wedge the actuator
    # on the way. The P4 has 16 MB of flash; set the cap generously above that.
    app = web.Application(middlewares=[one_at_a_time, rebooting],
                          client_max_size=17 * 1024 * 1024)
    app["car"] = car
    app["link"] = link
    app["lock"] = asyncio.Lock()
    app["rollback_mode"] = rollback_mode
    app["no_version"] = no_version
    app.add_routes([
        web.get(ENDPOINTS["root"], root),
        web.get(ENDPOINTS["status"], status),
        web.get(ENDPOINTS["version"], version),
        web.get(ENDPOINTS["calibration"], calib_get),
        web.post(ENDPOINTS["calibration"], calib_save),
        web.post(ENDPOINTS["spin"], calib_spin),
        web.post(ENDPOINTS["ota"], ota),
        web.get(CONFIG_PATH, cfg_get),
        web.post(CONFIG_PATH, cfg_post),
    ])
    return app


def car_from_args(args, now):
    """The car the flags describe. The one place a command-line word becomes state, so
    that a mock started with no flags — `tools/test-all.sh`'s — is the healthy car."""
    car = CarState(device=args.device, now=now, bus_ok=args.bus == BUS_OK,
                   camera=args.camera == "on", radio=args.radio, nvs_wiped=args.reset_at_boot,
                   write_fail=args.write_fail or (), battery_soc=args.battery_soc,
                   battery_absent=args.battery == BATTERY_ABSENT, battery_drain_x=args.battery_drain_x)
    car.rssi = args.rssi
    return car


def degradations(args):
    """The flags that are on, for the banner — so a mock left running from a rehearsal
    of the bus-down screen says so at a glance."""
    on = []
    if args.bus != BUS_OK:
        on.append(f"bus {args.bus}")
    if args.camera != "on":
        on.append(f"camera {args.camera}")
    if args.radio != RADIO_OK:
        on.append(f"radio {args.radio}")
    if args.reset_at_boot:
        on.append("reset-at-boot")
    for name in args.write_fail or ():
        on.append(f"write-fail {name}")
    if args.reboot_s is not None:
        on.append(f"reboot {args.reboot_s:g} s")
    if args.battery != BATTERY_OK:
        on.append(f"battery {args.battery}")
    if args.battery_soc != Battery.DEFAULT_SOC:
        on.append(f"battery-soc {args.battery_soc:g}")
    if args.battery_drain_x != 1.0:
        on.append(f"battery-drain-x {args.battery_drain_x:g}")
    return on


async def serve(args):
    loop = asyncio.get_running_loop()
    car = car_from_args(args, loop.time())
    impair = Impairment(args.loss_pct, args.rtt_ms, args.stall_ms, args.seed)

    _, link = await loop.create_datagram_endpoint(
        lambda: RTLink(car, impair, args.verbose, reboot_s=args.reboot_s),
        local_addr=(args.host, args.rt_port))
    runner = web.AppRunner(build_app(car, link, rollback_mode=args.rollback, no_version=args.no_version),
                           access_log=None)
    await runner.setup()
    await web.TCPSite(runner, args.host, args.port).start()

    with open(args.video_sample, "rb") as f:
        sample = f.read()
    _, video = await loop.create_datagram_endpoint(
        lambda: VideoLink(car, link, sample, args.video_loss_pct, args.video_reorder_pct,
                          args.video_dup_pct, args.seed, args.verbose),
        local_addr=(args.host, args.video_port))
    asyncio.create_task(video.run())

    where = lan_address() if args.host == "0.0.0.0" else args.host
    print(f"mock {car.device} {car.fw} (proto {PROTO})")
    print(f"  REST      http://{where}:{args.port}   /status /calibration* /ota "
          f"{CONFIG_PATH} ({', '.join(DOMAINS)})")
    print(f"  real-time udp://{where}:{args.rt_port}   hello/drive/bye, "
          f"{RT['telemetry_hz']} Hz telemetry")
    print(f"  link      {impair.describe()}; watchdog {RT['watchdog_ms']} ms, "
          f"auto-return {car.config['recovery']['window_ms']} ms")
    print(f"  video     udp://{where}:{args.video_port}   view -> {VIDEO['width']}x{VIDEO['height']} "
          f"@{VIDEO['fps']}, loss {args.video_loss_pct}%")
    if degradations(args):
        print(f"  degraded  {', '.join(degradations(args))}")

    await service_loop(link)


def nonnegative(text):
    v = float(text)
    if v < 0:
        raise argparse.ArgumentTypeError("must be 0 or more")
    return v


def percent(text):
    v = float(text)
    if not 0 <= v <= 100:
        raise argparse.ArgumentTypeError("must be 0..100")
    return v


def parser():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--host", default="0.0.0.0",
                   help="bind address; the default is reachable from the LAN")
    p.add_argument("--port", type=int, default=8080, help="REST port")
    p.add_argument("--rt-port", type=int, default=RT["port"],
                   help="real-time UDP port; only move it to run a second mock, since "
                        "the app and the car both use the contract's port")
    p.add_argument("--device", default=os.environ.get("MOCK_DEVICE", DEVICE),
                   help="identity to report; change it to exercise the wrong-car path")
    p.add_argument("--loss-pct", type=float, default=0.0,
                   help="percentage of datagrams dropped, each way")
    p.add_argument("--rtt-ms", type=float, default=0.0, help="round-trip latency to add")
    p.add_argument("--stall-ms", type=float, default=0.0,
                   help="every 5 s, stop servicing the socket for this long")
    p.add_argument("--seed", type=int, default=1,
                   help="impairment seed; the same rx loss pattern for the same client")
    p.add_argument("--rssi", type=int, default=-58,
                   help="signal to report; 0 is the contract's 'unavailable', which the "
                        "app renders differently from a very weak signal")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="log every frame instead of one line a second")
    p.add_argument("--rollback", action="store_true",
                   help="rehearsal: every successful /ota 'fails its first boot' — the mock "
                        "comes back on the old fw with rolled_back:true in /version and in "
                        "hello_ack's device")
    p.add_argument("--no-version", action="store_true", default=bool(os.environ.get("MOCK_NO_VERSION")),
                   help="answer 404 on /version until the first accepted OTA — a car older than the endpoint")
    p.add_argument("--video-port", type=int, default=VIDEO["port"], help="video port (default from the contract)")
    p.add_argument("--video-sample", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "sample.h264"))
    p.add_argument("--video-loss-pct", type=float, default=0.0, help="drop this share of video datagrams")
    p.add_argument("--video-reorder-pct", type=float, default=0.0, help="delay this share by one datagram")
    p.add_argument("--video-dup-pct", type=float, default=0.0, help="send this share twice")
    # Degradations (AJM-116): each names the word the car shows in the state it degrades.
    p.add_argument("--bus", choices=(BUS_OK, BUS_DOWN), default=BUS_OK,
                   help="motors.bus: `down` is a car whose PWM boards never came up — reachable "
                        "and updatable, drive accepted, nothing moves, spin is 409 busy")
    p.add_argument("--camera", choices=("on", "off"), default="on",
                   help="`off` is no sensor at boot: video.state off, every view ignored")
    p.add_argument("--radio", choices=(RADIO_OK, RADIO_MISMATCH, RADIO_UNAVAILABLE), default=RADIO_OK,
                   help="radio.state: `mismatch` is another version in radio.fw, `unavailable` "
                        "is radio.fw null")
    p.add_argument("--reset-at-boot", action="store_true",
                   help="storage.reset_at_boot true: this boot wiped the settings and the "
                        "calibration, and the car runs on the contract's defaults")
    p.add_argument("--write-fail", action="append", choices=tuple(DOMAINS) + ("calibration",),
                   metavar="{" + ",".join(DOMAINS) + ",calibration}",
                   help="the first changing write of this domain (or of the calibration table) "
                        "answers 500 write_failed and is rolled back; repeatable")
    p.add_argument("--reboot-s", type=nonnegative, default=None,
                   help=f"how long every port is silent after a flash (default {REBOOT_QUIET_S:g}, "
                        "rt_link.py's REBOOT_QUIET_S, longer than the app's stall timeout); "
                        "0 is no silence at all")
    p.add_argument("--battery", choices=(BATTERY_OK, BATTERY_ABSENT), default=BATTERY_OK,
                   help="`absent` is no power monitor on the bus: battery.state absent, every "
                        "number of the group null, the car drives on; `low` is not a flag — "
                        "start the pack under the threshold with --battery-soc")
    p.add_argument("--battery-soc", type=percent, default=Battery.DEFAULT_SOC, metavar="PCT",
                   help=f"where the pack starts, 0..100 (default {Battery.DEFAULT_SOC}); "
                        f"{Battery.LOW_PCT} or less is battery.state low")
    p.add_argument("--battery-drain-x", type=nonnegative, default=1.0, metavar="N",
                   help="drain N times faster than life (default 1: full throttle empties the "
                        f"{Battery.CAPACITY_MAH} mAh pack in about an hour); 600 is 15 %% a second "
                        "at full throttle, for a bar someone can watch melt")
    return p


def main():
    args = parser().parse_args()
    # Line buffering, so `mock_car.py > log &` shows the banner and the drops as they
    # happen rather than in 8 KB batches when something finally flushes.
    sys.stdout.reconfigure(line_buffering=True)
    try:
        asyncio.run(serve(args))
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
