#!/usr/bin/env python3
"""Watch the FPV stream the way the app will, and say whether it obeys the contract.

Opens a real-time session on RT_PORT (a view is accepted only from the session's owner),
keeps it alive with drive 0,0 at command_hz, subscribes on the video port every
subscribe_ms, reassembles with video_wire.Receiver, asks for a keyframe on every loss
and measures how long one takes, checks every header, and writes the Annex B stream to
--out for ffplay. Works against the mock (task 13) and the car — directly on its Wi-Fi
(stage 2) or through the dongle (stage 3).

After the observation window come the legs of car/video-stream that the window alone
does not exercise — each short, and none of them drives: `/status.video` mid-stream;
a foreign sid and a `hello` on the video port (silence toward that socket, the stream
stays with the viewer); the `video.enabled` switch both ways; eviction by a second
session (the stream ends, the new owner opens one of its own); `bye` (silence, and
`/status.video` back at `idle`). The chunk pacing of the keyframes seen in the window is
judged last — against the car; the mock bursts, and on loopback it is only reported.

A car whose `/status.video.state` is `off` (no sensor answered at boot) is asked to
ignore a second of views and is otherwise left alone: the stream legs do not apply.

Exit status 1 when a check failed.
"""
import argparse
import json
import os
import secrets
import select
import socket
import statistics
import sys
import time
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_car"))
from generated import GROUPS, PROTO, RT, VIDEO   # noqa: E402
from video_wire import FLAG_KEY, HDR, Receiver, unpack_header   # noqa: E402

K, T = RT["keys"], RT["types"]
DGRAM = HDR + VIDEO["chunk_bytes"] + 64

# The words of groups.video.state, from the contract; a rename there must break here.
VIDEO_STATES = next(f["values"] for f in GROUPS["video"]["fields"] if f["name"] == "state")
STATE_OFF, STATE_IDLE, STATE_STREAMING = "off", "idle", "streaming"
assert {STATE_OFF, STATE_IDLE, STATE_STREAMING} <= set(VIDEO_STATES), VIDEO_STATES

# The car's sender lets one chunk go every SEND_PERIOD_US (firmware/car/core/main/video_link.c,
# 3 ms, chosen on the bench of 2026-09-16 against what the dongle's USB drains). It has no
# key in the contract — a mirror, by hand, like the mock's. The verdict is a corridor, not
# the figure: half the step or less is a burst (loopback measures tens of microseconds),
# twice the step or more would starve the ring behind a keyframe.
PACE_MS = 3.0

# The car ends a stream "within a tick" of the switch, a bye or an eviction (CTL_TICK_MS,
# 100 ms, plus the frame the encoder is on); the mock within a frame period. Half a second
# is the grace every ending leg allows before it demands silence, as the switch leg always did.
END_GRACE_S = 0.5
SILENCE_S = 1.0


def enc(obj):
    return json.dumps(obj, separators=(",", ":")).encode()


def nal_types(annexb):
    """nal_unit_type of every NAL unit behind a 3- or 4-byte start code."""
    out, i, n = [], 0, len(annexb)
    while i + 3 < n:
        if annexb[i] == 0 and annexb[i + 1] == 0:
            if annexb[i + 2] == 1:
                out.append(annexb[i + 3] & 0x1F); i += 4; continue
            if annexb[i + 2] == 0 and i + 4 < n and annexb[i + 3] == 1:
                out.append(annexb[i + 4] & 0x1F); i += 5; continue
        i += 1
    return out


