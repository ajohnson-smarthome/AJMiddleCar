#!/usr/bin/env python3
"""video_wire.py against the C receiver's cases (firmware/car/core/test/test_video_wire.c)
and the header vectors in contract/car-api.json — the same bytes, the same verdicts."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from generated import VIDEO                                          # noqa: E402
from video_wire import (CHUNK, FLAG_KEY, HDR, PROTO, Receiver, chunk_count, chunks,  # noqa: E402
                        frame_newer, pack_header, unpack_header)


def payload(n, seed=1):
    return bytes((seed + i * 7) & 0xFF for i in range(n))


def send(rx, stream, frame, key, data, order=None, skip=None):
    """Feed one frame's chunks in `order` (indices), skipping `skip`; the last event."""
    ds = chunks(data, stream, frame, key, 1000 + frame)
    ev, out = Receiver.NONE, None
    for i in (order if order is not None else range(len(ds))):
        if i == skip:
            continue
        ev, out = rx.feed(ds[i])
    return ev, out


class Header(unittest.TestCase):
    def test_vectors_round_trip_and_reject(self):
        for vec in VIDEO["vectors"]:
            raw = bytes.fromhex(vec["bytes"])
            h = unpack_header(raw)
            if vec["valid"]:
                self.assertEqual(h, vec["header"], vec["name"])
                self.assertEqual(pack_header(**vec["header"]), raw, vec["name"])
            else:
                self.assertIsNone(h, vec["name"])
        self.assertIsNone(unpack_header(bytes.fromhex(VIDEO["vectors"][0]["bytes"])[:-1]))

    def test_frame_order_wraps(self):
        self.assertTrue(frame_newer(1, 0) and frame_newer(0, 65535) and frame_newer(32768, 1))
        self.assertFalse(frame_newer(65535, 0) or frame_newer(5, 5) or frame_newer(1, 32768))

    def test_chunking(self):
        self.assertEqual([chunk_count(n) for n in (0, 1, CHUNK, CHUNK + 1, CHUNK * 255, CHUNK * 255 + 1)],
                         [0, 1, 1, 2, 255, 0])
        ds = chunks(payload(3000), 1, 9, True, 5)
        self.assertEqual([len(d) for d in ds], [HDR + CHUNK, HDR + CHUNK, HDR + 3000 - 2 * CHUNK])
        self.assertEqual(unpack_header(ds[0])["count"], 3)
        self.assertEqual(unpack_header(ds[2])["chunk"], 2)
        self.assertEqual(unpack_header(ds[0])["flags"], FLAG_KEY)
        self.assertEqual(b"".join(d[HDR:] for d in ds), payload(3000))
        self.assertEqual(chunks(b"", 1, 0, False, 0), [])
        self.assertEqual(chunks(bytes(CHUNK * 255 + 1), 1, 0, False, 0), [])


