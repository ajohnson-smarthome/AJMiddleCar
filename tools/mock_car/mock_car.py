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

Impairment flags produce a deterministic run: the loss RNG is seeded from `--seed`, never
from the clock, so a session that misbehaved is reproducible by starting the mock the
same way.

    python3 mock_car.py                       # LAN, contract ports
    python3 mock_car.py --loss-pct 10         # verify a dropped datagram costs one tick
    python3 mock_car.py --host 127.0.0.1      # simulator only
"""
import argparse
import asyncio
import json
import os
import random
import socket
import sys
from collections import deque

from aiohttp import web

from generated import DEVICE, DOMAINS, PROTO, RT
from state import CarState, seq_is_newer, valid_seq

# The command frame's two axes. Unlike the port, the deadline and hello/seq/bye, these
# names are not in contract/car-api.json — all three implementations spell them out.
AXIS_T = "t"
AXIS_Y = "y"

# The service tick. The firmware's rt_link uses a 20 ms SO_RCVTIMEO as its clock, so the
# watchdog is checked at the same granularity here and telemetry rides a counter on it.
TICK_S = 0.020
PUSH_EVERY = round((1.0 / RT["telemetry_hz"]) / TICK_S)

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


class Impairment:
    """Packet loss, latency and stalls, deterministic for the whole run."""

    STALL_PERIOD_S = 5.0    # a stall arrives this often, so a session shows several

    def __init__(self, loss_pct=0.0, rtt_ms=0.0, stall_ms=0.0, seed=1):
        self.loss_pct = loss_pct
        self.delay_s = rtt_ms / 2000.0     # one way is half the round trip
        self.stall_s = stall_ms / 1000.0
        # One stream per direction. Sharing a single RNG would make the loss pattern
        # depend on how the timer-driven telemetry pushes interleave with the incoming
        # frames, and the point of a seed is that the tenth datagram of a session is
        # dropped in every run that starts the same way.
        self._rx_rng = random.Random(seed)
        self._tx_rng = random.Random(seed + 1)
        self._epoch = None

    def drops_rx(self):
        return self.loss_pct > 0 and self._rx_rng.random() * 100.0 < self.loss_pct

    def drops_tx(self):
        return self.loss_pct > 0 and self._tx_rng.random() * 100.0 < self.loss_pct

    def stalled(self, now):
        """True while the mock is pretending to be too busy to service its socket."""
        if self.stall_s <= 0:
            return False
        if self._epoch is None:
            self._epoch = now
        return (now - self._epoch) % self.STALL_PERIOD_S < self.stall_s

    def describe(self):
        if not (self.loss_pct or self.delay_s or self.stall_s):
            return "clean"
        return (f"loss {self.loss_pct:g}%, rtt {self.delay_s * 2000:g} ms, "
                f"stall {self.stall_s * 1000:g} ms every {self.STALL_PERIOD_S:g} s")


class RTLink(asyncio.DatagramProtocol):
    """The real-time channel: one owner, learned from `recvfrom` and evicted by `hello`."""

    def __init__(self, car, impair, verbose=False):
        self.car = car
        self.impair = impair
        self.verbose = verbose
        self.transport = None
        self.loop = asyncio.get_running_loop()
        self.owner = None          # (host, port) of the adopted session
        self.session = None        # its hello id
        self.last_seq = None
        self._rx = deque()         # timestamps of accepted commands, for rx_fps
        self._dropped = 0
        self._last_log = 0.0

    # ---- receive ---------------------------------------------------------------

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        now = self.loop.time()
        if self.impair.stalled(now):
            return                                  # a stalled car services nothing
        if self.impair.drops_rx():
            return
        if self.impair.delay_s:
            self.loop.call_later(self.impair.delay_s, self._handle, data, addr)
        else:
            self._handle(data, addr)

    def _handle(self, data, addr):
        now = self.loop.time()
        if len(data) > RT["max_datagram"]:
            self._drop(now, f"{len(data)} bytes over the cap")
            return
        try:
            frame = json.loads(data)
        except (ValueError, UnicodeDecodeError):
            self._drop(now, "malformed JSON")
            return
        if not isinstance(frame, dict):
            self._drop(now, "not an object")
            return

        hello = frame.get(RT["hello_field"])
        if hello is not None:
            self._adopt(hello, frame, addr, now)
            return

        # Everything else is owned traffic. A datagram from anyone else is dropped
        # without touching ownership or the watchdog — eviction happens by `hello` only.
        if self.owner is None or addr != self.owner:
            self._drop(now, f"not the owner ({addr[0]}:{addr[1]})")
            return

        seq = frame.get(RT["seq_field"])
        if not valid_seq(seq):
            self._drop(now, "seq missing or not a uint32")
            return
        if self.last_seq is not None and not seq_is_newer(seq, self.last_seq):
            self._drop(now, f"seq {seq} not newer than {self.last_seq}")
            return
        self.last_seq = seq

        if frame.get(RT["bye_field"]):
            print(f"rt: bye from session {self.session} — stopped, retreat suppressed")
            self.car.note_bye(now)
            self.owner, self.session, self.last_seq = None, None, None
            return

        if not self.car.note_command(frame.get(AXIS_T), frame.get(AXIS_Y), now):
            self._drop(now, "t/y missing or not finite")
            return
        self._rx.append(now)
        self._log_command(now, seq)

    def _adopt(self, hello, frame, addr, now):
        reply = {"proto": PROTO, RT["hello_field"]: hello,
                 "device": self.car.device, "fw": self.car.fw}
        if frame.get("proto") != PROTO:
            # Answer anyway — the reply names our version, so a client can say "this car
            # speaks a protocol I do not" instead of searching forever — but do not
            # adopt. A session neither side can parse is worse than no session.
            print(f"rt: hello with proto {frame.get('proto')!r}, this car speaks {PROTO}")
            self._send(reply, addr)
            return
        if self.owner != addr or self.session != hello:
            evicted = self.owner if self.owner and self.owner != addr else None
            self.owner, self.session, self.last_seq = addr, hello, None
            self.car.adopt_session(now)
            self._rx.clear()
            print(f"rt: adopted session {hello} from {addr[0]}:{addr[1]}"
                  + (f" (evicting {evicted[0]}:{evicted[1]})" if evicted else ""))
        # Reply to every hello, not only to the one that adopted: the app repeats it
        # until answered, so a lost reply must be answerable by the next repeat.
        self._send(reply, addr)

    # ---- send ------------------------------------------------------------------

    def _send(self, obj, addr):
        if self.impair.drops_tx():
            return
        data = json.dumps(obj, separators=(",", ":")).encode()
        if self.impair.delay_s:
            self.loop.call_later(self.impair.delay_s, self.transport.sendto, data, addr)
        else:
            self.transport.sendto(data, addr)

    def push_telemetry(self, now):
        if self.owner is None or self.impair.stalled(now):
            return
        self._send(self.car.telemetry(self.rx_fps(now)), self.owner)

    # ---- measurement and logging -----------------------------------------------

    def rx_fps(self, now):
        while self._rx and now - self._rx[0] > 1.0:
            self._rx.popleft()
        return len(self._rx)

    def _drop(self, now, why):
        self._dropped += 1
        if self.verbose or now - self._last_log > 1.0:
            self._last_log = now
            print(f"rt: dropped ({why}); {self._dropped} so far")

    def _log_command(self, now, seq):
        """One line a second: at 10 Hz a line per frame buries everything else."""
        if self.verbose or now - self._last_log > 1.0:
            self._last_log = now
            t, y = self.car.command
            print(f"rt: seq={seq} t={t:.2f} y={y:.2f} rx={self.rx_fps(now)}/s "
                  f"ctl={self.car.ctl}")


async def service_loop(car, link):
    """The car's own clock: the watchdog every tick, telemetry every fifth."""
    loop = asyncio.get_running_loop()
    next_at = loop.time()
    ticks = 0
    while True:
        next_at += TICK_S
        await asyncio.sleep(max(0.0, next_at - loop.time()))
        now = loop.time()
        line = car.tick(now)
        if line:
            print(line)
        ticks += 1
        if ticks % PUSH_EVERY == 0:
            link.push_telemetry(now)


