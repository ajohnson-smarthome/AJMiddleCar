"""The mock car's state, with no server attached.

Everything here is clock-free: the caller passes `now` (seconds, monotonic). That is what
lets `test_state.py` drive a watchdog trip and a five-second retreat in microseconds, and
it keeps `mock_car.py` down to plumbing with no behaviour worth testing hidden in it.

It also holds the wire *shapes* — `parse_frame` and the predicates under it — because
they are pure, they are what `control_proto.c` is, and the only useful test of them is
"does this datagram mean here what it means on the car".

Nothing in this file writes a range, a default, a deadline or a field name: `DOMAINS`,
`RT`, `CTL_VALUES` and `validate` come from `contract/car-api.json` via the generator,
which is the same source the firmware compiles. A literal here would be exactly the drift the schema exists to
prevent — the mock's old `/recover` default of off/3000, against the car's on/5000, is
why every simulator session taught that a car losing its link stops.
"""
import json
import math
import re
from collections import deque

from generated import CTL_VALUES, DEVICE, DOMAINS, RT, TELEMETRY_FIELDS, validate

# Ownership of the actuator, lowest priority first. `link_src_t` in
# firmware/p4/main/link.h is generated from the same list, and these names are what
# telemetry reports in `ctl`.
PRIORITY = tuple(CTL_VALUES)

# The vocabulary, named. Unpacked from the list rather than written out, because the
# generator emits per-name symbols for the other two implementations (C's `CTL_RT`,
# Swift's `CtlOwner.rt`) and for Python only the flat list — so this is the nearest
# thing to a generated symbol until `emit_python` catches up. Position is rank on all
# three sides, and a schema that adds or drops a value fails here, at import, instead
# of at the first `PRIORITY.index()` deep inside the arbiter.
CTL_NONE, CTL_RECOVER, CTL_CONSOLE, CTL_RT, CTL_CALIB, CTL_OTA, CTL_SAFE = PRIORITY

# The session id, as `parse_sid` in firmware/p4/main/control_proto.c accepts it:
# non-empty, alphanumeric, and short enough to fit that file's CONTROL_SID_MAX with its
# NUL. Not in contract/car-api.json on any of the three sides, so it is mirrored here by
# hand — see the report. Anything else is not "an id the car will not like": it is a
# datagram the car drops whole, because the id is echoed into the hello reply and a
# quote in it would let the sender shape that JSON.
SID_MAX_CHARS = 15         # mirrors CONTROL_SID_MAX - 1 in firmware/p4/main/control_proto.h
_SID_RE = re.compile(r"[A-Za-z0-9]{1,%d}\Z" % SID_MAX_CHARS)


