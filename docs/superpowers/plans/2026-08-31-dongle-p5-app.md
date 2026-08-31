# Dongle, App Side — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The app reaches the car through the dongle instead of joining the car's Wi-Fi, and drives
the dongle's own lifecycle — find it, update it, tell it which network to join, then talk to the car.

**Architecture:** One new client for the dongle's API, one seam that decides which interface the
car's traffic is pinned to, and the flow phases in front of the existing gate. Everything that
speaks the car's contract is untouched: `ConfigStore`, `RTFrame`, `CarTransport`'s framing, every
settings screen, the trick editor and the calibration wizard all address the car through
`CarHost` and do not move.

**Tech Stack:** SwiftUI, `Network.framework`, XcodeGen; pure Swift host tests run by
`tools/test-all.sh` with `swiftc` directly — no XCTest runtime.

**Spec:** `docs/superpowers/specs/2026-08-30-dongle-api-design.md`

## Two unknowns this plan is built around

Both are answered by one bench session and neither blocks the work — but each is a **seam**, not a
guess to be baked in. A task that hardcodes either is wrong even if it happens to be right.

**U1 — which interface type iOS presents for a CDC-NCM device.** The spec assumes
`.wiredEthernet`. Nobody has looked. `CarNet` pins every socket and `CarPath` monitors on this
value, so if it is `.other` instead, every connection in the app silently fails to open. Task 3
puts it in exactly one named place with both candidates written down, and Task 3's bench step is
the one line that settles it.

**U2 — whether `POST /net` can restart a failed join.** The firmware was fixed for this on branch
P4 (`net_api.c` now calls `wifi_sta_join` when the station is not connected), but that fix has
never run. If the bench shows otherwise, the app's retry lever changes shape and so does the
contract. Task 2 therefore treats "ask the dongle to try again" as its own function with one
implementation, so a change lands in one place.

## Global Constraints

- **The app is the only place the two contracts meet.** It reads `CarContract` for the car's
  identity and hands the SSID and password to the dongle as opaque strings. Nothing in
  `firmware/s3` learns whose they are, and nothing in the app teaches it.
- **Generated files are never hand-edited.** `app/AJMiddleCar/Generated/DongleAPI.swift` comes
  from `contract/dongle-api.json`. Neither side writes an agreed name, number or path as a
  literal — that includes the app.
- **The simulator does not change.** `CarHost` already branches on `targetEnvironment(simulator)`
  to reach `tools/mock_car` at `127.0.0.1`. There is no USB in the simulator and no dongle to
  mock: simulator builds keep talking to the mock directly. Only real-device builds go through
  the dongle. Do not build a dongle stub "for symmetry" — the spec rules it out explicitly.
- **Pure rules are host-tested.** Anything decidable without a socket goes in a module under
  `app/AJMiddleCar/` with a test in `app/tests/<name>/main.swift` and a `sources` file, exactly
  as `UpdateRules`, `GateRule` and `ConfigState` already are. `tools/test-all.sh` runs them with
  `swiftc`; there is no XCTest here.
- **`tools/test-all.sh` must be green before every commit**, and `cd app && xcodegen generate`
  then an `xcodebuild build` for the simulator must succeed — the simulator build is the only
  compile check this repo has for the parts that touch `Network.framework`.
- **No real credentials in any test fixture.** The car's SSID and password come from
  `CarContract` at runtime; tests use neutral strings.

---

## File Structure

| File | Responsibility |
|---|---|
| `app/AJMiddleCar/DongleStatus.swift` | *create* — **pure**: decode `/status` and `/net`, and the rules over them |
| `app/AJMiddleCar/DongleClient.swift` | *create* — the transport: `GET /status`, `GET`/`POST /net`, `POST /ota` |
| `app/AJMiddleCar/DongleLink.swift` | *create* — **pure**: what the app should do next, given a status |
| `app/AJMiddleCar/CarNet.swift` | *modify* — the interface seam (U1) |
| `app/AJMiddleCar/CarHost.swift` | *modify* — device builds address the dongle |
| `app/AJMiddleCar/CarPath.swift`, `LinkState.swift` | *modify* — the dongle's presence replaces "is there Wi-Fi" |
| `app/AJMiddleCar/UpdateClient.swift` | *modify* — a second asset and a second cache path |
| `app/AJMiddleCar/AppFlow.swift` | *modify* — phases for finding and updating the dongle |
| `app/AJMiddleCar/ConnectView.swift` | *modify* — "plug in the dongle" replaces "join network X" |
| `app/tests/donglestatus/`, `app/tests/donglelink/` | *create* — host tests for the two pure modules |
| `tools/test-all.sh` | *modify* — compile `DongleAPI.swift` alongside `CarAPI.swift` |

