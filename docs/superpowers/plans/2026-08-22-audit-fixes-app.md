# iOS App Audit Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the nine confirmed app-side audit findings — lifecycle races, a socket leak, a GitHub-only launch gate, lossy session events, inert retry, non-finite clamps — and give the session policy its first host tests.

**Architecture:** All changes stay inside the existing shapes: `CarTransport` (actor, owns sockets), `CarLink` (@MainActor, composes liveness), `AppFlow` (launch gate). New pure logic goes into small `enum` rule types (`GateRule`, `SessionPolicy`) host-tested via the existing `app/tests/<name>/main.swift` + `sources` mechanism — never into the actors. Session lifecycle events move to a lossless stream separate from latest-wins telemetry.

**Tech Stack:** Swift 6 / SwiftUI, Network.framework, swiftc host tests (no XCTest), XcodeGen.

**Spec:** `docs/superpowers/specs/2026-08-22-audit-fix-decisions.md` (section "App-local")

## Global Constraints

- Work in the worktree: `/Users/adamjohnson/VSCode/esp32-p4-car/.claude/worktrees/audit-fixes`. All commands below run from that repo root.
- Wire constants come ONLY from `app/AJMiddleCar/Generated/CarAPI.swift` (`CarContract.*`, `TelemetryKey.*`). Never hand-edit anything in `Generated/`.
- The XcodeGen target globs the whole `AJMiddleCar/` directory, so new `.swift` files need no `project.yml` change — but the final task regenerates and builds to prove it.
- Host tests: a suite is `app/tests/<name>/main.swift` (+ optional `sources` file listing app files one per line, relative to `app/AJMiddleCar/`); it must end with `if failures == 0 { print("test_<name>: OK") } else { exit(1) }`. `./tools/test-all.sh` compiles each with `swiftc` together with `Generated/CarAPI.swift` and runs it.
- The app is landscape-locked, Russian-localised (`L.swift`); these tasks add no user-facing copy.
- Every commit message ends with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`
- After each task run at minimum the swift host tests (they are seconds): `./tools/test-all.sh` runs everything and is acceptable too.

---

### Task 1: Non-finite input becomes 0, not full reverse

**Files:**
- Modify: `app/AJMiddleCar/RTFrame.swift:54`
- Modify: `app/AJMiddleCar/ControlModel.swift:13`
- Test: `app/tests/rtframe/main.swift`, `app/tests/intent/main.swift`

**Interfaces:**
- Consumes: nothing new.
- Produces: `RTFrame.clamp` / `ControlModel.clamp` map any non-finite `Double` to `0`. No signature changes.

Why: `Swift.max(-1, .nan)` returns `-1`, so a NaN anywhere upstream serialises as `"t":-1.00` — sustained full reverse on the wire (audit: RTFrame.swift:54). Worse, an unclamped NaN would `%.2f`-format as `nan`, which is not JSON.

- [ ] **Step 1: Write the failing tests**

Append to `app/tests/rtframe/main.swift`, just above the final `if failures == 0` line:

```swift
// A non-finite axis must serialise as a stop, not as full reverse: max(-1, .nan) is -1, and
// a NaN formatted raw would not even be JSON. The audit filed this as latent — no current
// input path produces NaN — and latent is exactly when to pin it.
check(RTFrame.command(seq: 1, t: .nan, y: .nan) == #"{"seq":1,"t":0.00,"y":0.00}"#,
      "NaN axes serialise as zero")
check(RTFrame.command(seq: 2, t: .infinity, y: -.infinity) == #"{"seq":2,"t":0.00,"y":0.00}"#,
      "infinite axes serialise as zero")
```

Append to `app/tests/intent/main.swift`, just above its final `if failures == 0` line:

```swift
// The same rule at the intent layer: a trick formula dividing by a runtime zero must not
// become a held full-reverse command.
check(ControlModel.clamp(.nan) == 0, "clamp(NaN) is 0")
check(ControlModel.clamp(.infinity) == 0, "clamp(+inf) is 0 (non-finite)")
check(ControlModel.clamp(-.infinity) == 0, "clamp(-inf) is 0 (non-finite)")
```

Note: the spec's rule is "non-finite → 0" — infinity included, even though it has a defined order. One rule, no special cases, and an infinite axis is always a bug upstream, never a direction.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `./tools/test-all.sh 2>&1 | sed -n '/swift host tests/,/mock host tests/p'`
Expected: `FAIL: NaN axes serialise as zero` (and the inf lines) then non-zero exit.

- [ ] **Step 3: Implement both clamps**

`app/AJMiddleCar/RTFrame.swift` line 54, replace:

```swift
    private static func clamp(_ v: Double) -> Double { Swift.min(1, Swift.max(-1, v)) }
```

with:

```swift
    /// Non-finite input is a stop, not a direction: max(-1, .nan) is -1, so an unguarded NaN
    /// would stream as sustained full reverse — and formatted raw it would not even be JSON.
    private static func clamp(_ v: Double) -> Double {
        v.isFinite ? Swift.min(1, Swift.max(-1, v)) : 0
    }
```

`app/AJMiddleCar/ControlModel.swift` line 13, replace:

```swift
    static func clamp(_ v: Double) -> Double { min(1, max(-1, v)) }
```

with:

```swift
    /// Non-finite input is a stop, not a direction — same rule as `RTFrame.clamp`, because a
    /// NaN that reaches either becomes a held command.
    static func clamp(_ v: Double) -> Double { v.isFinite ? min(1, max(-1, v)) : 0 }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `./tools/test-all.sh 2>&1 | sed -n '/swift host tests/,/mock host tests/p'`