def udp_socket():
    """Non-blocking: every wait in this tool goes through select, so a leg that watches
    two sockets at once (the viewer and a probe) never blocks on the quiet one while the
    busy one's queue overflows."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setblocking(False)
    return s


def drain(sock):
    """Every datagram waiting on sock, timestamped, without blocking."""
    out = []
    while True:
        try:
            data, _ = sock.recvfrom(DGRAM)
        except (BlockingIOError, socket.timeout):
            return out
        out.append((data, time.monotonic()))


def wait_readable(socks, timeout):
    """The sockets with a datagram waiting, after at most `timeout` seconds."""
    ready, _, _ = select.select(socks, [], [], timeout)
    return ready


def is_loopback(host):
    return host in ("127.0.0.1", "localhost", "::1")


class Session:
    """One real-time session and the socket it watches the stream from.

    `keepalive` is what the phone does between events: `drive` 0,0 at command_hz (the
    car's watchdog is 300 ms) with the telemetry drained, and `view` every subscribe_ms —
    `key:true` while `ask_key` is set, until the caller sees the keyframe land."""

    def __init__(self, rt, sid, rt_addr, video_addr):
        self.rt, self.sid = rt, sid
        self.rt_addr, self.video_addr = rt_addr, video_addr
        self.seq = 0
        self.viewer = udp_socket()
        self.view = enc({K["proto"]: PROTO, K["type"]: T["view"], K["session"]: sid})
        self.view_key = enc({K["proto"]: PROTO, K["type"]: T["view"], K["session"]: sid, K["key"]: True})
        self.ask_key = False
        self.next_drive = self.next_view = 0.0
        self.last_stream = None      # the `stream` of the last datagram the viewer took in

    def keepalive(self, drives=True, views=True):
        now = time.monotonic()
        if drives and now >= self.next_drive:
            self.seq += 1
            self.rt.sendto(enc({K["proto"]: PROTO, K["type"]: T["drive"], K["seq"]: self.seq,
                                K["throttle"]: 0, K["turn"]: 0}), self.rt_addr)
            self.next_drive = now + 1.0 / RT["command_hz"]
            drain(self.rt)                                   # telemetry
        if views and now >= self.next_view:
            self.send_view()
            self.next_view = now + VIDEO["subscribe_ms"] / 1000

    def send_view(self):
        self.viewer.sendto(self.view_key if self.ask_key else self.view, self.video_addr)

    def bye(self):
        self.seq += 1
        self.rt.sendto(enc({K["proto"]: PROTO, K["type"]: T["bye"], K["seq"]: self.seq}), self.rt_addr)

    def take(self):
        """The viewer's waiting datagrams; `last_stream` follows the well-formed ones."""
        got = drain(self.viewer)
        for data, _ in got:
            h = unpack_header(data)
            if h is not None:
                self.last_stream = h["stream"]
        return got


class VideoConformance:
    def __init__(self, host, rt_port, video_port, http_port, seconds, out, verbose, judge_pacing):
        self.rt_addr = (host, rt_port)
        self.video_addr = (host, video_port)
        self.http = f"http://{host}:{http_port}"
        self.seconds = seconds
        self.out = out
        self.verbose = verbose
        self.judge_pacing = judge_pacing
        self.failures = []
        self.pacing = []          # per multi-chunk keyframe seen in the window: mean ms between chunks
        self.intervals = []       # every gap between consecutive chunks of one keyframe, ms

    def check(self, ok, what):
        if not ok:
            self.failures.append(what)
            print(f"  FAIL  {what}")
        return ok

    # ---- the wire -----------------------------------------------------------------

    def open_session(self):
        """hello until hello_ack. Opened beside a live session, the hello evicts it — the
        eviction leg relies on exactly that."""
        rt = udp_socket()
        sid = secrets.token_hex(4)
        hello = enc({K["proto"]: PROTO, K["type"]: T["hello"], K["session"]: sid})
        for _ in range(15):
            rt.sendto(hello, self.rt_addr)
            deadline = time.monotonic() + 0.2
            while True:
                left = deadline - time.monotonic()
                if left <= 0 or not wait_readable([rt], left):
                    break
                for data, _ in drain(rt):
                    try:
                        f = json.loads(data)
                    except ValueError:
                        continue
                    if f.get(K["type"]) == T["hello_ack"] and f.get(K["session"]) == sid:
                        return Session(rt, sid, self.rt_addr, self.video_addr)
        raise SystemExit(f"no hello_ack from {self.rt_addr}")

    def post_config(self, body):
        req = urllib.request.Request(self.http + "/config", data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status

    def get_config(self):
        with urllib.request.urlopen(self.http + "/config", timeout=5) as r:
            return json.loads(r.read())

    def get_status_video(self, when):
        """The `video` group of GET /status, or None with a FAIL recorded — REST trouble
        is a named failure here, never an exception, for the same reason as in the switch
        leg: whatever the HTTP side did, the wire legs after it must still run."""
        try:
            with urllib.request.urlopen(self.http + "/status", timeout=5) as r:
                return json.loads(r.read())["video"]
        except (OSError, ValueError, KeyError) as e:
            self.check(False, f"GET /status {when} at {self.http}: {e!r}")
            return None

    def silence(self, session, socks, seconds, drives=True, views=True, before=None):
        """Keep `session` alive for `seconds`, and count what lands on each of `socks`.
        `before(now)` runs on every turn of the loop — for the leg that keeps probing."""
        heard = {s: 0 for s in socks}
        t0 = time.monotonic()
        while time.monotonic() - t0 < seconds:
            session.keepalive(drives, views)
            if before:
                before(time.monotonic())
            for s in wait_readable(socks, 0.05):
                got = session.take() if s is session.viewer else drain(s)
                heard[s] += len(got)
        return heard

    def first_keyframe(self, session, budget=2.0):
        """A fresh receiver, views at once and every subscribe_ms, `key:true` from the first
        loss on: (an IDR arrived within `budget`, the `stream` it came in)."""
        rx = Receiver()
        session.ask_key = False
        session.next_view = 0.0                          # the first view goes at once
        stream = None
        t0 = time.monotonic()
        while time.monotonic() - t0 < budget:
            session.keepalive()
            if not wait_readable([session.viewer], 0.05):
                continue
            for data, _ in session.take():
                h = unpack_header(data)
                if h is not None:
                    stream = h["stream"]
                ev, frame = rx.feed(data)
                if ev == Receiver.LOSS:
                    # The first frame of the stream is the IDR; one chunk of it gone and
                    # nothing decodes until the next one — planned 10 s out on the car. Ask,
                    # and keep asking every view, exactly as the main loop does.
                    if not session.ask_key:
                        session.ask_key = True
                        session.send_view()
                        if self.verbose:
                            print("    loss on the fresh stream — asked for a keyframe")
                elif ev == Receiver.FRAME and 5 in nal_types(frame):
                    return True, stream
        return False, stream

    # ---- legs ---------------------------------------------------------------------

    def check_off(self, session):
        """A car without a sensor: a `view` now and another half a second on, not one
        datagram back in the whole second."""
        heard = 0
        for _ in range(2):
            session.next_view = 0.0
            heard += self.silence(session, [session.viewer], SILENCE_S / 2)[session.viewer]
        self.check(heard == 0, f"video.state is off, yet {heard} datagram(s) answered a view")
        print(f"video.state off: views ignored for {SILENCE_S:.0f} s; the stream legs do not apply")

    def observe(self, session):
        """The main window: the stream as the drive screen sees it, for --seconds."""
        rx = Receiver()
        t0 = time.monotonic()
        session.next_drive = session.next_view = t0
        frames = key_frames = bytes_rx = dgrams = 0
        streams = set()
        key_asked_at = None
        key_latencies = []
        first_frame_at = None
        key_arrivals = {}     # (stream, frame) of a keyframe -> [(chunk, at)] while it is arriving
        sink = open(self.out, "wb") if self.out else None

        while time.monotonic() - t0 < self.seconds:
            session.keepalive()
            if not wait_readable([session.viewer], 0.05):
                continue
            for data, at in session.take():
                dgrams += 1
                bytes_rx += len(data)
                h = unpack_header(data)
                if self.check(h is not None, f"datagram {dgrams}: header rejected ({data[:12].hex()})"):
                    streams.add(h["stream"])
                    if h["flags"] & FLAG_KEY and h["count"] > 1:
                        key_arrivals.setdefault((h["stream"], h["frame"]), []).append((h["chunk"], at))
                ev, frame = rx.feed(data)
                if ev == Receiver.LOSS:
                    if key_asked_at is None:
                        key_asked_at = time.monotonic()
                        session.ask_key = True
                        session.send_view()
                        if self.verbose:
                            print("    loss — asked for a keyframe")
                elif ev == Receiver.FRAME:
                    frames += 1
                    types = nal_types(frame)
                    if first_frame_at is None:
                        first_frame_at = time.monotonic() - t0
                        self.check(5 in types, "the first delivered frame is not an IDR")
                    if 5 in types:
                        key_frames += 1
                        self.check(7 in types and 8 in types, f"keyframe {frames} carries no SPS/PPS")
                        if key_asked_at is not None:
                            key_latencies.append(time.monotonic() - key_asked_at)
                            key_asked_at = None
                            session.ask_key = False
                    if sink:
                        sink.write(frame)
                    if self.verbose:
                        print(f"    frame {frames}: {len(frame)} B, nal {types}")

        # The window the figures are over ends here: the legs below run their own clocks
        # and count nothing into these, so they must not stretch the divisor either.
        elapsed = time.monotonic() - t0
        session.ask_key = False
        if sink:
            sink.close()

        # Chunk pacing, per keyframe: the span from its first arrival to its last over the
        # chunk indices it covers — lost chunks widen no gap and a reordered one narrows
        # none, and the adapter's USB, which hands the phone chunks in blocks (~5 ms apart,
        # docs/protocol.md), bunches the gaps within a frame without moving its span.
        for arrivals in key_arrivals.values():
            arrivals.sort(key=lambda ca: ca[1])
            chunks = [c for c, _ in arrivals]
            gaps = max(chunks) - min(chunks)
            if gaps >= 1:
                self.pacing.append((arrivals[-1][1] - arrivals[0][1]) * 1000 / gaps)
                self.intervals += [(b - a) * 1000 for (_, a), (_, b) in zip(arrivals, arrivals[1:])]

        fps = frames / elapsed
        kbps = bytes_rx * 8 / elapsed / 1000
        loss_pct = 100.0 * rx.dropped / max(1, frames + rx.dropped)
        first = f"{first_frame_at:.2f} s" if first_frame_at is not None else "never"
        print(f"{dgrams} datagrams, {frames} frames ({key_frames} key), {rx.dropped} lost, "
              f"{rx.bad} bad; {fps:.1f} fps, {kbps:.0f} kbit/s, first frame after {first}")
        if key_latencies:
            print(f"keyframe on request: {min(key_latencies)*1000:.0f}..{max(key_latencies)*1000:.0f} ms "
                  f"over {len(key_latencies)} requests")
        self.check(frames > 0, "no frame was delivered")
        self.check(rx.bad == 0, f"{rx.bad} datagrams broke the header or length rules")
        self.check(len(streams) <= 1, f"the stream number changed mid-run: {sorted(streams)}")
        self.check(first_frame_at is None or first_frame_at < 2.0,
                   f"first frame took {first} (> 2 s)")   # `first`, not a format of None
        self.check(loss_pct < 5.0, f"{loss_pct:.1f}% of frames lost")
        self.check(all(l < 1.0 for l in key_latencies),
                   f"a keyframe took {max(key_latencies, default=0)*1000:.0f} ms to arrive after a request")
        self.check(key_frames >= 1, "no keyframe at all")

    def check_status_streaming(self):
        """GET /status while the stream runs: `streaming`, and a real fps."""
        v = self.get_status_video("mid-stream")
        if v is None:
            return
        self.check(v.get("state") == STATE_STREAMING,
                   f"mid-stream /status says video.state {v.get('state')!r}, not {STATE_STREAMING!r}")
        self.check(isinstance(v.get("fps"), int) and v["fps"] > 0,
                   f"mid-stream /status says video.fps {v.get('fps')!r} — nothing counted at the sender")
        print(f"/status mid-stream: video {v.get('state')}, {v.get('fps')} fps, {v.get('kbps')} kbit/s")

    def check_foreign(self, session):
        """A `view` under a sid that is not the owner's, and a `hello` under the owner's own
        sid, from a socket of their own, twice over a second: nothing comes back to that
        socket, and the stream goes on to the viewer meanwhile — ignored means unchanged."""
        probe = udp_socket()
        foreign_sid = secrets.token_hex(4)
        assert foreign_sid != session.sid
        shapes = [enc({K["proto"]: PROTO, K["type"]: T["view"], K["session"]: foreign_sid}),
                  enc({K["proto"]: PROTO, K["type"]: T["hello"], K["session"]: session.sid})]
        next_probe = 0.0

        def probing(now):
            nonlocal next_probe
            if now >= next_probe:
                for d in shapes:
                    probe.sendto(d, self.video_addr)
                next_probe = now + SILENCE_S / 2

        heard = self.silence(session, [probe, session.viewer], SILENCE_S, before=probing)
        probe.close()
        self.check(heard[probe] == 0,
                   f"a foreign sid or a hello on the video port drew {heard[probe]} datagram(s)")
        self.check(heard[session.viewer] > 0,
                   "the stream to the owner stopped while foreign datagrams hit the video port")

    def check_switch(self, session):
        """`video.enabled` off: two seconds of views, not one datagram. On: a frame, an IDR,
        within two seconds. The stream the run just watched ends within a tick, not at the
        subscribe timeout — a datagram after 0.5 s is a car that ignores the switch. The rt
        session is kept alive at command_hz throughout (the car's watchdog is 300 ms), and
        views go every subscribe_ms as the phone would send them.

        Every REST failure is a FAIL and an early return, never an exception: the legs after
        this must go out whatever the car's HTTP side did. The switch is put back to what
        it was even on Ctrl-C between the two POSTs — a run must not leave the car dark."""

        def post_switch(on):
            """True when the car took it; a refusal or an unreachable REST side is one FAIL."""
            try:
                status = self.post_config({"video": dict(video_cfg, enabled=on)})
            except OSError as e:      # a 404 (the adapter's own API), a refusal, or a hang-up mid-request
                self.check(False, f"POST /config video.enabled={str(on).lower()} at {self.http}: {e}")
                return False
            return self.check(status == 200, f"POST video.enabled={str(on).lower()} answered {status}")

        # A domain POST replaces the whole domain (cfg_api.c's two-pass rule — a partial
        # object is refused, not merged), so bitrate_kbps rides along unchanged.
        # OSError, not URLError: urlopen wraps only the request in URLError — a server that
        # accepts and then closes (the adapter relaying to a car whose HTTP side is down) raises
        # a raw ConnectionResetError, one that stalls a raw TimeoutError. Both are OSError, and
        # so is URLError.
        try:
            video_cfg = self.get_config()["video"]
        except OSError as e:
            self.check(False, f"GET /config at {self.http}: {e} — the switch leg was not run")
            return
        was_on = bool(video_cfg.get("enabled", True))
        left_on = was_on
        try:
            if not post_switch(False):
                return
            left_on = False
            self.silence(session, [session.viewer], END_GRACE_S)     # the tick the car may end the stream in
            heard = self.silence(session, [session.viewer], 2.0)[session.viewer]
            self.check(heard == 0, f"switched off, but {heard} datagram(s) still arrived")

            if not post_switch(True):
                return
            left_on = True
            got_idr, _ = self.first_keyframe(session)
            self.check(got_idr, "switched back on, but no keyframe within 2 s")
        finally:
            if left_on != was_on:
                try:
                    self.post_config({"video": video_cfg})
                except OSError as e:
                    self.check(False, f"restoring video.enabled={str(was_on).lower()}: {e}")

    def check_eviction(self, old, new):
        """`new` said hello while `old` was watching: within a tick the stream to `old`'s
        viewer ends, and a second of `old`'s views revives nothing. `new`'s first view then
        opens a stream of its own — another `stream`, an IDR first."""
        before = old.last_stream
        self.silence(new, [old.viewer], END_GRACE_S, before=lambda now: old.keepalive())
        heard = self.silence(new, [old.viewer], SILENCE_S, before=lambda now: old.keepalive())
        self.check(heard[old.viewer] == 0,
                   f"evicted by another hello, yet {heard[old.viewer]} datagram(s) still reached the old viewer")
        got_idr, stream = self.first_keyframe(new)
        self.check(got_idr, "the new owner's view opened no stream (no keyframe within 2 s)")
        self.check(stream is None or stream != before,
                   f"the new owner's stream carries the old stream number {stream}")

    def check_bye(self, session):
        """`bye` ends the session; the stream goes with it on the next tick, not at the
        subscribe timeout — views keep coming, as from a phone that left the drive screen
        with its video side still asking, and none of them counts without an owner."""
        session.bye()
        self.silence(session, [session.viewer], END_GRACE_S, drives=False)
        heard = self.silence(session, [session.viewer], SILENCE_S, drives=False)[session.viewer]
        self.check(heard == 0, f"after bye, {heard} datagram(s) still arrived")
        v = self.get_status_video("after bye")
        if v is not None:
            self.check(v.get("state") == STATE_IDLE,
                       f"after bye /status says video.state {v.get('state')!r}, not {STATE_IDLE!r}")

    def check_pacing(self):
        """The keyframes of the window, chunk by chunk: the median of their pacing is the
        car's step, not a burst. Reported everywhere; judged only where the sender is the
        car's — the mock sends each frame in one go, so loopback is not a verdict."""
        if not self.pacing:
            if self.judge_pacing:
                self.check(False, "no multi-chunk keyframe in the window to pace")
            return
        median = statistics.median(self.pacing)
        q = statistics.quantiles(self.intervals, n=10) if len(self.intervals) >= 10 else None
        hist = (f"; chunk gaps p10/p50/p90 {q[0]:.2f}/{q[4]:.2f}/{q[8]:.2f} ms" if q else "")
        line = f"keyframe pacing: median {median:.2f} ms per chunk over {len(self.pacing)} keyframe(s){hist}"
        if not self.judge_pacing:
            print(f"  skipped: {line} — measured, not judged: the mock sends a frame in one burst, "
                  f"the car one chunk every {PACE_MS:g} ms")
            return
        print(line)
        self.check(PACE_MS / 2 <= median <= PACE_MS * 2,
                   f"keyframe chunks pace at {median:.2f} ms, not about {PACE_MS:g} — "
                   + ("a burst, not a trickle" if median < PACE_MS / 2 else "the ring starves"))

    # ---- the run ------------------------------------------------------------------

    def run(self):
        video = self.get_status_video("before the session")
        first = self.open_session()
        print(f"session {first.sid} on {self.rt_addr}")
        if video is not None and video.get("state") == STATE_OFF:
            self.check_off(first)
            first.bye()
            return self.verdict()

        self.observe(first)
        self.check_status_streaming()
        self.check_foreign(first)
        self.check_switch(first)
        second = self.open_session()
        print(f"session {second.sid} evicts {first.sid}")
        self.check_eviction(first, second)
        self.check_bye(second)
        self.check_pacing()
        return self.verdict()

    def verdict(self):
        if self.failures:
            print(f"video conformance: {len(self.failures)} failure(s)")
            return 1
        print("video conformance: all checks passed")
        return 0


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("host", help="the car (192.168.4.1 on its Wi-Fi, 192.168.7.1 through the dongle) or the mock (127.0.0.1)")
    p.add_argument("--rt-port", type=int, default=RT["port"])
    p.add_argument("--video-port", type=int, default=VIDEO["port"])
    p.add_argument("--http-port", type=int, default=None,
                   help="the REST port with /config and /status: default 80 (the car's, also through "
                        "the adapter, whose own API on 8080 has neither), or 8080 (the mock's --port) "
                        "when the host is loopback")
    p.add_argument("--seconds", type=float, default=20.0)
    p.add_argument("--out", help="write the Annex B stream here (ffplay opens it)")
    p.add_argument("-v", "--verbose", action="store_true")
    a = p.parse_args()
    if a.http_port is None:
        a.http_port = 8080 if is_loopback(a.host) else 80
    # Loopback is the mock, which bursts each frame: its chunk pacing is reported, not judged.
    sys.exit(VideoConformance(a.host, a.rt_port, a.video_port, a.http_port, a.seconds, a.out, a.verbose,
                              judge_pacing=not is_loopback(a.host)).run())


if __name__ == "__main__":
    main()