---

### Task 1: The generated contract reaches the app, and `/status` decodes

`app/AJMiddleCar/Generated/DongleAPI.swift` has been committed since the contract branch and is
**referenced by nothing** — and `tools/test-all.sh`'s Swift loop compiles only `CarAPI.swift`, so
nothing has ever compiled it. A generated artifact no build touches is a file that can rot
silently. This task gives it a consumer and a compile.

**Files:**
- Create: `app/AJMiddleCar/DongleStatus.swift`, `app/tests/donglestatus/main.swift`, `app/tests/donglestatus/sources`
- Modify: `tools/test-all.sh`

**Interfaces:**
- Produces: `DongleStatus` (decoded `/status`), `DongleNet` (decoded `GET /net`), and
  `DongleStatus.parse(_:)` / `DongleNet.parse(_:)` taking `Data`. Tasks 2, 3 and 5 consume them.

- [ ] **Step 1: Compile the generated file in the host tests**

In `tools/test-all.sh`, the Swift loop compiles `app/AJMiddleCar/Generated/CarAPI.swift` into every
test binary. Add `app/AJMiddleCar/Generated/DongleAPI.swift` beside it, so both generated files are
compiled by every Swift host test whether or not that test uses them.

Both are `public enum`s of constants with no dependencies, so adding the second cannot break the
existing tests — confirm that by running the suite before writing anything else, and record it as
your baseline.

- [ ] **Step 2: Write the failing tests**

Create `app/tests/donglestatus/main.swift`. Follow the house style — read `app/tests/update/main.swift`
first: a `check(_:_:)` helper, named cases, a failure counter, `exit(1)` on any failure. Cover:

- a complete `/status` body decodes every field, and `device`, `fw`, `usb`, `rollback` and the
  nested `net` object each come out right
- `net.state` decodes to each of the four contract states, and an unknown state string is
  surfaced rather than silently mapped to one of them
- a `/status` body missing a field fails to parse rather than defaulting — a dongle that answers
  a truncated document is a dongle the app must not believe
- `rollback:true` is readable, because it is the one signal that distinguishes "never updated"
  from "the update was reverted"
- `GET /net` decodes `ssid` and `configured`, and **a body containing a `password` key is still
  accepted but the password is not stored anywhere on the Swift side** — the dongle never sends
  one, and the app must not grow a field to hold one if a future firmware slips
- an SSID with an escaped quote and one with a backslash both round-trip, since `net_cfg`
  deliberately allows both

Create `app/tests/donglestatus/sources` containing `DongleStatus.swift`.

- [ ] **Step 3: Run them and watch them fail**

Run: `tools/test-all.sh`
Expected: the new test binary fails to compile — `DongleStatus.swift` does not exist.

- [ ] **Step 4: Write the module**

Create `app/AJMiddleCar/DongleStatus.swift`. `Codable` structs decoded with `JSONDecoder`, with
`CodingKeys` **taken from `DongleStatusKey` and `DongleContract`**, never spelled as literals —
that is the whole reason the generated file exists. Required fields are non-optional so a missing
one throws.

`net.state` decodes into an enum with the four contract cases plus an `unknown(String)`, so a
firmware that grows a fifth state does not make the app fail to parse a document it otherwise
understands.

- [ ] **Step 5: Green, then commit**

```bash
tools/test-all.sh
git add app/AJMiddleCar/DongleStatus.swift app/tests/donglestatus tools/test-all.sh
git commit -m "feat(app): the dongle's contract gets a consumer and a compile"
```

---

### Task 2: `DongleClient` — talking to the dongle

The transport half. Modelled on `CalibClient.swift` — read it first; this is the same shape
against a different address.

**Files:**
- Create: `app/AJMiddleCar/DongleClient.swift`
- Modify: none

**Interfaces:**
- Consumes: `DongleStatus`, `DongleNet` (Task 1); `DongleContract` (generated).
- Produces:
  - `func status() async throws -> DongleStatus`
  - `func net() async throws -> DongleNet`
  - `func join(ssid: String, password: String) async throws`
  - `func retryJoin() async throws` — **see U2**
  - `func uploadFirmware(_ data: Data, progress: @escaping (Double) -> Void) async throws`

- [ ] **Step 1: The client**

