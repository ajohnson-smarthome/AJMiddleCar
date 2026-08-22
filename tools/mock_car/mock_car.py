#!/usr/bin/env python3
"""AJMiddleCar, mocked: the real-time UDP channel and the REST API.

Two servers over one `CarState` (state.py, where all the behaviour is and where it is
tested):

  * a UDP endpoint on the contract's real-time port, speaking hello / seq / bye and
    pushing telemetry to whoever owns the session;
  * the aiohttp REST server, whose five config domains are one handler pair registered
    in a loop over the schema.

It binds `0.0.0.0` by default, not loopback, because loopback exercises none of what
actually breaks on a phone: App Transport Security, local-network privacy, and interface
pinning. Point a real device at the address printed at startup.

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
import os
import socket
import sys

from aiohttp import web

from generated import DEVICE, DOMAINS, PROTO, RT
from rt_link import Impairment, RTLink, service_loop
from state import CarState

# A flash is the one REST call that takes real time; the mock spends it so a client's
# progress UI has something to show.
OTA_SECONDS = 2.0
OTA_MIN_BYTES = 4096       # the firmware refuses to erase a slot for anything smaller


def is_private(addr):
    """RFC 1918 — the ranges a phone and a Mac share on a home network or on the car's AP."""
    if addr.startswith("10.") or addr.startswith("192.168."):
        return True
    if addr.startswith("172."):
        second = addr.split(".")[1] if addr.count(".") >= 2 else "0"
        return second.isdigit() and 16 <= int(second) <= 31
    return False


def lan_address():
    """The address a phone on this network can reach.

    Two sources, because neither alone is reliable: asking the routing table which
    interface would carry traffic to a public address (no packet is sent) answers with the
    VPN tunnel when one is up, and `gethostbyname(gethostname())` answers 127.0.0.1 often
    enough to be useless. A private address is preferred over whatever the route named,
    since that is the one a phone on the same Wi-Fi — or on the car's AP — can dial.
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

def json_error(status, message, field=""):
    """The car's rejection shape: a 4xx carrying which key was at fault."""
    return web.json_response({"error": message, "field": field}, status=status)


@web.middleware
async def one_at_a_time(request, handler):
    """The firmware serves REST from a single httpd task, so requests queue behind
    each other — including behind the whole of an OTA upload and flash. A mock
    that answers /status mid-flash teaches a client the car can do that; the car
    holds its one task from the first body byte to the reboot."""
    async with request.app["lock"]:
        return await handler(request)


async def cfg_get(request):
    car = request.app["car"]
    return web.json_response(car.config[request.path])


async def cfg_post(request):
    car = request.app["car"]
    try:
        body = await request.json()
    except ValueError:
        return json_error(400, "malformed JSON")
    if not isinstance(body, dict):
        return json_error(400, "expected a JSON object")
    ok, err = car.apply_config(request.path, body)
    if not ok:
        return json_error(400, err, car.field_of(request.path, err))
    print(f"{request.path}: {car.config[request.path]}")
    return web.json_response({"ok": True})


async def status(request):
    car, link = request.app["car"], request.app["link"]
    now = asyncio.get_running_loop().time()
    return web.json_response({
        RT["device_field"]: car.device,
        RT["fw_field"]: car.fw,
        # The version gate: a client that cannot read this must refuse the car by name
        # rather than mis-parse it.
        RT["proto_field"]: PROTO,
        # A poll is not a push: `bump=False` keeps the real-time stream's `seq`
        # continuous however often something reads /status.
        **car.telemetry(link.rx_fps(now, "status"), bump=False),
        # `radio` is a /status-only object the schema does not describe, but the
        # version inside it is spelled with the contract's key, as status_api.c
        # spells it (`RT_KEY_FW`).
        "radio": {RT["fw_field"]: "mock", "expected": "mock", "ok": True},
    })


async def calib_get(request):
    return web.json_response({"calibrated": request.app["car"].calibrated})


async def calib_spin(request):
    car = request.app["car"]
    try:
        body = await request.json()
        pair, direction = body["pair"], body["dir"]
    except (ValueError, KeyError, TypeError):
        return json_error(400, "need {pair,dir}")
    if not isinstance(pair, int) or isinstance(pair, bool) or not 0 <= pair <= 3:
        return json_error(400, "pair must be 0..3", "pair")
    if direction not in (0, 1) or isinstance(direction, bool):
        return json_error(400, "dir must be 0 or 1", "dir")
    now = asyncio.get_running_loop().time()
    if not car.begin_spin(now, pair, direction):
        # The request is fine; the actuator is taken. The wizard must not advance — the
        # wheel did not turn, and four blind taps produce a table nothing can reject.
        return json_error(409, "actuator busy")
    print(f"calib: spin pair={pair} {'fwd' if direction else 'rev'}")
    # The firmware's order (calib_api.c): sleep the pulse out, release, then answer.
    # The reply lands after the wheel has stopped — the wizard's next step assumes
    # it — and the app lock is held throughout, as the single httpd task is.
    await asyncio.sleep(CarState.CALIB_HOLD_MS / 1000.0)
    car.end_spin()
    return web.json_response({"ok": True})


