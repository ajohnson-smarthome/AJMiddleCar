# Audit Fixes — Documents Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring every seam document back to the truth of the shipped UDP link layer — protocol.md, CLAUDE.md, the two 2026-08-21 specs, the cutover plan — and record the audit's contract decisions where the next reader will look.

**Architecture:** Pure documentation changes plus one doc-string key in the contract schema (regenerated, never hand-edited). `docs/protocol.md` is rewritten around its generated endpoints block, which must survive byte-identical; everything else is targeted edits. Each task is independently verifiable with grep/diff and `tools/check_contract.sh`.

**Tech Stack:** Markdown, JSON (contract schema), `tools/gen_contract.py`, `tools/check_contract.sh`.

**Spec:** `docs/superpowers/specs/2026-08-22-audit-fix-decisions.md` (sections "Wire & session semantics" rules 1–4, 8–12, and "Documents")

## Global Constraints

- **This plan executes LAST**, after `2026-08-22-audit-fixes-firmware.md`, `-mock-and-net.md` and `-app.md` have landed: it documents behavior those plans create (seq gate surviving a trip, sticky-bye rule, dead-sid memory, 10 s session mortality, JSON envelopes, `GET /` identity). Do not start it while any of those plans has unfinished tasks.
- Work only in the worktree `/Users/adamjohnson/VSCode/esp32-p4-car/.claude/worktrees/audit-fixes` (branch `audit-fixes`).
- The generated endpoints block in `docs/protocol.md` (`<!-- generated:endpoints -->` … `<!-- /generated:endpoints -->`) must stay **byte-identical**: `tools/check_contract.sh` diffs exactly that region against a fresh generation and fails the tree on any drift. It must print `contract: no drift` after every task.
- No code changes in this plan. The single generated-file change (Task 5's `tools/mock_car/generated.py`) comes out of `tools/gen_contract.py`, never an editor.
- Preserve the repo's documentation voice: en dashes, bold key terms, second-person-free prose, hard-wrapped ~98-column lines.
- Every commit message ends with the trailer line `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- After the final task, `tools/test-all.sh` must exit 0.

---

### Task 1: Rewrite `docs/protocol.md` to the UDP wire

**Files:**
- Modify: `docs/protocol.md` (full rewrite around the generated block)

**Interfaces:**
- Consumes: shipped behavior from the firmware/mock/app plans; wire constants from `contract/car-api.json`; the generated endpoints block currently in the file.
- Produces: the seam document later tasks and future sessions cite. No code interface.

- [ ] **Step 1: Capture the generated block baseline**

Run:
```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car/.claude/worktrees/audit-fixes
sed -n '/generated:endpoints/,/\/generated:endpoints/p' docs/protocol.md > /tmp/endpoints-before.md
wc -l /tmp/endpoints-before.md
```
Expected: 9 lines (marker, header, separator, five domain rows, closing marker).

- [ ] **Step 2: Replace the file**

Overwrite `docs/protocol.md` with exactly the content below. The generated block is included verbatim — paste it as printed, do not retype it.

````markdown
# Wire protocol — app ↔ car

The contract between `app/` and `firmware/p4/`. These two never reference each other in code;
this document and `tools/mock_car` are the whole seam. Either side should be reimplementable
from this file alone.

Everything is JSON. The car is a WPA2 softAP; the app talks to the gateway address directly and
never reads the SSID (it has no such entitlement).

| | |
|---|---|
| Network | SSID `AJMiddleCar`, WPA2, password `drive1234` |
| Address | `192.168.4.1` (simulator builds talk to the mock at `127.0.0.1` — same UDP port, REST on `:8080`) |
| Channels | control and telemetry on UDP `4210`; REST on `:80` for configuration and OTA |

The numbers both sides must agree on — the port, the two datagram caps, the rates, the watchdog
deadline, the session-idle limit, the protocol version and every wire key — live in
`contract/car-api.json` and are generated into all four expressions of this contract. No
implementation writes them as literals, and neither does this file except by example.

## The real-time channel — UDP `4210`

Every datagram is a single JSON object. Two size limits answer different questions: the car
accepts an app→car datagram of at most **96 bytes** (`max_command`) and drops anything larger;
both sides read into **320-byte** buffers (`max_datagram`), because a telemetry frame runs
119–156 bytes and a receive buffer sized from the command cap would truncate every one.

Datagrams are parsed strictly, and identically on both implementations: keys are read at the
top level only, numbers follow JSON grammar (no leading `+`, no bare `.` mantissa, no leading
zeros), and a datagram that spells the same top-level key twice is dropped whole. The car scans
flat, the mock runs `json.loads` — anything looser than JSON would drive one and not the other.

### Session open — `hello`, app → car, repeated ~5 Hz until answered

```json
{"proto":1,"hello":"7f3a91c2"}
```

`hello` carries the session id: the app sends 8 hex characters; an acceptor takes 1–15
alphanumerics. `proto` must be an integer: a hello speaking a protocol the car does not is
answered by name (so the mismatch is visible, and the forced-update gate can act on it) but
**not** adopted; a malformed one — `1.5`, a string — is dropped without a reply.

**Every hello is answered**, repeats included — the sender repeats the handshake until it hears
back, so a lost reply must be answerable by the next repeat:

```json
{"proto":1,"hello":"7f3a91c2","device":"ajmiddlecar","fw":"v1.0+517"}
```

Identity arrives on the first exchange, over the channel that then carries telemetry: this
reply, not `/status`, is the app's "is this our car" test. `device` is load-bearing. Both cars
in this family serve this same API at this same address, so a client **must** compare it
against the one car it drives and refuse anything else. Treating a mismatch as "offline" is
wrong: the user has to change networks, not wait.

### Ownership

The car serves one client: the sender of the most recently adopted `hello` owns the session,
and datagrams from every other address are dropped. Last hello wins, from any address — a
second pult that knows the password takes the car silently, and the displaced client is not
notified. That is a recorded deferral, not an oversight (see the cutover plan's post-audit
amendments): the displaced app notices its telemetry going stale within ~3 s and re-hellos, so
two live pults fight in slow motion rather than co-drive.

Three hellos do **not** move ownership: a repeat from the current owner with the same sid
(answered, not re-adopted — a retransmitted handshake must not reset a live session); a hello
whose proto the car does not speak; and — while a session is live — a hello carrying the sid
of a recently ended one, because the car remembers the last few sids that ended by `bye`, by
eviction or by idling out, so a network-delayed duplicate of an old handshake cannot evict a
live driver. When no session is live, any well-formed hello adopts: refusing a dead sid there
would wedge a client whose session idled out mid-handshake, and a stale duplicate displaces
nobody — the phantom session just idles out again.

**On adopting a session** the car releases the previous session's stop-grant, clears the
breadcrumb history (a new session has no path to retrace), resets the sequence gate, and leaves
the control watchdog **disarmed** — it arms on the first accepted command, because that is the
thing it measures.

### Command — app → car, 10 Hz

```json
{"seq":1234,"t":0.50,"y":-0.25}
```

`t` is throttle, `y` is yaw, both floats in `[-1, 1]`, formatted with two decimals and a
period, never a comma; the firmware clamps. `seq` is a monotonic `uint32`; the car drops any
datagram whose `seq` is not newer than the last accepted one, compared as
`(int32_t)(seq - last) > 0` so wraparound is correct. **Every app→car datagram except `hello`
carries `seq`** — one without it is dropped, including a goodbye, because a frame without `seq`
is a frame that bypasses replay protection.

**The client streams the held command continuously at 10 Hz — it does not send events.** Two
reasons, both mandatory:

1. A single frame is a ~40 ms pulse. A motor does not visibly move.
2. The stream *is* the liveness signal. Silence for **300 ms** trips the watchdog.

On the watchdog trip the car does not simply stop: it replays its recent command history in
reverse, negated, to retrace its way back into radio range, aborting the moment a fresh frame
arrives. A client that pauses its stream mid-drive will therefore see the car reverse. Send
`{"seq":…,"t":0,"y":0}` to stop; stop streaming only when disconnecting deliberately.

A trip does **not** clear the sequence gate: a network-delayed duplicate from before the
dropout is still stale and still dropped. A stream that resumes after a dropout resumes with
newer `seq`s and passes the gate. And sessions are mortal: strictly more than **10 s**
(`session_idle_ms`) after its last activity — the last accepted command, or the adoption
itself when none ever followed — a session whose watchdog is not armed is over: ownership
clears, the telemetry push stops, the sid joins the dead-sid list, and resuming takes a fresh
`hello`. Armed silence is the watchdog's world; this clock runs only while the watchdog is
disarmed — after a trip, or after a handshake that never commanded.

### Goodbye — app → car

```json
{"seq":1235,"t":0,"y":0,"bye":1}
```

Stop, suppress the retreat, drop ownership. Sent when the scene leaves `.active`, when the
drive screen is dismissed, and on teardown. On a `bye` the car stops, clears the breadcrumb
history (which is what actually suppresses the retreat — replaying an empty history moves
nothing), disarms the watchdog, and releases its stop-grant immediately, so OTA, the
calibration wizard and the console stay reachable while the app is away. The one exception:
when a flash or a calibration pulse holds the actuator, the goodbye leaves that hold untouched —
a backgrounded app must not hand the motors back mid-flash. **Ownership is not resumable:**
after `bye` the app opens a new session with a fresh `hello` and a fresh sid.

### Telemetry — car → app, 5 Hz

Pushed to the owner's address on the same socket, unsolicited:

```json
{"seq":88,"rx_fps":10,"rssi":-58,"wdt_trips":0,"uptime_s":812,"heap":200000,
 "calibrated":true,"bus_ok":true,"ctl":"rt"}
```

`seq` is the push counter, so a client can drop a reordered datagram. `rx_fps` is control
frames received per second, a direct measure of the uplink. `rssi` is the AP-side signal for
the connected station, `0` when unavailable — clients should fall back to their own latency
measure. `wdt_trips` counts watchdog trips since boot; a rising count means the link is
dropping.

`ctl` names the source that currently owns the actuator — `rt`, `console`, `calib`, `recover`,
`ota`, `safe`, or `none`. It is how a client tells "the car is ignoring me because something
outranks me" from "the car is not hearing me". A car retreating under its own command reports
`recover`, which is the only way to show that honestly.

`bus_ok` is false once a write to the motor driver has failed and has not since succeeded. A
car with `bus_ok: false` is reachable, updatable and undriveable — a state worth distinguishing
from being offline, and the one a car boots into when its I2C bus is unplugged.

A failed push does **not** stop the pushing: a full send buffer is a moment, not a
disconnection. The push stops when the session ends — on `bye`, on eviction, or when the
session idles out.

## `GET /status` — the REST identity line

Still served — for humans, scripts, and the radio report; the app's identity test is the hello
reply, and liveness afterwards comes from telemetry freshness, not from polling this.

```json
{"device":"ajmiddlecar","fw":"v1.0+517","proto":1,
 "seq":88,"rx_fps":10,"rssi":-58,"wdt_trips":0,"uptime_s":812,"heap":200000,
 "calibrated":true,"bus_ok":true,"ctl":"rt",
 "radio":{"fw":"3.0.6","expected":"3.0.6","ok":true}}
```

The identity keys and the telemetry block are spelled from the same schema as the wire's, so a
rename cannot present as a different car. One divergence to know: `/status`'s `rx_fps` is a
per-consumer delta — `0` on the first poll after boot and after a gap of 10 s or more — where
the push's is continuous.

`radio` reports the ESP32-C6 co-processor that provides WiFi. Its image is pinned in `board.h`
and delivered out of band — over SDIO from the host, or over its UART header
(`firmware/c6/README.md`) — never through `/ota`. `ok:false` means the image on the radio is
not the one this firmware expects. Nothing else in the system reports this, so a client should
surface it.

## Configuration — REST

All bodies and responses are JSON. Every value is validated on the car: a malformed body, a
wrong-typed or fractional number, or a value outside its range gets `400` — every domain
rejects, none clamp, and an unrecognised `quad` is refused, not defaulted. Every accepted POST
persists to NVS immediately, and a POST of unchanged values does not rewrite flash.

<!-- generated:endpoints -->
| Endpoint | GET returns | POST body | Ranges |
|---|---|---|---|
| `/ramp` | `{"ramp_ms":…}` | same | `ramp_ms` 0..2000 |
| `/trim` | `{"trim_pct":…}` | same | `trim_pct` -30..30 |
| `/recover` | `{"enabled":…, "window_ms":…}` | same | `enabled` true \| false<br>`window_ms` 1000..10000 |
| `/wheel` | `{"diameter_mm":…, "ppr":…, "gear_x100":…, "quad":…}` | same | `diameter_mm` 20..150<br>`ppr` 1..1000<br>`gear_x100` 100..30000<br>`quad` 1 \| 2 \| 4 |
| `/dims` | `{"track_mm":…, "wheelbase_mm":…}` | same | `track_mm` 60..300<br>`wheelbase_mm` 90..360 |
<!-- /generated:endpoints -->

Calibration is not a config domain and is not generated — each of its endpoints has its
own body shape:

| Endpoint | GET returns | POST body | Range |
|---|---|---|---|
| `/calib` | `{"calibrated":true}` | — | — |
| `/calib/spin` | — | `{"pair":0,"dir":1}` | pair `0..3`, dir `0` reverse / `1` forward; pulses ~0.6 s, and the `200` lands only after the pulse ends — the wizard's "which wheel turned?" must not race a spinning wheel. `409` when a higher-priority source holds the actuator — the wheel did **not** turn, and a client must not advance its wizard |
| `/calib/save` | — | `{"wheels":[{"pair":0,"sign":1},…]}` | exactly 4 entries, order FL, FR, RL, RR; `pair` `0..3` unique, `sign` ±1 |

A successful POST — any POST, `/calib/*` and `/ota` included — answers `{"ok":true}`. A
rejected one answers `4xx` with `{"error":"…","field":"…"}`, where `field` names the offending
key — or is empty when the fault is with the body as a whole. Both carry
`Content-Type: application/json`. The body may arrive in any number of TCP segments; the car
reads until `Content-Length` is satisfied.

`GET /` returns the one-line plain-text identity `<device> <fw>`. There is no web UI.

### What the values mean

- **ramp** — slew-rate limit on acceleration, in ms to full scale. Rise is bounded, fall is
  instant, so stopping is never delayed.
- **trim** — straightness correction. Slows the faster side by this percentage.
- **recover** — the reverse-replay retreat described above; `window_ms` is how far back the
  breadcrumb history reaches.
- **wheel / dims** — geometry, used by the app to draw trajectories and to compute manoeuvres
  such as the donut's diameter. The car stores them; it does not yet compute speed from them.
- **calibration** — which channel pair drives which corner and in which direction. Without it
  the car does not know which wheel is which; `/status`'s `calibrated` is false until saved.

## Firmware update — `POST /ota`

Body is the raw application image (`ajmiddlecar.bin`), sent as a single request. The car stops
the motors, holds the actuator for the whole flash, writes the inactive OTA slot, and reboots
into it; the reply is `{"ok":true}` before the reboot. Images under 4 KB are rejected, and so
are bytes that are not an ESP application image — the magic is checked on the first write, the
whole image at the end. A stalled upload is abandoned after roughly 30 seconds of silence.
While the flash runs the car's REST is effectively down — its one server task is busy
writing — so a client should expect concurrent requests to stall rather than fail fast. On the
next boot the firmware marks the image valid, which cancels the bootloader's rollback — so an
image that cannot boot far enough to do that is rolled back automatically.

The radio co-processor's image is **not** delivered this way — see `/status` above and
`firmware/c6/README.md`.

## Not part of this protocol

The USB console (`mix <t> <y>`) is a local debug REPL on the serial port, not a socket. It is
plain text, and commands sent through it are deliberately exempt from the watchdog so that a
bench session does not stop every 300 ms.
````

- [ ] **Step 3: Verify the generated block survived and the old wire is gone**

Run:
```bash
sed -n '/generated:endpoints/,/\/generated:endpoints/p' docs/protocol.md | diff /tmp/endpoints-before.md -
bash tools/check_contract.sh
grep -cE 'ws_fps|ws_control|WebSocket|`/ws`|2\.11\.7|clamp silently|falls back to the default 4' docs/protocol.md
grep -c 'session_idle_ms' docs/protocol.md
```
Expected: empty diff; `contract: no drift`; `0` banished-term matches (grep exits 1); `1` or more `session_idle_ms` matches.

- [ ] **Step 4: Commit**

```bash
git add docs/protocol.md
git commit -m "docs(protocol): the wire is UDP — rewrite the seam document to match the car

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: CLAUDE.md — module list and gotchas back to the shipped tree

**Files:**
- Modify: `CLAUDE.md:79-95` (module list) and `CLAUDE.md:146-147` (gotcha 1)

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: the orientation document future sessions load first.

- [ ] **Step 1: Fix the module list**

In the "Firmware architecture" list, replace these three entries.

The `car.{c,h}` entry (currently "clamps, mixes, plans, and hands the duties to the ramp
task…"):

```markdown
- `car.{c,h}` — clamps, mixes, plans, and offers the duties to the actuator arbiter. Holds the
  mutex around the calibration read, with a bounded 200 ms wait so a stuck holder cannot wedge
  the watchdog.
```

The `ramp.{c,h}` and `watchdog.{c,h}` entries (currently "50 Hz task, the **sole writer** to
the PCA9685…" and "50 Hz check; 300 ms without a control frame calls
`recovery_on_link_lost()`."):

```markdown
- `ramp.{c,h}` — *pure* slew step plus the `/ramp` config; the 50 Hz actuator task lives in
  `link.c`. Bounded rise, instant fall.
- `link.{c,h}` — the actuator arbiter (who may command the motors: `rt`, `console`, `calib`,
  `recover`, `ota`, `safe`) and the 50 Hz task that is the **sole writer** to the PCA9685.
- `rt_link.{c,h}` — the UDP real-time channel: session ownership, the sequence gate, the
  control watchdog (300 ms without a command calls `recovery_on_link_lost()`), and the 5 Hz
  telemetry push. `watchdog.h` keeps only the pure staleness predicate.
```

The catch-all line (currently "`pca9685`, `wifi_ap`, `http_server`, `ws_control`, `telemetry`,
… and the seven `*_api` modules"):

```markdown
- `pca9685`, `wifi_ap`, `http_server`, `telemetry`, `calibration`, `wheel`, `dims`,
  `trim`, `cfg_json` and the four `*_api` modules — driver, transport, config, persistence.
```

- [ ] **Step 2: Fix gotcha 1**

Replace:

```markdown
1. **`mix` on the console is exempt from the watchdog** — bench debugging does not stop every
   300 ms. Only `/ws` traffic feeds it.
```

with:

```markdown
1. **`mix` on the console is exempt from the watchdog** — bench debugging does not stop every
   300 ms. Only UDP command datagrams on `rt_link` feed it.
```

- [ ] **Step 3: Verify**

Run:
```bash
grep -cE 'ws_control|`/ws`|seven \*_api|seven `\*_api`' CLAUDE.md
grep -c 'rt_link' CLAUDE.md
```
Expected: `0` (grep exits 1) for the first; `1` or more for the second.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md
git commit -m "docs(claude): the module map learns the cutover happened

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Stamp the two 2026-08-21 specs where they lie

**Files:**
- Modify: `docs/superpowers/specs/2026-08-21-link-layer-rearchitecture.md:205-209` (the enum sketch)
- Modify: `docs/superpowers/specs/2026-08-21-wifi-pinned-networking.md:1` (header note)

**Interfaces:** none — historical records gaining correction notes; their text otherwise stays.

- [ ] **Step 1: Correct the rearchitecture spec's enum sketch**

Immediately after the code fence containing `SRC_CONSOLE = 0, SRC_RT, SRC_CALIB, SRC_RECOVER,
SRC_OTA, SRC_SAFE`, insert:

```markdown
> **Superseded (2026-08-22):** the sketch above is not the shipped order. `link.h` ships
> `LINK_SRC_NONE = -1, LINK_SRC_RECOVER = 0, LINK_SRC_CONSOLE, LINK_SRC_RT, LINK_SRC_CALIB,
> LINK_SRC_OTA, LINK_SRC_SAFE` — recover ranks *below* a returning driver and below the
> console, and `link.h`'s comments argue each neighbouring pair. The schema's `ctl_values`
> and the mock agree with `link.h`, not with this sketch.
```

- [ ] **Step 2: Head the wifi-pinned spec with its supersession**

Insert directly under the `# Pinning the car's traffic to Wi-Fi` title line:

```markdown
> **Superseded (2026-08-22):** the components this spec designs — `CarHTTP`, `CarSocket` over
> `NWProtocolWebSocket`, a `CarConnection` interface — were replaced by the UDP cutover;
> the shipped owners are `CarTransport` (one actor for the UDP session and the pinned HTTP
> requests) and `CarNet` (the one place the pinning rule lives). The load-bearing rule —
> `requiredInterfaceType = .wifi`, applied only off-simulator — survives unchanged. The
> 40-second general-path bench check below has **not** been re-run against the UDP transport;
> `docs/bringup.md` owns that box until it has.
```

- [ ] **Step 3: Verify**

Run:
```bash
grep -c 'Superseded (2026-08-22)' docs/superpowers/specs/2026-08-21-link-layer-rearchitecture.md docs/superpowers/specs/2026-08-21-wifi-pinned-networking.md
```
Expected: `1` in each file.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-08-21-link-layer-rearchitecture.md docs/superpowers/specs/2026-08-21-wifi-pinned-networking.md
git commit -m "docs(specs): stamp the pre-cutover sketches that contradict the shipped code

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Cutover plan post-audit amendments, and the eviction-notice idea

**Files:**
- Modify: `docs/superpowers/plans/2026-08-21-link-layer-cutover.md:30` (sid grammar), `:90` (hello-reply wording), end of file (amendments section)
- Modify: `docs/IDEAS.md` (one new entry)

**Interfaces:**
- Consumes: rules 1–4, 10, 11 from the decisions spec.
- Produces: the contract document the firmware/mock implementers read.

- [ ] **Step 1: Fix the sid sentence (line 30)**

Replace:

```markdown
`hello` is a per-session id, 8 hex characters. The car adopts this sender as its owner and drops
```

with:

```markdown
`hello` is a per-session id: the app sends 8 hex characters, and an acceptor takes 1–15
alphanumerics. The car adopts this sender as its owner and drops
```

- [ ] **Step 2: Fix the hello-reply wording (line 90)**

Replace `**Hello reply**, once per adopted session:` with:

```markdown
**Hello reply**, sent in answer to every hello — repeats included, adopted or not:
```

- [ ] **Step 3: Append the amendments section**

At the end of the file:

```markdown

---

## Post-audit amendments (2026-08-22)

The 2026-08-22 audit of this wire confirmed 51 defects; four changed the session rules above,
and one deferral is recorded so it reads as a decision rather than a gap. The decisions spec is
`docs/superpowers/specs/2026-08-22-audit-fix-decisions.md`.

1. **The sequence gate survives a trip.** "Reset the sequence gate" happens on adopt and on
   `bye` only. Clearing it on a trip let one network-delayed pre-dropout duplicate re-arm the
   watchdog, drive the car at stale stick values, and abort a retreat in progress.
2. **A goodbye during a sticky hold (OTA, calibration) neither steals nor releases that
   hold.** The `bye` still clears the breadcrumbs, disarms the watchdog and ends the session —
   but `car_stop(LINK_SRC_SAFE)` over a sticky owner handed the motors back mid-flash.
3. **Dead sids are remembered.** While a session is live, a hello carrying the sid of a
   recently ended one is answered but not re-adopted, so a duplicated old handshake cannot
   evict a live driver. With no live session, any hello adopts — refusing there would wedge a
   client whose session idled out mid-handshake, and a stale duplicate displaces nobody.
4. **Sessions are mortal.** Strictly more than `session_idle_ms` (schema, 10000) after its
   last activity — the last accepted command, or the adoption when none ever followed — a
   session whose watchdog is not armed ends: ownership clears, telemetry stops, the sid joins
   the dead list. Before this, a car whose driver vanished pushed telemetry to the dead
   address forever and held the stale-frame window open with it.
5. **Deferred: an eviction notice to a displaced owner.** Last hello still wins silently;
   two live pults oscillate on their ~3 s stall guards (pinned by a mock test). The one-datagram
   notice is in `docs/IDEAS.md`; it changes the wire, so it waits for a protocol bump.
```

- [ ] **Step 4: Add the IDEAS.md entry**

Append to `docs/IDEAS.md`, matching the file's Russian bullet style:

```markdown
- **Датаграмма-уведомление вытесненному пульту** (2026-08-22). Сейчас last-hello-wins молчалив:
  второй телефон забирает машинку, а первый узнаёт об этом только по остывшей телеметрии (~3 с)
  и пере-hello — два живых пульта осциллируют владением. Одна датаграмма
  `{"evicted":"<sid>"}` на адрес вытесненного дала бы приложению честный экран «вас вытеснили»
  вместо «поиска». Меняет провод (новый тип кадра во все три реализации + контракт) — ждёт
  повышения `proto`. Осцилляция запинена тестом в mock; решение об отсрочке записано в
  post-audit amendments плана катовера.
```

- [ ] **Step 5: Verify**

Run:
```bash
grep -c 'Post-audit amendments' docs/superpowers/plans/2026-08-21-link-layer-cutover.md
grep -c 'once per adopted session' docs/superpowers/plans/2026-08-21-link-layer-cutover.md
grep -c 'Датаграмма-уведомление' docs/IDEAS.md
```
Expected: `1`; `0` (grep exits 1); `1`.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/plans/2026-08-21-link-layer-cutover.md docs/IDEAS.md
git commit -m "docs(contract): what the audit changed in the session rules, and what it deferred

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: sid grammar in the schema, via the generator

**Files:**
- Modify: `contract/car-api.json` (rt section gains a `doc` key)
- Regenerate: `tools/mock_car/generated.py` (via `tools/gen_contract.py` — never by hand)

**Interfaces:**
- Consumes: the sid grammar wording from Task 4.
- Produces: the schema self-documents the one wire value whose grammar lives nowhere machine-adjacent. The C and Swift emitters read named keys only, so `cfg_table.inc` and `CarAPI.swift` must come out unchanged; `emit_python` reprs the whole rt dict, so `generated.py` changes by exactly one line.

- [ ] **Step 1: Add the doc key**

In `contract/car-api.json`, inside the `"rt"` object (after the `"yaw_field"` line, keeping the
2-space indent), add:

```json
    "doc": "hello carries the session id: producers send 8 hex characters; acceptors take 1-15 alphanumerics"
```

(with a comma added to the previous line — keep the file valid JSON).

- [ ] **Step 2: Regenerate and inspect**

Run:
```bash
python3 tools/gen_contract.py
git diff --stat
git diff tools/mock_car/generated.py | head -20
```
Expected: exactly two modified files — `contract/car-api.json` and `tools/mock_car/generated.py`
(the RT dict line gains the `'doc': …` entry). `cfg_table.inc`, `CarAPI.swift` and
`docs/protocol.md` untouched.

- [ ] **Step 3: Verify the tree agrees with itself**

Run:
```bash
bash tools/check_contract.sh
./tools/test-all.sh
```
Expected: `contract: no drift`; test-all ends with `== all green ==`.

- [ ] **Step 4: Commit**

```bash
git add contract/car-api.json tools/mock_car/generated.py
git commit -m "feat(contract): the sid grammar joins the schema it always belonged to

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```