Create `app/AJMiddleCar/DongleClient.swift`. Address the dongle at
`DongleContract.host` : `DongleContract.port`, paths from `DongleContract.statusPath` /
`.netPath` / `.otaPath`. Nothing here may spell an address, a port or a path as a literal.

**Which transport.** `CarTransport` uses `Network.framework` because the car's Wi-Fi has no
internet and iOS demotes it out of the general path — a problem the dongle does not have, since
the phone keeps its own Wi-Fi throughout. But the dongle sits on its own interface, and Task 3
establishes that the app pins the car's traffic to that interface. Use the same `CarNet` parameters
for the dongle, for one reason: whatever pins the car's traffic to the dongle's wire must pin the
dongle's own traffic too, or the two halves can disagree about which interface exists. Say so in a
comment.

**`retryJoin()` is its own function (U2).** Today it POSTs the stored credentials again, which is
what the firmware's fixed `net_api.c` acts on. It exists as a separate entry point rather than a
second call to `join` so that if the bench shows `POST /net` is not the retry lever, exactly one
function changes. Its comment must say that, and name U2.

**The upload is the car's shape.** The spec: "the POST path itself is unchanged — the dongle's
`/ota` is the car's shape." Reuse whatever `UpdateClient` already uses to push to the car rather
than writing a second uploader.

- [ ] **Step 2: Build and commit**

There is no host test for this task — it is all sockets, exactly as the firmware's relays were,
and inventing one would test a mock of `Network.framework`. The pure rules it acts on were tested
in Task 1 and are tested again in Task 5.

```bash
cd app && xcodegen generate && cd ..
xcodebuild build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle 2>&1 | tail -5
tools/test-all.sh
git add app/AJMiddleCar/DongleClient.swift
git commit -m "feat(app): a client for the dongle's own API"
```

---

### Task 3: The interface seam (U1)

The single most consequential line in this plan, and the one nobody can verify without hardware.

`CarNet.pinToWiFi` currently sets `requiredInterfaceType = .wifi` on every socket the app opens to
the car, and `CarPath` monitors `NWPathMonitor(requiredInterfaceType: .wifi)`. Both exist because
of a measured behaviour: joined to the car's softAP, iOS demotes Wi-Fi out of the general path
after about forty seconds while the interface keeps working.

**With the dongle, that reasoning inverts.** The phone keeps its own Wi-Fi — with internet — the
whole time, so the general path stays satisfied and the demotion problem disappears. What replaces
it is a different question: *is the dongle's interface present at all*. That is what `CarPath` must
answer now.

**Files:**
- Modify: `app/AJMiddleCar/CarNet.swift`, `app/AJMiddleCar/CarHost.swift`,
  `app/AJMiddleCar/CarPath.swift`, `app/AJMiddleCar/LinkState.swift`

**Interfaces:**
- Produces: `CarNet.dongleInterface` — the one named constant every pin and every monitor reads.

- [ ] **Step 1: Name the unknown once**

In `CarNet.swift`, add a single `static let dongleInterface: NWInterface.InterfaceType`. Its comment
carries U1 in full: the spec assumes `.wiredEthernet`; nobody has looked; the candidates are
`.wiredEthernet` and `.other`; the bench step that settles it is Task 3 Step 4; and if it is wrong
every connection in the app silently fails to open rather than failing loudly.

Then replace `.wifi` in `pinToWiFi` with it, and rename that function to say what it now does.
`prohibitedInterfaceTypes = [.cellular]` stays — it is unrelated to which interface carries the car
and still correct.

The simulator branch does not change and must keep its comment.

- [ ] **Step 2: Device builds address the dongle**

In `CarHost.swift`, the non-simulator branch: `host` becomes `DongleContract.host`; `port` becomes
`DongleContract.relayHttpPort`; `rtPort` becomes `DongleContract.relayRtPort`.

Those last two are numerically what they already were — the car keeps its native ports and the
relay listens on them, which is exactly why `CarContract` does not move. Taking them from
`DongleContract` rather than leaving the literals says *whose* decision they now are, and means the
cross-schema test added on branch P4 is what guards them.

Leave the simulator branch alone in full: it reaches the mock and there is no dongle there.

- [ ] **Step 3: `CarPath` monitors the dongle's interface**

Replace the Wi-Fi monitor with one on `CarNet.dongleInterface`, and rewrite the type's doc comment:
the two-monitor design stays, but its reason changes from "Wi-Fi with no internet gets demoted" to
"the dongle's interface appears and disappears with a cable, and the general path now belongs to
the phone's own Wi-Fi". The local-network-denial check on both monitors stays exactly as it is —
it is the one state waiting cannot fix, and it is unrelated to which interface carries the car.

