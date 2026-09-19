"""The mock car's state, with no server attached.

Everything here is clock-free: the caller passes `now` (seconds, monotonic). That is what
lets `test_state.py` drive a watchdog trip and a five-second retreat in microseconds, and
it keeps `mock_car.py` down to plumbing with no behaviour worth testing hidden in it.

It also holds the wire *shapes* — `parse_frame` and the predicates under it — because
they are pure, they are what `control_proto.c` is, and the only useful test of them is
"does this datagram mean here what it means on the car".

Nothing in this file writes a range, a default, a deadline or a field name: `DOMAINS`,
`RT`, `GROUPS` and `validate_config` come from `contract/car-api.json` via the generator,
which is the same source the firmware compiles. A literal here would be exactly the drift the schema exists to
prevent — the mock's old `/recover` default of off/3000, against the car's on/5000, is
why every simulator session taught that a car losing its link stops.
"""
import json
import math
import re
from collections import deque

from generated import (CALIBRATION, DEVICE, DOMAINS, ENVELOPE, GROUPS, PROTO, RT,
                       TELEMETRY_GROUPS, from_wire, to_wire, validate_config)

# Ownership of the actuator, lowest priority first — the `owner` field of
# GROUPS["motors"], the same list firmware/car/core/main/link.h's `link_src_t` is
# generated from. Position is rank on all three sides, and the per-name symbols
# below spell `link_src_t`'s values in `link_src_t`'s order.
_owner_field = next(f for f in GROUPS["motors"]["fields"] if f["name"] == "owner")
(OWNER_IDLE, OWNER_RECOVERING, OWNER_CONSOLE, OWNER_REMOTE, OWNER_CALIBRATION,
 OWNER_UPDATE, OWNER_SAFE_STOP) = _owner_field["values"]
PRIORITY = tuple(_owner_field["values"])

# The session id, as `parse_sid` in firmware/car/core/main/control_proto.c accepts it:
# non-empty, alphanumeric, and short enough to fit that file's CONTROL_SID_MAX with its
# NUL. Not in contract/car-api.json on any of the three sides, so it is mirrored here by
# hand — see the report. Anything else is not "an id the car will not like": it is a
# datagram the car drops whole, because the id is echoed into the hello reply and a
# quote in it would let the sender shape that JSON.
SID_MAX_CHARS = 15         # mirrors CONTROL_SID_MAX - 1 in firmware/car/core/main/control_proto.h
_SID_RE = re.compile(r"[A-Za-z0-9]{1,%d}\Z" % SID_MAX_CHARS)


def valid_sid(v):
    """Would control_parse_frame accept this `session` value?

    ASCII-only on purpose: Python's `str.isalnum()` says yes to "é" and C's `isalnum`
    says no, and the mock exists to be as strict as the car, not as strict as Python.
    """
    return isinstance(v, str) and _SID_RE.match(v) is not None


def number(v):
    """A JSON number, as cJSON and control_proto.c's `parse_num` see one, or None.

    A JSON boolean and a numeric *string* are both rejected. `float(True)` is 1.0 and
    `float("0.5")` is 0.5, so the obvious `float(v)` would drive on frames the car drops
    outright: its tokeniser stops at the first character outside "0123456789+-.eE", so
    `true` and `"0.5"` fail and take the whole datagram with them. A client that works
    in the simulator and is silently inert on hardware is the one failure this mock
    exists to prevent.
    """
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return None
    f = float(v)
    if not math.isfinite(f):        # NaN and the infinities are not commands
        return None
    return f


def clamp_axis(v):
    """The car clamps; a client that sends 1.5 gets 1.0, not a rejection."""
    f = number(v)
    return None if f is None else max(-1.0, min(1.0, f))


def seq_is_newer(seq, last):
    """The car's `(int32_t)(seq - last) > 0`, so a session that runs past 2^32 keeps going.

    Comparing the numbers directly would reject every frame after the wrap, which on a
    10 Hz stream is a link that dies once and never recovers.
    """
    delta = (seq - last) & 0xFFFFFFFF
    if delta >= 0x80000000:
        delta -= 0x100000000
    return delta > 0


