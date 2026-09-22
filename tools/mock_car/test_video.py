#!/usr/bin/env python3
"""Splitting an Annex B file into the frames the mock sends, and the trickle they leave in."""
import os
import pathlib
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from video import RING_SLOTS, SEND_PERIOD_US, VideoLink, _nal_starts, access_units   # noqa: E402
from video_wire import unpack_header   # noqa: E402
import json
from generated import PROTO, RT, VIDEO   # noqa: E402
K, T = RT["keys"], RT["types"]

SC = b"\x00\x00\x00\x01"
SPS, PPS, SEI = b"\x67\x42\x00\x1f", b"\x68\xce\x38\x80", b"\x06\x05\x01"
IDR, P = b"\x65\x88\x84", b"\x41\x9a\x02"


class _Bits:
    """An RBSP bit reader with the two Exp-Golomb codes an SPS is written in."""

    def __init__(self, rbsp):
        self.b, self.i = rbsp, 0

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.b[self.i >> 3] >> (7 - (self.i & 7))) & 1)
            self.i += 1
        return v

    def ue(self):
        zeros = 0
        while self.u(1) == 0:
            zeros += 1
        return (1 << zeros) - 1 + self.u(zeros)

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)


def sps_size(annexb):
    """(width, height) from the first SPS in an Annex B stream — ITU-T H.264 7.3.2.1.1, read
    far enough to reach the picture size and the cropping rectangle."""
    for _, body in _nal_starts(annexb):
        if annexb[body] & 0x1F == 7:
            break
    else:
        raise ValueError("no SPS")
    # Strip emulation prevention: 00 00 03 xx -> 00 00 xx (7.4.1).
    rbsp = re.sub(rb"\x00\x00\x03(?=[\x00-\x03])", b"\x00\x00", annexb[body + 1:body + 64])
    r = _Bits(rbsp)
    profile_idc = r.u(8)
    r.u(8); r.u(8)                                  # constraint flags, level_idc
    r.ue()                                          # seq_parameter_set_id
    chroma_format_idc = 1
    if profile_idc in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        chroma_format_idc = r.ue()
        if chroma_format_idc == 3:
            r.u(1)                                  # separate_colour_plane_flag
        r.ue(); r.ue(); r.u(1)                      # bit depths, transform bypass
        if r.u(1):                                  # seq_scaling_matrix_present_flag
            raise ValueError("scaling matrices: not a Baseline SPS")
    r.ue()                                          # log2_max_frame_num_minus4
    poc_type = r.ue()
    if poc_type == 0:
        r.ue()
    elif poc_type == 1:
        r.u(1); r.se(); r.se()
        for _ in range(r.ue()):
            r.se()
    r.ue(); r.u(1)                                  # max_num_ref_frames, gaps allowed
    width_mbs = r.ue() + 1
    height_map_units = r.ue() + 1
    frame_mbs_only = r.u(1)
    if not frame_mbs_only:
        r.u(1)                                      # mb_adaptive_frame_field_flag
    r.u(1)                                          # direct_8x8_inference_flag
    crop = [r.ue() for _ in range(4)] if r.u(1) else [0, 0, 0, 0]
    unit_x = 1 if chroma_format_idc in (0, 3) else 2
    unit_y = (1 if chroma_format_idc in (0, 3) else 2) * (2 - frame_mbs_only)
    if chroma_format_idc == 2:
        unit_y //= 2
    width = width_mbs * 16 - (crop[0] + crop[1]) * unit_x
    height = (2 - frame_mbs_only) * height_map_units * 16 - (crop[2] + crop[3]) * unit_y
    return width, height


