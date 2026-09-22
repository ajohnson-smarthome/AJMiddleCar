"""The mock's video channel: a `view` from the live session's owner starts the clip.

`sample.h264` (Annex B, Baseline, VIDEO["width"] x VIDEO["height"] — the car's cropped
16:9 picture, not its 4:3 sensor frame — IDR every 45 frames with SPS/PPS before each: the
shape the car's encoder produces) plays in a loop at VIDEO["fps"], one access unit per
frame, chunked with video_wire.chunks exactly as the car chunks. The frame does not leave in
one burst: like video_link.c, the encoder's tick puts it into a ring of RING_SLOTS slots and
a sender lets one chunk go every SEND_PERIOD_US, so a keyframe leaves as a trickle. A frame
that finds the ring full is skipped before it is "encoded" — `pos` and `frame` stay put, the
receiver sees no gap — and counted in `video.dropped`, the car's rule. `frame` keeps counting
across loops and `stream` stays put: to the receiver a looped clip is one long stream.
A `key:true` re-sends the most recent IDR access unit in place, without moving `pos` (R3):
seeking forward to the clip's next IDR could take up to a whole loop. Losses, reordering
and duplicates are seeded, like rt_link.Impairment, so a failing conformance run is
repeatable.
"""
import asyncio
import bisect
import random

import collections

from generated import PROTO, RT, VIDEO
from state import VIDEO_IDLE, VIDEO_OFF, VIDEO_STREAMING, parse_frame
from video_wire import chunks

K, T = RT["keys"], RT["types"]

# Mirrors of video_link.c, which has no key for them in the contract — test_mirrors.py
# compares them with the source.
RING_SLOTS = 6            # frames encoded and waiting for the sender
SEND_PERIOD_US = 3000     # one chunk leaves per period


def _nal_starts(b):
    out, i, n = [], 0, len(b)
    while i + 2 < n:
        if b[i] == 0 and b[i + 1] == 0:
            if b[i + 2] == 1:
                out.append((i, i + 3)); i += 3; continue
            if b[i + 2] == 0 and i + 3 < n and b[i + 3] == 1:
                out.append((i, i + 4)); i += 4; continue
        i += 1
    return out


def access_units(annexb):
    """[(bytes, is_idr)] — one entry per frame. A new unit starts at an SPS, or at a slice
    when the current unit already holds one. Start codes are kept: the car sends Annex B."""
    starts = _nal_starts(annexb)
    units, cur, cur_has_slice, cur_idr = [], b"", False, False
    for k, (sc, body) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else len(annexb)
        nal = annexb[sc:end]
        t = annexb[body] & 0x1F
        if t == 7 or (t in (1, 5) and cur_has_slice):
            if cur:
                units.append((cur, cur_idr))
            cur, cur_has_slice, cur_idr = b"", False, False
        cur += nal
        if t in (1, 5):
            cur_has_slice = True
        if t == 5:
            cur_idr = True
    if cur:
        units.append((cur, cur_idr))
    return units


def timeout_s():
    return VIDEO["subscribe_timeout_ms"] / 1000