Expected: `test_rtframe: OK`, `test_intent: OK`, no FAIL lines.

- [ ] **Step 5: Commit**

```bash
git add app/AJMiddleCar/RTFrame.swift app/AJMiddleCar/ControlModel.swift app/tests/rtframe/main.swift app/tests/intent/main.swift
git commit -m "fix(app): non-finite stick values clamp to stop, not full reverse

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Publish only on change

**Files:**
- Modify: `app/AJMiddleCar/CarLink.swift:148-154` (recompute), `app/AJMiddleCar/CarLink.swift:124-134` (telemetry case)

**Interfaces:**
- Consumes: nothing new.
- Produces: no API change; `state` and `lastTelemetry` fire `objectWillChange` only when the value moved.

Why: the 200 ms decay task assigns `state` unconditionally — five root-tree invalidations a second for the app's whole life (audit: CarLink.swift:153). `AppFlow.carIdentified` already documents and avoids this exact pattern.

- [ ] **Step 1: Guard the two publishes**

In `recompute()`, replace:

```swift
        let age = lastFrame.map { (ContinuousClock.now - $0).seconds }
        state = LinkRule.compose(path: pathState, session: session, telemetry: telemetry, age: age)
```

with:

```swift
        let age = lastFrame.map { (ContinuousClock.now - $0).seconds }
        let next = LinkRule.compose(path: pathState, session: session, telemetry: telemetry, age: age)
        // Only on a real change: the decay tick re-asks five times a second, and `@Published`
        // emits on assignment whether or not the value moved.
        if state != next { state = next }
```

In `consume()`'s `.telemetry` case, replace `lastTelemetry = t` with:

```swift
                if lastTelemetry != t { lastTelemetry = t }
```

(`telemetry = t` and `lastFrame = ContinuousClock.now` stay unconditional — they are not `@Published`.)

- [ ] **Step 2: Verify it still compiles and behaves**

Run: `./tools/test-all.sh 2>&1 | tail -3`
Expected: `== all green ==` (CarLink itself has no host suite; `Link` and `Telemetry` are `Equatable` already — the carlink suite exercises `LinkRule`).

- [ ] **Step 3: Commit**

```bash
git add app/AJMiddleCar/CarLink.swift
git commit -m "perf(app): CarLink publishes state and telemetry only on change

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Session lifecycle events become lossless; telemetry becomes latest-wins

**Files:**
- Modify: `app/AJMiddleCar/CarTransport.swift:18-26` (Event), `:61` (listener), `:70-78` (streams), `:287-292` (receiveLoop)
- Modify: `app/AJMiddleCar/CarLink.swift:99-146` (consume)

**Interfaces:**
- Consumes: nothing new.
- Produces: `CarTransport.Event` loses `.telemetry`; new `func telemetryFrames() -> AsyncStream<Telemetry>`; `events()` becomes unbounded. `CarLink` consumes both streams. (Verified: no file outside these two references `CarTransport.Event`.)

Why: `.sessionOpened` shares one 8-slot lossy buffer with 5 Hz telemetry; a busy main thread ≥1.6 s around adoption evicts it, and nothing ever re-announces adoption — UI stuck on the radar behind a live session (audit: CarTransport.swift:72). Chosen mechanism: two streams, because buffering policy is a property of the content — lifecycle events are rare and must all arrive (unbounded, rate bounded by the session loop itself); of telemetry only the newest matters (`bufferingNewest(1)`).

- [ ] **Step 1: Reshape the transport side**

In `CarTransport.swift`, remove the `case telemetry(Telemetry)` line from `enum Event` (keep the other three cases).

Below `private var listener: AsyncStream<Event>.Continuation?` add:

```swift
    private var telemetryListener: AsyncStream<Telemetry>.Continuation?
```

Replace the whole `events()` function and add the new stream:

```swift
    /// Session lifecycle, lossless. Its rate is bounded by the session loop itself — a
    /// handful of events per reconnect — so `.unbounded` cannot grow. What must never happen
    /// is `.sessionOpened` being evicted by a telemetry backlog: it is emitted once per
    /// session, and losing it leaves the app on the radar behind a live, streaming session.
    func events() -> AsyncStream<Event> {
        let (stream, cont) = AsyncStream<Event>.makeStream(bufferingPolicy: .unbounded)
        listener?.finish()
        listener = cont
        return stream
    }

    /// Telemetry, latest-wins. One slot: a consumer that falls behind skips straight to the
    /// newest frame instead of replaying a queue of stale ones.
    func telemetryFrames() -> AsyncStream<Telemetry> {
        let (stream, cont) = AsyncStream<Telemetry>.makeStream(bufferingPolicy: .bufferingNewest(1))
        telemetryListener?.finish()
        telemetryListener = cont
        return stream
    }
```

In `receiveLoop()`, replace `if case .telemetry(let t) = RTFrame.parse(text) { emit(.telemetry(t)) }` with:

```swift
            if case .telemetry(let t) = RTFrame.parse(text) { telemetryListener?.yield(t) }
```

- [ ] **Step 2: Reshape the consumer**

In `CarLink.swift`, replace the whole `consume()` function with:

