# Mock Fidelity & Cross-Implementation Safety Net — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the mock behave like the firmware everywhere the 2026-08-22 audit caught it lying, and build the cross-implementation safety net (UDP conformance, both-bounds contract guard, name-keyed ctl symbols) so this class of drift cannot return silently.

**Architecture:** All mock behaviour lives in `tools/mock_car/state.py` (pure, clock-as-argument) and `tools/mock_car/rt_link.py` (protocol, loop/transport injected); `mock_car.py` stays plumbing. Every behavioural change lands as a stdlib-only unit test first, then the implementation, then a commit. Cross-implementation checks live in `tools/conformance.py` (REST) and the new `tools/conformance_rt.py` (UDP), both runnable against the mock in `tools/test-all.sh` and against a real car by hand.

**Tech Stack:** Python 3 stdlib for state/link/tests; aiohttp only inside `mock_car.py` (venv at `tools/mock_car/.venv`); bash for `test-all.sh`.

**Spec:** `docs/superpowers/specs/2026-08-22-audit-fix-decisions.md` — rules are cited by number below.

## Global Constraints

- Work in the worktree `/Users/adamjohnson/VSCode/esp32-p4-car/.claude/worktrees/audit-fixes` (branch `audit-fixes`). All paths below are relative to that root.
- `test_state.py`, `test_rtlink.py`, `test_gen_contract.py`, `conformance.py`, `conformance_rt.py` stay **stdlib-only** (no aiohttp, no venv) so they run against a car from a bare clone.
- A range/default/deadline/field-name literal in the mock is a bug: those come from `generated.py` (regenerate via `python3 tools/gen_contract.py`, never hand-edit generated files; `bash tools/check_contract.sh` must stay green).
- Behaviour constants that mirror the firmware by hand (`CALIB_HOLD_MS`, `TICK_S`, …) carry a comment naming the firmware file they mirror — keep that pattern for any new one.
- Run the full suite with `./tools/test-all.sh` before every commit. Every commit message ends with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`
- Cross-plan dependencies (stated per task): Task 5 consumes `RT["session_idle_ms"]` added to the schema by the **firmware plan**'s contract task; Task 12's conformance additions pass against the mock immediately but against a real car only after the firmware plan's REST-unification tasks.

---

### Task 1: Duplicate-key rejection and the shared pinned frames (rules 5, 6)

**Files:**
- Modify: `tools/mock_car/state.py` (function `parse_frame`, ~line 143)
- Test: `tools/mock_car/test_state.py` (class `TestWireShapes`)

**Interfaces:**
- Consumes: nothing new.
- Produces: `parse_frame(data, max_command=None)` unchanged signature; new behaviour: a datagram whose JSON carries a duplicate key (at any nesting level — stricter than the car's top-level-only detection, which is the mock's safe direction) returns `None`.

- [ ] **Step 1: Write the failing tests** — append to `TestWireShapes` in `tools/mock_car/test_state.py`:

```python
    def test_a_duplicate_key_drops_the_whole_datagram(self):
        """Rule 5: the car takes the first duplicate, json.loads keeps the last —
        the only shared answer is to drop the frame on both sides."""
        self.assertIsNone(parse_frame(b'{"seq":9,"t":0.5,"y":0,"t":0.9}'))
        self.assertIsNone(parse_frame(b'{"proto":1,"hello":"ab","proto":1}'))

    def test_the_shared_pinned_frames(self):
        """The spec's rule-6 table, verbatim. The firmware host tests pin the same
        eight bytes with the same outcomes; a change here without a change there
        is wire drift."""
        dropped = [
            b'{"seq":5,"junk":{"t":0.9},"y":0.5}',   # no top-level t
            b'{"seq":7,"t":.5,"y":0}',               # bare . mantissa
            b'{"seq":8,"t":+1,"y":0}',               # leading +
            b'{"seq":9,"t":0.5,"y":0,"t":0.9}',      # duplicate key
            b'{"proto":1.5,"hello":"abcd1234"}',     # fractional proto
        ]
        for frame in dropped:
            self.assertIsNone(parse_frame(frame), frame)
        self.assertEqual(parse_frame(b'{"seq":10,"t":0.50,"y":-0.25}'),
                         {"seq": 10, "t": 0.50, "y": -0.25})
        self.assertEqual(parse_frame(b'{"proto":1,"hello":"7f3a91c2"}'),
                         {"proto": 1, "hello": "7f3a91c2"})
        self.assertEqual(parse_frame(b'{"proto":2,"hello":"7f3a91c2"}'),
                         {"proto": 2, "hello": "7f3a91c2"})
```

(The last frame parses; *classification* — answered by name, not adopted — is RTLink's job and is already pinned by `test_a_foreign_proto_is_answered_but_not_adopted`.)

- [ ] **Step 2: Run to verify the new tests fail**

Run: `python3 tools/mock_car/test_state.py TestWireShapes -v 2>&1 | tail -15`
Expected: `test_a_duplicate_key_drops_the_whole_datagram ... FAIL` (frame parses, last dup wins) and `test_the_shared_pinned_frames ... FAIL` on the duplicate-key entry. The grammar entries (`.5`, `+1`, `1.5`-proto) already pass — `json.loads` and `valid_seq` reject them today; the test pins that.

- [ ] **Step 3: Implement** — in `tools/mock_car/state.py`, add below `valid_sid` (before `parse_frame`):

```python
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
```

and in `parse_frame` change the load:

```python
    try:
        frame = json.loads(data, object_pairs_hook=_no_duplicates)
    except (ValueError, UnicodeDecodeError):
        return None
```

- [ ] **Step 4: Run the mock suites**

Run: `python3 tools/mock_car/test_state.py && python3 tools/mock_car/test_rtlink.py`
Expected: OK twice, no failures anywhere (the hook must not break the happy-path tests).

- [ ] **Step 5: Run the full suite and commit**

```bash
./tools/test-all.sh
git add tools/mock_car/state.py tools/mock_car/test_state.py
git commit -m "fix(mock): a duplicate key drops the datagram, and the shared frame table is pinned

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: The sequence gate survives a trip (rule 1)

**Files:**
- Modify: `tools/mock_car/rt_link.py` (method `RTLink.tick`, ~line 213)
- Test: `tools/mock_car/test_rtlink.py` (class `TestWatchdog`, test `test_a_trip_clears_the_sequence_gate`)

**Interfaces:**
- Consumes: nothing new.
- Produces: `RTLink.last_seq` is no longer cleared by a trip; it resets only in `_adopt` and on bye. The firmware plan implements the identical rule in `rt_session_trip` (rt_link.h).

- [ ] **Step 1: Rewrite the pinning test** — in `tools/mock_car/test_rtlink.py`, replace the whole method `test_a_trip_clears_the_sequence_gate` (keep it inside `TestWatchdog`) with:

```python
    def test_a_trip_keeps_the_sequence_gate(self):
        """Rule 1 of the 2026-08-22 audit-fix spec: the gate survives a trip.

        Clearing it opened the window the audit confirmed: one network-delayed
        duplicate of a pre-dropout command was accepted as the resumed stream,
        drove the car at stale stick values and aborted the retreat. A genuinely
        resuming same-session stream carries monotonically newer seqs and passes
        the kept gate; only adopt and bye reset it.
        """
        rt, car, loop = link()
        send(rt, hello())
        loop.t = 1.0
        send(rt, cmd(500, 0.9))
        loop.t = 1.0 + DEADLINE_S + 0.05
        self.assertIsNotNone(rt.tick(loop.t))
        self.assertEqual(car.wdt_trips, 1)
        self.assertEqual(rt.last_seq, 500, "the gate survives the trip")
        # Channel ownership survives too: a stream that comes back is the same
        # session, and the app would otherwise be ignored until it said hello.
        self.assertEqual(rt.owner, APP)
        send(rt, cmd(499, -0.5))                 # the delayed pre-dropout duplicate
        self.assertEqual(car.command, (0.0, 0.0), "a stale frame cannot drive the car")
        send(rt, cmd(501, -0.5))                 # the genuinely resumed stream
        self.assertEqual(car.command, (-0.5, 0.0), "the resumed stream is heard")
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 tools/mock_car/test_rtlink.py TestWatchdog -v`
Expected: FAIL — `rt.last_seq` is `None` after the trip (tick still clears it).

- [ ] **Step 3: Implement** — in `tools/mock_car/rt_link.py`, replace `RTLink.tick` (code and docstring) with:

```python
    def tick(self, now):
        """One service tick of the car's clock.

        The sequence gate deliberately survives a trip (audit-fix spec rule 1):
        silence proves the stream is dead, but the gate is what stops a
        network-delayed duplicate of a pre-dropout command from being accepted as
        the resumed stream — driving the car at stale stick values and aborting
        the retreat. A same-session stream that resumes carries newer seqs and
        passes; the gate resets only on adopt and on bye.

        Channel ownership survives too: a stream that resumes after a dropout is
        the same session, and evicting it would ignore the driver until the app
        noticed.
        """
        return self.car.tick(now)
```

- [ ] **Step 4: Run the mock suites**

Run: `python3 tools/mock_car/test_rtlink.py && python3 tools/mock_car/test_state.py`
Expected: OK twice.

- [ ] **Step 5: Full suite and commit**

