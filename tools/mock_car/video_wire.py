"""The video datagram — the Python of firmware/car/core/main/video_wire.h.

Header (12 bytes, big-endian): proto, flags, stream, reserved, frame u16, chunk, count,
captured_ms u32. The receiver's rules are the C receiver's, in the same order; the
scenarios in test_video_wire.py are the C test's, so a rule changed in one place fails
the other. The mock's video.py sends with `chunks`; conformance_video.py receives with
`Receiver`.
"""
import struct

from generated import VIDEO

_HEADER = struct.Struct(">BBBBHBBI")
HDR = VIDEO["header_bytes"]
CHUNK = VIDEO["chunk_bytes"]
PROTO = VIDEO["wire_proto"]
FLAG_KEY = 0x01
MAX_CHUNKS = 255

assert _HEADER.size == HDR


def pack_header(proto, flags, stream, frame, chunk, count, captured_ms):
    return _HEADER.pack(proto, flags, stream, 0, frame, chunk, count, captured_ms)


def unpack_header(data):
    """The header as a dict, or None when it is not one this receiver accepts. Strict:
    a set reserved byte or an undefined flag is a format nobody agreed on."""
    if len(data) < HDR:
        return None
    proto, flags, stream, reserved, frame, chunk, count, captured_ms = _HEADER.unpack_from(data)
    if proto != PROTO or flags & ~FLAG_KEY or reserved != 0 or count == 0 or chunk >= count:
        return None
    return {"proto": proto, "flags": flags, "stream": stream, "frame": frame,
            "chunk": chunk, "count": count, "captured_ms": captured_ms}


def frame_newer(a, b):
    """RFC 1982 on 16 bits: (int16)(a - b) > 0."""
    d = (a - b) & 0xFFFF
    return 0 < d < 0x8000


def chunk_count(n):
    if n == 0:
        return 0
    c = (n + CHUNK - 1) // CHUNK
    return 0 if c > MAX_CHUNKS else c


def chunks(payload, stream, frame, keyframe, captured_ms):
    """Every datagram of one frame, in order; [] when it does not fit 255 chunks."""
    n = chunk_count(len(payload))
    flags = FLAG_KEY if keyframe else 0
    return [pack_header(PROTO, flags, stream, frame & 0xFFFF, i, n, captured_ms & 0xFFFFFFFF)
            + payload[i * CHUNK:(i + 1) * CHUNK] for i in range(n)]


class Receiver:
    NONE, FRAME, LOSS, BAD = "none", "frame", "loss", "bad"

    def __init__(self, cap=MAX_CHUNKS * CHUNK):
        self.cap = cap
        self.dropped = 0
        self.bad = 0
        self._stream = None
        self._last = None          # newest frame finished or abandoned
        self._cur = None           # {frame, count, flags, parts: dict[int, bytes]}
        self._wait_key = True

    def _abandon(self):
        self._last = self._cur["frame"]
        self._cur = None
        self._wait_key = True
        self.dropped += 1

    def feed(self, dgram):
        h = unpack_header(dgram)
        if h is None:
            self.bad += 1
            return self.BAD, None
        body = dgram[HDR:]
        last = h["chunk"] == h["count"] - 1
        if not body or len(body) > CHUNK or (not last and len(body) != CHUNK):
            self.bad += 1
            return self.BAD, None

        if self._stream != h["stream"]:
            self._stream = h["stream"]
            self._cur = None
            self._last = None
            self._wait_key = True

        ev = self.NONE
        if self._cur is not None and h["frame"] != self._cur["frame"]:
            if not frame_newer(h["frame"], self._cur["frame"]):
                return self.NONE, None
            self._abandon()
            ev = self.LOSS
        if self._cur is None:
            if self._last is not None and not frame_newer(h["frame"], self._last):
                return ev, None
            self._cur = {"frame": h["frame"], "count": h["count"], "flags": h["flags"], "parts": {}}
            if h["count"] * CHUNK > self.cap:
                self._abandon()
                return self.LOSS, None
        elif h["count"] != self._cur["count"] or h["flags"] != self._cur["flags"]:
            self._abandon()
            return self.LOSS, None

        parts = self._cur["parts"]
        if h["chunk"] in parts:
            return ev, None
        parts[h["chunk"]] = body
        if len(parts) < self._cur["count"]:
            return ev, None

        done = self._cur
        self._cur = None
        self._last = done["frame"]
        if done["flags"] & FLAG_KEY:
            self._wait_key = False
        if self._wait_key:
            return ev, None
        return self.FRAME, b"".join(parts[i] for i in range(done["count"]))