```swift
    private func consume() async {
        let events = await transport.events()
        let frames = await transport.telemetryFrames()
        // Two streams, one consumer. Ordering between them is not guaranteed; a stale frame
        // landing after `.sessionClosed` only refreshes `lastTelemetry` — `LinkRule.compose`
        // still requires an adopted session to say `.live`, so it cannot resurrect the link.
        await withTaskGroup(of: Void.self) { group in
            group.addTask { @MainActor [weak self] in
                for await e in events {
                    self?.handle(e)
                    self?.recompute()
                }
            }
            group.addTask { @MainActor [weak self] in
                for await t in frames {
                    self?.apply(t)
                    self?.recompute()
                }
            }
            await group.waitForAll()
        }
    }

    private func handle(_ event: CarTransport.Event) {
        switch event {
        case .sessionOpened(let device, let fw):
            self.device = device
            lastTelemetrySeq = nil
            if device == CarContract.device {
                // The firmware version is published only for our own car. It feeds the launch
                // gate, and a foreign car's build number there can force an OTA onto a car
                // that is not ours — routing straight around the wrong-car screen.
                self.fw = fw
                session = .adopted(device: device, fw: fw)
                fetchRadio()
                // The car is reachable exactly now. Prefetching from `onAppear` ran while the
                // gate was still talking to GitHub, so both GETs timed out and every trick
                // spent the session on the fallback geometry the `/dims` work replaced.
                config?.prefetchDriveGeometry()
            } else {
                self.fw = nil
                session = .foreign(device: device)
            }
        case .protoMismatch(let theirs):
            self.fw = nil
            self.device = nil
            session = .protoMismatch(theirs: theirs)
        case .sessionClosed:
            // A foreign identity — or a protocol we cannot speak — survives the session that
            // discovered it: the transport reopens every few seconds and would otherwise
            // flicker the screen naming the problem back to a radar.
            session = session.survivingSessionEnd
            telemetry = nil
            lastFrame = nil
            lastTelemetrySeq = nil
        }
    }

    private func apply(_ t: Telemetry) {
        // Ordered by the car's own counter: a reordered datagram walks uptime, the trip
        // count and the calibration flag backwards, and the mandatory-calibration sheet
        // keys on that flag.
        if let seq = t.seq, let last = lastTelemetrySeq, !RTFrame.seqNewer(seq, than: last) {
            return
        }
        if let seq = t.seq { lastTelemetrySeq = seq }
        telemetry = t
        if lastTelemetry != t { lastTelemetry = t }
        lastFrame = ContinuousClock.now
    }
```