def valid_seq(v):
    """A `seq` (or `proto`) is a uint32. A JSON boolean is an int in Python and is not
    one here.

    Used to be deliberately stricter than the car here: `control_proto.c`'s old
    `parse_u32` tokenised `1.5` down to `1` and accepted it. It no longer does —
    `json_int_shape` there now rejects a non-integer token exactly as this does,
    so the two sides are equally strict and this function is no longer the safe
    direction against a gap, just the shared rule stated twice.
    """
    return isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= 0xFFFFFFFF


def _no_duplicates(pairs):
    """json.loads object_pairs_hook: a key spelled twice drops the datagram.

    The car's scanner takes the *first* duplicate, json.loads keeps the *last* —
    byte-identical datagrams driving two implementations differently, in the worst
    case at different speeds. The shared rule (spec 2026-08-22, rule 5) is that a
    duplicate drops the frame. This hook fires at every nesting level, one notch
    stricter than the car's top-level-only detection — strictness in the mock is
    the safe direction, as with valid_seq.
    """
    d = {}
    for k, v in pairs:
        if k in d:
            raise ValueError(f"duplicate key {k!r}")
        d[k] = v
    return d


_APP_DESC_OFFSET = 32          # esp_image_header_t (24 B) + esp_image_segment_header_t (8 B)
_APP_DESC_MAGIC = 0xABCD5432   # esp_app_desc.h ESP_APP_DESC_MAGIC_WORD


def parse_image_version(data):
    """The version a real ESP application image embeds, or None.

    esp_app_desc_t sits at the start of the first segment: image header (24 bytes,
    first byte 0xE9), one segment header (8), then the descriptor — magic_word first,
    version[32] at its offset 16, i.e. absolute offset 48. This is what the car will
    report after flashing these bytes, so the simulator must report the same — a
    synthetic bump hid every asset-vs-tag mismatch from rehearsal (2026-08-23 audit).
    """
    if len(data) < _APP_DESC_OFFSET + 48 or data[0] != 0xE9:
        return None
    magic = int.from_bytes(data[_APP_DESC_OFFSET:_APP_DESC_OFFSET + 4], "little")
    if magic != _APP_DESC_MAGIC:
        return None
    raw = data[_APP_DESC_OFFSET + 16:_APP_DESC_OFFSET + 48]
    ver = raw.split(b"\x00", 1)[0].decode("ascii", "replace")
    return ver or None


def build_number(fw):
    """The integer after `+` in `fw` (e.g. 9001 from `v1.0+9001`), or -1 when fw
    carries none — `device.build` in the hello reply and in /status."""
    _, sep, build = fw.rpartition("+")
    return int(build) if sep and build.isdigit() else -1


def parse_frame(data, max_command=None):
    """One inbound datagram -> a dict of the fields it carried, or None to drop it.

    The mock's `control_parse_frame` (firmware/car/core/main/control_proto.c). Same answer for
    the same bytes is the whole point, so the rules are the car's, not JSON's:

      * over the *command* cap -> dropped. `max_datagram` sizes a receive buffer; what
        the car agrees to act on is `max_command`, and the difference is the room a
        telemetry frame needs on the way out.
      * `type` is required and must be one the app sends: hello, drive, bye, view.
        Anything else (missing, unknown, or one of the car->app types) has nothing to
        act on.
      * every key that is present must parse, or the whole datagram is dropped. A frame
        with a good `throttle` and a broken `seq` is not a command with a missing
        sequence number; it is corrupt.
      * `throttle` and `turn` come as a pair. One axis without the other is a truncated
        frame, not an instruction to hold the other at zero.
      * hello and view need `session`; view may carry `key`. drive needs `seq` and both
        axes; bye needs `seq`. Axes on a bye are tolerated (the app sends them) but
        never required.

    Range is deliberately not checked here either — the arbiter clamps.
    """
    cap = RT["max_command"] if max_command is None else max_command
    if not data or len(data) > cap:
        return None
    try:
        frame = json.loads(data, object_pairs_hook=_no_duplicates)
    except (ValueError, UnicodeDecodeError):
        return None
    if not isinstance(frame, dict):
        return None
    K, T = RT["keys"], RT["types"]
    kind = frame.get(K["type"])
    if kind not in (T["hello"], T["drive"], T["bye"], T["view"]):
        return None
    out = {K["type"]: kind}
    for key in (K["proto"], K["seq"]):
        if key in frame:
            if not valid_seq(frame[key]):
                return None
            out[key] = frame[key]
    if K["session"] in frame:
        if not valid_sid(frame[K["session"]]):
            return None
        out[K["session"]] = frame[K["session"]]
    if K["key"] in frame:
        if not isinstance(frame[K["key"]], bool):
            return None
        out[K["key"]] = frame[K["key"]]
    has_t, has_y = K["throttle"] in frame, K["turn"] in frame
    if has_t or has_y:
        if not (has_t and has_y):
            return None
        t, y = number(frame[K["throttle"]]), number(frame[K["turn"]])
        if t is None or y is None:
            return None
        out[K["throttle"]], out[K["turn"]] = t, y
    if kind == T["hello"] and K["session"] not in out:
        return None
    if kind == T["view"] and K["session"] not in out:
        return None
    if kind == T["drive"] and (K["seq"] not in out or K["throttle"] not in out):
        return None
    if kind == T["bye"] and K["seq"] not in out:
        return None
    return out