# ---- REST ----------------------------------------------------------------------

def json_error(status, message, field=""):
    """The car's rejection shape: a 4xx carrying which key was at fault."""
    return web.json_response({"error": message, "field": field}, status=status)


@web.middleware
async def one_at_a_time(request, handler):
    """The firmware serves REST from a single httpd task, so requests queue behind each
    other. A mock that answers four at once teaches a client the car is more responsive
    than it is.

    /ota manages the lock itself: it holds it while the image is arriving, which is when
    the car is genuinely occupied, and drops it for the flash, which is when the actuator
    is held but the socket is not — the window in which a client that presses Spin must
    be told 409 rather than left hanging.
    """
    if request.path == "/ota":
        return await handler(request)
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
        "device": car.device,
        "fw": car.fw,
        # The version gate: a client that cannot read this must refuse the car by name
        # rather than mis-parse it.
        "proto": PROTO,
        **car.telemetry(link.rx_fps(now)),
        "radio": {"fw": "mock", "expected": "mock", "ok": True},
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
    car = request.app["car"]
    async with request.app["lock"]:
        data = await request.read()
    if len(data) < OTA_MIN_BYTES:
        return json_error(400, "image too small")
    now = asyncio.get_running_loop().time()
    car.begin_ota(now)
    print(f"ota: {len(data)} bytes — motors stopped, flashing")
    await asyncio.sleep(OTA_SECONDS)
    car.end_ota()
    print(f"ota: done, now running {car.fw}")
    return web.json_response({"ok": True})


async def root(request):
    """The car serves a one-line identity here; there is no web UI."""
    car = request.app["car"]
    return web.Response(text=f"{car.device} {car.fw}\n")


def build_app(car, link):
    app = web.Application(middlewares=[one_at_a_time])
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

    await service_loop(car, link)


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
                   help="impairment seed; the same seed replays the same run")
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