```bash
./tools/test-all.sh
git add tools/mock_car/rt_link.py tools/mock_car/test_rtlink.py
git commit -m "fix(mock): the sequence gate survives a watchdog trip

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: A goodbye during a sticky hold leaves the hold alone (rule 2)

**Files:**
- Modify: `tools/mock_car/state.py` (method `CarState.note_bye`, ~line 300)
- Modify: `tools/mock_car/rt_link.py` (the bye branch of `_handle`, ~line 137)
- Test: `tools/mock_car/test_state.py` (class `TestActuatorOwnership`)

**Interfaces:**
- Consumes: `CTL_OTA`, `CTL_CALIB` (module constants in state.py).
- Produces: `note_bye(now) -> bool` — True when the stop landed via SAFE, False when a sticky holder (OTA/CALIB) kept the actuator and the stop was deliberately skipped. Either way the history is cleared, the watchdog disarmed. The firmware plan applies the same guard in `on_bye`.

- [ ] **Step 1: Write the failing tests** — in `tools/mock_car/test_state.py`, replace the whole method `test_a_goodbye_during_an_ota_does_not_wedge_the_mock` with:

```python
    def test_a_goodbye_during_an_ota_leaves_the_flash_its_grant(self):
        """Rule 2: bye must not steal-and-release a sticky hold. Before this rule
        the SAFE grab displaced OTA and released to NONE, so anything could drive
        the motors for the rest of the flash — and the restart could land mid-drive."""
        car = CarState(now=0.0)
        stream(car, 0.5, 0.0, 0.0, 5)
        car.begin_ota(1.0)
        self.assertFalse(car.note_bye(1.1), "no stop was issued; the flash holds")
        self.assertEqual(car.ctl, CTL_OTA, "the goodbye did not touch the grant")
        self.assertEqual(car.history_len, 0, "the retreat is still suppressed")
        self.assertFalse(car.begin_spin(1.2, 0, 1), "still flashing; a spin is refused")
        for k in range(50):
            self.assertIsNone(car.tick(1.2 + k * 0.05))
        self.assertEqual(car.wdt_trips, 0, "the watchdog was still disarmed")
        car.end_ota()
        self.assertEqual(car.ctl, CTL_NONE, "not wedged: the flash's own end releases")
        self.assertTrue(car.begin_spin(5.0, 0, 1))

    def test_a_goodbye_during_a_spin_leaves_the_pulse_alone(self):
        car = CarState(now=0.0)
        self.assertTrue(car.begin_spin(0.0, 1, 1))
        self.assertFalse(car.note_bye(0.1))
        self.assertEqual(car.ctl, CTL_CALIB)
        self.assertEqual(car.command, (1.0, 0.0), "the pulse is not flattened")
        car.tick(CarState.CALIB_HOLD_MS / 1000.0 + 0.01)
        self.assertEqual(car.ctl, CTL_NONE, "the pulse lapses on its own schedule")
        self.assertEqual(car.command, (0.0, 0.0))
```

- [ ] **Step 2: Run to verify they fail**

Run: `python3 tools/mock_car/test_state.py TestActuatorOwnership -v 2>&1 | tail -12`
Expected: both new tests FAIL — today `note_bye` takes SAFE over OTA/CALIB (returns True, ctl ends `none`, the pulse is flattened).

- [ ] **Step 3: Implement** — in `tools/mock_car/state.py`, inside `note_bye`, replace the block after the docstring:

```python
        self._now = now
        # Clearing the history is also what ends a replay still running from an earlier
        # dropout: on the car the retreat task aborts as soon as the breadcrumb sequence
        # moves under it (recovery.c's `s_seq != snap_seq`).
        self._history.clear()
        self._retreating = False
        self._armed = False
        if self._owner in (CTL_OTA, CTL_CALIB):
            # Rule 2 (audit-fix spec): a sticky hold is not stolen and not released.
            # The motors are already stopped (OTA) or under the wizard's pulse; the
            # goodbye's other duties — history, watchdog, session — are done above
            # and by the caller. Grabbing SAFE here displaced OTA's grant and then
            # released it to NONE, unlocking the motors for the rest of the flash.
            return False
        stopped = self._take(CTL_SAFE, now, None)
        if stopped:
            self._t = self._y = 0.0
        self._release(CTL_SAFE)
        return stopped
```

and extend the docstring's numbered list with one line after step 4: `A sticky holder (OTA, CALIB) is exempt from steps 1-2: see rule 2 of docs/superpowers/specs/2026-08-22-audit-fix-decisions.md.`

In `tools/mock_car/rt_link.py`, the bye branch's else-arm message is now reachable and wrong ("REFUSED" reads as an error). Replace the if/else around `note_bye`:

```python
            if self.car.note_bye(now):
                print(f"rt: bye from session {self.session} — stopped, history cleared")
            else:
                # A sticky holder (an OTA flash, a calibration pulse) keeps the
                # actuator through a goodbye — rule 2 of the audit-fix spec. The
                # goodbye still ended the session and cleared the history.
                print(f"rt: bye from session {self.session} — history cleared, "
                      f"{self.car.ctl} keeps the actuator")
```

- [ ] **Step 4: Run the mock suites**

Run: `python3 tools/mock_car/test_state.py && python3 tools/mock_car/test_rtlink.py`
Expected: OK twice — in particular `test_a_goodbye_leaves_the_wizard_free_to_spin` (bye with no sticky holder) must still pass unchanged.

- [ ] **Step 5: Full suite and commit**

```bash
./tools/test-all.sh
git add tools/mock_car/state.py tools/mock_car/rt_link.py tools/mock_car/test_state.py
git commit -m "fix(mock): a goodbye during an OTA or a spin leaves the sticky grant alone

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Dead-sid memory (rule 3)

**Files:**
- Modify: `tools/mock_car/rt_link.py` (`RTLink.__init__`, `_adopt`, the bye branch of `_handle`)
- Test: `tools/mock_car/test_rtlink.py` (class `TestAdoption`)