In `LinkState.swift`, `PathState`'s `.wifiUp` / `.noWifi` no longer name the right thing. Rename to
say what they now mean — the dongle's wire is up, or it is not — and update every use. Keep
`localNetworkDenied` unchanged.

- [ ] **Step 4: The bench step that settles U1 — write it down, do not guess**

Add to `firmware/s3/README.md`'s bench table a pending row: *which `NWInterface.InterfaceType` the
dongle presents on a real device*. The measurement is one throwaway build that logs
`NWPath.availableInterfaces` while the dongle is attached, and it decides `CarNet.dongleInterface`.

Do **not** attempt to answer it from the simulator: there is no USB there, and a simulator answer
would be a wrong answer that looks like a right one.

- [ ] **Step 5: Build both, then commit**

```bash
cd app && xcodegen generate && cd ..
xcodebuild build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle 2>&1 | tail -5
tools/test-all.sh
git add app/AJMiddleCar/CarNet.swift app/AJMiddleCar/CarHost.swift app/AJMiddleCar/CarPath.swift \
        app/AJMiddleCar/LinkState.swift firmware/s3/README.md
git commit -m "feat(app): the car is reached over the dongle's wire, named in one place"
```

---

### Task 4: One release, two images

`tools/release.sh` has attached `ajdongle.bin` beside `ajmiddlecar.bin` since branch P3, and both
carry the same version. The producer side is done; the consumer side is this task.

**Files:**
- Modify: `app/AJMiddleCar/UpdateClient.swift`, `app/AJMiddleCar/UpdateRules.swift` (only if a rule
  needs to become device-aware), `app/tests/update/main.swift`

**Interfaces:**
- Consumes: `DongleClient.uploadFirmware` (Task 2).
- Produces: an asset name and a cache path per device, and `mustUpdate` answerable for either.

- [ ] **Step 1: Extend the host tests first**

`UpdateRules` is already host-tested in `app/tests/update/`. Add cases for the second image: the
cache path for the dongle differs from the car's, both are derived rather than spelled twice, and
a cached image for one device is never offered for the other. If `mustUpdate` needs to take a
device, add cases proving the car's gate is unchanged by the dongle's.

Run `tools/test-all.sh`; the new cases must fail.

- [ ] **Step 2: A second asset and a second cache path**

`UpdateClient.assetName` is `"ajmiddlecar.bin"` with a comment explaining that matching "first file
ending in .bin" silently picks the wrong image the day a release carries two. **That day has
arrived.** Make the asset name per device, keep the exact-name matching, and update the comment to
say the situation it warned about is now the situation.

The cache path likewise: `firmware-latest.bin` becomes one path per device. A stale car image must
never be uploadable to the dongle.

- [ ] **Step 3: Green, build, commit**

```bash
tools/test-all.sh
cd app && xcodegen generate && cd ..
xcodebuild build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle 2>&1 | tail -5
git add app/AJMiddleCar/UpdateClient.swift app/AJMiddleCar/UpdateRules.swift app/tests/update
git commit -m "feat(app): one release, two images, two caches"
```

---

### Task 5: The flow — what the app does next, and the screens that say it

The sequence the user described, and the spec records:

> open the app, check whether a dongle is there; if not, say so; if it is, check for a newer
> version and update it; then check whether it has been told which network to join, and tell it if
> not; then connect to the car, check its version, update if needed — and then drive.

**The decision belongs in a pure module.** Every branch above is a function of a `DongleStatus`, a
latest release tag, and the car's own state — no sockets. That makes it host-testable, which is the
only way this many branches gets verified without a device.

**Files:**
- Create: `app/AJMiddleCar/DongleLink.swift`, `app/tests/donglelink/main.swift`, `app/tests/donglelink/sources`
- Modify: `app/AJMiddleCar/AppFlow.swift`, `app/AJMiddleCar/ConnectView.swift`

**Interfaces:**
- Consumes: `DongleStatus` (Task 1), `UpdateRules` (Task 4).
- Produces: `DongleLink.next(status:latestTag:) -> DongleStep`, an enum the flow switches on.

- [ ] **Step 1: Write the failing tests**

Create `app/tests/donglelink/main.swift`, in the house style. Cover every branch, and these in
particular because they are the ones a hand-written flow gets wrong:

- no dongle answering at all → the step that says "plug it in", not an error
- dongle present, firmware behind the latest release → update the **dongle** before anything else
  the spec is explicit about the order: "The dongle updates before the car… Settle the pipe before
  pushing the long transfer down it"