class VideoLink(asyncio.DatagramProtocol):
    def __init__(self, car, link, sample, loss_pct=0.0, reorder_pct=0.0, dup_pct=0.0, seed=1,
                 verbose=False, loop=None):
        self.car = car
        self.link = link                     # the rt session: a view is accepted only from its sid
        self.units = access_units(sample)
        if not self.units:
            raise ValueError("the sample has no frames")
        # R3: precomputed so a `want_key` tick can answer in O(log n) without moving `pos` —
        # searching forward to the *next* IDR could cost a whole loop (up to keyint frames).
        self._idr_indices = [i for i, (_, key) in enumerate(self.units) if key]
        if not self._idr_indices:
            raise ValueError("the sample has no IDR frame")
        self.loss_pct, self.reorder_pct, self.dup_pct = loss_pct, reorder_pct, dup_pct
        self.rng = random.Random(seed)
        self.verbose = verbose
        self.loop = loop if loop is not None else asyncio.get_running_loop()
        self.transport = None
        self.peer = None
        self.sid = None                      # the rt session the stream opened under
        self.last_view = None
        self.stream = 0
        self.frame = 0
        self.pos = 0
        self.want_key = False
        self._held = None                    # a datagram delayed by one slot (reordering)
        # Encoded frames waiting for the sender, oldest first: each is a list of datagrams,
        # the head one partly sent. Never longer than RING_SLOTS.
        self._ring = collections.deque()
        self._sent_bytes = 0
        self._sent_frames = 0
        self._sec_at = self.loop.time()

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        f = parse_frame(data)
        if f is None or f[K["type"]] != T["view"] or f.get(K["proto"]) != PROTO:
            return
        if self.link.session is None or f[K["session"]] != self.link.session:
            return
        if self.car.video_state == VIDEO_OFF:
            return                          # no sensor since boot (`--camera off`): every view ignored
        if not self.car.config["video"]["enabled"]:
            return                          # the switch: a view opens nothing, whoever sends it
        now = self.loop.time()
        if self.peer is None:
            self.stream = (self.stream + 1) & 0xFF
            self.frame = 0
            self.pos = 0
            self.sid = f[K["session"]]
            self.car.video_state = VIDEO_STREAMING
            print(f"video: view from {addr[0]}:{addr[1]} — streaming (stream {self.stream})")
        elif f[K["session"]] != self.sid:
            # The stream belongs to the sid it opened under (video_sub.h, AJM-40): the new
            # owner's view is not a refresh of the old owner's stream — taking it would carry
            # the stream, mid-GOP, to the new address. The next tick ends the stream (owner
            # changed), and the new owner's next view opens one of its own, from an IDR.
            return
        elif addr != self.peer:
            print(f"video: viewer moved to {addr[0]}:{addr[1]}")
        self.peer = addr
        self.last_view = now
        if f.get(K["key"]):
            self.want_key = True

    def _stop(self, why):
        if self.peer is not None:
            print(f"video: {why} — stream ends")
        self.peer = None
        self.sid = None
        self.last_view = None
        self.car.video_state = VIDEO_IDLE
        self.car.video_fps = self.car.video_kbps = 0
        # A datagram held for reordering must not survive into the next stream — flushed
        # by a future _emit, it would carry this stream's (now stale) `stream` number.
        # Nor may the frames still in the ring: the car's stream_close empties it too.
        self._held = None
        self._ring.clear()

    def _emit(self, dgram):
        """One datagram through the impairments and out."""
        if self.loss_pct and self.rng.random() * 100 < self.loss_pct:
            return
        if self.dup_pct and self.rng.random() * 100 < self.dup_pct:
            self.transport.sendto(dgram, self.peer)
            self._sent_bytes += len(dgram)
        if self.reorder_pct and self.rng.random() * 100 < self.reorder_pct and self._held is None:
            self._held = dgram              # goes out after the next one
            return
        self.transport.sendto(dgram, self.peer)
        self._sent_bytes += len(dgram)
        if self._held is not None:
            held, self._held = self._held, None
            self.transport.sendto(held, self.peer)
            self._sent_bytes += len(held)

    def _last_idr_at_or_before(self, pos):
        """R3 controller ruling: the clip is 3 s with one IDR per loop, so seeking forward to
        the *next* IDR could delay a requested keyframe by up to 3 s — past the conformance's
        < 1 s budget. Instead replay the most recent IDR access unit again (a valid IDR with
        SPS/PPS; the decoder does not care that it repeats), wrapping to the end of the clip
        when `pos` precedes the first IDR."""
        j = bisect.bisect_right(self._idr_indices, pos) - 1
        return self._idr_indices[j] if j >= 0 else self._idr_indices[-1]

    def tick(self, now):
        """One period of the encoder: the next frame into the ring. Returns True when there was nothing to send: no viewer,
        or the stream just ended (session over, owner changed, viewer gone, switch off)."""
        if self.peer is None:
            return True
        if self.link.session != self.sid:
            # Nobody (bye, idled out) or somebody else (evicted by another hello): either
            # ends the stream on this tick, not at the timeout — the car's rule.
            self._stop("session over" if self.link.session is None else "owner changed")
            return True
        if not self.car.config["video"]["enabled"]:
            self._stop("video switched off")   # the car's rule: within one tick, not the timeout
            return True
        if now - self.last_view > timeout_s():
            self._stop("viewer gone")
            return True
        if len(self._ring) >= RING_SLOTS:
            # The sender is behind: skipped before the "encoder" sees it, like the car —
            # `pos`, `frame` and a pending `want_key` all stay for the next tick, so the
            # receiver sees no gap and the clip's references stay whole.
            self.car.video_dropped += 1
            return False
        if self.want_key:
            # R3: repeat the last IDR encountered, in place — `pos` does not move, so the
            # clip resumes from exactly where it was on the next tick.
            self.want_key = False
            idr_idx = self._last_idr_at_or_before(self.pos)
            au, key = self.units[idr_idx][0], True
        else:
            au, key = self.units[self.pos]
            self.pos = (self.pos + 1) % len(self.units)
        self._ring.append(list(chunks(au, self.stream, self.frame, key, int(now * 1000) & 0xFFFFFFFF)))
        self.frame = (self.frame + 1) & 0xFFFF
        if now - self._sec_at >= 1.0:
            self.car.video_fps = self._sent_frames
            self.car.video_kbps = int(self._sent_bytes * 8 / 1000 / (now - self._sec_at))
            self._sent_frames = self._sent_bytes = 0
            self._sec_at = now
        return False

    def send_tick(self):
        """One period of the sender: the next chunk of the oldest frame in the ring. A frame
        counts toward `fps` when its last chunk leaves — at the sender, as on the car."""
        if not self._ring or self.peer is None:
            return
        frame = self._ring[0]
        self._emit(frame.pop(0))
        if not frame:
            self._ring.popleft()
            self._sent_frames += 1

    async def _every(self, period, step):
        """`step` on a fixed beat. A beat missed by more than a period is lost, not made up
        in a burst — the car's pace timer notifies a task that takes the notification once."""
        next_at = self.loop.time()
        while True:
            next_at += period
            now = self.loop.time()
            if next_at < now:
                next_at = now
            await asyncio.sleep(next_at - now)
            step()

    async def run(self):
        await asyncio.gather(
            self._every(1.0 / VIDEO["fps"], lambda: self.tick(self.loop.time())),
            self._every(SEND_PERIOD_US / 1e6, self.send_tick))
