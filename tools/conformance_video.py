#!/usr/bin/env python3
"""Watch the FPV stream the way the app will, and say whether it obeys the contract.

Opens a real-time session on RT_PORT (a view is accepted only from the session's owner),
keeps it alive with drive 0,0 at command_hz, subscribes on the video port every
subscribe_ms, reassembles with video_wire.Receiver, asks for a keyframe on every loss
and measures how long one takes, checks every header, and writes the Annex B stream to
--out for ffplay. Works against the mock (task 13) and the car — directly on its Wi-Fi
(stage 2) or through the dongle (stage 3).

Exit status 1 when a check failed.
"""
import argparse
import json
import os
import secrets
import socket
import sys
import time
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_car"))
from generated import PROTO, RT, VIDEO   # noqa: E402
from video_wire import HDR, Receiver, unpack_header   # noqa: E402

K, T = RT["keys"], RT["types"]


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


class VideoConformance:
    def __init__(self, host, rt_port, video_port, http_port, seconds, out, verbose):
        self.rt_addr = (host, rt_port)
        self.video_addr = (host, video_port)
        self.http = f"http://{host}:{http_port}"
        self.seconds = seconds
        self.out = out
        self.verbose = verbose
        self.failures = []

    def check(self, ok, what):
        if not ok:
            self.failures.append(what)
            print(f"  FAIL  {what}")
        return ok

    def open_session(self):
        """hello until hello_ack; returns (socket, sid)."""
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(0.2)
        sid = secrets.token_hex(4)
        hello = enc({K["proto"]: PROTO, K["type"]: T["hello"], K["session"]: sid})
        for _ in range(15):
            s.sendto(hello, self.rt_addr)
            try:
                data, _ = s.recvfrom(RT["max_datagram"])
            except socket.timeout:
                continue
            try:
                f = json.loads(data)
            except ValueError:
                continue
            if f.get(K["type"]) == T["hello_ack"] and f.get(K["session"]) == sid:
                return s, sid
        raise SystemExit(f"no hello_ack from {self.rt_addr}")

    def post_config(self, body):
        req = urllib.request.Request(self.http + "/config", data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status

    def get_config(self):
        with urllib.request.urlopen(self.http + "/config", timeout=5) as r:
            return json.loads(r.read())

    def check_switch(self, rt, v, view, view_key, seq):
        """`video.enabled` off: two seconds of views, not one datagram. On: a frame, an IDR,
        within two seconds. The stream the run just watched ends within a tick, not at the
        subscribe timeout — a datagram after 0.5 s is a car that ignores the switch. The rt
        session is kept alive at command_hz throughout (the car's watchdog is 300 ms), and
        views go every subscribe_ms as the phone would send them.

        Every REST failure is a FAIL and an early return, never an exception: the bye after
        this leg must go out whatever the car's HTTP side did. The switch is put back to what
        it was even on Ctrl-C between the two POSTs — a run must not leave the car dark."""
        drive_period = 1.0 / RT["command_hz"]
        view_period = VIDEO["subscribe_ms"] / 1000
        next_drive = next_view = 0.0
        ask_key = False       # a chunk went missing in the "on" leg — view with key until an IDR lands

        def keepalive():
            nonlocal seq, next_drive, next_view
            now = time.monotonic()
            if now >= next_drive:
                seq += 1
                rt.sendto(enc({K["proto"]: PROTO, K["type"]: T["drive"], K["seq"]: seq,
                               K["throttle"]: 0, K["turn"]: 0}), self.rt_addr)
                next_drive = now + drive_period
                try:
                    while True:
                        rt.recvfrom(RT["max_datagram"])
                except (socket.timeout, BlockingIOError):
                    pass
            if now >= next_view:
                v.sendto(view_key if ask_key else view, self.video_addr)
                next_view = now + view_period

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
            return seq
        was_on = bool(video_cfg.get("enabled", True))
        left_on = was_on
        try:
            if not post_switch(False):
                return seq
            left_on = False
            t0 = time.monotonic()
            while time.monotonic() - t0 < 0.5:          # the tick the car is allowed to end the stream in
                keepalive()
                try:
                    v.recvfrom(HDR + VIDEO["chunk_bytes"] + 64)
                except socket.timeout:
                    pass
            heard = 0
            t0 = time.monotonic()
            while time.monotonic() - t0 < 2.0:
                keepalive()
                try:
                    v.recvfrom(HDR + VIDEO["chunk_bytes"] + 64)
                    heard += 1
                except socket.timeout:
                    pass
            self.check(heard == 0, f"switched off, but {heard} datagram(s) still arrived")

            if not post_switch(True):
                return seq
            left_on = True
            rx = Receiver()
            got_idr = False
            next_view = 0.0                                # the first view goes at once
            t0 = time.monotonic()
            while time.monotonic() - t0 < 2.0 and not got_idr:
                keepalive()
                try:
                    data, _ = v.recvfrom(HDR + VIDEO["chunk_bytes"] + 64)
                except socket.timeout:
                    continue
                ev, frame = rx.feed(data)
                if ev == Receiver.LOSS:
                    # The first frame of the new stream is the IDR; one chunk of it gone and
                    # nothing decodes until the next one — planned 10 s out on the car. Ask,
                    # and keep asking every view, exactly as the main loop does.
                    if not ask_key:
                        ask_key = True
                        v.sendto(view_key, self.video_addr)
                        if self.verbose:
                            print("    loss after the switch — asked for a keyframe")
                elif ev == Receiver.FRAME and 5 in nal_types(frame):
                    got_idr = True
            self.check(got_idr, "switched back on, but no keyframe within 2 s")
        finally:
            if left_on != was_on:
                try:
                    self.post_config({"video": video_cfg})
                except OSError as e:
                    self.check(False, f"restoring video.enabled={str(was_on).lower()}: {e}")
        return seq

    def run(self):
        rt, sid = self.open_session()
        rt.setblocking(False)     # R2: draining telemetry must never stall the 10 Hz drive loop
        print(f"session {sid} on {self.rt_addr}")
        v = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        v.settimeout(0.05)
        view = enc({K["proto"]: PROTO, K["type"]: T["view"], K["session"]: sid})
        view_key = enc({K["proto"]: PROTO, K["type"]: T["view"], K["session"]: sid, K["key"]: True})

        rx = Receiver()
        seq = 0
        t0 = time.monotonic()
        next_drive = next_view = t0
        frames = key_frames = bytes_rx = dgrams = 0
        streams = set()
        key_asked_at = None
        key_latencies = []
        first_frame_at = None
        sink = open(self.out, "wb") if self.out else None

        while time.monotonic() - t0 < self.seconds:
            now = time.monotonic()
            if now >= next_drive:
                seq += 1
                rt.sendto(enc({K["proto"]: PROTO, K["type"]: T["drive"], K["seq"]: seq,
                               K["throttle"]: 0, K["turn"]: 0}), self.rt_addr)
                next_drive = now + 1.0 / RT["command_hz"]
                try:
                    while True:
                        rt.recvfrom(RT["max_datagram"])      # drain telemetry
                except (socket.timeout, BlockingIOError):
                    pass
            if now >= next_view:
                v.sendto(view_key if key_asked_at else view, self.video_addr)
                next_view = now + VIDEO["subscribe_ms"] / 1000
            try:
                data, _ = v.recvfrom(HDR + VIDEO["chunk_bytes"] + 64)
            except socket.timeout:
                continue
            dgrams += 1
            bytes_rx += len(data)
            h = unpack_header(data)
            if self.check(h is not None, f"datagram {dgrams}: header rejected ({data[:12].hex()})"):
                streams.add(h["stream"])
            ev, frame = rx.feed(data)
            if ev == Receiver.LOSS:
                if key_asked_at is None:
                    key_asked_at = time.monotonic()
                    v.sendto(view_key, self.video_addr)
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
                if sink:
                    sink.write(frame)
                if self.verbose:
                    print(f"    frame {frames}: {len(frame)} B, nal {types}")

        # The window the figures are over ends here: the switch leg below runs its own clock
        # and counts nothing into these, so it must not stretch the divisor either.
        elapsed = time.monotonic() - t0
        if sink:
            sink.close()

        seq = self.check_switch(rt, v, view, view_key, seq)
        rt.sendto(enc({K["proto"]: PROTO, K["type"]: T["bye"], K["seq"]: seq + 1}), self.rt_addr)
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
                   help="the REST port with /config: default 80 (the car's, also through the adapter, "
                        "whose own API on 8080 has no /config), or 8080 (the mock's --port) when "
                        "the host is loopback")
    p.add_argument("--seconds", type=float, default=20.0)
    p.add_argument("--out", help="write the Annex B stream here (ffplay opens it)")
    p.add_argument("-v", "--verbose", action="store_true")
    a = p.parse_args()
    if a.http_port is None:
        a.http_port = 8080 if a.host in ("127.0.0.1", "localhost", "::1") else 80
    sys.exit(VideoConformance(a.host, a.rt_port, a.video_port, a.http_port, a.seconds, a.out, a.verbose).run())


if __name__ == "__main__":
    main()