(The `.telemetry` case moved into `apply(_:)` including Task 2's equality guard; delete the old switch entirely.)

- [ ] **Step 3: Verify**

Run: `./tools/test-all.sh 2>&1 | tail -3` — expected `== all green ==`.
Then compile the app target (catches any missed `.telemetry` reference):

```bash
cd app && xcodegen generate >/dev/null && xcodebuild -quiet build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-audit && cd ..
```

Expected: build succeeds (warnings acceptable, errors not).

- [ ] **Step 4: Commit**

```bash
git add app/AJMiddleCar/CarTransport.swift app/AJMiddleCar/CarLink.swift
git commit -m "fix(app): session lifecycle events can no longer be lost to telemetry backlog

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Every failed connect cancels its socket

**Files:**
- Modify: `app/AJMiddleCar/CarTransport.swift:305-335` (connect)

**Interfaces:**
- Consumes: nothing new.
- Produces: no API change; no `NWConnection` outlives a failed `connect()`.

Why: the `.failed` and `.waiting` arms resume the continuation but never call `socket.cancel()`; the state handler captures `socket` strongly while `NWConnection` retains the handler — a retain cycle, one leaked (and in `.waiting`, still path-watching) connection per retry, ~700/hour foregrounded away from the car (audit: CarTransport.swift:317).

- [ ] **Step 1: Cancel on both failure arms**

In `connect()`'s `stateUpdateHandler`, replace the two failure arms:

```swift
                    case .failed(let e):
                        once.resume(.failure(CarError.from(e, path: socket.currentPath)))
                    case .waiting(let e):
                        // With the interface pinned, waiting means the Wi-Fi path is not there
                        // yet. Fail now and let the backoff retry rather than sit in a state
                        // that may never resolve.
                        once.resume(.failure(CarError.from(e, path: socket.currentPath)))
```

with:

```swift
                    case .failed(let e):
                        once.resume(.failure(CarError.from(e, path: socket.currentPath)))
                        // A socket that never became `conn` is nobody else's to cancel. Without
                        // this the handler↔connection retain cycle leaks one NWConnection per
                        // failed attempt — and a `.waiting` one keeps path-watching and later
                        // goes `.ready` as an open socket nothing observes.
                        socket.cancel()
                    case .waiting(let e):
                        // With the interface pinned, waiting means the Wi-Fi path is not there
                        // yet. Fail now and let the backoff retry rather than sit in a state
                        // that may never resolve.
                        once.resume(.failure(CarError.from(e, path: socket.currentPath)))
                        socket.cancel()
```

(`once` is a OneShot: the `.cancelled` state change that follows `socket.cancel()` resumes nothing.)

- [ ] **Step 2: Verify**

Run: `./tools/test-all.sh 2>&1 | tail -3` — expected `== all green ==`.
Manual check note (not automatable here): on a device off the car's Wi-Fi the reconnect loop now cycles without accumulating connections; in Instruments the `NWConnection` count stays flat.

- [ ] **Step 3: Commit**

```bash
git add app/AJMiddleCar/CarTransport.swift
git commit -m "fix(app): cancel the socket when connect() fails or waits

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: The wrong-car hold becomes cancellable — retry actually retries

**Files:**
- Modify: `app/AJMiddleCar/CarTransport.swift:186-203` (the two 10 s holds), new members near `:57`
- Modify: `app/AJMiddleCar/CarLink.swift:93-97` (retryAfterWrongCar)

**Interfaces:**
- Consumes: nothing new.
- Produces: `CarTransport.retryNow()` (nonisolated-safe actor method, callable from CarLink); `holdIdentity()` private.

Why: the wrong-car/wrong-proto screens' retry button only mutates CarLink state; the transport sleeps out an uncancelled `Task.sleep(for: .seconds(10))`, so the user watches a radar for up to 10 s plus backoff after switching networks (audit: CarLink.swift:93).

- [ ] **Step 1: Extract the hold into a cancellable task**

In `CarTransport`, below `private var everAdopted = false` add:

```swift
    /// The wrong-car / wrong-proto hold. Stored so the retry button can abort it: without
    /// that the transport sleeps out its ten seconds while the user watches a radar that
    /// claims to be retrying.
    private var identityHold: Task<Void, Error>?
```

Below `sayGoodbye(on:)` add:

```swift
    /// Hold the session after a car identified itself as undriveable — long enough that the
    /// screen naming the problem is not a flicker between radar sweeps, but abortable the
    /// moment the user asks for another look.
    private func holdIdentity() async {
        let hold = Task { try await Task.sleep(for: .seconds(10)) }
        identityHold = hold
        // Outer cancellation (stop()) must not wait out the unstructured hold.
        await withTaskCancellationHandler {
            _ = try? await hold.value
        } onCancel: {
            hold.cancel()
        }
        identityHold = nil
    }

    /// The retry button on the wrong-car and wrong-protocol screens.
    func retryNow() { identityHold?.cancel() }
```

In `session()`, replace both `try await Task.sleep(for: .seconds(10))` lines (the `.protoMismatch` arm and the foreign-device guard) with:

```swift
            await holdIdentity()
```

(The `throw CarError...` lines that follow each stay exactly as they are; a cancelled hold falls through to the same throw, the run loop notices `Task.isCancelled` only when the whole transport is stopping.)

- [ ] **Step 2: Poke the transport from the retry button**

In `CarLink.swift`, replace `retryAfterWrongCar()`:

```swift
    /// The wrong-car and wrong-protocol screens' retry: forget what the car said about itself and
    /// look again. Nothing else clears either, on purpose — neither is a transient failure to
    /// retry silently behind a radar sweep.
    func retryAfterWrongCar() {
        session = .none
        device = nil
        recompute()
        // The transport is mid-hold on the session that discovered the identity; abort it so
        // the next hello goes out now, not after the remainder of the ten seconds.
        Task { [transport] in await transport.retryNow() }
    }
```

- [ ] **Step 3: Verify**

Run: `./tools/test-all.sh 2>&1 | tail -3` — expected `== all green ==`.
Manual check note: with the mock impersonating the other car (`MOCK_DEVICE=esp32-car`), the wrong-car screen's retry now produces a fresh hello within ~1 s (visible in the mock's log) instead of after 10 s.

- [ ] **Step 4: Commit**

```bash
git add app/AJMiddleCar/CarTransport.swift app/AJMiddleCar/CarLink.swift
git commit -m "fix(app): wrong-car retry aborts the transport's 10 s hold

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: The radio version survives one missed /status

**Files:**
- Modify: `app/AJMiddleCar/CarLink.swift:156-164` (fetchRadio), stop path, new member
- Modify: `app/AJMiddleCar/FirmwareView.swift:25-28` (.task)

**Interfaces:**
- Consumes: nothing new.
- Produces: `CarLink.refreshRadio()` (public, @MainActor).

Why: the radio co-processor version is fetched exactly once per adoption with a 2 s timeout, racing the config prefetch at a single-threaded server; any failure leaves `radio` nil silently for the whole session, hiding the load-bearing pinned-version mismatch (audit: CarLink.swift:157; CLAUDE.md gotcha 10).

- [ ] **Step 1: Retry with backoff, cancellable, refreshable**

In `CarLink`, below `private var decay: Task<Void, Never>?` add:

```swift
    private var radioFetch: Task<Void, Never>?
```

Replace `fetchRadio()` with:

```swift
    /// `/status` is one GET against a single-request server that is busy with the geometry
    /// prefetch fired in the same instant — one miss must not hide a radio mismatch for the
    /// whole session. Four tries, backing off; the task is replaced on refetch and cancelled
    /// when the link stops.
    private func fetchRadio() {
        radioFetch?.cancel()
        radioFetch = Task { [weak self, transport] in
            for delay: Double in [0, 1, 2, 4] {
                if delay > 0 { try? await Task.sleep(for: .seconds(delay)) }
                if Task.isCancelled { return }
                if let data = try? await transport.get("/status", timeout: 2),
                   let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                   let r = j["radio"] as? [String: Any],
                   let fw = r[CarContract.fwField] as? String {
                    self?.radio = Radio(fw: fw, ok: r["ok"] as? Bool ?? true)
                    return
                }
            }
        }
    }

    /// FirmwareView calls this on appear: the radio line is that screen's reason to exist,
    /// and an OTA just behind us may have changed the answer.
    func refreshRadio() { fetchRadio() }
```

In `stop(graceful:)` (after `decay?.cancel(); decay = nil`) add:

```swift
        radioFetch?.cancel(); radioFetch = nil
