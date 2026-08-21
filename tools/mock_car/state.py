"""The mock car's state, with no server attached.

Everything here is clock-free: the caller passes `now` (seconds, monotonic). That is what
lets `test_state.py` drive a watchdog trip and a five-second retreat in microseconds, and
it keeps `mock_car.py` down to plumbing with no behaviour worth testing hidden in it.

Nothing in this file writes a range, a default or a deadline: `DOMAINS`, `RT` and
`validate` come from `contract/car-api.json` via the generator, which is the same source
the firmware compiles. A literal here would be exactly the drift the schema exists to
prevent — the mock's old `/recover` default of off/3000, against the car's on/5000, is
why every simulator session taught that a car losing its link stops.
"""
import math
from collections import deque

from generated import DEVICE, DOMAINS, RT, TELEMETRY_FIELDS, validate

# Ownership of the actuator, lowest priority first. Mirrors `link_src_t` in
# firmware/p4/main/link.h; these names are what telemetry reports in `ctl`. They are not
# in the schema, so they are the one part of the wire spelled out here.
PRIORITY = ("none", "recover", "console", "rt", "calib", "ota", "safe")


def clamp_axis(v):
    """The car clamps; a client that sends 1.5 gets 1.0, not a rejection."""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(f):        # NaN and the infinities are not commands
        return None
    return max(-1.0, min(1.0, f))


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
    """A `seq` is a uint32. A JSON boolean is an int in Python and is not one here."""
    return isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= 0xFFFFFFFF


class CarState:
    """Config, the control watchdog, the retreat, and everything telemetry reports."""

    # Constants that belong to the firmware's behaviour rather than to the wire, kept at
    # the values firmware/p4/main defines so the mock retreats for the same duration the
    # car does.
    MOVE_EPS = 0.02        # recovery.c: below this a sample counts as stationary
    TAIL_MS = 400          # recovery.c: cap on the newest segment's reverse duration
    CALIB_HOLD_MS = 600    # link.h LINK_HOLD_CALIB_MS: one identification pulse

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
        self._owner = "none"
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
            self._release("recover")
        if self._take("rt", now, RT["watchdog_ms"] / 1000.0):
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
        """
        self._now = now
        self._t = self._y = 0.0
        # A stationary newest breadcrumb, so a later retreat starts from a car at rest
        # rather than replaying the last thing this session was doing at speed.
        self._history.append((0.0, 0.0, now))
        self._evict(now)
        self._armed = False
        self._retreating = False
        self._owner, self._owner_until = "safe", None   # holds zero until a new session

    def adopt_session(self, now):
        """A new `hello` was adopted: release the stop a previous `bye` left in place.

        The breadcrumb history survives, as it does on the car: it is a record of where
        the wheels have been, and nothing about a new session moved them.
        """
        self._now = now
        self._armed = False
        self._retreating = False
        self._release("safe")
        self._release("recover")

    def tick(self, now):
        """Advance time. Returns a log line at the moments worth printing, else None."""
        self._now = now
        line = None
        if self._armed and (now - self._last_rx) * 1000.0 > RT["watchdog_ms"]:
            line = self._trip(now)
        elif self._retreating and now >= self._retreat_until:
            self._retreating = False
            self._t = self._y = 0.0
            self._release("recover")
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
            self._stop()
            return f"{head} — stopped (auto-return off)"
        if not self._history or not self._any_motion():
            # A history of nothing but zeros retraces to where the car already is, so
            # the honest answer is to stop rather than to perform a retreat.
            self._stop()
            return f"{head} — stopped (nothing to retrace)"

        self._retreating = True
        self._retreat_until = now + self._retreat_duration(now)
        self._take("recover", now, None)
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

    def _stop(self):
        self._t = self._y = 0.0
        self._retreating = False
        self._release("recover")

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
            self._owner, self._owner_until = "none", None

    def _lapsed(self, now):
        if self._owner == "none":
            return True
        if self._owner_until is None:
            return False
        return now >= self._owner_until

    def _expire(self, now):
        if self._lapsed(now):
            self._owner, self._owner_until = "none", None

    # ---- calibration, OTA ------------------------------------------------------

    def begin_spin(self, now, pair, direction):
        """Take the actuator for one identification pulse. False when something outranks."""
        self._now = now
        self._expire(now)
        if not self._take("calib", now, self.CALIB_HOLD_MS / 1000.0):
            return False
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
        self._owner, self._owner_until = "ota", None

    def end_ota(self, flashed=True):
        if flashed:
            self.fw = _bump_build(self.fw)
        self._release("ota")

    def set_bus_ok(self, ok):
        self._bus_ok = ok

    # ---- telemetry -------------------------------------------------------------

    def telemetry(self, rx_fps):
        """The 5 Hz frame, built by walking the schema.

        Assembling it from `TELEMETRY_FIELDS` rather than from a literal dict means a
        field added to the contract and not to the map below raises here, instead of
        going quietly missing on the wire where only a client notices.
        """
        self._tele_seq += 1
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