class CarState:
    """Config, the control watchdog, the retreat, and everything telemetry reports."""

    # Constants that belong to the firmware's behaviour rather than to the wire, kept at
    # the values firmware/car/core/main defines so the mock retreats for the same duration the
    # car does.
    # None of these three is in contract/car-api.json, so they are mirrored by hand from
    # the file named beside each one. See the report for the schema additions that would
    # let them be generated instead.
    MOVE_EPS = 0.02        # firmware/car/core/main/recovery.c: below this a sample is stationary
    SEG_MAX_MS = 250       # firmware/car/core/main/recovery.h RECOVER_SEG_MAX_MS: per-segment cap
    CALIB_HOLD_MS = 600    # firmware/car/core/main/link.h LINK_HOLD_CALIB_MS: one pulse

    def __init__(self, device=DEVICE, fw="v1.0+9000", now=0.0):
        self.device = device
        self.fw = fw
        self.rollback = False    # the previous "OTA" was rolled back — /status mirrors it
        self.nvs_wiped = False   # one-boot flag after a simulated NVS migration erase
        self.rssi = -58
        self.heap = 200000
        # Internal integers throughout — a `fixed` field (gear_ratio) is held as ratio x100,
        # exactly as `defaults` already is in the schema; `to_wire`/`from_wire` are the only
        # places that ever see the decimal the wire uses.
        self.config = {key: dict(d["defaults"]) for key, d in DOMAINS.items()}

        self._started = now
        self._now = now
        self._t = 0.0
        self._y = 0.0
        self._history = deque()        # (t, y, ts), oldest first
        self._armed = False            # the watchdog only trips on traffic that then stops
        self._last_rx = now
        self._wdt_trips = 0
        self._retreating = False
        self._retreat_until = now
        self._owner = OWNER_IDLE
        self._owner_until = None       # None means the grant is sticky
        self._calibrated = False
        self._calibration = {}         # corner -> (pair, inverted)
        self._bus_ok = True
        self._tele_seq = 0
        # The video channel's counters, written by video.py: `idle` and zeros until a
        # viewer subscribes, then whatever the looped clip is sending.
        self.video_state = "idle"
        self.video_fps = 0
        self.video_kbps = 0
        self.video_dropped = 0

    # ---- what the outside reads ------------------------------------------------

    @property
    def wdt_trips(self):
        return self._wdt_trips

    @property
    def ctl(self):
        return self._owner

    @property
    def bus_ok(self):
        return self._bus_ok

    @property
    def calibrated(self):
        return self._calibrated

    @property
    def retreating(self):
        return self._retreating

    @property
    def command(self):
        """The (t, y) the actuator is holding — negated history while retreating."""
        return (self._t, self._y)

    @property
    def history_len(self):
        return len(self._history)

    @property
    def armed(self):
        """The control watchdog is armed — a command was accepted since the last
        adopt, trip or goodbye."""
        return self._armed

    # ---- configuration ---------------------------------------------------------

    def apply_config(self, body):
        """Validate the WHOLE body, then apply every present domain. Returns (True, None)
        or (False, (code, field, message)) — the car's two-pass rule: a body that is
        half right changes nothing.
        """
        ok, err = validate_config(body)
        if not ok:
            return False, err
        for key in body:
            self.config[key] = from_wire(key, body[key])
        return True, None

    def config_wire(self):
        """Every domain as GET /config answers it: fixed fields as decimals."""
        return {key: to_wire(key, values) for key, values in self.config.items()}

    # ---- the control channel ---------------------------------------------------

    def note_command(self, t, y, now):
        """One accepted control frame: breadcrumb, watchdog, actuator.

        Returns False for an axis that is not a finite number, having changed nothing —
        a malformed datagram must not feed the watchdog it was too broken to command.
        """
        t, y = clamp_axis(t), clamp_axis(y)
        if t is None or y is None:
            return False
        self._now = now
        # A parsed frame proves the link is alive, which is the only thing the watchdog
        # measures. The breadcrumb is not: a command something outranked never moved the
        # car, and recording it would corrupt the path the retreat retraces.
        self._last_rx = now
        self._armed = True
        if self._retreating:
            # The driver is back. recovery.c aborts mid-replay for exactly this reason:
            # the retreat exists to reach the driver, so hearing from them ends it.
            self._retreating = False
            self._release(OWNER_RECOVERING)
        # The firmware holds RT for one actuator tick PAST this deadline
        # (link.h LINK_HOLD_RT_MS = RT_WATCHDOG_MS + LINK_TICK_MS) so a lapse can
        # never zero the wheels before the trip is declared; the audit-fix spec files
        # that margin as firmware-local. It is moot here: `tick` runs `_trip` — which
        # takes OWNER_RECOVERING and sets the reversed command — before `_expire` ever
        # looks at the lapsed grant, so there is no window to cover and no invented
        # tick in a mock that has none.
        if self._take(OWNER_REMOTE, now, RT["watchdog_ms"] / 1000.0):
            self._t, self._y = t, y
            self._history.append((t, y, now))
        self._evict(now)
        self._expire(now)
        return True

    def note_bye(self, now):
        """A deliberate goodbye: stop, suppress the retreat, drop ownership.

        This is the whole point of the `bye` type. Without it, backgrounding the app is
        indistinguishable from walking out of range, and the car reverses along its own
        path with the controls off-screen.

        The four steps, in the order the plan's "Session lifecycle — who owns the
        actuator, and when" names them, because that section exists precisely because
        three implementations answered this differently:

          1. `car_stop(LINK_SRC_SAFE)`, with the result **checked**. Returned here, so a
             stop that was not applied is a line in the log rather than a silence.
          2. Release SAFE immediately. Holding it sticky would suppress the retreat too,
             but it would also lock out OTA, the wizard and the console until an app
             reconnected — background the app and the car cannot be flashed over the air.
          3. Clear the breadcrumb history. *This* is what suppresses the retreat:
             `_any_motion()` over an empty history is false, so even a later trip stops
             rather than retraces. The mechanism is the empty history, not the grant.
          4. Disarm the watchdog — silence that was announced is not a loss.

        A sticky holder (OTA, CALIB) is exempt from steps 1-2: see rule 2 of
        docs/superpowers/specs/2026-08-22-audit-fix-decisions.md.

        Ownership of the channel is dropped by the caller: `bye` is not resumable, and
        the next session arrives with a fresh `hello`.
        """
        self._now = now
        # Clearing the history is also what ends a replay still running from an earlier
        # dropout: on the car the retreat task aborts as soon as the breadcrumb sequence
        # moves under it (recovery.c's `s_seq != snap_seq`).
        self._history.clear()
        self._retreating = False
        self._armed = False
        if self._owner in (OWNER_UPDATE, OWNER_CALIBRATION):
            # Rule 2 (audit-fix spec): a sticky hold is not stolen and not released.
            # The motors are already stopped (OTA) or under the wizard's pulse; the
            # goodbye's other duties — history, watchdog, session — are done above
            # and by the caller. Grabbing SAFE here displaced OTA's grant and then
            # released it to IDLE, unlocking the motors for the rest of the flash.
            return False
        stopped = self._take(OWNER_SAFE_STOP, now, None)
        if stopped:
            self._t = self._y = 0.0
        self._release(OWNER_SAFE_STOP)
        return stopped

    def adopt_session(self, now):
        """A new `hello` was adopted. The plan's other half of the lifecycle:

          1. Release SAFE, so a previous session's stop does not outlive it.
          2. Clear the breadcrumb history. A new session has no path to retrace, and
             retreating along the *previous* driver's path is worse than not retreating
             at all.
          3. Reset the sequence gate — the link's own state, done by `RTLink._adopt`.
          4. Leave the watchdog **disarmed**. It arms on the first accepted command,
             because a command is the thing it measures; arming it on the handshake
             trips a session whose first command is still in flight.

        A repeat `hello` from the same peer and sid is answered but does not land here,
        so a retransmitted handshake cannot reset a live session.
        """
        self._now = now
        self._history.clear()
        self._armed = False
        self._retreating = False
        self._release(OWNER_SAFE_STOP)
        self._release(OWNER_RECOVERING)

    def forget_path(self, now):
        """Throw away the breadcrumb path — `recovery_forget()` on the car, whose
        liveness bump is what aborts recovery.c's `retreat_task` the next time it
        checks, mid-replay or not. A retreat still in flight loses OWNER_RECOVERING's
        grant right here: the firmware's task calls
        `link_release_must(LINK_SRC_RECOVER)` unconditionally once its abort check
        trips, and only while it still holds the actuator — so the release is what
        silently zeroes the wheels. There is no explicit stop on this path, unlike a
        trip with nothing to retrace (`_stop`): forgetting the path is not the same
        event as deciding to stop.

        A session's idle expiry uses this (rule 4, amended: a dead driver's path
        must never keep replaying, in flight or not). `adopt_session` inlines the
        same three lines for the same reason — a new driver has no path to retrace
        either.
        """
        self._now = now
        self._history.clear()
        if self._retreating:
            self._retreating = False
            self._release(OWNER_RECOVERING)

    def tick(self, now):
        """Advance time. Returns a log line at the moments worth printing, else None."""
        self._now = now
        line = None
        if self._armed and (now - self._last_rx) * 1000.0 > RT["watchdog_ms"]:
            line = self._trip(now)
        elif self._retreating and now >= self._retreat_until:
            self._retreating = False
            # The release is the stop: it zeroes only if the retreat still owns the
            # actuator, so an exhausted replay cannot flatten a pulse that outranked it.
            self._release(OWNER_RECOVERING)
            line = "recover: retrace exhausted — stopped"
        self._expire(now)
        return line

    def _trip(self, now):
        self._wdt_trips += 1
        self._armed = False        # trips once; only a returning frame re-arms it
        self._evict(now)
        silent = int((now - self._last_rx) * 1000.0)
        head = f"wdt: no control frame for {silent} ms"

        if not self.config["recovery"]["enabled"]:
            # recovery_on_link_lost stops and returns without reaching the ring, so
            # the crumbs outlive a trip with auto-return off, as they do on the car.
            self._stop(now)
            return f"{head} — stopped (auto-return off)"
        # recovery.c's `snapshot_consume`: the in-window samples are what this trip
        # retraces, and the ring is cleared with them — whether the replay then runs,
        # is refused, or there is nothing to replay. The retreat's own motion is never
        # recorded, so a ring that survived the trip made a second loss inside
        # window_ms replay ground the first retreat had already covered, on top of it.
        path = list(self._history)
        self._history.clear()
        if not path or not self._any_motion(path):
            # A history of nothing but zeros retraces to where the car already is, so
            # the honest answer is to stop rather than to perform a retreat.
            self._stop(now)
            return f"{head} — stopped (nothing to retrace)"

        if not self._take(OWNER_RECOVERING, now, None):
            # recovery.c aborts the whole replay the first time car_drive is refused,
            # rather than marching through the timeline unheard. Driving anyway would
            # reverse the car out from under an OTA or a calibration pulse while this
            # side still reports that they hold the wheels.
            return f"{head} — retrace refused ({self._owner} holds the actuator)"
        self._retreating = True
        self._retreat_until = now + self._retreat_duration(path, now)
        t, y, _ = path[-1]
        self._t, self._y = -t, -y
        return f"{head} — retracing {len(path)} samples in reverse"

    def _retreat_duration(self, path, now):
        """How long the reverse replay of `path` takes, as recovery.c computes it.

        Each sample is held for the gap to the next-newer one — the newest for the
        time it was held until the link went quiet — and **every** one of those
        segments is capped at SEG_MAX_MS, which is what `recovery_seg_ms` does to
        each `dur` in retreat_task's loop. Capping only the newest segment credited
        the retrace with dead air: a stream that stuttered for four seconds mid-drive
        replayed that pause in full, reversing long after it had retraced the ground
        it actually covered.
        """
        ts = [s[2] for s in path]
        cap = self.SEG_MAX_MS / 1000.0
        total = min(now - ts[-1], cap)
        for older, newer in zip(ts, ts[1:]):
            total += min(newer - older, cap)
        return total

    def _any_motion(self, path):
        return any(abs(t) > self.MOVE_EPS or abs(y) > self.MOVE_EPS
                   for t, y, _ in path)

    def _evict(self, now):
        window = self.config["recovery"]["window_ms"] / 1000.0
        while self._history and (now - self._history[0][2]) > window:
            self._history.popleft()

    def _stop(self, now):
        """`car_stop(LINK_SRC_RECOVER)` then `link_release` — an arbitrated zero.

        Not a poke at the wheels: recovery.c stops through the arbiter, so a watchdog
        trip during a calibration pulse or a flash leaves that source's command alone.
        The release is what zeroes, and only if the take succeeded.
        """
        self._retreating = False
        if self._take(OWNER_RECOVERING, now, None):
            self._release(OWNER_RECOVERING)

    # ---- the actuator arbiter --------------------------------------------------

    def _take(self, src, now, hold_s):
        """Grant `src` the actuator unless something that outranks it still holds it."""
        if not self._lapsed(now) and PRIORITY.index(src) < PRIORITY.index(self._owner):
            return False
        self._owner = src
        self._owner_until = None if hold_s is None else now + hold_s
        return True

    def _release(self, src):
        if self._owner == src:
            self._drop_grant()

    def _lapsed(self, now):
        if self._owner == OWNER_IDLE:
            return True
        if self._owner_until is None:
            return False
        return now >= self._owner_until

    def _expire(self, now):
        if self._lapsed(now):
            self._drop_grant()

    def _drop_grant(self):
        """Nobody owns the actuator, so the actuator holds zero.

        link.c makes this one fact twice — `link_release` memsets the target, and the
        actuator task zeroes on a lapsed grant — because "ownership lapsed" has to mean
        something physical. Without it `ctl == "idle"` and "the wheels are stopped" are
        independent here and coupled on the car, and a calibration pulse that lapses
        leaves the mock at full throttle with nobody driving.
        """
        self._owner, self._owner_until = OWNER_IDLE, None
        self._t = self._y = 0.0

    # ---- calibration, OTA ------------------------------------------------------

    def begin_spin(self, now, pair, direction):
        """Take the actuator for one identification pulse. False when something outranks."""
        self._now = now
        self._expire(now)
        if not self._take(OWNER_CALIBRATION, now, self.CALIB_HOLD_MS / 1000.0):
            return False
        # Taking the actuator from a retreat is what aborts it on the car: the next
        # car_drive(RECOVER) is refused. Leaving the flag set here would let the
        # retreat's own expiry zero the pulse halfway through.
        self._retreating = False
        self._t = 1.0 if direction else -1.0     # a pulse, not a mixed command
        self._y = 0.0
        return True

    def end_spin(self):
        """The pulse is over: release CALIB, which zeroes if the pulse still owns.

        calib_api.c does exactly this after its vTaskDelay, *before* replying, so
        the wizard's 200 arrives with the wheel already stopped and the grant gone.
        """
        self._release(OWNER_CALIBRATION)

    def save_calibration(self, wheels):
        """Mirrors calib_api.c: four wheels, each corner once, pairs 0..3 each once."""
        corners, keys = CALIBRATION["corners"], CALIBRATION["keys"]
        if not isinstance(wheels, list) or len(wheels) != 4:
            return False, ("wrong_type", keys["wheels"], "expected four wheels")
        table, seen = {}, set()
        for i, w in enumerate(wheels):
            where = f"{keys['wheels']}[{i}]"
            if not isinstance(w, dict):
                return False, ("wrong_type", where, "wheel needs {corner,pair,inverted}")
            for key in w:
                if key not in (keys["corner"], keys["pair"], keys["inverted"]):
                    return False, ("unknown_field", where, "no such field")
            corner, pair, inverted = w.get(keys["corner"]), w.get(keys["pair"]), w.get(keys["inverted"])
            if not isinstance(corner, str) or not isinstance(inverted, bool) \
                    or isinstance(pair, bool) or not isinstance(pair, (int, float)):
                return False, ("wrong_type", where, "wheel needs {corner,pair,inverted}")
            # aiohttp's request.json() accepts Infinity, NaN and integers of any size —
            # none of which cJSON_IsNumber's C side would ever hand the firmware a whole
            # number for. int(pair) raises OverflowError on an infinity or a Python int
            # too large for a float, and ValueError on NaN; math.isfinite raises the same
            # OverflowError on that oversized int before it even reaches float(). A
            # rejection is the answer either way, not a 500 from an uncaught exception.
            try:
                whole = math.isfinite(pair) and float(pair) == int(pair)
            except (OverflowError, ValueError):
                whole = False
            if not whole:
                return False, ("wrong_type", where, "wheel needs {corner,pair,inverted}")
            if corner not in corners:
                return False, ("not_allowed", where, "unknown corner")
            if corner in seen:
                return False, ("not_allowed", where, "corner repeated")
            seen.add(corner)
            if not 0 <= int(pair) < CALIBRATION["pairs"]:
                return False, ("out_of_range", where, "pair 0..3")
            table[corner] = (int(pair), inverted)
        if {p for p, _ in table.values()} != set(range(CALIBRATION["pairs"])):
            return False, ("not_allowed", keys["wheels"], "pairs must be 0..3, each once")
        self._calibration = table
        self._calibrated = True
        return True, None

    def calibration_table(self):
        """The wheels array GET /calibration answers: FL, FR, RL, RR; empty when not calibrated."""
        if not self._calibrated:
            return []
        keys = CALIBRATION["keys"]
        return [{keys["corner"]: c, keys["pair"]: self._calibration[c][0], keys["inverted"]: self._calibration[c][1]}
                for c in CALIBRATION["corners"]]

    def begin_ota(self, now):
        """Nothing commands the motors during a flash; the grant is sticky.

        Through the arbiter, as ota_api.c goes through car_stop(LINK_SRC_OTA):
        a refusal is the firmware's 409 "actuator busy", and the simulator must
        be able to exhibit it. Returns False without touching anything when a
        higher-priority holder refuses.

        The control watchdog is left as it is — car_stop is all ota_api.c does, and
        rt_link's silence check keeps running under the flash. A stream that stops
        as the flash begins trips it after rt.watchdog_ms: `link.timeouts` grows,
        the retrace is refused by this sticky grant (`_trip`), the wheels stay at
        zero. Disarming here hid that timeout from the simulator alone.
        """
        self._now = now
        self._expire(now)
        if not self._take(OWNER_UPDATE, now, None):
            return False
        self._t = self._y = 0.0
        self._retreating = False
        return True

    def end_ota(self, flashed=True, version=None):
        if flashed:
            self.fw = version or _bump_build(self.fw)
        self._release(OWNER_UPDATE)

    def set_bus_ok(self, ok):
        self._bus_ok = ok

    # ---- telemetry -------------------------------------------------------------

    def status_groups(self, rx_hz):
        """The link/motors/system/video groups, built by walking the schema so a field added to
        the contract and not to the map below raises here rather than going missing on
        the wire.
        """
        values = {
            "link": {"rx_hz": int(rx_hz), "rssi_dbm": self.rssi if self.rssi != 0 else None,
                     "timeouts": self._wdt_trips},
            "motors": {"bus": "ok" if self._bus_ok else "down", "calibrated": self._calibrated,
                       "owner": self._owner},
            "system": {"uptime_s": int(self._now - self._started), "free_heap": self.heap},
            "video": {"state": self.video_state, "fps": self.video_fps, "kbps": self.video_kbps,
                      "dropped": self.video_dropped},
        }
        return {g: {f["name"]: values[g][f["name"]] for f in GROUPS[g]["fields"]} for g in TELEMETRY_GROUPS}

    def telemetry(self, rx_hz, bump=True):
        """The 5 Hz frame, built by walking the schema (via `status_groups`).

        `bump=False` for a reader that is not the real-time channel. `seq` numbers the
        pushed stream, and `status_groups` is shared with a `/status` poll, whose
        `bump=False` keeps the real-time stream's `seq` continuous however often
        something else reads the same live state.
        """
        if bump:
            self._tele_seq += 1
        K, T = RT["keys"], RT["types"]
        return {ENVELOPE["proto"]: PROTO, K["type"]: T["telemetry"], K["seq"]: self._tele_seq,
                **self.status_groups(rx_hz)}


def _bump_build(fw):
    """`v1.0+9000` -> `v1.0+9001`. The app compares the build, so an OTA must move it."""
    head, sep, build = fw.rpartition("+")
    if not sep or not build.isdigit():
        return fw
    return f"{head}+{int(build) + 1}"