class SpsSize(unittest.TestCase):
    """Three SPS as x264 writes them (Constrained Baseline 3.1, VUI, emulation-prevention
    bytes inside): whole macroblocks, a cropped bottom edge, a cropped bottom edge at a
    different width."""

    def test_reads_a_baseline_sps(self):
        # 1280x720 — 80x45 macroblocks, no cropping.
        sps = bytes.fromhex("6742c01fd9005005bb0110000003001000000301e0f1832480")
        self.assertEqual(sps_size(SC + sps), (1280, 720))

    def test_honours_the_cropping_rectangle(self):
        # 1920x1080 — 68 map units tall, 8 rows cropped off the bottom.
        sps = bytes.fromhex("6742c01fdb01e0089f970110000003001000000301e0f1832e")
        self.assertEqual(sps_size(SC + sps), (1920, 1080))
        # 640x360 — 23 map units tall, 8 rows cropped, and a `00 00 03` earlier in the RBSP.
        sps = bytes.fromhex("6742c01fd900a02ff970110000030001000003001e0f183248")
        self.assertEqual(sps_size(SC + sps), (640, 360))

    def test_raises_without_an_sps(self):
        with self.assertRaises(ValueError):
            sps_size(SC + PPS + SC + IDR)


class AccessUnits(unittest.TestCase):
    def test_idr_with_headers_then_p_frames(self):
        stream = SC + SPS + SC + PPS + SC + SEI + SC + IDR + SC + P + SC + P + SC + SPS + SC + PPS + SC + IDR
        aus = access_units(stream)
        self.assertEqual([k for _, k in aus], [True, False, False, True])
        self.assertEqual(aus[0][0], SC + SPS + SC + PPS + SC + SEI + SC + IDR)
        self.assertEqual(aus[1][0], SC + P)
        self.assertEqual(aus[3][0], SC + SPS + SC + PPS + SC + IDR)

    def test_three_byte_start_codes_and_aud(self):
        stream = b"\x00\x00\x01\x09\xf0" + b"\x00\x00\x01" + IDR + b"\x00\x00\x01\x09\xf0" + b"\x00\x00\x01" + P
        aus = access_units(stream)
        self.assertEqual(len(aus), 2)
        self.assertTrue(aus[0][1] and not aus[1][1])

    def test_the_sample_is_what_the_car_would_send(self):
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sample.h264")
        aus = access_units(open(path, "rb").read())
        self.assertGreaterEqual(len(aus), 40)
        self.assertTrue(aus[0][1], "the clip must open with an IDR")
        for au, key in aus:
            if key:
                self.assertIn(b"\x67", au[:16], "every IDR carries SPS/PPS")
        self.assertLessEqual(os.path.getsize(path), 400 * 1024)
        # AJM-110: the wire carries the car's cropped 16:9 picture, VIDEO["width"] x
        # VIDEO["height"] — a 4:3 clip would teach the simulator a frame the car never sends.
        self.assertEqual(sps_size(aus[0][0]), (VIDEO["width"], VIDEO["height"]),
                         "the clip's SPS must name the contract's picture size")


class _FakeLoop:
    now = 0.0

    def time(self):
        return self.now


class _FakeTransport:
    def sendto(self, data, addr):
        pass


class _Recorder:
    """A transport that remembers when each datagram left, by the fake loop's clock."""

    def __init__(self, loop):
        self.loop, self.sent = loop, []

    def sendto(self, data, addr):
        self.sent.append((self.loop.now, unpack_header(data)))


class _FakeLink:
    session = "sid"


class _FakeCar:
    video_state = "idle"
    video_fps = 0
    video_kbps = 0
    video_dropped = 0
    config = {"video": {"bitrate_kbps": 2500, "enabled": True}}


class StopClearsHeldDatagram(unittest.TestCase):
    """A datagram delayed for reordering must not survive a stopped stream — flushed by a
    future _emit, it would carry the ended stream's (now stale) `stream` number into
    whatever streams next."""

    def test_stop_clears_a_reorder_held_datagram(self):
        sample = SC + SPS + SC + PPS + SC + IDR
        v = VideoLink(_FakeCar(), _FakeLink(), sample, loop=_FakeLoop())
        v.transport = _FakeTransport()
        v.peer = ("127.0.0.1", 4211)
        v._held = b"stale datagram from the ended stream"
        v._stop("test stop")
        self.assertIsNone(v._held)


def _view(sid="sid", key=False):
    f = {K["proto"]: PROTO, K["type"]: T["view"], K["session"]: sid}
    if key:
        f[K["key"]] = True
    return json.dumps(f).encode()