**Interfaces:**
- Consumes: nothing new.
- Produces: `RTLink.dead_sids` — a `deque(maxlen=4)` of sids whose sessions ended (bye, eviction; Tasks 5 and 8 add expiry and reboot). A hello carrying a remembered dead sid is answered but not adopted **while a live session exists**; when no session is live, any hello adopts — even a remembered-dead sid (rule 3's refinement: refusing there would wedge a client whose session idled out mid-handshake, and a stale duplicate adopting displaces nobody). The firmware plan mirrors the rule, the refinement, and the capacity of 4.

- [ ] **Step 1: Write the failing tests** — append to `TestAdoption` in `tools/mock_car/test_rtlink.py`:

```python
    def test_a_dead_sessions_hello_is_answered_but_not_adopted(self):
        """Rule 3: a network-duplicated hello of a session that already ended must
        not evict the live driver. Session 1 dies by bye; its delayed hello
        duplicate arrives while session 2 is driving."""
        rt, car, _ = link()
        send(rt, hello("dead0001"))
        send(rt, cmd(1, 0.5))
        send(rt, cmd(2, 0.0, 0.0, **{BYE: 1}))            # session 1 ends itself
        send(rt, hello("beef0002"), addr=OTHER)           # session 2 adopts
        send(rt, cmd(1, 0.7), addr=OTHER)
        answered = len(rt.transport.sent)
        send(rt, hello("dead0001"))                       # the delayed duplicate
        self.assertEqual(rt.owner, OTHER, "the live driver keeps the session")
        self.assertEqual(rt.session, "beef0002")
        self.assertEqual(len(rt.transport.sent), answered + 1,
                         "the dead hello is answered — a lost-reply retry must not hang")
        self.assertEqual(rt.transport.sent[-1][1], APP)
        send(rt, cmd(2, -0.3), addr=OTHER)
        self.assertEqual(car.command, (-0.3, 0.0), "the live stream still drives")

    def test_an_evicted_sid_is_dead_too(self):
        rt, car, _ = link()
        send(rt, hello("evict001"))
        send(rt, cmd(1, 0.5))
        send(rt, hello("beef0002"), addr=OTHER)           # evicts evict001
        send(rt, hello("evict001"))                       # a stale duplicate returns
        self.assertEqual(rt.owner, OTHER, "an evicted sid cannot re-adopt")

    def test_the_dead_ring_is_bounded(self):
        rt, car, _ = link()
        for i in range(6):              # each hello evicts the last: 5 dead sids recorded
            send(rt, hello(f"sid{i:05d}"))
        # The ring keeps the last 4 dead (sid00001..sid00004); sid00000 fell off.
        # A live session (sid00005) exists throughout, so refusal is in force.
        send(rt, hello("sid00000"), addr=OTHER)
        self.assertEqual(rt.session, "sid00000",
                         "a sid old enough to leave the ring may live again")
        send(rt, hello("sid00004"), addr=APP)             # still remembered
        self.assertEqual(rt.session, "sid00000", "and still refused while someone is live")

    def test_with_no_live_session_even_a_dead_sid_adopts(self):
        """Rule 3's refinement: refusing a dead sid when nobody is live would wedge
        a client whose session idled out mid-handshake, and adopting a stale
        duplicate displaces nobody — the phantom dies again by rule 4."""
        rt, car, _ = link()
        send(rt, hello("phoenix1"))
        send(rt, cmd(1, 0.5))
        send(rt, cmd(2, 0.0, 0.0, **{BYE: 1}))            # dies; nobody else arrives
        self.assertIsNone(rt.owner)
        send(rt, hello("phoenix1"))                       # the same sid returns
        self.assertEqual(rt.owner, APP, "with no live session, any hello adopts")
        self.assertEqual(rt.session, "phoenix1")
```

- [ ] **Step 2: Run to verify they fail**

Run: `python3 tools/mock_car/test_rtlink.py TestAdoption -v 2>&1 | tail -10`
Expected: the first two and the ring test FAIL (no ring exists; the dead hello re-adopts today). `test_with_no_live_session_even_a_dead_sid_adopts` passes trivially today and exists to pin the refinement against an over-eager implementation.

- [ ] **Step 3: Implement** — in `tools/mock_car/rt_link.py`:

In `__init__`, after `self.last_seq = None`:

```python
        # Sids of sessions that ended — by goodbye, eviction, expiry or the mock's
        # simulated reboot. A hello carrying one is answered but never re-adopted
        # (audit-fix spec rule 3): a network-duplicated handshake of a dead session
        # must not evict the live driver. Capacity mirrors the firmware's ring of 4.
        self.dead_sids = deque(maxlen=4)
```

In the bye branch of `_handle`, before `self.owner, self.session, self.last_seq = None, None, None`:

```python
            self.dead_sids.append(self.session)
```

In `_adopt`, after the proto check and before the `if self.owner != addr or self.session != sid:` block:

```python
        if self.owner is not None and sid in self.dead_sids:
            # A dead session's hello — usually a duplicate the network held onto.
            # Answered, so a client retrying a lost reply is not left hanging, but
            # never adopted over whoever is driving now. Only over someone: with no
            # live session any hello adopts (rule 3's refinement), or a client whose
            # session idled out mid-handshake could never get back in.
            self._send(reply, addr)
            return
```

And inside the adoption block, record the evicted sid — extend the eviction line:

```python
        if self.owner != addr or self.session != sid:
            evicted = self.owner if self.owner and self.owner != addr else None
            if self.session is not None and self.session != sid:
                self.dead_sids.append(self.session)
```

- [ ] **Step 4: Run the mock suites**

Run: `python3 tools/mock_car/test_rtlink.py && python3 tools/mock_car/test_state.py`
Expected: OK twice. `test_after_a_goodbye_the_old_peer_is_a_stranger` re-hellos with a *fresh* sid and must still adopt.

- [ ] **Step 5: Full suite and commit**

```bash
./tools/test-all.sh
git add tools/mock_car/rt_link.py tools/mock_car/test_rtlink.py
git commit -m "feat(mock): dead-sid memory — a dead session's hello cannot evict the live driver

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Sessions are mortal (rule 4) — DEPENDS on the firmware plan's contract task

**Files:**
- Modify: `tools/mock_car/rt_link.py` (`RTLink.__init__`, `_handle`, `_adopt`, `tick`)
- Modify: `tools/mock_car/state.py` (new read-only property `CarState.armed`)
- Test: `tools/mock_car/test_rtlink.py` (class `TestWatchdog`)

**Interfaces:**
- Consumes: `RT["session_idle_ms"]` (int, 10000) from `tools/mock_car/generated.py` — **added to `contract/car-api.json` and regenerated by the firmware plan's task "the contract gains session_idle_ms"**. Verify before starting: `python3 -c "import sys; sys.path.insert(0,'tools/mock_car'); from generated import RT; print(RT['session_idle_ms'])"` must print `10000`. If it raises `KeyError`, run that firmware-plan task first.
- Produces: `CarState.armed` (read-only bool: the control watchdog is armed) and mortality per amended rule 4 — while the watchdog is **not** armed, the session ends when **strictly more** than `RT["session_idle_ms"]` ms have passed since its **last activity**: the last accepted command, or the adoption itself when none was ever accepted (so a hello-only session ages out too). At exactly 10000 ms the session is still alive. On expiry: sid recorded dead, ownership cleared, telemetry stops. The firmware implements the same last-activity anchor and strict inequality. (Note the consequence: the trip fires ~`watchdog_ms` after the last command, so a tripped session dies ~`watchdog_ms` *earlier* than trip+idle.)

- [ ] **Step 1: Write the failing test** — append to `TestWatchdog` in `tools/mock_car/test_rtlink.py`:

```python
    def test_a_tripped_session_expires_past_the_idle_window(self):
        """Amended rule 4: sessions are mortal, anchored at their LAST ACTIVITY —
        the last accepted command — not at the trip. Without this the car pushed
        telemetry to a vanished owner forever, and the kept seq gate (rule 1)
        would hold state for a peer that will never return. At exactly
        session_idle_ms the session is still alive; strictly past it, it ends."""
        idle_s = RT["session_idle_ms"] / 1000.0
        rt, car, loop = link()
        loop.t = 1.0
        send(rt, hello("mortal01"))
        send(rt, cmd(10, 0.5))                                # last activity: t = 1.0
        loop.t = 1.0 + DEADLINE_S + 0.05
        self.assertIsNotNone(rt.tick(loop.t))                 # the trip disarms
        loop.t = 1.0 + idle_s                                 # exactly the window
        rt.tick(loop.t)
        self.assertEqual(rt.owner, APP, "at exactly idle_ms the session still lives")
        rt.transport.sent.clear()
        rt.push_telemetry(loop.t)
        self.assertEqual(len(rt.transport.sent), 1, "telemetry still flows")
        loop.t = 1.0 + idle_s + 0.05                          # strictly past it
        rt.tick(loop.t)
        self.assertIsNone(rt.owner, "the session expired, watchdog_ms earlier than trip+idle")
        self.assertIn("mortal01", rt.dead_sids)
        rt.transport.sent.clear()
        rt.push_telemetry(loop.t)
        self.assertEqual(rt.transport.sent, [], "no more telemetry to a dead peer")
        send(rt, hello("fresh002"))
        self.assertEqual(rt.owner, APP, "a fresh hello starts over")

    def test_activity_moves_the_expiry_deadline(self):
        idle_s = RT["session_idle_ms"] / 1000.0
        rt, car, loop = link()
        loop.t = 1.0
        send(rt, hello("mortal02"))
        send(rt, cmd(10, 0.5))                                # activity at 1.0
        loop.t = 1.0 + DEADLINE_S + 0.05
        rt.tick(loop.t)                                       # trip
        loop.t = 5.0
        send(rt, cmd(11, 0.5))                                # activity moves to 5.0
        loop.t = 1.0 + idle_s + 0.05                          # past the OLD deadline
        rt.tick(loop.t)                                       # (trips again — fine)
        self.assertEqual(rt.owner, APP, "the resumed command moved the deadline")
        loop.t = 5.0 + idle_s + 0.05                          # past the new one
        rt.tick(loop.t)
        self.assertIsNone(rt.owner, "which then passes in its own time")

    def test_a_hello_only_session_expires_too(self):
        """A session that never commanded has its adoption as its last activity —
        the watchdog never armed, so the idle clock runs from the handshake. The
        firmware behaves the same; without this the phantom lives forever."""
        idle_s = RT["session_idle_ms"] / 1000.0
        rt, car, loop = link()
        loop.t = 1.0
        send(rt, hello("ghost001"))
        loop.t = 1.0 + idle_s
        rt.tick(loop.t)
        self.assertEqual(rt.owner, APP, "at exactly the window it still lives")
        loop.t = 1.0 + idle_s + 0.05
        rt.tick(loop.t)
        self.assertIsNone(rt.owner, "a handshake that never commanded ages out")
        self.assertIn("ghost001", rt.dead_sids)
```

- [ ] **Step 2: Run to verify they fail**

Run: `python3 tools/mock_car/test_rtlink.py TestWatchdog -v 2>&1 | tail -10`
Expected: all three new tests FAIL — today the session never expires (`rt.owner` stays `APP`; `dead_sids` misses the sids).

- [ ] **Step 3: Implement** — first in `tools/mock_car/state.py`, add to the "what the outside reads" property block:

```python
    @property
    def armed(self):
        """The control watchdog is armed — a command was accepted since adopt/trip."""
        return self._armed
```

then in `tools/mock_car/rt_link.py`:

`__init__`, after `self.dead_sids = ...`:

```python
        self._last_activity = None   # last accepted command, or the adoption itself
```

In `_handle`, the accepted-command tail currently reads `self.car.note_command(...)` — gate the bookkeeping on acceptance:

```python
        if self.car.note_command(frame[RT["throttle_field"]], frame[RT["yaw_field"]], now):
            self._last_activity = now
            self._rx.append(now)
            self._log_command(now, seq)
```

In `_adopt`, inside the adoption block (next to `self._rx.clear()`): `self._last_activity = now`.

In `tick`, after the `self.car.tick(now)` call (keep Task 2's docstring; add to it: `While the watchdog is not armed, a session strictly more than RT["session_idle_ms"] past its last activity — last accepted command, or the adoption itself — expires: rule 4. The sid joins dead_sids.`):

```python
    def tick(self, now):
        line = self.car.tick(now)
        if (self.owner is not None and not self.car.armed
                and self._last_activity is not None
                and (now - self._last_activity) * 1000.0 > RT["session_idle_ms"]):
            print(f"rt: session {self.session} expired — more than "
                  f"{RT['session_idle_ms']} ms past its last activity")
            self.dead_sids.append(self.session)
            self.owner, self.session, self.last_seq = None, None, None
            self._last_activity = None
        return line
```

(Strictly `>`, and gated on `not self.car.armed`: a healthy armed stream can never expire, and the disarmed states — post-trip, hello-only, and the flash's own disarm — all age out on the same last-activity clock.)

- [ ] **Step 4: Run the mock suites**

Run: `python3 tools/mock_car/test_rtlink.py && python3 tools/mock_car/test_state.py`
Expected: OK twice.

- [ ] **Step 5: Full suite and commit**

```bash
./tools/test-all.sh
git add tools/mock_car/rt_link.py tools/mock_car/test_rtlink.py
git commit -m "feat(mock): a tripped session expires after session_idle_ms

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: /status rx_fps mirrors the firmware's per-consumer accumulator (rule 12)

**Files:**
- Modify: `tools/mock_car/rt_link.py` (`RTLink.__init__`, `_handle`, `rx_fps`, `push_telemetry`, `_log_command`)
- Modify: `tools/mock_car/mock_car.py` (the `status` handler, ~line 137)
- Test: `tools/mock_car/test_rtlink.py` (new class `TestRxFps`)

**Interfaces:**
- Consumes: nothing new.
- Produces: `RTLink.rx_fps(now, who)` where `who` is `"push"` or `"status"` — a per-consumer delta mirroring `fps_now` in `firmware/p4/main/telemetry.c`: 0 on a consumer's first read, 0 when the gap since its last read is ≥ 10 s, else `int(frames_delta / dt)`; the accumulator updates on every read. The 1-second window deque stays only for the human log line via the private `_window_fps(now)`.

- [ ] **Step 1: Write the failing tests** — append to `tools/mock_car/test_rtlink.py`:

```python
class TestRxFps(Quiet):
    """rx_fps semantics = firmware's fps_now (telemetry.c): per-consumer delta."""

    def stream(self, rt, loop, start, count, first_seq=1):
        for k in range(count):
            loop.t = start + k * 0.1
            send(rt, cmd(first_seq + k, 0.5))

    def test_the_first_status_read_is_zero(self):
        rt, _, loop = link()
        send(rt, hello())
        self.stream(rt, loop, 1.0, 10)
        self.assertEqual(rt.rx_fps(loop.t, "status"), 0,
                         "no previous poll to delta against — the car answers 0")

    def test_a_prompt_second_read_measures_the_stream(self):
        rt, _, loop = link()
        send(rt, hello())
        rt.rx_fps(1.0, "status")                       # primes the accumulator
        self.stream(rt, loop, 1.0, 10)                 # 10 frames over 0.9 s
        self.assertEqual(rt.rx_fps(2.0, "status"), 10)

    def test_a_gap_of_ten_seconds_reads_zero(self):
        rt, _, loop = link()
        send(rt, hello())
        rt.rx_fps(1.0, "status")
        self.stream(rt, loop, 1.0, 10)
        self.assertEqual(rt.rx_fps(12.0, "status"), 0, "a stale delta is not a rate")
        self.stream(rt, loop, 12.0, 10, first_seq=11)
        self.assertEqual(rt.rx_fps(13.0, "status"), 10, "and the next prompt read works")

    def test_status_reads_do_not_perturb_the_push_consumer(self):
        rt, _, loop = link()
        send(rt, hello())
        rt.rx_fps(1.0, "push")
        self.stream(rt, loop, 1.0, 10)
        rt.rx_fps(1.5, "status")
        rt.rx_fps(1.7, "status")
        self.assertEqual(rt.rx_fps(2.0, "push"), 10,
                         "each consumer deltas against its own last read")
```

- [ ] **Step 2: Run to verify they fail**

Run: `python3 tools/mock_car/test_rtlink.py TestRxFps -v`
Expected: `TypeError: rx_fps() takes 2 positional arguments but 3 were given` — the consumer argument does not exist yet.

- [ ] **Step 3: Implement** — in `tools/mock_car/rt_link.py`:

`__init__`: after `self._rx = deque()` add:

```python
        self._frames = 0           # total accepted commands, the counter fps deltas read
        self._fps_last = {}        # per-consumer (frames, at) — telemetry.c's fps_now
```

In `_handle`'s accepted tail (from Task 5), add `self._frames += 1` before `self._rx.append(now)`.

Replace `rx_fps` with:

```python
    def rx_fps(self, now, who):
        """telemetry.c's fps_now: a delta against this consumer's previous read.

        0 on the first read and after a >=10 s gap — a stale delta is not a rate —
        and the accumulator updates either way, so the next prompt read measures.
        `who` is "push" or "status"; each consumer deltas against its own history,
        which is why a 1 Hz /status poll cannot perturb the pushed stream's number.
        """
        fps = 0
        prev = self._fps_last.get(who)
        if prev is not None:
            frames, at = prev
            dt = now - at
            if 0 < dt < 10.0:
                fps = int((self._frames - frames) / dt)
        self._fps_last[who] = (self._frames, now)
        return fps

    def _window_fps(self, now):
        """The old 1-second window, kept only for the human log line."""
        while self._rx and now - self._rx[0] > 1.0:
            self._rx.popleft()
        return len(self._rx)
```

`push_telemetry`: `self._send(self.car.telemetry(self.rx_fps(now, "push")), self.owner)`.
`_log_command`: use `self._window_fps(now)` in the f-string instead of `self.rx_fps(now)`.

In `tools/mock_car/mock_car.py`'s `status` handler: `**car.telemetry(link.rx_fps(now, "status"), bump=False),`.

- [ ] **Step 4: Run the mock suites**

Run: `python3 tools/mock_car/test_rtlink.py && python3 tools/mock_car/test_state.py`
Expected: OK twice. `test_a_push_goes_to_the_owner_and_carries_the_live_state` asserts `rx_fps == 1` from a push — with the delta design the first push reads 0... **if it fails on that assertion, update that one assertion to `frame["rx_fps"], 0` with the comment `# first read of the push consumer — fps_now answers 0 until it has a delta`**; that is the firmware's real first-push behaviour and the old window value was the drift this task removes.

- [ ] **Step 5: Full suite and commit**

```bash
./tools/test-all.sh
git add tools/mock_car/rt_link.py tools/mock_car/mock_car.py tools/mock_car/test_rtlink.py
git commit -m "fix(mock): rx_fps is a per-consumer delta, as telemetry.c computes it

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: /calib/spin answers after the pulse, not before (mock-local rule)

**Files:**
- Modify: `tools/mock_car/state.py` (new method `CarState.end_spin`)
- Modify: `tools/mock_car/mock_car.py` (the `calib_spin` handler, ~line 150)
- Test: `tools/mock_car/test_state.py` (class `TestActuatorOwnership`)

**Interfaces:**
- Consumes: `CarState.CALIB_HOLD_MS` (existing, mirrors `LINK_HOLD_CALIB_MS`).
- Produces: `CarState.end_spin()` — releases the CALIB grant (no-op if something else owns); the handler sleeps `CALIB_HOLD_MS` **while holding the app lock**, releases, then answers — `calib_api.c`'s order (`vTaskDelay(pdMS_TO_TICKS(LINK_HOLD_CALIB_MS)); link_release(LINK_SRC_CALIB); reply`). Task 12 pins the visible timing cross-implementation.

- [ ] **Step 1: Write the failing test** — append to `TestActuatorOwnership` in `tools/mock_car/test_state.py`:

```python
    def test_end_spin_releases_the_pulse_and_only_the_pulse(self):
        """calib_api.c sleeps the pulse out and releases before replying, so the
        200 lands after the wheel has stopped — the wizard's next step assumes it."""
        car = CarState(now=0.0)
        self.assertTrue(car.begin_spin(0.0, 1, 1))
        car.end_spin()
        self.assertEqual(car.ctl, CTL_NONE)
        self.assertEqual(car.command, (0.0, 0.0), "the release is the stop")
        car.begin_ota(1.0)
        car.end_spin()
        self.assertEqual(car.ctl, CTL_OTA, "end_spin never releases someone else's grant")
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 tools/mock_car/test_state.py TestActuatorOwnership -v 2>&1 | tail -6`
Expected: `AttributeError: 'CarState' object has no attribute 'end_spin'`.

- [ ] **Step 3: Implement** — in `tools/mock_car/state.py`, after `begin_spin`:

```python
    def end_spin(self):
        """The pulse is over: release CALIB, which zeroes if the pulse still owns.

        calib_api.c does exactly this after its vTaskDelay, *before* replying, so
        the wizard's 200 arrives with the wheel already stopped and the grant gone.
        """
        self._release(CTL_CALIB)
```

In `tools/mock_car/mock_car.py`'s `calib_spin`, replace the tail after the 409 check:

```python
    print(f"calib: spin pair={pair} {'fwd' if direction else 'rev'}")
    # The firmware's order (calib_api.c): sleep the pulse out, release, then answer.
    # The reply lands after the wheel has stopped — the wizard's next step assumes
    # it — and the app lock is held throughout, as the single httpd task is.
    await asyncio.sleep(CarState.CALIB_HOLD_MS / 1000.0)
    car.end_spin()
    return web.json_response({"ok": True})
```

(`CarState` is already imported in mock_car.py.)

- [ ] **Step 4: Run the mock suites**

Run: `python3 tools/mock_car/test_state.py && python3 tools/mock_car/test_rtlink.py`
Expected: OK twice.

- [ ] **Step 5: Full suite and commit**

```bash
./tools/test-all.sh
git add tools/mock_car/state.py tools/mock_car/mock_car.py tools/mock_car/test_state.py
git commit -m "fix(mock): /calib/spin answers after the pulse, as the car does

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: The OTA line — arbiter, grant-before-read, magic, lock, reboot (mock-local rules)

**Files:**
- Modify: `tools/mock_car/state.py` (`CarState.begin_ota`)
- Modify: `tools/mock_car/mock_car.py` (`one_at_a_time` middleware, `ota` handler)
- Modify: `tools/mock_car/rt_link.py` (`REBOOT_QUIET_S`, `RTLink.__init__`, `datagram_received`, `push_telemetry`, new `simulate_reboot`)
- Test: `tools/mock_car/test_state.py` (class `TestActuatorOwnership`), `tools/mock_car/test_rtlink.py` (new class `TestReboot`)

**Interfaces:**
- Consumes: `dead_sids` (Task 4).
- Produces: `CarState.begin_ota(now) -> bool` (False when a higher-priority holder refuses — the firmware's 500 "actuator busy" path); `RTLink.simulate_reboot(now)` — records the sid dead, clears the session, and makes the link deaf **and** mute for `REBOOT_QUIET_S` = 4.0 s (a hand-mirrored constant: longer than the app's 3 s `stallTimeout` in `CarTransport.swift`, so the app tears down and re-hellos into the bumped fw).

- [ ] **Step 1: Write the failing state-level test** — append to `TestActuatorOwnership` in `tools/mock_car/test_state.py`:

```python
    def test_ota_goes_through_the_arbiter(self):
        """ota_api.c takes the actuator through the checked car_stop(LINK_SRC_OTA)
        and answers 500 when refused; the mock used to seize it unconditionally,
        so the simulator never exhibited that 500."""
        car = CarState(now=0.0)
        # SAFE is only ever held transiently by note_bye, so stage it directly.
        self.assertTrue(car._take(CTL_SAFE, 0.0, None))
        self.assertFalse(car.begin_ota(0.1), "SAFE outranks a flash, as on the car")
        car._release(CTL_SAFE)
        self.assertTrue(car.begin_ota(0.2))
        fw = car.fw
        car.end_ota(flashed=False)
        self.assertEqual(car.ctl, CTL_NONE, "an aborted flash releases the grant")
        self.assertEqual(car.fw, fw, "and does not bump the build")
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 tools/mock_car/test_state.py TestActuatorOwnership -v 2>&1 | tail -6`
Expected: FAIL — `begin_ota` returns `None` today (no return statement) and seizes even from SAFE.

- [ ] **Step 3: Implement the state half** — in `tools/mock_car/state.py`, replace `begin_ota`:

```python
    def begin_ota(self, now):
        """Nothing commands the motors during a flash; the grant is sticky.

        Through the arbiter, as ota_api.c goes through car_stop(LINK_SRC_OTA):
        a refusal is the firmware's 500 "actuator busy", and the simulator must
        be able to exhibit it. Returns False without touching anything when a
        higher-priority holder refuses.
        """
        self._now = now
        self._expire(now)
        if not self._take(CTL_OTA, now, None):
            return False
        self._t = self._y = 0.0
        self._armed = False
        self._retreating = False
        return True
```

- [ ] **Step 4: Run state tests**

Run: `python3 tools/mock_car/test_state.py`
Expected: OK — the existing OTA tests call `begin_ota` from states OTA outranks, so they keep passing.

- [ ] **Step 5: Write the failing link-level test** — append to `tools/mock_car/test_rtlink.py` (and extend the import line to `from rt_link import Impairment, REBOOT_QUIET_S, RTLink`):

```python
class TestReboot(Quiet):
    def test_a_simulated_reboot_is_deaf_mute_and_forgets_the_session(self):
        """After a real flash the car reboots: telemetry gaps past the app's 3 s
        stall, the session dies, and the reconnect's hello reply carries the new
        fw. The mock's OTA used to keep the session alive through the flash, so
        the app's success detector could never fire in the simulator."""
        rt, car, loop = link()
        send(rt, hello("flasher1"))
        send(rt, cmd(1, 0.2))
        car.begin_ota(0.0)
        car.end_ota()                             # bumps car.fw
        rt.simulate_reboot(10.0)
        self.assertIsNone(rt.owner)
        self.assertIn("flasher1", rt.dead_sids)
        rt.transport.sent.clear()
        loop.t = 11.0                             # inside the quiet window
        send(rt, hello("fresh003"))
        self.assertIsNone(rt.owner, "a rebooting car hears nothing")
        self.assertEqual(rt.transport.sent, [])
        rt.push_telemetry(loop.t)
        self.assertEqual(rt.transport.sent, [], "and says nothing")
        loop.t = 10.0 + REBOOT_QUIET_S + 0.1
        send(rt, hello("fresh003"))
        self.assertEqual(rt.owner, APP, "the reconnect adopts")
        self.assertEqual(rt.transport.sent[-1][0][RT["fw_field"]], car.fw,
                         "and the hello reply carries the bumped fw")
```

- [ ] **Step 6: Run to verify it fails**

Run: `python3 tools/mock_car/test_rtlink.py TestReboot -v`
Expected: `ImportError: cannot import name 'REBOOT_QUIET_S'`.

- [ ] **Step 7: Implement the link half** — in `tools/mock_car/rt_link.py`:

Module level, next to `TICK_S`:

```python
# How long the mock is deaf and mute after a "flash", simulating the reboot.
# Hand-mirrored against the app: CarTransport.swift's stallTimeout is 3 s, and the
# gap must exceed it so the app tears down, re-hellos, and reads the new fw from
# the hello reply — which is the only place it learns fw.
REBOOT_QUIET_S = 4.0
```

`__init__`: add `self._quiet_until = None`.

`datagram_received`, first lines:

```python
    def datagram_received(self, data, addr):
        now = self.loop.time()
        if self._quiet_until is not None and now < self._quiet_until:
            return                                  # "rebooting": deaf
        if self.impair.stalled(now):
            ...
```

`push_telemetry`, extend the guard:

```python
        if self.owner is None or self.impair.stalled(now):
            return
        if self._quiet_until is not None and now < self._quiet_until:
            return                                  # "rebooting": mute
```

(the owner is `None` during the quiet window anyway; the guard keeps the invariant if that ever changes). New method after `tick`:

```python
    def simulate_reboot(self, now):
        """The flash ended: the car reboots. Session dead, link silent for a while.

        What the app observes on hardware — a telemetry gap past its stall guard,
        then a fresh handshake answering with the new fw — must be observable in
        the simulator too, or the OTA screen's success detector can never fire.
        """
        if self.session is not None:
            self.dead_sids.append(self.session)
        self.owner, self.session, self.last_seq = None, None, None
        self._last_activity = None
        self._quiet_until = now + REBOOT_QUIET_S
        print(f"rt: 'rebooting' — deaf and mute for {REBOOT_QUIET_S:g} s")
```

- [ ] **Step 8: Rewire the handler and the middleware** — in `tools/mock_car/mock_car.py`:

Replace the middleware (the `/ota` exemption goes away — the firmware's single httpd task serves nothing else during a flash, so neither does the mock):

```python
@web.middleware
async def one_at_a_time(request, handler):
    """The firmware serves REST from a single httpd task, so requests queue behind
    each other — including behind the whole of an OTA upload and flash. A mock
    that answers /status mid-flash teaches a client the car can do that; the car
    holds its one task from the first body byte to the reboot."""
    async with request.app["lock"]:
        return await handler(request)
```

Replace the `ota` handler (no inner lock — the middleware holds it):

```python
async def ota(request):
    car, link = request.app["car"], request.app["link"]
    now = asyncio.get_running_loop().time()
    # The car stops the motors and takes the sticky grant before reading a single
    # body byte (car_stop(LINK_SRC_OTA) is ota_api.c's first statement), and
    # answers 500 when something outranks the flash.
    if not car.begin_ota(now):
        return json_error(500, "actuator busy")
    data = await request.read()
    if len(data) < OTA_MIN_BYTES:
        car.end_ota(flashed=False)
        return json_error(400, "image too small")
    if data[0] != 0xE9:
        # esp_ota_write validates the ESP image magic on the first write, and
        # ota_api.c answers 500 "ota write failed". Any 4 KB blob used to flash
        # here and bump fw — the exact wrong-release-asset path the app could
        # never rehearse.
        car.end_ota(flashed=False)
        return json_error(500, "ota write failed")
    print(f"ota: {len(data)} bytes — motors stopped, flashing")
    await asyncio.sleep(OTA_SECONDS)
    car.end_ota()
    print(f"ota: done, now running {car.fw} — 'rebooting'")
    link.simulate_reboot(asyncio.get_running_loop().time())
    return web.json_response({"ok": True})
```

- [ ] **Step 9: Run the mock suites**

Run: `python3 tools/mock_car/test_rtlink.py && python3 tools/mock_car/test_state.py`
Expected: OK twice.

- [ ] **Step 10: Full suite and commit**

`./tools/test-all.sh` must stay green — conformance's existing `/ota (32 bytes)` case still gets its 400. Then:

```bash
git add tools/mock_car/state.py tools/mock_car/mock_car.py tools/mock_car/rt_link.py \
        tools/mock_car/test_state.py tools/mock_car/test_rtlink.py
git commit -m "fix(mock): the OTA line tells the truth — arbiter, grant-before-read, image magic, lock, reboot

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: /calib/save rejects the types the car rejects (mock-local rule, spec rule 7)

**Files:**
- Modify: `tools/mock_car/state.py` (`CarState.save_calibration`)
- Test: `tools/mock_car/test_state.py` (class `TestCalibration`)

**Interfaces:**
- Consumes: nothing new.
- Produces: `save_calibration(wheels) -> bool` rejects string-, bool- and fractional-typed `pair`/`sign`; integral floats (`1.0`) stay accepted, as cJSON accepts them.

- [ ] **Step 1: Write the failing tests** — in `TestCalibration`, extend `test_invalid_tables_are_refused`'s list with three entries (after `"wheels"`):

```python
                       [{"pair": "0", "sign": "1"},                      # strings
                        {"pair": "1", "sign": "1"},
                        {"pair": "2", "sign": "1"},
                        {"pair": "3", "sign": "1"}],
                       [{"pair": p, "sign": True} for p in range(4)],    # booleans
                       [{"pair": p + 0.5, "sign": 1} for p in range(4)],  # fractions
```

and append one test:

```python
    def test_integral_floats_are_numbers(self):
        """cJSON sees 1.0 as a number with valueint 1; so does the car."""
        car = CarState()
        wheels = [{"pair": float(p), "sign": 1.0} for p in range(4)]
        self.assertTrue(car.save_calibration(wheels))
```

- [ ] **Step 2: Run to verify they fail**

Run: `python3 tools/mock_car/test_state.py TestCalibration -v`
Expected: `test_invalid_tables_are_refused` FAILs — `int("0")` and `int(True)` coerce today.

- [ ] **Step 3: Implement** — in `tools/mock_car/state.py`, replace `save_calibration`'s try-block:

```python
        try:
            if not isinstance(wheels, list) or len(wheels) != 4:
                return False
            for w in wheels:
                for key in ("pair", "sign"):
                    v = w[key]
                    # cJSON_IsNumber: a JSON number, never a bool or a string —
                    # and rule 7 has both sides reject fractions. int("0") and
                    # int(True) coerced here while the car answered 400, the
                    # works-in-sim/fails-on-car trap on the one endpoint that
                    # guards the calibration table.
                    if isinstance(v, bool) or not isinstance(v, (int, float)):
                        return False
                    if float(v) != int(v):
                        return False
            pairs = {int(w["pair"]) for w in wheels}
            signs = [int(w["sign"]) for w in wheels]
        except (TypeError, ValueError, KeyError):
            return False
```

- [ ] **Step 4: Run, full suite, commit**

```bash
python3 tools/mock_car/test_state.py && python3 tools/mock_car/test_rtlink.py
./tools/test-all.sh
git add tools/mock_car/state.py tools/mock_car/test_state.py
git commit -m "fix(mock): /calib/save rejects the types cJSON rejects

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 10: Pin the two-phone oscillation; record the eviction-notice deferral (rule 11)

**Files:**
- Modify: `docs/IDEAS.md` (append one bullet)
- Test: `tools/mock_car/test_rtlink.py` (new class `TestTwoPhones`)

**Interfaces:** none — this pins existing behaviour and records a deferral.

- [ ] **Step 1: Write the pinning test** — append to `tools/mock_car/test_rtlink.py`:

```python
class TestTwoPhones(Quiet):
    def test_last_hello_wins_and_nobody_is_told(self):
        """Rule 11: the audit flagged silent hijack and the ~3 s two-phone
        ownership oscillation; the spec defers the eviction notice and pins the
        behaviour instead. If a notice is ever added, this is the test to rewrite."""
        rt, car, _ = link()
        send(rt, hello("phoneaaa"), addr=APP)
        send(rt, cmd(1, 0.5), addr=APP)
        sent_before = len(rt.transport.sent)
        send(rt, hello("phonebbb"), addr=OTHER)      # B silently steals the session
        self.assertEqual(rt.owner, OTHER)
        to_a = [d for d in rt.transport.sent[sent_before:] if d[1] == APP]
        self.assertEqual(to_a, [], "the displaced phone is told nothing")
        send(rt, cmd(2, 0.9), addr=APP)              # A keeps streaming, unheard
        self.assertEqual(car.command, (0.5, 0.0), "A's frames no longer land")
        self.assertEqual(car.history_len, 0, "adoption wiped A's breadcrumbs")
        send(rt, hello("phoneaa2"), addr=APP)        # A's stall guard re-hellos
        self.assertEqual(rt.owner, APP, "and the theft works both ways, forever")
```

- [ ] **Step 2: Run — expect PASS** (this pins current behaviour): `python3 tools/mock_car/test_rtlink.py TestTwoPhones -v`. If it fails, the mock has drifted from the documented model — stop and investigate.

- [ ] **Step 3: Append to `docs/IDEAS.md`** (keep the file's Russian, dated-bullet style):

```markdown
- **Датаграмма-уведомление о выселении с RT-канала** (2026-08-22). Сейчас второй телефон
  молча угоняет сессию (last hello wins), вытесненный узнаёт по погасшей телеметрии через
  ~3 с — и крадёт обратно: устойчивая осцилляция с периодом ~3 с на два открытых пульта
  (запинено `test_rtlink.TestTwoPhones`). Идея: на adopt слать вытесненному адресу один
  датаграм `{"evicted":"<sid>"}`, а в апп — экран «пультом завладел другой телефон».
  Отложено аудитом 2026-08-22: правка провода на всех трёх сторонах ради редкого сценария.
```

- [ ] **Step 4: Full suite and commit**

```bash
./tools/test-all.sh
git add tools/mock_car/test_rtlink.py docs/IDEAS.md
git commit -m "test(mock): pin the two-phone last-hello-wins oscillation; record the eviction-notice deferral

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 11: The contract range guard asserts both bounds, word-bounded (safety net)

**Files:**
- Modify: `tools/test_gen_contract.py` (`test_ranges_match_the_firmware_today`, ~line 86)

**Interfaces:** none.

- [ ] **Step 1: Strengthen the guard** — replace the final two lines of `test_ranges_match_the_firmware_today` (`for lo, hi in expected.values(): self.assertIn(...)`) with:

```python
        for (path, name), (lo, hi) in expected.items():
            for bound in (lo, hi):
                # Word-bounded: "2000" must not be satisfied by "12000", and "30"
                # must not be found inside "-30". The min bounds were entirely
                # unchecked before — a firmware floor drifting from the schema is
                # exactly what this test exists to catch.
                pat = rf"(?<![\w.-]){re.escape(str(bound))}(?![\w.])"
                self.assertRegex(src, pat,
                                 f"{path} {name}: bound {bound} not in the firmware sources")
```

(`re` is already imported at the top of the file.)

- [ ] **Step 2: Run — expect PASS on the honest tree**

Run: `python3 tools/test_gen_contract.py TestSchema.test_ranges_match_the_firmware_today -v`
Expected: PASS — every bound, min included, exists as a literal today (`RECOVER_WIN_MIN_MS 1000`, `pct < -30`, `v >= 0`, `WHEEL_PPR_MIN 1`, …).

- [ ] **Step 3: Prove it catches the drift the old guard missed**

```bash
sed -i '' 's/RECOVER_WIN_MIN_MS 1000/RECOVER_WIN_MIN_MS 500/' firmware/p4/main/recovery.h
python3 tools/test_gen_contract.py TestSchema.test_ranges_match_the_firmware_today -v
git checkout firmware/p4/main/recovery.h
```

Expected: FAIL while mutated (`/recover window_ms: bound 1000 not in the firmware sources`), then the checkout restores it. The old substring-of-max guard stayed green under this exact mutation.

- [ ] **Step 4: Full suite and commit**

```bash
./tools/test-all.sh
git add tools/test_gen_contract.py
git commit -m "test(contract): the range guard asserts both bounds, word-bounded

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 12: Name-keyed ctl symbols reach Python (safety net)

**Files:**
- Modify: `tools/gen_contract.py` (`emit_python`)
- Modify: `tools/mock_car/state.py` (imports; delete the positional unpack)
- Regenerate: `tools/mock_car/generated.py`
- Test: `tools/test_gen_contract.py` (`TestPythonEmitter`), `tools/mock_car/test_state.py` (`TestOwnershipVocabulary`)

**Interfaces:**
- Produces: `generated.py` module constants `CTL_NONE = 'none'`, `CTL_RECOVER = 'recover'`, `CTL_CONSOLE`, `CTL_RT`, `CTL_CALIB`, `CTL_OTA`, `CTL_SAFE`. `state.py` re-exports them unchanged, so every existing `from state import CTL_*` keeps working.

- [ ] **Step 1: Write the failing emitter test** — append to `TestPythonEmitter` in `tools/test_gen_contract.py`:

```python
    def test_ctl_symbols_are_name_keyed(self):
        """Reordering ctl_values in the schema must not silently re-rank the
        mock's arbiter against the car's hand-written link_src_t (whose build
        guard checks only the count). Name-keyed symbols make state.py immune
        to position, as C's CTL_RT and Swift's CtlOwner.rt already are."""
        for v in load()["ctl_values"]:
            self.assertEqual(self.ns[f"CTL_{v.upper()}"], v)
```

and to `TestOwnershipVocabulary` in `tools/mock_car/test_state.py`:

```python
    def test_the_symbols_spell_the_wire_values(self):
        self.assertEqual((CTL_NONE, CTL_RECOVER, CTL_RT, CTL_OTA),
                         ("none", "recover", "rt", "ota"))
```

- [ ] **Step 2: Run to verify the emitter test fails**

Run: `python3 tools/test_gen_contract.py TestPythonEmitter -v 2>&1 | tail -6`
Expected: `KeyError: 'CTL_NONE'` — emit_python emits only the flat list today.

- [ ] **Step 3: Implement** — in `tools/gen_contract.py`'s `emit_python`, replace the single `CTL_VALUES` entry in the returned list with:

```python
        f"CTL_VALUES = {schema['ctl_values']!r}",
        "",
        "# Name-keyed, like C's CTL_RT and Swift's CtlOwner.rt. Position in",
        "# CTL_VALUES is still rank; these names free callers from the unpack.",
        *[f"CTL_{v.upper()} = {v!r}" for v in schema["ctl_values"]],
```

Regenerate and verify no drift: `python3 tools/gen_contract.py && bash tools/check_contract.sh` → `contract: no drift`.

In `tools/mock_car/state.py`: extend the `from generated import ...` line to

```python
from generated import (CTL_CALIB, CTL_CONSOLE, CTL_NONE, CTL_OTA, CTL_RECOVER,
                       CTL_RT, CTL_SAFE, CTL_VALUES, DEVICE, DOMAINS, RT,
                       TELEMETRY_FIELDS, validate)
```

delete the `CTL_NONE, CTL_RECOVER, ... = PRIORITY` unpack and shrink its comment block to:

```python
# Ownership of the actuator, lowest priority first. `link_src_t` in
# firmware/p4/main/link.h is generated from the same list, and these names are
# what telemetry reports in `ctl`. Position is rank on all three sides; the
# per-name symbols come from the generator, same as C's and Swift's.
PRIORITY = tuple(CTL_VALUES)
```

- [ ] **Step 4: Run everything and commit**

```bash
python3 tools/test_gen_contract.py && python3 tools/mock_car/test_state.py \
  && python3 tools/mock_car/test_rtlink.py && ./tools/test-all.sh
git add tools/gen_contract.py tools/mock_car/generated.py tools/mock_car/state.py \
        tools/test_gen_contract.py tools/mock_car/test_state.py
git commit -m "feat(contract): emit_python gains name-keyed ctl symbols; state.py drops the positional unpack

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 13: conformance.py asserts the unified bodies (rules 8, 9; after Tasks 7–8)

**Files:**
- Modify: `tools/conformance.py` (`expect_rejected`, `calibration`, `ota`, `identity`, module docstring, imports)

**Interfaces:**
- Consumes: mock behaviour from Tasks 7 (spin timing) and 8 (OTA magic/arbiter). `CarState.CALIB_HOLD_MS` via `from state import CarState` (stdlib-safe, mock_car dir is already on sys.path).
- **Cross-plan:** against the **mock** these checks pass as soon as Tasks 7–8 land; against a **real car** they pass only after the firmware plan's "REST bodies become the JSON envelope" and "GET / identity" tasks. Until then a bench run fails on exactly those lines — deliberate: the suite now asserts the contract, not the intersection.

- [ ] **Step 1: Generalize the rejection helper** — change the signature and first lines of `expect_rejected`:

```python
    def expect_rejected(self, where, path, body, field=None, raw=None, status=400):
        """A rejection carrying the `{"error","field"}` envelope.

        Since the 2026-08-22 unification (spec rule 8) this covers /calib* and
        /ota too, not only the config domains."""
        got, ctype, parsed, _ = self.call("POST", path, body, raw=raw)
        if not self.expect_json(where, got, ctype, parsed, status):
            return
```

(the remaining body of the method is unchanged except it no longer references the old local name `status` for the response — keep the error/field checks as they are).

- [ ] **Step 2: Rewrite `calibration()`'s POST checks** — keep the `GET /calib` block, then replace every `expect_status` call in the method with:

```python
        self.expect_rejected("POST /calib/spin pair=9", "/calib/spin",
                             {"pair": 9, "dir": 1}, "pair")
        self.expect_rejected("POST /calib/spin dir=7", "/calib/spin",
                             {"pair": 0, "dir": 7}, "dir")
        self.expect_rejected("POST /calib/spin (empty body)", "/calib/spin", {})
        self.expect_rejected("POST /calib/spin (malformed JSON)", "/calib/spin",
                             None, raw=b"{not json")

        # The one call that moves the car. 200 (it span) and 409 (something
        # outranks the wizard) are both conformant — but a 200 must land AFTER
        # the pulse: calib_api.c sleeps it out so the wizard's "which wheel
        # turned?" prompt appears with the wheel already stopped, and a client
        # paced against an instant reply mispaces on hardware.
        t0 = time.monotonic()
        status, ctype, parsed, _ = self.call("POST", "/calib/spin", {"pair": 0, "dir": 1})
        elapsed = time.monotonic() - t0
        self.check(status in (200, 409), f"spin: status {status}, want 200 or 409")
        self.check(ctype.startswith("application/json"),
                   f"spin: Content-Type {ctype!r}, want application/json")
        if status == 200:
            self.check(parsed == {"ok": True}, f"spin: body {parsed}, want {{'ok':true}}")
            floor = CarState.CALIB_HOLD_MS / 1000.0 * 0.8
            self.check(elapsed >= floor,
                       f"spin: 200 in {elapsed:.2f}s, want >= {floor:.2f}s (after the pulse)")
        else:
            self.check(isinstance(parsed, dict) and isinstance(parsed.get("error"), str),
                       f"spin 409: body {parsed}, want the error envelope")

        # Only rejections: a valid table cannot be un-saved.
        self.expect_rejected("POST /calib/save (three wheels)", "/calib/save",
                             {"wheels": [{"pair": p, "sign": 1} for p in range(3)]}, "wheels")
        self.expect_rejected("POST /calib/save (duplicate pairs)", "/calib/save",
                             {"wheels": [{"pair": 0, "sign": 1}] * 4}, "wheels")
        self.expect_rejected("POST /calib/save (sign 0)", "/calib/save",
                             {"wheels": [{"pair": p, "sign": 0} for p in range(4)]}, "wheels")
        self.expect_rejected("POST /calib/save (string pairs)", "/calib/save",
                             {"wheels": [{"pair": str(p), "sign": 1} for p in range(4)]},
                             "wheels")
        self.expect_rejected("POST /calib/save (no wheels)", "/calib/save", {}, "wheels")
```

Add `import time` to the imports. Update the method's docstring (it no longer claims bodies are unasserted).

- [ ] **Step 3: Rewrite `ota()`**:

```python
    def ota(self):
        print("/ota")
        # Neither body flashes anything: the first is under the size floor, the
        # second fails the ESP image magic on the first write. On a real car both
        # do stop the motors and briefly take the actuator, like the spin above.
        self.expect_rejected("POST /ota (32 bytes)", "/ota", None,
                             raw=b"\xe9" + b"\x00" * 31)
        self.expect_rejected("POST /ota (4 KB, bad magic)", "/ota", None,
                             raw=b"\x00" * 4096, status=500)
```

- [ ] **Step 4: Rewrite `identity()`** (rule 9 — the line leads with the device id):

```python
    def identity(self):
        print("/")
        _, _, parsed, _ = self.call("GET", "/status")
        device = (parsed or {}).get(RT["device_field"], "")
        status, _, _, payload = self.call("GET", "/")
        self.check(status == 200, f"/: status {status}, want 200")
        line = payload.decode(errors="replace").strip()
        self.check(bool(line), "/: empty identity line")
        self.check(bool(device) and line.split()[:1] == [device],
                   f"/: {line!r} does not lead with the device id {device!r}")
```

- [ ] **Step 5: Update the module docstring** — replace the paragraph beginning `` `/calib*` and `/ota` are **asserted by status code only** `` with:

```
`/calib*` and `/ota` are asserted against the same `{"ok":true}` / `{"error","field"}`
envelope as the config domains — unified on 2026-08-22 (audit-fix spec rule 8). A
firmware older than that unification fails exactly those lines; that is the point.
The spin's 200 is also held to land only after the pulse, the timing calib_api.c
documents the wizard as assuming.
```

and extend the "three things it does anyway" sentence with: `and the two rejected /ota bodies stop the motors and briefly take the actuator on a real car`.

- [ ] **Step 6: Run against the mock and commit**

```bash
./tools/test-all.sh
git add tools/conformance.py
git commit -m "test(conformance): /calib* and /ota held to the unified JSON envelope, spin timing pinned

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 14: conformance_rt.py — the UDP wire gets a cross-implementation check (safety net)

**Files:**
- Create: `tools/conformance_rt.py`
- Modify: `tools/test-all.sh` (the conformance block)

**Interfaces:**
- Consumes: `generated.py`'s `PROTO`, `RT`, `CTL_VALUES`, `TELEMETRY_FIELDS`.
- Produces: `python3 tools/conformance_rt.py <host:port>` → exit 0 all-passed / 1 failures / 2 unreachable. Run by `test-all.sh` against the mock's RT port; by hand against `192.168.4.1:4210`.

- [ ] **Step 1: Write the tool** — create `tools/conformance_rt.py`:

```python
#!/usr/bin/env python3
"""The real-time (UDP) conformance matrix, run against the mock or a real car.

    python3 tools/conformance_rt.py 127.0.0.1:4237
    python3 tools/conformance_rt.py 192.168.4.1:4210

What conformance.py is to REST, this is to the wire the app drives on: the hello
handshake, replies to repeats, the telemetry push and its schema, silence toward
a displaced socket, the goodbye, and the datagrams both sides must drop — spoken
as real datagrams against a real socket. The audit found the RT channel had no
cross-implementation check at all: the mock tested itself, the firmware tested
itself, and only REST was compared. This is the comparison.

Stdlib only — no venv needed against a car. The dropped frames come from the
rule-6 table in docs/superpowers/specs/2026-08-22-audit-fix-decisions.md, the
same table test_state.py and the firmware host tests pin.
"""
import argparse
import json
import os
import secrets
import socket
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_car"))
from generated import CTL_VALUES, PROTO, RT, TELEMETRY_FIELDS   # noqa: E402

JSON_TYPES = {"int": int, "bool": bool, "str": str}

DROPPED_FRAMES = [                       # rule 6: both sides drop these whole
    b'{"seq":5,"junk":{"t":0.9},"y":0.5}',
    b'{"seq":7,"t":.5,"y":0}',
    b'{"seq":8,"t":+1,"y":0}',
    b'{"seq":9,"t":0.5,"y":0,"t":0.9}',
    b'{"proto":1.5,"hello":"abcd1234"}',
    b'{"bye":1}',                        # a goodbye without a seq is dropped too
]


class Unreachable(Exception):
    pass


def enc(obj):
    return json.dumps(obj, separators=(",", ":")).encode()


class RTConformance:
    def __init__(self, host, port, verbose=False):
        self.addr = (host, port)
        self.verbose = verbose
        self.failures = []

    def check(self, ok, what):
        if not ok:
            self.failures.append(what)
            print(f"  FAIL  {what}")
        return ok

    def sock(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(0.2)
        return s

    def recv_frame(self, s, deadline_s):
        """The next parseable frame within deadline_s, or None."""
        end = time.monotonic() + deadline_s
        while time.monotonic() < end:
            try:
                data, _ = s.recvfrom(RT["max_datagram"])
            except socket.timeout:
                continue
            try:
                frame = json.loads(data)
            except ValueError:
                self.check(False, f"unparseable datagram from the car: {data[:60]!r}")
                continue
            if self.verbose:
                print(f"    <- {frame}")
            return frame
        return None

    def recv_matching(self, s, pred, deadline_s):
        end = time.monotonic() + deadline_s
        while time.monotonic() < end:
            f = self.recv_frame(s, end - time.monotonic())
            if f is not None and pred(f):
                return f
        return None

    def drain(self, s):
        while True:
            try:
                s.recvfrom(RT["max_datagram"])
            except socket.timeout:
                return

    def handshake(self, s, sid, proto=PROTO):
        """Send hello at the app's retry cadence until answered."""
        frame = {RT["proto_field"]: proto, RT["hello_field"]: sid}
        for _ in range(15):                                 # ~3 s at 5 Hz
            s.sendto(enc(frame), self.addr)
            reply = self.recv_matching(s, lambda f: RT["hello_field"] in f, 0.2)
            if reply is not None:
                return reply
        raise Unreachable(f"no hello reply from {self.addr[0]}:{self.addr[1]}")

    def run(self):
        sid = secrets.token_hex(4)
        s = self.sock()

        print("hello")
        reply = self.handshake(s, sid)
        self.check(reply.get(RT["proto_field"]) == PROTO,
                   f"hello reply proto {reply.get(RT['proto_field'])!r}, want {PROTO}")
        self.check(reply.get(RT["hello_field"]) == sid,
                   f"hello reply echoes {reply.get(RT['hello_field'])!r}, want {sid!r}")
        for key in (RT["device_field"], RT["fw_field"]):
            self.check(isinstance(reply.get(key), str) and reply[key],
                       f"hello reply {key} is {reply.get(key)!r}, want a nonempty string")
        again = self.handshake(s, sid)
        self.check(again.get(RT["hello_field"]) == sid,
                   "a repeated hello is answered (a lost reply must be recoverable)")

        print("wrong proto")
        s2 = self.sock()
        foreign = self.handshake(s2, secrets.token_hex(4), proto=PROTO + 1)
        self.check(foreign.get(RT["proto_field"]) == PROTO,
                   "a foreign proto is answered by name, so a client can stop searching")
        s2.close()

        print("telemetry")
        seq = 0
        frames = []
        end = time.monotonic() + 1.5
        next_send = 0.0
        while time.monotonic() < end:
            now = time.monotonic()
            if now >= next_send:
                seq += 1
                s.sendto(enc({RT["seq_field"]: seq, RT["throttle_field"]: 0.0,
                              RT["yaw_field"]: 0.0}), self.addr)
                next_send = now + 1.0 / RT["command_hz"]
            f = self.recv_frame(s, 0.05)
            if f is not None and "ctl" in f:
                frames.append(f)
        self.check(len(frames) >= 3,
                   f"telemetry: {len(frames)} frames in 1.5 s of streaming, want >= 3")
        if frames:
            f = frames[-1]
            names = [t["name"] for t in TELEMETRY_FIELDS]
            self.check(sorted(f) == sorted(names),
                       f"telemetry keys {sorted(f)}, want {sorted(names)}")
            for t in TELEMETRY_FIELDS:
                want = JSON_TYPES[t["type"]]
                got = f.get(t["name"])
                self.check(isinstance(got, want)
                           and not (want is int and isinstance(got, bool)),
                           f"telemetry {t['name']} is {got!r}, want {t['type']}")
            self.check(f.get("ctl") in CTL_VALUES,
                       f"telemetry ctl {f.get('ctl')!r} not in {CTL_VALUES}")

        print("dropped datagrams")
        pad = b"x" * (RT["max_command"] + 1 - len(b'{"seq":0,"t":0,"y":0,"p":""}'))
        oversized = b'{"seq":0,"t":0,"y":0,"p":"' + pad + b'"}'
        for bad in DROPPED_FRAMES + [oversized, enc({RT["seq_field"]: 1,
                                                     RT["throttle_field"]: 0.9,
                                                     RT["yaw_field"]: 0.0})]:
            s.sendto(bad, self.addr)                        # the last is a stale seq
        self.drain(s)
        stray = self.recv_matching(s, lambda f: f.get(RT["hello_field"]) == "abcd1234", 0.5)
        self.check(stray is None, "a malformed hello (proto:1.5) must not be answered")
        seq += 1
        s.sendto(enc({RT["seq_field"]: seq, RT["throttle_field"]: 0.0,
                      RT["yaw_field"]: 0.0}), self.addr)
        alive = self.recv_matching(s, lambda f: "ctl" in f, 1.0)
        self.check(alive is not None,
                   "the session survives the dropped datagrams and still pushes")

        print("eviction")
        s3 = self.sock()
        sid3 = secrets.token_hex(4)
        self.handshake(s3, sid3)
        self.drain(s)
        displaced = self.recv_matching(s, lambda f: "ctl" in f, 0.7)
        self.check(displaced is None,
                   "after an eviction the displaced socket hears nothing")
        moved = self.recv_matching(s3, lambda f: "ctl" in f, 1.0)
        self.check(moved is not None, "telemetry follows the new owner")

        print("bye")
        s3.sendto(enc({RT["seq_field"]: 1, RT["throttle_field"]: 0,
                       RT["yaw_field"]: 0, RT["bye_field"]: 1}), self.addr)
        time.sleep(0.3)
        self.drain(s3)
        after = self.recv_matching(s3, lambda f: "ctl" in f, 1.0)
        self.check(after is None, "telemetry stops after a goodbye")
        s.close()
        s3.close()
        return self.failures


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("target", help="host:port, e.g. 127.0.0.1:4237 or 192.168.4.1:4210")
    p.add_argument("-v", "--verbose", action="store_true", help="log every frame")
    args = p.parse_args()
    host, _, port = args.target.partition(":")
    suite = RTConformance(host, int(port or RT["port"]), args.verbose)
    print(f"rt conformance against udp://{host}:{suite.addr[1]}")
    try:
        failures = suite.run()
    except Unreachable as e:
        print(f"unreachable: {e}")
        return 2
    if failures:
        print(f"\n{len(failures)} failure(s):")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\nrt conformance: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Run it against a live mock by hand**

```bash
tools/mock_car/.venv/bin/python tools/mock_car/mock_car.py --host 127.0.0.1 \
    --port 8139 --rt-port 4239 >/tmp/rtconf-mock.log 2>&1 &
MOCK=$!; sleep 1
python3 tools/conformance_rt.py 127.0.0.1:4239; echo "exit: $?"
kill $MOCK
```

Expected: every section prints with no `FAIL` lines, then `rt conformance: all checks passed`, `exit: 0`.

- [ ] **Step 3: Verify the unreachable path**

Run: `python3 tools/conformance_rt.py 127.0.0.1:1; echo "exit: $?"`
Expected: `unreachable: no hello reply from 127.0.0.1:1` and `exit: 2` (after ~3 s of retries).

- [ ] **Step 4: Wire into test-all.sh** — in the conformance block of `tools/test-all.sh`, after the `python3 tools/conformance.py "http://127.0.0.1:$PORT"` line, add:

```bash
    python3 tools/conformance_rt.py "127.0.0.1:$RT_PORT"
```

(The block already starts the mock with `--rt-port "$RT_PORT"`, so the same instance serves both matrices.)

- [ ] **Step 5: Full suite and commit**

```bash
./tools/test-all.sh
git add tools/conformance_rt.py tools/test-all.sh
git commit -m "feat(net): UDP conformance — the RT wire finally has a cross-implementation check

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Execution order and cross-plan dependencies

Tasks 1–4 and 6–11 are independent of the other plans. Task 5 needs the firmware plan's
`session_idle_ms` contract task first. Task 12 touches `gen_contract.py`, which the
firmware plan also touches (different functions — coordinate by running whichever comes
second after a fresh `python3 tools/gen_contract.py`). Tasks 13–14 come last within this
plan (13 needs Tasks 7–8; 14 is the capstone); both run green against the mock on their
own, and against a real car only after the firmware plan's REST-unification and identity
tasks land.