```

- [ ] **Step 2: Refetch when the firmware screen appears**

In `FirmwareView.swift`, replace the `.task` modifier:

```swift
        .task {
            if let dp = debugPhase { phase = dp; return }
            link.refreshRadio()
            await check()
        }
```

- [ ] **Step 3: Verify**

Run: `./tools/test-all.sh 2>&1 | tail -3` — expected `== all green ==`.
Manual check note: against the mock, kill `/status` once (e.g. stop the mock for the first two seconds after adoption, restart it) — the radio line appears within ~7 s instead of never.

- [ ] **Step 4: Commit**

```bash
git add app/AJMiddleCar/CarLink.swift app/AJMiddleCar/FirmwareView.swift
git commit -m "fix(app): radio version fetch retries and refetches on the firmware screen

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: The launch gate works offline when a firmware is cached

**Files:**
- Create: `app/AJMiddleCar/GateRule.swift`
- Modify: `app/AJMiddleCar/AppFlow.swift:26-43` (startupCheck)
- Create: `app/tests/gate/main.swift`, `app/tests/gate/sources`

**Interfaces:**
- Consumes: `UpdateClient.hasCachedFile`, `UpdateClient.cachedBuild` (existing statics).
- Produces: `GateRule.canProceedOffline(hasCachedFile: Bool, cachedBuild: Int?) -> Bool`.

Why: `startupCheck()` is the only path to `.awaitingCar` and unconditionally requires the GitHub probe AND a successful release fetch; a phone in the field without internet dead-ends on retry screens forever with a healthy car broadcasting and a valid image cached (audit: AppFlow.swift:28). With the gate handed over and `latestTag` nil, `UpdateClient.mustUpdate` is inert by design — the same state a release without a build number produces.

- [ ] **Step 1: Write the failing test**

Create `app/tests/gate/sources`:

```
GateRule.swift
```

Create `app/tests/gate/main.swift`:

```swift
// Host test for the launch gate's offline rule. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// GitHub being unreachable must not strand a phone next to a healthy car — but only when
// there is actually something to flash-compare later: a cached file AND its recorded build.
check(GateRule.canProceedOffline(hasCachedFile: true, cachedBuild: 517),
      "cached file + build proceeds offline")
check(!GateRule.canProceedOffline(hasCachedFile: false, cachedBuild: 517),
      "no file: the first-ever launch still needs the internet")
check(!GateRule.canProceedOffline(hasCachedFile: true, cachedBuild: nil),
      "file without a recorded build is not a known version")
check(!GateRule.canProceedOffline(hasCachedFile: false, cachedBuild: nil),
      "nothing cached, nothing to proceed with")

if failures == 0 { print("test_gate: OK") } else { exit(1) }
```

- [ ] **Step 2: Run to verify it fails**

Run: `./tools/test-all.sh 2>&1 | sed -n '/swift host tests/,/mock host tests/p'`
Expected: swiftc error — `cannot find 'GateRule' in scope` (compile failure is the failing state).

- [ ] **Step 3: Implement**

Create `app/AJMiddleCar/GateRule.swift`:

```swift
import Foundation

/// The launch gate's offline rule, pure so it is host-tested.
///
/// GitHub being unreachable must not strand a phone standing next to a healthy car: when a
/// cached firmware image exists (the file and its recorded build), the gate hands over and
/// the forced-update comparison simply has no `latestTag` to work with — the same inert
/// state a release without a build number produces.
enum GateRule {
    static func canProceedOffline(hasCachedFile: Bool, cachedBuild: Int?) -> Bool {
        hasCachedFile && cachedBuild != nil
    }
}
```

In `AppFlow.swift`, replace `startupCheck()`'s two guards and add the helper:

```swift
    /// Run the pre-connect gate (internet probe → latest release → download if needed).
    func startupCheck() async {
        phase = .checkInternet
        guard await UpdateClient.internetReachable() else {
            phase = offlineFallback(or: .noInternet)
            return
        }
        phase = .checkUpdate
        guard let rel = await client.latestRelease() else {
            phase = offlineFallback(or: .checkFailed)
            return
        }
        latestTag = rel.tag
        let latestBuild = UpdateClient.buildNumber(rel.tag)
        if UpdateClient.needsDownload(latestBuild: latestBuild,
                                      cachedBuild: UpdateClient.cachedBuild,
                                      hasCachedFile: UpdateClient.hasCachedFile) {
            phase = .downloading
            let t0 = Date()
            guard await client.download(rel.assetURL) != nil else { phase = .checkFailed; return }
            await UpdateClient.holdAtLeast(UpdateClient.downloadMinDisplay, since: t0)
            if let b = latestBuild { UpdateClient.recordCache(build: b, tag: rel.tag) }
        }
        phase = .awaitingCar
    }

    /// GitHub unreachable or unusable: a cached image is enough to drive — the gate exists
    /// to force updates when it can know about them, not to require the internet.
    private func offlineFallback(or failure: Phase) -> Phase {
        GateRule.canProceedOffline(hasCachedFile: UpdateClient.hasCachedFile,
                                   cachedBuild: UpdateClient.cachedBuild) ? .awaitingCar : failure
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `./tools/test-all.sh 2>&1 | sed -n '/swift host tests/,/mock host tests/p'`
Expected: `test_gate: OK` among the suites, no FAIL lines.

- [ ] **Step 5: Commit**

```bash
git add app/AJMiddleCar/GateRule.swift app/AJMiddleCar/AppFlow.swift app/tests/gate
git commit -m "fix(app): launch gate proceeds offline when a firmware is already cached

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: One serialized lifecycle — the scene handler can no longer kill the link