class TheSwitch(unittest.TestCase):
    """`video.enabled` false: a view opens nothing, and a stream that is running ends on the
    next tick — the car's rule, so the conformance sweep can tell a car that ignores the
    switch from one that honours it."""

    def test_off_ignores_the_owners_view(self):
        car = _FakeCar()
        car.config = {"video": {"bitrate_kbps": 2500, "enabled": False}}
        v = VideoLink(car, _FakeLink(), SC + SPS + SC + PPS + SC + IDR, loop=_FakeLoop())
        v.transport = _FakeTransport()
        v.datagram_received(_view(), ("127.0.0.1", 40000))
        self.assertIsNone(v.peer)
        self.assertEqual(car.video_state, "idle")

    def test_switching_off_mid_stream_ends_it(self):
        car = _FakeCar()
        car.config = {"video": {"bitrate_kbps": 2500, "enabled": True}}
        v = VideoLink(car, _FakeLink(), SC + SPS + SC + PPS + SC + IDR, loop=_FakeLoop())
        v.transport = _FakeTransport()
        v.datagram_received(_view(), ("127.0.0.1", 40000))
        self.assertIsNotNone(v.peer)
        car.config["video"]["enabled"] = False
        self.assertTrue(v.tick(0.0))          # one tick of run(): stopped
        self.assertIsNone(v.peer)
        self.assertEqual(car.video_state, "idle")


class NoSensor(unittest.TestCase):
    """`car/video-stream`: no sensor at boot is `video.state: off` until the next reboot,
    and in `off` every `view` is ignored — no subscription, nothing on the port (AJM-116)."""

    def test_off_ignores_the_owners_view(self):
        car = _FakeCar()
        car.video_state = "off"
        v = VideoLink(car, _FakeLink(), SC + SPS + SC + PPS + SC + IDR, loop=_FakeLoop())
        v.transport = _FakeTransport()
        v.datagram_received(_view(key=True), ("127.0.0.1", 40000))
        self.assertIsNone(v.peer)
        self.assertFalse(v.want_key)
        self.assertEqual(car.video_state, "off")
        self.assertTrue(v.tick(0.0), "nothing to send")
        self.assertEqual(car.video_state, "off", "a tick does not turn off into idle")


class OwnerChange(unittest.TestCase):
    """A stream belongs to the sid it opened under (AJM-40): once another hello has taken
    the rt session, it ends on the next tick — not at `subscribe_timeout_ms` — and the
    new owner's `view` is not a refresh of it but the start of a stream of its own."""

    def _streaming(self, sid="A"):
        car, link = _FakeCar(), _FakeLink()
        link.session = sid
        v = VideoLink(car, link, SC + SPS + SC + PPS + SC + IDR + SC + P, loop=_FakeLoop())
        v.transport = _FakeTransport()
        v.datagram_received(_view(sid), ("127.0.0.1", 40000))
        self.assertEqual(car.video_state, "streaming")
        self.assertFalse(v.tick(0.0))          # one frame out: the stream is live
        return car, link, v

    def test_eviction_ends_the_stream_on_the_next_tick(self):
        car, link, v = self._streaming("A")
        link.session = "B"                      # another hello took the session
        self.assertTrue(v.tick(0.1))            # well inside subscribe_timeout_ms
        self.assertIsNone(v.peer)
        self.assertEqual(car.video_state, "idle")
        self.assertEqual((car.video_fps, car.video_kbps), (0, 0))

    def test_session_over_still_ends_the_stream(self):
        car, link, v = self._streaming("A")
        link.session = None                     # bye, or idled out
        self.assertTrue(v.tick(0.1))
        self.assertIsNone(v.peer)
        self.assertEqual(car.video_state, "idle")

    def test_the_new_owners_view_is_not_a_refresh_of_the_old_stream(self):
        car, link, v = self._streaming("A")
        link.session = "B"
        v.loop.now = 0.1
        v.datagram_received(_view("B", key=True), ("127.0.0.1", 40001))
        self.assertEqual(v.peer, ("127.0.0.1", 40000), "the running stream did not move")
        self.assertEqual(v.last_view, 0.0, "nor was its deadline refreshed")
        self.assertFalse(v.want_key, "nor did the stranger's key:true force an IDR into it")
        self.assertTrue(v.tick(0.1), "the same tick ends it")

    def test_the_new_owner_opens_a_stream_of_its_own(self):
        car, link, v = self._streaming("A")
        first = v.stream
        link.session = "B"
        self.assertTrue(v.tick(0.1))
        v.datagram_received(_view("B"), ("127.0.0.1", 40001))
        self.assertEqual(v.peer, ("127.0.0.1", 40001))
        self.assertEqual(v.stream, (first + 1) & 0xFF, "a new stream number")
        self.assertEqual((v.frame, v.pos), (0, 0), "from the top of the clip: its IDR")
        self.assertEqual(car.video_state, "streaming")


SAMPLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sample.h264")


class Trickle(unittest.TestCase):
    """video_link.c's sender: an encoded frame goes into a ring of RING_SLOTS and leaves one
    chunk every SEND_PERIOD_US — a trickle, not a burst, so the conformance's pacing leg
    judges the mock the way it judges the car (AJM-168). Time is the fake loop's: the
    encoder's tick at VIDEO["fps"] and the sender's at the step, interleaved by their clocks."""

    def _streaming(self, sample):
        loop = _FakeLoop()
        car = _FakeCar()
        v = VideoLink(car, _FakeLink(), sample, loop=loop)
        v.transport = _Recorder(loop)
        v.datagram_received(_view(), ("127.0.0.1", 40000))
        return car, v

    def _run(self, v, seconds):
        step, period = SEND_PERIOD_US / 1e6, 1.0 / VIDEO["fps"]
        next_enc = next_send = 0.0
        while min(next_enc, next_send) < seconds:
            if next_enc <= next_send:
                v.loop.now = next_enc
                v.last_view = next_enc               # the viewer keeps its views coming
                v.tick(next_enc)
                next_enc += period
            else:
                v.loop.now = next_send
                v.send_tick()
                next_send += step

    def test_a_frame_leaves_one_chunk_per_step(self):
        car, v = self._streaming(pathlib.Path(SAMPLE).read_bytes())
        self._run(v, len(v.units) / VIDEO["fps"] * 2)        # the clip twice round
        frames = {}
        for t, h in v.transport.sent:
            frames.setdefault(h["frame"], []).append((t, h["chunk"], h["count"]))
        multi = [f for f in frames.values() if f[0][2] > 1]
        self.assertTrue(multi, "the sample has multi-chunk frames")
        for f in multi:
            self.assertEqual([c for _, c, _ in f], list(range(f[0][2])), "whole and in order")
            for (a, _, _), (b, _, _) in zip(f, f[1:]):
                self.assertAlmostEqual((b - a) * 1e6, SEND_PERIOD_US, places=3)

    def test_the_stock_clip_drops_nothing(self):
        car, v = self._streaming(pathlib.Path(SAMPLE).read_bytes())
        self._run(v, len(v.units) / VIDEO["fps"] * 2)
        self.assertEqual(car.video_dropped, 0, "six slots carry the clip at the contract's fps")
        sent = sorted({h["frame"] for _, h in v.transport.sent})
        self.assertEqual(sent, list(range(len(sent))), "no frame number skipped")

    def test_a_full_ring_skips_the_frame_and_counts_it(self):
        car, v = self._streaming(SC + SPS + SC + PPS + SC + IDR + SC + P)
        for _ in range(RING_SLOTS):
            self.assertFalse(v.tick(0.0))
        pos, frame = v.pos, v.frame
        v.want_key = True
        self.assertFalse(v.tick(0.0), "a skipped frame does not end the stream")
        self.assertEqual(car.video_dropped, 1)
        self.assertEqual((v.pos, v.frame), (pos, frame), "skipped before the encoder: no gap")
        self.assertTrue(v.want_key, "the keyframe asked for is still owed")
        self.assertEqual(v.transport.sent, [], "the encoder's tick sends nothing itself")
        v.send_tick()
        self.assertFalse(v.tick(0.0), "a freed slot takes the next frame")
        self.assertEqual(car.video_dropped, 1)

    def test_a_stopped_stream_leaves_nothing_in_the_ring(self):
        car, v = self._streaming(SC + SPS + SC + PPS + SC + IDR + SC + P)
        v.tick(0.0)
        v._stop("test stop")
        v.send_tick()
        self.assertEqual(v.transport.sent, [], "no chunk of the ended stream after its end")


if __name__ == "__main__":
    unittest.main()