def valid_sid(v):
    """Would control_parse_frame accept this `hello` value?

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
    """A `seq` is a uint32. A JSON boolean is an int in Python and is not one here.

    Deliberately stricter than the car in one place: `control_proto.c`'s `parse_u32`
    tokenises `1.5` down to `1` and accepts it. Strictness in the mock is the safe
    direction — a client that works here works there, and the reverse is the trap this
    file exists to close — so the mock rejects it and the car's coercion is a firmware
    nit, not something to copy.
    """
    return isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= 0xFFFFFFFF


def parse_frame(data, max_command=None):
    """One inbound datagram -> a dict of the fields it carried, or None to drop it.

    The mock's `control_parse_frame` (firmware/p4/main/control_proto.c). Same answer for
    the same bytes is the whole point, so the rules are the car's, not JSON's:

      * over the *command* cap -> dropped. `max_datagram` sizes a receive buffer; what
        the car agrees to act on is `max_command`, and the difference is the room a
        telemetry frame needs on the way out.
      * every key that is present must parse, or the whole datagram is dropped. A frame
        with a good `t` and a broken `seq` is not a command with a missing sequence
        number; it is corrupt.
      * `t` and `y` come as a pair. One axis without the other is a truncated frame, not
        an instruction to hold the other at zero.
      * a datagram carrying no hello, no axes and no goodbye has nothing to act on.
        A *bare* goodbye is not that: `{"seq":n,"bye":1}` says stop, and the car acts
        on it (`control_proto.c`: `if (!f.has_hello && !f.has_ty && !f.bye) return -1`).
        `{"bye":0}` is not a goodbye and carries nothing else, so it goes.

    Range is deliberately not checked here either — the arbiter clamps.
    """
    cap = RT["max_command"] if max_command is None else max_command
    if not data or len(data) > cap:
        return None
    try:
        frame = json.loads(data)
    except (ValueError, UnicodeDecodeError):
        return None
    if not isinstance(frame, dict):
        return None

    out = {}
    for key in (RT["proto_field"], RT["seq_field"]):
        if key in frame:
            if not valid_seq(frame[key]):
                return None
            out[key] = frame[key]

    if RT["hello_field"] in frame:
        if not valid_sid(frame[RT["hello_field"]]):
            return None
        out[RT["hello_field"]] = frame[RT["hello_field"]]

    if RT["bye_field"] in frame:
        v = frame[RT["bye_field"]]
        if isinstance(v, bool):
            out[RT["bye_field"]] = v
        else:
            # The wire says 1, but JSON has two ways to say yes. A string does not
            # become a goodbye by being the word "yes".
            n = number(v)
            if n is None:
                return None
            out[RT["bye_field"]] = n != 0.0

    t, y = frame.get(RT["throttle_field"]), frame.get(RT["yaw_field"])
    has_t = RT["throttle_field"] in frame
    has_y = RT["yaw_field"] in frame
    if has_t or has_y:
        if not (has_t and has_y):
            return None
        t, y = number(t), number(y)
        if t is None or y is None:
            return None
        out[RT["throttle_field"]], out[RT["yaw_field"]] = t, y

    if not (RT["hello_field"] in out or RT["throttle_field"] in out
            or out.get(RT["bye_field"])):
        return None
    return out


class CarState:
    """Config, the control watchdog, the retreat, and everything telemetry reports."""

    # Constants that belong to the firmware's behaviour rather than to the wire, kept at
    # the values firmware/p4/main defines so the mock retreats for the same duration the
    # car does.
    # None of these three is in contract/car-api.json, so they are mirrored by hand from
    # the file named beside each one. See the report for the schema additions that would
    # let them be generated instead.
    MOVE_EPS = 0.02        # firmware/p4/main/recovery.c: below this a sample is stationary
    TAIL_MS = 400          # firmware/p4/main/recovery.c: cap on the newest segment
    CALIB_HOLD_MS = 600    # firmware/p4/main/link.h LINK_HOLD_CALIB_MS: one pulse

    def __init__(self, device=DEVICE, fw="v1.0+9000", now=0.0):
        self.device = device
        self.fw = fw
        self.rssi = -58
        self.heap = 200000
        self.config = {path: dict(d["defaults"]) for path, d in DOMAINS.items()}

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
        self._owner = CTL_NONE
        self._owner_until = None       # None means the grant is sticky
        self._calibrated = False
        self._bus_ok = True
        self._tele_seq = 0

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

    # ---- configuration ---------------------------------------------------------

    def apply_config(self, path, body):
        """Validate and apply one config domain. Returns (ok, reason).

        A rejected body changes nothing at all. The firmware validates every field
        before it writes any of them, so a partially-applied record is a state neither
        side can be in — and a client that reads back after a 400 must see what it had.
        """
        ok, err = validate(path, body)
        if not ok:
            return False, err
        self.config[path] = {f["name"]: body[f["name"]] for f in DOMAINS[path]["fields"]}
        return True, ""

    def field_of(self, path, reason):
        """Which key `validate` is complaining about, for the reply's `field`.

        The message text belongs to the generator, so this matches rather than parses:
        an unrecognised wording costs an empty `field`, which the protocol already
        allows for a fault with the body as a whole.
        """
        for f in DOMAINS.get(path, {}).get("fields", []):
            if reason.startswith(f["name"] + " ") or reason.startswith("missing " + f["name"]):
                return f["name"]
        return ""

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
            self._release(CTL_RECOVER)
        if self._take(CTL_RT, now, RT["watchdog_ms"] / 1000.0):
            self._t, self._y = t, y
            self._history.append((t, y, now))
        self._evict(now)
        self._expire(now)
        return True

    def note_bye(self, now):
        """A deliberate goodbye: stop, suppress the retreat, drop ownership.

        This is the whole point of the `bye` field. Without it, backgrounding the app is
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
        stopped = self._take(CTL_SAFE, now, None)
        if stopped:
            self._t = self._y = 0.0
        self._release(CTL_SAFE)
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
        self._release(CTL_SAFE)
        self._release(CTL_RECOVER)

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
            self._release(CTL_RECOVER)
            line = "recover: retrace exhausted — stopped"
        self._expire(now)
        return line

    def _trip(self, now):
        self._wdt_trips += 1
        self._armed = False        # trips once; only a returning frame re-arms it
        self._evict(now)
        silent = int((now - self._last_rx) * 1000.0)
        head = f"wdt: no control frame for {silent} ms"

        if not self.config["/recover"]["enabled"]:
            self._stop(now)
            return f"{head} — stopped (auto-return off)"
        if not self._history or not self._any_motion():
            # A history of nothing but zeros retraces to where the car already is, so
            # the honest answer is to stop rather than to perform a retreat.
            self._stop(now)
            return f"{head} — stopped (nothing to retrace)"

        if not self._take(CTL_RECOVER, now, None):
            # recovery.c aborts the whole replay the first time car_drive is refused,
            # rather than marching through the timeline unheard. Driving anyway would
            # reverse the car out from under an OTA or a calibration pulse while this
            # side still reports that they hold the wheels.
            return f"{head} — retrace refused ({self._owner} holds the actuator)"
        self._retreating = True
        self._retreat_until = now + self._retreat_duration(now)
        t, y, _ = self._history[-1]
        self._t, self._y = -t, -y
        return f"{head} — retracing {len(self._history)} samples in reverse"

    def _retreat_duration(self, now):
        """How long the reverse replay takes, as recovery.c computes it.

        Each sample is held for the gap to the next-newer one, and the newest for the
        time it was held until the link went quiet, capped at TAIL_MS. Summed, all the
        inner gaps telescope into the span of the history.
        """
        newest = self._history[-1][2]
        oldest = self._history[0][2]
        tail = min(now - newest, self.TAIL_MS / 1000.0)
        return tail + (newest - oldest)

    def _any_motion(self):
        return any(abs(t) > self.MOVE_EPS or abs(y) > self.MOVE_EPS
                   for t, y, _ in self._history)

    def _evict(self, now):
        window = self.config["/recover"]["window_ms"] / 1000.0
        while self._history and (now - self._history[0][2]) > window:
            self._history.popleft()

    def _stop(self, now):
        """`car_stop(LINK_SRC_RECOVER)` then `link_release` — an arbitrated zero.

        Not a poke at the wheels: recovery.c stops through the arbiter, so a watchdog
        trip during a calibration pulse or a flash leaves that source's command alone.
        The release is what zeroes, and only if the take succeeded.
        """
        self._retreating = False
        if self._take(CTL_RECOVER, now, None):
            self._release(CTL_RECOVER)

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
        if self._owner == CTL_NONE:
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
        something physical. Without it `ctl == "none"` and "the wheels are stopped" are
        independent here and coupled on the car, and a calibration pulse that lapses
        leaves the mock at full throttle with nobody driving.
        """
        self._owner, self._owner_until = CTL_NONE, None
        self._t = self._y = 0.0

    # ---- calibration, OTA ------------------------------------------------------

    def begin_spin(self, now, pair, direction):
        """Take the actuator for one identification pulse. False when something outranks."""
        self._now = now
        self._expire(now)
        if not self._take(CTL_CALIB, now, self.CALIB_HOLD_MS / 1000.0):
            return False
        # Taking the actuator from a retreat is what aborts it on the car: the next
        # car_drive(RECOVER) is refused. Leaving the flag set here would let the
        # retreat's own expiry zero the pulse halfway through.
        self._retreating = False
        self._t = 1.0 if direction else -1.0     # a pulse, not a mixed command
        self._y = 0.0
        return True

    def save_calibration(self, wheels):
        """Mirrors calibration_valid: four entries, unique pairs 0..3, signs ±1."""
        try:
            if not isinstance(wheels, list) or len(wheels) != 4:
                return False
            pairs = {int(w["pair"]) for w in wheels}
            signs = [int(w["sign"]) for w in wheels]
        except (TypeError, ValueError, KeyError):
            return False
        if pairs != {0, 1, 2, 3} or any(s not in (-1, 1) for s in signs):
            return False
        self._calibrated = True
        return True

    def begin_ota(self, now):
        """Nothing commands the motors during a flash; the grant is sticky."""
        self._now = now
        self._t = self._y = 0.0
        self._armed = False
        self._retreating = False
        self._owner, self._owner_until = CTL_OTA, None

    def end_ota(self, flashed=True):
        if flashed:
            self.fw = _bump_build(self.fw)
        self._release(CTL_OTA)

    def set_bus_ok(self, ok):
        self._bus_ok = ok

    # ---- telemetry -------------------------------------------------------------

    def telemetry(self, rx_fps, bump=True):
        """The 5 Hz frame, built by walking the schema.

        Assembling it from `TELEMETRY_FIELDS` rather than from a literal dict means a
        field added to the contract and not to the map below raises here, instead of
        going quietly missing on the wire where only a client notices.

        `bump=False` for a reader that is not the real-time channel. `seq` numbers the
        pushed stream, and telemetry.c keeps a counter per consumer for exactly this
        reason: a `/status` poll at 1 Hz must not make the 5 Hz stream skip a number
        once a second, because skipping is how the app measures loss.
        """
        if bump:
            self._tele_seq += 1
        # The keys below are the schema's telemetry field names, and the generator emits
        # no symbols for them in any of the three languages (the firmware spells them in
        # telemetry.h the same way). They are safe as literals *because* of the
        # comprehension underneath: a field renamed in the schema raises KeyError here
        # rather than going quietly missing on the wire.
        values = {
            "seq": self._tele_seq,
            "rx_fps": int(rx_fps),
            "rssi": self.rssi,
            "wdt_trips": self._wdt_trips,
            "uptime_s": int(self._now - self._started),
            "heap": self.heap,
            "calibrated": self._calibrated,
            "bus_ok": self._bus_ok,
            "ctl": self._owner,
        }
        return {f["name"]: values[f["name"]] for f in TELEMETRY_FIELDS}


def _bump_build(fw):
    """`v1.0+9000` -> `v1.0+9001`. The app compares the build, so an OTA must move it."""
    head, sep, build = fw.rpartition("+")
    if not sep or not build.isdigit():
        return fw
    return f"{head}+{int(build) + 1}"