**Files:**
- Modify: `app/AJMiddleCar/CarLink.swift:60-97` (start/stop become a FIFO chain)
- Modify: `app/AJMiddleCar/AJMiddleCarApp.swift:37-50` (scene handler), `app/AJMiddleCar/AppFlow.swift` (Phase.opensLink)

**Interfaces:**
- Consumes: Task 3's `consume()` shape; Task 6's `radioFetch` property (cancelled in `shutDown`).
- Produces: `CarLink.requestStop(graceful: Bool)` (sync enqueue), `AppFlow.Phase.opensLink: Bool`. `CarLink.start()` keeps its signature; `CarLink.stop(graceful:) async` remains and awaits the chain.

Why (two findings): (1) the unstructured stop task enqueued on every departure from `.active` races the synchronous `start()` on the next arrival — in the losing interleavings the guards `pump == nil` / `runTask == nil` make start a no-op and the stop then tears everything down, leaving nobody to revive the link until the next background cycle (audit: AJMiddleCarApp.swift:48). (2) `link.start()` fires on every `.active` regardless of `flow.phase`, opening an invisible adopted session behind the no-internet screen (audit: AJMiddleCarApp.swift:42). The fix: lifecycle operations enqueue synchronously onto one FIFO chain inside CarLink (the codebase's `httpTail` pattern), so execution order equals call order on the main thread; and the scene handler gates on the same phase set the root uses and skips the stop on the `.inactive` step of a foregrounding.

- [ ] **Step 1: Give CarLink the FIFO lifecycle chain**

In `CarLink`, below `private var pathSub: AnyCancellable?` add:

```swift
    /// Lifecycle operations run strictly in call order. `start()` and `requestStop()` enqueue
    /// synchronously on the main actor, so the order the scene handler calls them in is the
    /// order they execute in — the unstructured stop task racing a later start() is how the
    /// link used to die until the next background/foreground cycle.
    private var lifecycle: Task<Void, Never>?

    private func enqueue(_ op: @escaping @MainActor () async -> Void) {
        let prev = lifecycle
        lifecycle = Task { await prev?.value; await op() }
    }
```

Replace `start()` and `stop(graceful:)` with:

```swift
    /// Open the channel. Idempotent; the transport owns the reconnect loop.
    func start() {
        enqueue { [weak self] in await self?.beginPumping() }
    }

    /// Leaving the app is a goodbye said in words — the car is told to stop rather than left
    /// to notice. Synchronous enqueue: callers in view handlers must not need an await for
    /// the ordering guarantee to hold.
    func requestStop(graceful: Bool) {
        enqueue { [weak self] in await self?.shutDown(graceful: graceful) }
    }

    func stop(graceful: Bool) async {
        requestStop(graceful: graceful)
        await lifecycle?.value
    }

    private func beginPumping() async {
        guard pump == nil else { return }
        pump = Task { [weak self] in await self?.consume() }
        // Liveness has to expire on its own: telemetry stopping is silence, and silence
        // generates no event to react to.
        decay = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(200))
                self?.recompute()
            }
        }
        await transport.start()
    }

    private func shutDown(graceful: Bool) async {
        // Idempotent: the scene path can enqueue two stops back to back (.inactive, then
        // .background), and the second must be a no-op rather than a second goodbye.
        guard pump != nil else { return }
        pump?.cancel(); pump = nil
        decay?.cancel(); decay = nil
        radioFetch?.cancel(); radioFetch = nil
        await transport.stop(graceful: graceful)
        telemetry = nil
        lastFrame = nil
        lastTelemetrySeq = nil
        // Backgrounding is not a verdict on who the car is. Leaving `.active` — which a Control
        // Center pull-down alone does — used to clear `.protoMismatch`, so the screen naming the
        // mismatch flipped to the radar until the next hello reply landed.
        session = session.survivingSessionEnd
        recompute()
    }
```

(This absorbs Task 6's `radioFetch` cancel; the old `stop` body is gone.)

- [ ] **Step 2: Name the phases that open the link**

In `AppFlow.swift`, inside `enum Phase`, add:

```swift
        /// The phases whose screens open the link — the same set `root` switches on. The
        /// scene handler restarts the link on `.active` and must not open a session behind
        /// a gate screen that says there is nothing to talk to.
        var opensLink: Bool {
            switch self {
            case .updateRequired, .awaitingCar, .ready: return true
            case .checkInternet, .noInternet, .checkUpdate, .checkFailed, .downloading: return false
            }
        }
```

- [ ] **Step 3: Rewrite the scene handler**

In `AJMiddleCarApp.swift`, replace the `.onChange(of: scenePhase)` modifier:

```swift
            .onChange(of: scenePhase) { oldPhase, newPhase in
                switch newPhase {
                case .active:
                    // The config prefetch is not here: nothing can be read from a car the app
                    // has not met yet. And the gate decides whether there is a link to open at
                    // all — starting behind the no-internet screen opened an invisible session
                    // that streamed zeros and outranked the bench console.
                    if flow.phase.opensLink { link.start() }
                case .inactive:
                    // Only on the way down. On the way up (.background → .inactive → .active)
                    // the link is already stopped, and a stop enqueued here would cancel the
                    // start the .active step is about to make.
                    if oldPhase == .active {
                        intent.neutral()
                        link.requestStop(graceful: true)
                    }
                case .background:
                    intent.neutral()
                    link.requestStop(graceful: true)
                @unknown default:
                    break
                }
            }
```

- [ ] **Step 4: Verify**

Run: `./tools/test-all.sh 2>&1 | tail -3` — expected `== all green ==`.
Compile: `cd app && xcodegen generate >/dev/null && xcodebuild -quiet build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-audit && cd ..` — expected: success.
Manual check note: in the simulator against the mock, pull Control Center down and release ten times in a row — the link must come back live every time (this was interleaving-dependent before; ten rounds is the smoke bar, not proof).

- [ ] **Step 5: Commit**

```bash
git add app/AJMiddleCar/CarLink.swift app/AJMiddleCar/AJMiddleCarApp.swift app/AJMiddleCar/AppFlow.swift
git commit -m "fix(app): serialize the link lifecycle and gate scene restarts by flow phase

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: SessionPolicy — the transport's decisions become host-tested

**Files:**
- Create: `app/AJMiddleCar/SessionPolicy.swift`
- Modify: `app/AJMiddleCar/CarTransport.swift` (`awaitHello`, `backoff`, `holdIdentity`)
- Create: `app/tests/sessionpolicy/main.swift`, `app/tests/sessionpolicy/sources`

**Interfaces:**
- Consumes: `RTFrame.Inbound` (existing), Task 5's `holdIdentity()`.
- Produces: `SessionPolicy.HandshakeOutcome` (`.identity(device:fw:)`, `.protoMismatch(theirs:)`, `.ignore`); `SessionPolicy.handshakeOutcome(_:sid:)`; `SessionPolicy.backoffBase(attempt:pathBlocked:everAdopted:base:cap:discoveryCap:) -> Double`; `SessionPolicy.identityHoldSeconds`.

Why: the 620-line session machine has zero tests on any side (audit: CarTransport.swift:168). A full transport test rig is out of scope; what IS pure — reply filtering by sid, backoff progression and its caps, the hold duration — moves into `SessionPolicy` and gets pinned. The sid filter is the line that makes ownership non-resumable; the discovery cap is what the contract's "hello at ~5 Hz until answered" turns into on this side.

- [ ] **Step 1: Write the failing test**

Create `app/tests/sessionpolicy/sources`:

```
SessionPolicy.swift
RTFrame.swift
```

Create `app/tests/sessionpolicy/main.swift`:

```swift
// Host test for the transport's pure session decisions. Run with swiftc; no XCTest.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// -- handshake filtering: a reply for another sid is a leftover from a previous socket. ----
let sid = "7f3a91c2"
let ourReply = RTFrame.parse(#"{"proto":1,"hello":"7f3a91c2","device":"ajmiddlecar","fw":"v1.0+517"}"#)
check(SessionPolicy.handshakeOutcome(ourReply, sid: sid)
        == .identity(device: "ajmiddlecar", fw: "v1.0+517"), "our sid's reply is the identity")

let staleReply = RTFrame.parse(#"{"proto":1,"hello":"deadbeef","device":"ajmiddlecar","fw":"v1.0+517"}"#)
check(SessionPolicy.handshakeOutcome(staleReply, sid: sid) == .ignore,
      "another sid's reply is ignored — ownership is not resumable")

let mismatch = RTFrame.parse(#"{"proto":2,"hello":"7f3a91c2"}"#)
check(SessionPolicy.handshakeOutcome(mismatch, sid: sid) == .protoMismatch(theirs: 2),
      "a proto mismatch for our sid is reported, not ignored")
let staleMismatch = RTFrame.parse(#"{"proto":2,"hello":"deadbeef"}"#)
check(SessionPolicy.handshakeOutcome(staleMismatch, sid: sid) == .ignore,
      "a proto mismatch for another sid is a leftover too")

var telemetry = Telemetry(); telemetry.uptimeS = 5
check(SessionPolicy.handshakeOutcome(.telemetry(telemetry), sid: sid) == .ignore,
      "telemetry during the handshake is not an answer")
check(SessionPolicy.handshakeOutcome(nil, sid: sid) == .ignore, "garbage is ignored")

// -- backoff: exponential, capped, and capped LOWER before any car ever answered. ----------
check(SessionPolicy.backoffBase(attempt: 0, pathBlocked: false, everAdopted: true) == 0.1,
      "attempt 0 is the base")
check(SessionPolicy.backoffBase(attempt: 1, pathBlocked: false, everAdopted: true) == 0.1,
      "attempt 1 is the base")
check(SessionPolicy.backoffBase(attempt: 2, pathBlocked: false, everAdopted: true) == 0.2,
      "attempt 2 doubles")
check(SessionPolicy.backoffBase(attempt: 10, pathBlocked: false, everAdopted: true) == 5.0,
      "the cap holds after adoption")
check(SessionPolicy.backoffBase(attempt: 10, pathBlocked: false, everAdopted: false) == 1.0,
      "discovery is capped at one second: a car switched on late is found within a second")
check(SessionPolicy.backoffBase(attempt: 10, pathBlocked: true, everAdopted: false) == 5.0,
      "a blocked path earns the full cap — waiting there costs nothing")

// -- the identity hold is long enough to read, and one place owns the number. --------------
check(SessionPolicy.identityHoldSeconds == 10, "the wrong-car hold is ten seconds")

if failures == 0 { print("test_sessionpolicy: OK") } else { exit(1) }
```

- [ ] **Step 2: Run to verify it fails**

Run: `./tools/test-all.sh 2>&1 | sed -n '/swift host tests/,/mock host tests/p'`
Expected: swiftc error — `cannot find 'SessionPolicy' in scope`.

- [ ] **Step 3: Implement SessionPolicy**

Create `app/AJMiddleCar/SessionPolicy.swift`:

```swift
import Foundation

/// The session's pure decisions, extracted from `CarTransport` so they are host-tested.
/// The transport keeps the sockets and the clock; this answers "what does this reply mean"
/// and "how long do we wait" — the rules the cutover plan specifies and nothing pinned.
enum SessionPolicy {
    /// What one inbound datagram means to a handshake waiting on `sid`. A reply for another
    /// session id is a leftover from a previous socket; ignoring it is what makes ownership
    /// non-resumable rather than accidentally inherited.
    enum HandshakeOutcome: Equatable {
        case identity(device: String, fw: String)
        case protoMismatch(theirs: Int)
        case ignore
    }

    static func handshakeOutcome(_ inbound: RTFrame.Inbound?, sid: String) -> HandshakeOutcome {
        switch inbound {
        case .helloReply(let replySid, let device, let fw) where replySid == sid:
            return .identity(device: device, fw: fw)
        case .protoMismatch(let replySid, let theirs) where replySid == sid:
            return .protoMismatch(theirs: theirs)
        default:
            return .ignore
        }
    }

    /// Seconds before the next connection attempt, pre-jitter. Exponential, capped — and
    /// capped lower while no car has ever answered and the path is fine, because the
    /// contract asks for hello "repeated at ~5 Hz until answered", and a five-second sleep
    /// earned by a car that was not switched on yet is a car found five seconds late.
    static func backoffBase(attempt: Int,
                            pathBlocked: Bool,
                            everAdopted: Bool,
                            base: Double = 0.1,
                            cap: Double = 5.0,
                            discoveryCap: Double = 1.0) -> Double {
        let ceiling = (everAdopted || pathBlocked) ? cap : discoveryCap
        return min(ceiling, base * pow(2, Double(max(0, attempt - 1))))
    }

    /// How long a session holds after the car identified itself as undriveable (wrong car,
    /// wrong protocol) — long enough that the screen naming the problem is not a flicker
    /// between radar sweeps.
    static let identityHoldSeconds: Double = 10
}
```

- [ ] **Step 4: Make CarTransport use it**

In `awaitHello(sid:)`, replace the `switch RTFrame.parse(text)` block with:

```swift
            switch SessionPolicy.handshakeOutcome(RTFrame.parse(text), sid: sid) {
            case .identity(let device, let fw):
                return .identity(Identity(device: device, fw: fw))
            case .protoMismatch(let theirs):
                return .protoMismatch(theirs: theirs)
            case .ignore:
                continue
            }
```

Replace `backoff(_:pathBlocked:)`'s body with:

```swift
        .seconds(SessionPolicy.backoffBase(attempt: attempt,
                                           pathBlocked: pathBlocked,
                                           everAdopted: everAdopted,
                                           base: Self.backoffBase,
                                           cap: Self.backoffCap,
                                           discoveryCap: Self.discoveryCap)
                 * Double.random(in: 0.75...1.25))
```

(Keep the doc comment; the statics `backoffBase`/`backoffCap`/`discoveryCap` stay as the transport's tuning, the progression rule lives in the policy. Note the property is `Self.backoffBase` while the parameter is `base:` — no rename needed.)

In `holdIdentity()`, replace `.seconds(10)` with `.seconds(SessionPolicy.identityHoldSeconds)`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `./tools/test-all.sh 2>&1 | sed -n '/swift host tests/,/mock host tests/p'`
Expected: `test_sessionpolicy: OK`, no FAIL lines.

- [ ] **Step 6: Commit**

```bash
git add app/AJMiddleCar/SessionPolicy.swift app/AJMiddleCar/CarTransport.swift app/tests/sessionpolicy
git commit -m "test(app): extract and pin the transport's pure session decisions

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 10: Full verification

**Files:** none new.

- [ ] **Step 1: Full host suite**

Run: `./tools/test-all.sh`
Expected: `== all green ==` with `test_gate: OK` and `test_sessionpolicy: OK` among the swift suites.

- [ ] **Step 2: App build from regenerated project**

```bash
cd app && xcodegen generate && \
xcodebuild -quiet build -scheme AJMiddleCar \
  -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-audit; cd ..
```

Expected: `xcodegen` lists the new files (GateRule.swift, SessionPolicy.swift) in the project; build succeeds. (`iPhone 17` exists on this Mac — verified with `xcrun simctl list devices available`.)

- [ ] **Step 3: Commit anything the regenerate touched**

`app/AJMiddleCar.xcodeproj` is gitignored, so normally nothing — `git status --short` should be clean. If it is not, stop and report rather than committing stray files.

---

## Deliberately out of scope

- The two-phone hijack/oscillation (audit rt_link.h:65 #2): wire-level, deferred by the decisions spec (rule 11) — mock/docs plans own the pinning.
- A full CarTransport fake-socket rig: `SessionPolicy` covers the pure decisions; injecting a socket into the actor is a rearchitecture this plan deliberately avoids.
- `DriveView`'s onDisappear comment (lines 145-157) references "the only callers of link.start()" — still true after Task 8 (scene handler + root onAppear); no change needed.
