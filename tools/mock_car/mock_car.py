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
"""
import argparse
import asyncio
import math
import os
import socket
import sys

from aiohttp import web

from generated import (CALIBRATION, CONFIG_PATH, DEVICE, DOMAINS, ENDPOINTS, ENVELOPE,
                       GROUPS, PROTO, RT)
from rt_link import Impairment, RTLink, service_loop
from state import CarState, build_number, parse_image_version

# A flash is the one REST call that takes real time; the mock spends it so a client's
# progress UI has something to show.
OTA_SECONDS = 2.0
OTA_MIN_BYTES = 4096       # the firmware refuses to erase a slot for anything smaller


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


@web.middleware
async def one_at_a_time(request, handler):
    """The firmware serves REST from a single httpd task, so requests queue behind
    each other — including behind the whole of an OTA upload and flash. A mock
    that answers /status mid-flash teaches a client the car can do that; the car
    holds its one task from the first body byte to the reboot."""
    async with request.app["lock"]:
        return await handler(request)


async def cfg_get(request):
    return reply(request.app["car"].config_wire())


async def cfg_post(request):
    car = request.app["car"]
    try:
        body = await request.json()
    except ValueError:
        return json_error(400, "bad_json", "malformed JSON")
    ok, err = car.apply_config(body)
    if not ok:
        code, field, message = err
        return json_error(400, code, message, field)
    print(f"{CONFIG_PATH}: {car.config_wire()}")
    return reply(car.config_wire())


async def status(request):
    car, link = request.app["car"], request.app["link"]
    now = asyncio.get_running_loop().time()
    dev = [f["name"] for f in GROUPS["device"]["fields"]]
    # Schema order (STATUS_GROUPS): device, link, motors, radio, storage, system —
    # `radio` and `storage` are /status-only diagnostics the schema does not describe,
    # inserted between the two groups `status_groups` already returns in order.
    groups = car.status_groups(link.rx_fps(now, "status"))
    return reply({
        "device": dict(zip(dev, [car.device, car.fw, build_number(car.fw), car.rollback])),
        "link": groups["link"],
        "motors": groups["motors"],
        "radio": {"fw": "mock", "expected": "mock", "state": "ok"},
        "storage": {"reset_at_boot": car.nvs_wiped},
        "system": groups["system"],
    })


async def calib_get(request):
    car = request.app["car"]
    k = CALIBRATION["keys"]
    return reply({k["calibrated"]: car.calibrated, k["wheels"]: car.calibration_table()})


async def calib_spin(request):
    car = request.app["car"]
    k = CALIBRATION["keys"]
    try:
        body = await request.json()
    except ValueError:
        return json_error(400, "bad_json", "malformed JSON")
    if not isinstance(body, dict):
        return json_error(400, "bad_json", "expected a JSON object")
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
        # The request is fine; the actuator is taken. The wizard must not advance — the
        # wheel did not turn, and four blind taps produce a table nothing can reject.
        return json_error(409, "busy", "actuator busy")
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
    try:
        body = await request.json()
    except ValueError:
        return json_error(400, "bad_json", "malformed JSON")
    if not isinstance(body, dict) or k["wheels"] not in body:
        return json_error(400, "missing_field", "required", k["wheels"])
    for key in body:
        if key != k["wheels"]:
            return json_error(400, "unknown_field", "no such field", key)
    ok, err = car.save_calibration(body[k["wheels"]])
    if not ok:
        code, field, message = err
        return json_error(400, code, message, field)
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
    if data[0] != 0xE9:
        # esp_ota_write validates the ESP image magic on the first write, and
        # ota_api.c answers "not an ESP image". Any 4 KB blob used to flash here
        # and bump fw — the exact wrong-release-asset path the app could never
        # rehearse.
        car.end_ota(flashed=False)
        return json_error(400, "not_firmware", "not an ESP image")
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
        print(f"ota: 'rolled back' — reporting {car.fw}, rollback:true")
    else:
        print(f"ota: done, now running {car.fw} — 'rebooting'")
    link.simulate_reboot(asyncio.get_running_loop().time())
    return reply({ENVELOPE["ok"]: True})


async def root(request):
    """The car serves a one-line identity here; there is no web UI."""
    car = request.app["car"]
    return web.Response(text=f"{car.device} {car.fw}\n")


def build_app(car, link, rollback_mode=False):
    # aiohttp's default client_max_size is 1 MB. A real image is already ~0.75 MB
    # (firmware/car/core/build/ajmiddlecar.bin) and growing, so the default would 413 a
    # legitimate upload — and, without the read() guard above, wedge the actuator
    # on the way. The P4 has 16 MB of flash; set the cap generously above that.
    app = web.Application(middlewares=[one_at_a_time], client_max_size=17 * 1024 * 1024)
    app["car"] = car
    app["link"] = link
    app["lock"] = asyncio.Lock()
    app["rollback_mode"] = rollback_mode
    app.add_routes([
        web.get(ENDPOINTS["root"], root),
        web.get(ENDPOINTS["status"], status),
        web.get(ENDPOINTS["calibration"], calib_get),
        web.post(ENDPOINTS["calibration"], calib_save),
        web.post(ENDPOINTS["spin"], calib_spin),
        web.post(ENDPOINTS["ota"], ota),
        web.get(CONFIG_PATH, cfg_get),
        web.post(CONFIG_PATH, cfg_post),
    ])
    return app


async def serve(args):
    loop = asyncio.get_running_loop()
    car = CarState(device=args.device, now=loop.time())
    car.rssi = args.rssi
    impair = Impairment(args.loss_pct, args.rtt_ms, args.stall_ms, args.seed)

    _, link = await loop.create_datagram_endpoint(
        lambda: RTLink(car, impair, args.verbose), local_addr=(args.host, args.rt_port))
    runner = web.AppRunner(build_app(car, link, rollback_mode=args.rollback), access_log=None)
    await runner.setup()
    await web.TCPSite(runner, args.host, args.port).start()

    where = lan_address() if args.host == "0.0.0.0" else args.host
    print(f"mock {car.device} {car.fw} (proto {PROTO})")
    print(f"  REST      http://{where}:{args.port}   /status /calibration* /ota "
          f"{CONFIG_PATH} ({', '.join(DOMAINS)})")
    print(f"  real-time udp://{where}:{args.rt_port}   hello/drive/bye, "
          f"{RT['telemetry_hz']} Hz telemetry")
    print(f"  link      {impair.describe()}; watchdog {RT['watchdog_ms']} ms, "
          f"auto-return {car.config['recovery']['window_ms']} ms")

    await service_loop(link)


def main():
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
                        "comes back on the old fw with rollback:true in /status")
    args = p.parse_args()
    # Line buffering, so `mock_car.py > log &` shows the banner and the drops as they
    # happen rather than in 8 KB batches when something finally flushes.
    sys.stdout.reconfigure(line_buffering=True)
    try:
        asyncio.run(serve(args))
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