async def calib_save(request):
    car = request.app["car"]
    try:
        wheels = (await request.json())["wheels"]
    except (ValueError, KeyError, TypeError):
        return json_error(400, "need {wheels:[4x{pair,sign}]}", "wheels")
    if not car.save_calibration(wheels):
        return json_error(400, "need 4 unique pairs 0..3 with signs ±1", "wheels")
    print(f"calib: saved {wheels}")
    return web.json_response({"ok": True})


async def ota(request):
    car, link = request.app["car"], request.app["link"]
    now = asyncio.get_running_loop().time()
    # The car stops the motors and takes the sticky grant before reading a single
    # body byte (car_stop(LINK_SRC_OTA) is ota_api.c's first statement), and
    # answers 500 when something outranks the flash.
    if not car.begin_ota(now):
        return json_error(500, "actuator busy")
    try:
        data = await request.read()
    except Exception:
        # A client that aborts mid-upload (aiohttp sets the payload exception on
        # connection loss) must not leave CTL_OTA held forever — ota_api.c's own
        # recv-error path is esp_ota_abort + link_release_must + 400 "recv error".
        # hold_s is None (sticky), so nothing else times this grant out.
        car.end_ota(flashed=False)
        raise
    if len(data) < OTA_MIN_BYTES:
        car.end_ota(flashed=False)
        return json_error(400, "image too small")
    if data[0] != 0xE9:
        # esp_ota_write validates the ESP image magic on the first write, and
        # ota_api.c answers 500 "ota write failed". Any 4 KB blob used to flash
        # here and bump fw — the exact wrong-release-asset path the app could
        # never rehearse.
        car.end_ota(flashed=False)
        return json_error(500, "ota write failed")
    print(f"ota: {len(data)} bytes — motors stopped, flashing")
    await asyncio.sleep(OTA_SECONDS)
    car.end_ota()
    print(f"ota: done, now running {car.fw} — 'rebooting'")
    link.simulate_reboot(asyncio.get_running_loop().time())
    return web.json_response({"ok": True})


async def root(request):
    """The car serves a one-line identity here; there is no web UI."""
    car = request.app["car"]
    return web.Response(text=f"{car.device} {car.fw}\n")


def build_app(car, link):
    # aiohttp's default client_max_size is 1 MB. A real image is already ~0.75 MB
    # (firmware/p4/build/ajmiddlecar.bin) and growing, so the default would 413 a
    # legitimate upload — and, without the read() guard above, wedge the actuator
    # on the way. The P4 has 16 MB of flash; set the cap generously above that.
    app = web.Application(middlewares=[one_at_a_time], client_max_size=17 * 1024 * 1024)
    app["car"] = car
    app["link"] = link
    app["lock"] = asyncio.Lock()
    routes = [web.get("/", root), web.get("/status", status),
              web.get("/calib", calib_get), web.post("/calib/spin", calib_spin),
              web.post("/calib/save", calib_save), web.post("/ota", ota)]
    # One handler pair for every config domain: the mock cannot disagree with the car
    # about a range, because neither of them has one written down.
    for path in DOMAINS:
        routes += [web.get(path, cfg_get), web.post(path, cfg_post)]
    app.add_routes(routes)
    return app


async def serve(args):
    loop = asyncio.get_running_loop()
    car = CarState(device=args.device, now=loop.time())
    car.rssi = args.rssi
    impair = Impairment(args.loss_pct, args.rtt_ms, args.stall_ms, args.seed)

    _, link = await loop.create_datagram_endpoint(
        lambda: RTLink(car, impair, args.verbose), local_addr=(args.host, args.rt_port))
    runner = web.AppRunner(build_app(car, link), access_log=None)
    await runner.setup()
    await web.TCPSite(runner, args.host, args.port).start()

    where = lan_address() if args.host == "0.0.0.0" else args.host
    print(f"mock {car.device} {car.fw} (proto {PROTO})")
    print(f"  REST      http://{where}:{args.port}   /status /calib* /ota "
          + " ".join(DOMAINS))
    print(f"  real-time udp://{where}:{args.rt_port}   hello/seq/bye, "
          f"{RT['telemetry_hz']} Hz telemetry")
    print(f"  link      {impair.describe()}; watchdog {RT['watchdog_ms']} ms, "
          f"auto-return {car.config['/recover']['window_ms']} ms")

    await service_loop(link)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--host", default="0.0.0.0",
                   help="bind address; the default is reachable from a real phone")
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