- dongle present and current, `configured` false → send the car's credentials
- `configured` true but `net.state` is `idle` or `joining` → wait, do not re-POST
- `net.state` is `failed` → the retry step, **not** the configure step — the credentials are
  already stored and correct; what failed was the join (this is where U2 lands)
- `net.state` is `connected` → hand off to the car's existing gate, unchanged
- `rollback` true → the dongle reverted an update; the app must say so rather than offering the
  same update again forever, which is the loop that field exists to break
- an `unknown` net state → treated as not-ready rather than as connected

Create `app/tests/donglelink/sources` listing `DongleLink.swift` and `DongleStatus.swift`.

- [ ] **Step 2: Run them and watch them fail**

Run: `tools/test-all.sh`
Expected: the new binary fails to compile.

- [ ] **Step 3: The module**

Create `app/AJMiddleCar/DongleLink.swift`: a `DongleStep` enum and one pure function returning it.
No `async`, no networking, no `@MainActor` — if it needs any of those, the decision has leaked out
of the module and into the flow.

- [ ] **Step 4: The flow and the screen**

`AppFlow.Phase` gains the dongle's phases in front of the existing ones. The car's forced-update
gate is untouched — the spec: "One gate covers both… One screen, two updates in sequence." Extend
`opensLink` correctly: a dongle phase must not open a car session behind a screen that says there
is nothing to talk to, which is the rule that already governs the existing phases.

`ConnectView` currently instructs the user to join a Wi-Fi network by name and password. Replace
that with the dongle's states — absent, present but not configured, joining, cannot find the car.
The app is Russian-localised and landscape-locked, and every split screen draws its own header via
`SplitScreen`: do not add a `navigationTitle`.

- [ ] **Step 5: Green, build, commit**

```bash
tools/test-all.sh
cd app && xcodegen generate && cd ..
xcodebuild build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle 2>&1 | tail -5
git add app/AJMiddleCar/DongleLink.swift app/tests/donglelink app/AJMiddleCar/AppFlow.swift app/AJMiddleCar/ConnectView.swift
git commit -m "feat(app): find the dongle, update it, point it at the car, then drive"
```

---

## What this plan does not do

- **Build a dongle stub or a dongle mock.** The spec rules it out: the simulator keeps talking to
  `tools/mock_car` directly, and dongle-specific screens are exercised on hardware. Building a
  stub in advance would be building a second thing to keep true.
- **Take `signalLevel` from the dongle.** The spec calls it optional and the existing fallback
  keeps working. It is a one-line change once there is a device to see it on, and it is not worth
  a task before then.
- **Touch anything that speaks the car's contract.** `ConfigStore`, `RTFrame`, `CarTransport`'s
  framing, the settings screens, the trick editor and the calibration wizard are all unchanged.
  If a change here needs one of them to move, the change is wrong.
- **Answer U1 or U2.** Both are seams with a named place to change and a bench step that settles
  them.

## Bench verification, when hardware is available

This plan's pure modules — `DongleStatus`, `DongleLink`, the update rules — are host-tested and
that is real coverage of the branching. Everything that opens a socket is not, and cannot be here.

The dongle's own bench debt (`docs/superpowers/plans/2026-08-30-dongle-p4-radio-relay.md`) comes
first: an app pointed at a dongle that has never been proven to relay will produce failures that
belong to the firmware, and debugging them from the app is the expensive way round.

Then, in order:

1. **Answer U1** — the throwaway build that logs `NWPath.availableInterfaces` with the dongle
   attached. Set `CarNet.dongleInterface` to what it says. Every step below depends on this one.
2. **Answer U2** — with the car powered off, watch the app reach the `failed` step, then let it
   retry and confirm the dongle actually tries again.
3. **The whole sequence, cold** — a dongle with no stored network, an app that has never run:
   plug in, watch it find the dongle, offer the update if there is one, send the credentials, wait
   out the join, and reach the drive screen without a single manual step.
4. **Drive.** Commands and telemetry both flowing, for a full minute and then five. This is the
   same step P4's bench procedure names, and it can be run before any of this app work using the
   simulator's launch arguments — do that first, because it is the step most likely to change what
   this plan has to be.
5. **Unplug mid-drive.** The app must reach a screen that says the wire is gone, not a hang. This
   is the state `PathState`'s renamed cases exist to express, and nothing else tests them.
6. **Record every result** in `firmware/s3/README.md`'s bench table.
