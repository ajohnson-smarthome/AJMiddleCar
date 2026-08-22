"""The mock car's real-time channel: hello / seq / bye over UDP, and the telemetry push.

Split out of `mock_car.py` for the reason `state.py` was: everything with behaviour worth
testing lives where a test can reach it without a socket. This module imports nothing
outside the standard library, so `test_rtlink.py` runs under the system interpreter with
no venv — adoption, eviction, the sequence window, the goodbye and the datagram caps are
covered by tests rather than by a session someone remembers to run.

`RTLink` talks to its loop through exactly two calls, `loop.time()` and
`loop.call_later()`, and to the network through `transport.sendto()`. A test supplies its
own of each and drives `datagram_received` directly.
"""
import asyncio
import json
import random
from collections import deque

from generated import PROTO, RT
from state import parse_frame, seq_is_newer

# The service tick. firmware/p4/main/rt_link.c uses a 20 ms SO_RCVTIMEO as its clock
# (TICK_MS there), so the watchdog is checked at the same granularity here and telemetry
# rides a counter on it. Not in contract/car-api.json — mirrored by hand from that file,
# unlike PUSH_EVERY below, which is derived from the schema's telemetry rate.
TICK_S = 0.020
PUSH_EVERY = round((1.0 / RT["telemetry_hz"]) / TICK_S)