class Receiving(unittest.TestCase):
    def test_the_c_scenarios(self):
        rx = Receiver()
        self.assertEqual(send(rx, 1, 0, False, payload(3000))[0], Receiver.NONE)      # P before any key
        ev, out = send(rx, 1, 1, True, payload(3000, 42))
        self.assertEqual((ev, out), (Receiver.FRAME, payload(3000, 42)))
        ev, out = send(rx, 1, 2, False, payload(2900, 7), order=[2, 0, 1])
        self.assertEqual((ev, out), (Receiver.FRAME, payload(2900, 7)))
        ev, out = send(rx, 1, 3, False, payload(3000), order=[0, 1, 1, 2])
        self.assertEqual((ev, len(out)), (Receiver.FRAME, 3000))
        self.assertEqual(send(rx, 1, 4, False, payload(3000), skip=1)[0], Receiver.NONE)
        self.assertEqual(send(rx, 1, 5, False, payload(3000), order=[0])[0], Receiver.LOSS)
        self.assertEqual(rx.dropped, 1)
        self.assertEqual(send(rx, 1, 5, False, payload(3000), order=[1, 2])[0], Receiver.NONE)
        self.assertEqual(send(rx, 1, 6, False, payload(3000))[0], Receiver.NONE)
        self.assertEqual(send(rx, 1, 7, True, payload(3000))[0], Receiver.FRAME)
        self.assertEqual(send(rx, 1, 8, False, payload(3000))[0], Receiver.FRAME)
        self.assertEqual(send(rx, 1, 7, True, payload(3000), order=[0])[0], Receiver.NONE)   # straggler
        self.assertEqual(send(rx, 1, 9, False, payload(3000))[0], Receiver.FRAME)
        self.assertEqual(send(rx, 1, 10, False, payload(3000), skip=2)[0], Receiver.NONE)
        ev, out = send(rx, 1, 11, True, payload(100))
        self.assertEqual((ev, len(out)), (Receiver.FRAME, 100))
        self.assertEqual(rx.dropped, 2)

    def test_edge_stream_and_corruption(self):
        rx = Receiver()
        self.assertEqual(send(rx, 2, 65535, True, payload(3000))[0], Receiver.FRAME)
        self.assertEqual(send(rx, 2, 0, False, payload(3000))[0], Receiver.FRAME)
        self.assertEqual(send(rx, 3, 0, False, payload(3000))[0], Receiver.NONE)            # new stream waits
        self.assertEqual(send(rx, 3, 1, True, payload(3000))[0], Receiver.FRAME)
        self.assertEqual(send(rx, 3, 2, False, payload(3000), order=[0])[0], Receiver.NONE)
        bad = pack_header(PROTO, 0, 3, 2, 1, 4, 0) + bytes(CHUNK)
        self.assertEqual(rx.feed(bad)[0], Receiver.LOSS)                                 # count disagrees
        mid = pack_header(PROTO, 0, 3, 3, 0, 3, 0)
        for d in (mid + bytes(100), mid, mid + bytes(CHUNK + 1)):
            self.assertEqual(rx.feed(d)[0], Receiver.BAD)
        self.assertEqual(rx.feed(bytes.fromhex("020103000007001e000c6539") + bytes(CHUNK))[0], Receiver.BAD)
        small = Receiver(cap=5000)
        self.assertEqual(small.feed(pack_header(PROTO, FLAG_KEY, 1, 0, 0, 200, 0) + bytes(CHUNK))[0], Receiver.LOSS)
        self.assertEqual(small.dropped, 1)

    def test_stream_change_mid_frame_discards_silently(self):
        """R7(a): a stream change while a frame is in progress. The abandoned stream-1
        frame must not report LOSS (rule 3 discards silently on a stream change), and
        dropped must not increase — this is a reset, not a loss."""
        rx = Receiver()
        self.assertEqual(send(rx, 1, 0, True, payload(3000))[0], Receiver.FRAME)
        ds = chunks(payload(3000), 1, 1, False, 1001)
        self.assertEqual(rx.feed(ds[0])[0], Receiver.NONE)          # stream 1 frame 1, in progress
        ev, out = send(rx, 2, 0, True, payload(3000, 9))
        self.assertEqual((ev, out), (Receiver.FRAME, payload(3000, 9)))
        self.assertEqual(rx.dropped, 0)

    def test_flags_mismatch_mid_frame_is_loss(self):
        """R7(b): a flags mismatch mid-frame (same count, same frame) is corruption,
        reported as LOSS with dropped incrementing."""
        rx = Receiver()
        self.assertEqual(send(rx, 1, 0, True, payload(3000))[0], Receiver.FRAME)
        ds = chunks(payload(3000, 7), 1, 1, False, 1001)          # frame 1: flags 0, count 3
        self.assertEqual(rx.feed(ds[0])[0], Receiver.NONE)
        mismatched = pack_header(PROTO, FLAG_KEY, 1, 1, 1, len(ds), 1001) + ds[1][HDR:]
        ev, _ = rx.feed(mismatched)
        self.assertEqual(ev, Receiver.LOSS)
        self.assertEqual(rx.dropped, 1)

    def test_bad_header_and_straggler_leave_state_untouched(self):
        """R7(c): a bad header and a straggler from an already-delivered frame must not
        reset wait_key or the frame bookkeeping — the next in-order P-frame still delivers."""
        rx = Receiver()
        self.assertEqual(send(rx, 1, 0, True, payload(3000))[0], Receiver.FRAME)
        self.assertEqual(send(rx, 1, 1, False, payload(3000))[0], Receiver.FRAME)
        bad = pack_header(0, 0, 1, 2, 0, 3, 0) + bytes(CHUNK)        # foreign proto
        self.assertEqual(rx.feed(bad)[0], Receiver.BAD)
        straggler = chunks(payload(3000), 1, 1, False, 1001)[0]      # already-delivered frame 1
        self.assertEqual(rx.feed(straggler)[0], Receiver.NONE)
        self.assertEqual(send(rx, 1, 2, False, payload(3000))[0], Receiver.FRAME)


if __name__ == "__main__":
    unittest.main()
