#!/usr/bin/env python3
"""Splitting an Annex B file into the frames the mock sends, one datagram burst each."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from video import VideoLink, access_units   # noqa: E402

SC = b"\x00\x00\x00\x01"
SPS, PPS, SEI = b"\x67\x42\x00\x1f", b"\x68\xce\x38\x80", b"\x06\x05\x01"
IDR, P = b"\x65\x88\x84", b"\x41\x9a\x02"


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


class _FakeLoop:
    def time(self):
        return 0.0


class _FakeTransport:
    def sendto(self, data, addr):
        pass


class _FakeLink:
    session = "sid"


class _FakeCar:
    video_state = "idle"
    video_fps = 0
    video_kbps = 0


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


if __name__ == "__main__":
    unittest.main()