class Impairment:
    """Packet loss, latency and stalls, seeded rather than clocked."""

    STALL_PERIOD_S = 5.0    # a stall arrives this often, so a session shows several

    def __init__(self, loss_pct=0.0, rtt_ms=0.0, stall_ms=0.0, seed=1):
        self.loss_pct = loss_pct
        self.delay_s = rtt_ms / 2000.0     # one way is half the round trip
        self.stall_s = stall_ms / 1000.0
        # One stream per direction, so that which datagram of an incoming session gets
        # dropped does not depend on how many telemetry pushes happened to interleave
        # with it. The rx pattern is then reproducible from the seed alone; the tx one
        # is not, because the number of pushes before a session starts depends on when
        # the client showed up. Same seed, same run, only if the client behaves the same.
        self._rx_rng = random.Random(seed)
        self._tx_rng = random.Random(seed + 1)
        self._epoch = None

    def drops_rx(self):
        return self.loss_pct > 0 and self._rx_rng.random() * 100.0 < self.loss_pct

    def drops_tx(self):
        return self.loss_pct > 0 and self._tx_rng.random() * 100.0 < self.loss_pct

    def stalled(self, now):
        """True while the mock is pretending to be too busy to service its socket.

        The cycle starts at the first datagram rather than at startup, so a stall lands
        inside a session however long the mock idled before one arrived.
        """
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

    def __init__(self, car, impair, verbose=False, loop=None):
        self.car = car
        self.impair = impair
        self.verbose = verbose
        self.transport = None
        # Injectable so a test can hold the clock still; None means the running loop,
        # which is what mock_car.py passes by not passing anything.
        self.loop = loop if loop is not None else asyncio.get_running_loop()
        self.owner = None          # (host, port) of the adopted session
        self.session = None        # its hello id
        self.last_seq = None
        self._rx = deque()         # timestamps of accepted commands, for rx_fps
        self._dropped = 0
        self._last_drop_log = 0.0
        self._last_cmd_log = 0.0

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
        # One parse, before anything is decided: a datagram the car would drop must not
        # move ownership, burn a sequence number or feed the watchdog on the way to
        # being rejected. `parse_frame` is the car's parser, cap included.
        frame = parse_frame(data)
        if frame is None:
            self._drop(now, f"unparseable, or over the {RT['max_command']}-byte "
                            f"command cap ({len(data)} bytes)")
            return

        if RT["hello_field"] in frame:
            self._adopt(frame, addr, now)
            return

        # Everything else is owned traffic. A datagram from anyone else is dropped
        # without touching ownership or the watchdog — eviction happens by `hello` only.
        if self.owner is None or addr != self.owner:
            self._drop(now, f"not the owner ({addr[0]}:{addr[1]})")
            return

        seq = frame.get(RT["seq_field"])
        if seq is None:
            self._drop(now, "no seq")
            return
        # Replay protection is what leaving TCP buys: a reordered or duplicated command
        # costs one dropped datagram instead of blocking the queue behind a retransmit.
        if self.last_seq is not None and not seq_is_newer(seq, self.last_seq):
            self._drop(now, f"seq {seq} not newer than {self.last_seq}")
            return
        self.last_seq = seq

        if frame.get(RT["bye_field"]):
            # A goodbye needs no axes: `{"seq":n,"bye":1}` is a complete instruction, and
            # the car acts on one. The seq gate above still applies — every app->car
            # datagram except `hello` carries `seq`, or it is dropped.
            if self.car.note_bye(now):
                print(f"rt: bye from session {self.session} — stopped, history cleared")
            else:
                # A sticky holder (an OTA flash, a calibration pulse) keeps the
                # actuator through a goodbye — rule 2 of the audit-fix spec. The
                # goodbye still ended the session and cleared the history.
                print(f"rt: bye from session {self.session} — history cleared, "
                      f"{self.car.ctl} keeps the actuator")
            # Ownership is not resumable: the next session says hello again.
            self.owner, self.session, self.last_seq = None, None, None
            return

        self.car.note_command(frame[RT["throttle_field"]], frame[RT["yaw_field"]], now)
        self._rx.append(now)
        self._log_command(now, seq)

    def _adopt(self, frame, addr, now):
        sid = frame[RT["hello_field"]]
        reply = {RT["proto_field"]: PROTO, RT["hello_field"]: sid,
                 RT["device_field"]: self.car.device, RT["fw_field"]: self.car.fw}
        if frame.get(RT["proto_field"]) != PROTO:
            # Answer anyway — the reply names our version, so a client can say "this car
            # speaks a protocol I do not" instead of searching forever — but do not
            # adopt. A session neither side can parse is worse than no session.
            print(f"rt: hello with proto {frame.get(RT['proto_field'])!r}, "
                  f"this car speaks {PROTO}")
            self._send(reply, addr)
            return
        if self.owner != addr or self.session != sid:
            evicted = self.owner if self.owner and self.owner != addr else None
            # Step 3 of the plan's adoption sequence: the sequence gate is the link's
            # state, so it is reset here; `adopt_session` does the other three. A repeat
            # hello from the same peer and sid reaches neither, which is what stops a
            # retransmitted handshake from resetting a live session.
            self.owner, self.session, self.last_seq = addr, sid, None
            self.car.adopt_session(now)
            self._rx.clear()
            print(f"rt: adopted session {sid} from {addr[0]}:{addr[1]}"
                  + (f" (evicting {evicted[0]}:{evicted[1]})" if evicted else ""))
        # Reply to every hello, not only to the one that adopted: the app repeats it
        # until answered, so a lost reply must be answerable by the next repeat.
        self._send(reply, addr)

    # ---- send ------------------------------------------------------------------

    def _send(self, obj, addr):
        data = json.dumps(obj, separators=(",", ":")).encode()
        if len(data) > RT["max_datagram"]:
            # rt_link.c refuses the same send. The cap is the receive buffer at the
            # other end, so an oversized reply is not a long reply — it is a truncated
            # one, and a truncated identity parses as a different car.
            print(f"rt: refusing a {len(data)}-byte reply, over the "
                  f"{RT['max_datagram']}-byte datagram cap")
            return
        if self.impair.drops_tx():
            return
        if self.impair.delay_s:
            self.loop.call_later(self.impair.delay_s, self.transport.sendto, data, addr)
        else:
            self.transport.sendto(data, addr)

    def tick(self, now):
        """One service tick of the car's clock.

        The sequence gate deliberately survives a trip (audit-fix spec rule 1):
        silence proves the stream is dead, but the gate is what stops a
        network-delayed duplicate of a pre-dropout command from being accepted as
        the resumed stream — driving the car at stale stick values and aborting
        the retreat. A same-session stream that resumes carries newer seqs and
        passes; the gate resets only on adopt and on bye.

        Channel ownership survives too: a stream that resumes after a dropout is
        the same session, and evicting it would ignore the driver until the app
        noticed.
        """
        return self.car.tick(now)

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
        # A clock of its own, so a stream of drops cannot suppress the command line —
        # during a loss test the pairing of the two is the thing being read.
        self._dropped += 1
        if self.verbose or now - self._last_drop_log > 1.0:
            self._last_drop_log = now
            print(f"rt: dropped ({why}); {self._dropped} so far")

    def _log_command(self, now, seq):
        """One line a second: at 10 Hz a line per frame buries everything else."""
        if self.verbose or now - self._last_cmd_log > 1.0:
            self._last_cmd_log = now
            t, y = self.car.command
            print(f"rt: seq={seq} t={t:.2f} y={y:.2f} rx={self.rx_fps(now)}/s "
                  f"ctl={self.car.ctl}")


async def service_loop(link):
    """The car's own clock: the watchdog every tick, telemetry every fifth.

    Everything goes through the link rather than through the car, because a watchdog
    trip is not only the car's business — it also invalidates the sequence gate.
    """
    loop = asyncio.get_running_loop()
    next_at = loop.time()
    ticks = 0
    while True:
        next_at += TICK_S
        await asyncio.sleep(max(0.0, next_at - loop.time()))
        now = loop.time()
        line = link.tick(now)
        if line:
            print(line)
        ticks += 1
        if ticks % PUSH_EVERY == 0:
            link.push_telemetry(now)
