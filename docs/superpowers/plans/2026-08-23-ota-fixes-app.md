# OTA Fixes — App Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the app-side findings of the 2026-08-23 OTA-chain audit: the forced-update gate works offline-first, an acknowledged upload is never reported as failure, rollback is visible and loop-free, downloads are validated before they touch the cache, and the radio line stops hiding.

**Architecture:** Pure decisions move next to the existing rule enums (`UpdateRules` joins `GateRule`/`SessionPolicy`) and get swiftc host tests; the actors and views keep their shapes. `CarLink` grows a three-state radio status and a `rollback` flag read from the same `/status` fetch that already exists. `FirmwareView` gains two phases (`.flashed`, plus a forced-mode escape) rather than a redesign.

**Tech Stack:** Swift 6 / SwiftUI, Network.framework, swiftc host tests (no XCTest), XcodeGen.

**Spec:** `docs/superpowers/specs/2026-08-23-ota-fix-decisions.md` (decisions 1, 4, 5, 6, 14–18)

## Global Constraints

- Work in the worktree: `/Users/adamjohnson/VSCode/esp32-p4-car/.claude/worktrees/ota-fixes`. All commands run from that repo root.
- Wire constants come ONLY from `app/AJMiddleCar/Generated/CarAPI.swift`. Never hand-edit `Generated/`.
- The XcodeGen target globs `AJMiddleCar/`, so new `.swift` files need no `project.yml` change; the final task regenerates and builds to prove it.
- Host tests: `app/tests/<name>/main.swift` (+ optional `sources` listing app files relative to `app/AJMiddleCar/`), ending with `if failures == 0 { print("test_<name>: OK") } else { exit(1) }`. `./tools/test-all.sh` compiles each with swiftc together with `Generated/CarAPI.swift`.
- The app is landscape-locked and Russian-localised: every new user-facing string goes through `L.swift` + `Resources/ru.lproj/Localizable.strings`.
- **Cross-plan note:** the `/status` `rollback` key and the mock's `--rollback` rehearsal land in the core plan (`2026-08-23-ota-fixes-core.md`). The app parses the key tolerantly (absent → `nil`), so no task here hard-depends on it; end-to-end rollback rehearsal against the mock only works after the core plan's mock task. Check with: `grep -n 'rollback' tools/mock_car/mock_car.py tools/mock_car/state.py || echo core-plan-not-landed`.
- After every task: `./tools/test-all.sh` green, then commit. Commit style: `fix(app)`/`feat(app)` lowercase subject, body says why, and every message ends with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

### Task 1: `UpdateRules` — the update decisions become a pure, host-tested module

**Files:**
- Create: `app/AJMiddleCar/UpdateRules.swift`
- Modify: `app/AJMiddleCar/UpdateClient.swift` (statics become one-line forwards)
- Create: `app/tests/update/main.swift`, `app/tests/update/sources`

**Interfaces:**
- Consumes: nothing new.
- Produces (Tasks 2, 3, 7 and the views rely on these exact names):
  - `UpdateRules.normalize(_ v: String?) -> String`
  - `UpdateRules.buildNumber(_ version: String?) -> Int?`
  - `UpdateRules.isUpdateAvailable(running: String?, latest: String?) -> Bool`
  - `UpdateRules.needsDownload(latestBuild: Int?, cachedBuild: Int?, hasCachedFile: Bool) -> Bool`
  - `UpdateRules.mustUpdate(carFw: String?, latestTag: String?) -> Bool`
  - `UpdateRules.isValidImage(firstByte: UInt8?, size: Int) -> Bool` (decision 6's validation rule)
  - `UpdateClient` keeps its static names as forwards, so no call site outside this task changes.

Why: `UpdateClient.swift` cannot compile in a host test (it references `CarTransport`), so its pure logic — including the new download-validation rule — has never been host-testable. Same move as `GateRule`/`SessionPolicy`.

- [ ] **Step 1: Write the failing test**

Create `app/tests/update/sources`:

```
UpdateRules.swift
```

Create `app/tests/update/main.swift`:

```swift
// Host test for the update decisions. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// -- version parsing, against the strings the firmware actually ships -----------------
check(UpdateRules.buildNumber("v1.0+584") == 584, "firmware-shaped tag parses")
check(UpdateRules.buildNumber("v1.0") == nil, "no build number is nil")
check(UpdateRules.buildNumber(nil) == nil, "nil is nil")
check(UpdateRules.normalize("v1.2-3-gabc") == "1.2", "normalize strips v and -suffix")

// -- gate decisions -------------------------------------------------------------------
check(UpdateRules.mustUpdate(carFw: "v1.0+500", latestTag: "v1.0+584"), "behind → forced")
check(!UpdateRules.mustUpdate(carFw: "v1.0+584", latestTag: "v1.0+584"), "equal → free")
check(!UpdateRules.mustUpdate(carFw: "v1.0+600", latestTag: "v1.0+584"), "dev build ahead → free")
check(UpdateRules.mustUpdate(carFw: "v0.9", latestTag: "v1.0+584"), "pre-versioning car → forced")
check(!UpdateRules.mustUpdate(carFw: "v1.0+500", latestTag: nil), "no known release → gate inert")

check(UpdateRules.isUpdateAvailable(running: "v1.0+500", latest: "v1.0+584"), "newer is available")
check(!UpdateRules.isUpdateAvailable(running: "v1.0+584", latest: "v1.0+584"), "same is not")
check(UpdateRules.needsDownload(latestBuild: 584, cachedBuild: 500, hasCachedFile: true),
      "stale cache re-downloads")
check(!UpdateRules.needsDownload(latestBuild: 584, cachedBuild: 584, hasCachedFile: true),
      "current cache does not")

// -- decision 6: what may enter the firmware cache ------------------------------------
check(UpdateRules.isValidImage(firstByte: 0xE9, size: 763_088), "a real image passes")
check(!UpdateRules.isValidImage(firstByte: 0x3C, size: 763_088),
      "an HTML error page ('<') is not firmware")
check(!UpdateRules.isValidImage(firstByte: 0xE9, size: 4095), "under the 4 KB floor fails")
check(UpdateRules.isValidImage(firstByte: 0xE9, size: 4096), "exactly the floor passes")
check(!UpdateRules.isValidImage(firstByte: nil, size: 0), "an empty body fails")

if failures == 0 { print("test_update: OK") } else { exit(1) }
```

- [ ] **Step 2: Run to verify it fails**

Run: `./tools/test-all.sh 2>&1 | sed -n '/swift host tests/,/mock host tests/p'`
Expected: swiftc error — `cannot find 'UpdateRules' in scope` (compile failure is the failing state).

- [ ] **Step 3: Implement**

Create `app/AJMiddleCar/UpdateRules.swift` — move the five statics' BODIES verbatim from `UpdateClient.swift` (they are pure already) and add the validation rule:

```swift
import Foundation

/// The update chain's pure decisions, extracted from `UpdateClient` so they are host-tested.
/// `UpdateClient` keeps the sockets, the cache files and the sessions; this answers "which
/// version wins" and "what may enter the firmware cache".
enum UpdateRules {
    /// Normalize a version like "v1.2" / "v1.2-3-gabc" → "1.2" for comparison.
    static func normalize(_ v: String?) -> String {
        guard let v else { return "" }
        var s = v
        if s.hasPrefix("v") { s.removeFirst() }
        if let dash = s.firstIndex(of: "-") { s = String(s[s.startIndex..<dash]) }
        return s
    }

    /// Build number after the first "+" (e.g. "v1.2+246" -> 246); nil if absent/non-numeric.
    static func buildNumber(_ version: String?) -> Int? {
        guard let version, let plus = version.firstIndex(of: "+") else { return nil }
        let digits = version[version.index(after: plus)...].prefix { $0.isNumber }
        return digits.isEmpty ? nil : Int(digits)
    }

    /// Update available iff both versions carry a build number and latest > running.
    /// Falls back to normalized string inequality when a build number is missing.
    static func isUpdateAvailable(running: String?, latest: String?) -> Bool {
        if let r = buildNumber(running), let l = buildNumber(latest) { return l > r }
        return normalize(latest) != normalize(running)
    }

    /// Need to (re)download the .bin: only when there IS a versioned latest release, and the
    /// cached file is missing or its build differs from the latest.
    static func needsDownload(latestBuild: Int?, cachedBuild: Int?, hasCachedFile: Bool) -> Bool {
        guard let latestBuild else { return false }
        return !hasCachedFile || cachedBuild != latestBuild
    }

    /// Forced update required iff the latest release carries a build number AND either the
    /// running firmware predates versioning (no build number) or its build is lower.
    static func mustUpdate(carFw: String?, latestTag: String?) -> Bool {
        guard let latest = buildNumber(latestTag) else { return false }
        guard let car = buildNumber(carFw) else { return true }
        return latest > car
    }

    /// Decision 6: what may enter the firmware cache. An ESP application image starts with
    /// 0xE9 and the car rejects anything under 4 KB — a GitHub error page satisfies neither,
    /// and caching one used to poison the cache until a strictly newer release existed.
    static func isValidImage(firstByte: UInt8?, size: Int) -> Bool {
        firstByte == 0xE9 && size >= 4096
    }
}
```

In `app/AJMiddleCar/UpdateClient.swift`, replace the five statics' bodies with one-line forwards (keep the doc comments on the forwards, so call sites keep their documentation):

```swift
    static func normalize(_ v: String?) -> String { UpdateRules.normalize(v) }
    static func buildNumber(_ version: String?) -> Int? { UpdateRules.buildNumber(version) }
    static func isUpdateAvailable(running: String?, latest: String?) -> Bool {
        UpdateRules.isUpdateAvailable(running: running, latest: latest)
    }
    static func needsDownload(latestBuild: Int?, cachedBuild: Int?, hasCachedFile: Bool) -> Bool {
        UpdateRules.needsDownload(latestBuild: latestBuild, cachedBuild: cachedBuild,
                                  hasCachedFile: hasCachedFile)
    }
    static func mustUpdate(carFw: String?, latestTag: String?) -> Bool {
        UpdateRules.mustUpdate(carFw: carFw, latestTag: latestTag)
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `./tools/test-all.sh 2>&1 | sed -n '/swift host tests/,/mock host tests/p'`
Expected: `test_update: OK` among the suites, `test_gate: OK` still green, no FAIL lines.

- [ ] **Step 5: Commit**

```bash
git add app/AJMiddleCar/UpdateRules.swift app/AJMiddleCar/UpdateClient.swift app/tests/update
git commit -m "test(app): the update decisions move to UpdateRules and gain host tests

UpdateClient references CarTransport, so its pure logic was never
host-testable; the new image-validation rule (decision 6) needs to be.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: The gate learns to work offline — latestTag seeding, download-failure fallback, and a clearable forced phase

**Files:**
- Modify: `app/AJMiddleCar/GateRule.swift` (new pure rule)
- Modify: `app/AJMiddleCar/AppFlow.swift` (offlineFallback, startupCheck download path, carIdentified)
- Modify: `app/tests/gate/main.swift`

**Interfaces:**
- Consumes: `UpdateClient.cachedTag` (existing).
- Produces: `GateRule.offlineLatestTag(cachedTag: String?, hasCachedFile: Bool, cachedBuild: Int?) -> String?` — Tasks 7's cache fallback shares the same "last known release" notion.

Why (three audit findings): an offline launch left `latestTag` nil for the process lifetime, so the forced gate never fired — including against pre-versioning firmware; the gate's download-failure path skipped the offline fallback its two sibling failure paths get; and `carIdentified` could not clear `.updateRequired` even after the car rebooted into the new build (decisions 4a, 16, 4c).

- [ ] **Step 1: Write the failing test**

Append to `app/tests/gate/main.swift`, above the final `if failures == 0` line:

```swift
// Decision 4a: an offline launch seeds the forced gate with the last-known release, so
// mustUpdate can still compare — a cached tag is only trustworthy when the cached image
// it describes actually exists.
check(GateRule.offlineLatestTag(cachedTag: "v1.0+584", hasCachedFile: true, cachedBuild: 584)
        == "v1.0+584", "offline launch seeds the last-known tag")
check(GateRule.offlineLatestTag(cachedTag: "v1.0+584", hasCachedFile: false, cachedBuild: 584)
        == nil, "no file: the tag describes nothing flashable")
check(GateRule.offlineLatestTag(cachedTag: "v1.0+584", hasCachedFile: true, cachedBuild: nil)
        == nil, "no recorded build: not a known version")
check(GateRule.offlineLatestTag(cachedTag: nil, hasCachedFile: true, cachedBuild: 584)
        == nil, "no tag recorded, nothing to seed")
```

- [ ] **Step 2: Run to verify it fails**

Run: `./tools/test-all.sh 2>&1 | sed -n '/swift host tests/,/mock host tests/p'`
Expected: swiftc error — `type 'GateRule' has no member 'offlineLatestTag'`.

- [ ] **Step 3: Implement the rule**

In `app/AJMiddleCar/GateRule.swift`, add inside the enum:

```swift
    /// Decision 4a: the tag the forced gate compares against when GitHub was unreachable —
    /// the last release this phone downloaded, but only while the cached image that tag
    /// describes still exists. Without this an offline launch left `latestTag` nil for the
    /// whole session and the forced gate silently never fired.
    static func offlineLatestTag(cachedTag: String?, hasCachedFile: Bool,
                                 cachedBuild: Int?) -> String? {
        guard canProceedOffline(hasCachedFile: hasCachedFile, cachedBuild: cachedBuild) else {
            return nil
        }
        return cachedTag
    }
```

- [ ] **Step 4: Wire AppFlow**

In `app/AJMiddleCar/AppFlow.swift`:

**(a)** Replace `offlineFallback(or:)`:

```swift
    /// GitHub unreachable or unusable: a cached image is enough to drive — and enough to
    /// force with. Seeding `latestTag` from the cache is what keeps the forced gate armed
    /// offline (decision 4a); without it `mustUpdate` compared against nil and every car,
    /// pre-versioning ones included, drove unforced whenever the launch had no internet.
    private func offlineFallback(or failure: Phase) -> Phase {
        guard GateRule.canProceedOffline(hasCachedFile: UpdateClient.hasCachedFile,
                                         cachedBuild: UpdateClient.cachedBuild) else {
            return failure
        }
        latestTag = GateRule.offlineLatestTag(cachedTag: UpdateClient.cachedTag,
                                              hasCachedFile: UpdateClient.hasCachedFile,
                                              cachedBuild: UpdateClient.cachedBuild)
        return .awaitingCar
    }
```

**(b)** In `startupCheck()`, the download-failure line

```swift
            guard await client.download(rel.assetURL) != nil else { phase = .checkFailed; return }
```

becomes:

```swift
            guard await client.download(rel.assetURL) != nil else {
                // The two failure paths above fall back to the cache; a failed download of a
                // NEWER release must not strand a phone that still holds the previous one.
                phase = offlineFallback(or: .checkFailed)
                return
            }
```

**(c)** In `carIdentified(fw:)`, replace the phase guard:

```swift
        guard phase == .awaitingCar || phase == .ready || phase == .updateRequired else { return }
```

and extend the function's doc comment with one line:

```swift
    /// `.updateRequired` is in the set so the gate can CLEAR: a car that reboots into the
    /// required build must release the forced screen even if FirmwareView's own confirmation
    /// window missed the reconnect (decision 4c).
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `./tools/test-all.sh 2>&1 | sed -n '/swift host tests/,/mock host tests/p'`
Expected: `test_gate: OK` with the new cases, no FAIL lines.

- [ ] **Step 6: Commit**

```bash
git add app/AJMiddleCar/GateRule.swift app/AJMiddleCar/AppFlow.swift app/tests/gate/main.swift
git commit -m "fix(app): the forced gate survives an offline launch and can clear itself

latestTag is seeded from the cached release when GitHub is unreachable, the
gate's download-failure path gets the same offline fallback as its siblings,
and carIdentified may clear .updateRequired once the car runs the required
build (decisions 4a, 16, 4c).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: The cache moves to Application Support, and downloads are validated before they touch it

**Files:**
- Modify: `app/AJMiddleCar/UpdateClient.swift` (cachedBinURL, migration, download)
- Modify: `app/AJMiddleCar/AppFlow.swift` (startupCheck's recordCache moves into the download call)
- Modify: `app/AJMiddleCar/FirmwareView.swift` (download() passes recordAs)

**Interfaces:**
- Consumes: `UpdateRules.isValidImage` (Task 1), `UpdateRules.buildNumber` (Task 1).
- Produces: `UpdateClient.download(_ url: URL, recordAs: (build: Int, tag: String)?) async -> URL?` — validated-or-nothing, records metadata itself; `UpdateClient.migrateCacheIfNeeded()` (static, called once at startup).

Why (three audit findings): `download()` ignored the HTTP status, so a 404/5xx body was cached as firmware and stuck until a strictly newer release; the cache lived in purgeable `Caches/` — the offline gate's lifeline could evaporate; and FirmwareView's download path never called `recordCache`, leaving kBuild/kTag describing a different file (decision 6).

- [ ] **Step 1: Implement in UpdateClient**

**(a)** Replace `cachedBinURL` and add the migration:

```swift
    /// Application Support, not Caches: this file is the offline gate's lifeline (GateRule),
    /// and iOS may purge Caches under storage pressure — evaporating the one thing that lets
    /// a phone in the field proceed without internet. Excluded from backup: it is a cache in
    /// spirit, just not one the OS may unilaterally delete.
    static var cachedBinURL: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory,
                                           in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("firmware-latest.bin")
    }

    /// One-time move of a pre-existing cache from the old Caches location. Called at launch;
    /// a no-op when there is nothing to migrate or the new file already exists.
    static func migrateCacheIfNeeded() {
        let old = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("firmware-latest.bin")
        let new = cachedBinURL
        guard FileManager.default.fileExists(atPath: old.path),
              !FileManager.default.fileExists(atPath: new.path) else { return }
        try? FileManager.default.moveItem(at: old, to: new)
        excludeFromBackup(new)
    }

    private static func excludeFromBackup(_ url: URL) {
        var u = url
        var rv = URLResourceValues()
        rv.isExcludedFromBackup = true
        try? u.setResourceValues(rv)
    }
```

**(b)** Replace `download(_:)` with the validating, recording version:

```swift
    /// Decision 6: nothing enters the firmware cache unvalidated. URLSession does not throw
    /// on 404/403/5xx, so an error page used to be cached as firmware and recorded as the
    /// latest build — poisoning the cache until a strictly newer release existed. On any
    /// failure the existing cache and its metadata are left untouched.
    func download(_ url: URL, recordAs: (build: Int, tag: String)? = nil) async -> URL? {
        downloadProgress = 0
        let session = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
        defer { session.finishTasksAndInvalidate() }
        do {
            let (tmp, resp) = try await session.download(from: url)
            guard (resp as? HTTPURLResponse)?.statusCode == 200 else { return nil }
            let size = (try? FileManager.default.attributesOfItem(atPath: tmp.path)[.size]
                            as? Int) ?? 0
            let firstByte = FileHandle(forReadingAtPath: tmp.path)
                .flatMap { defer { try? $0.close() }; return try? $0.read(upToCount: 1)?.first }
            guard UpdateRules.isValidImage(firstByte: firstByte, size: size) else { return nil }
            let dest = UpdateClient.cachedBinURL
            try? FileManager.default.removeItem(at: dest)
            try FileManager.default.moveItem(at: tmp, to: dest)
            UpdateClient.excludeFromBackup(dest)
            if let r = recordAs { UpdateClient.recordCache(build: r.build, tag: r.tag) }
            return dest
        } catch { return nil }
    }
```

- [ ] **Step 2: Move the call sites onto the new signature**

In `app/AJMiddleCar/AppFlow.swift`:
- In `startupCheck()`, replace the download block's body:

```swift
            phase = .downloading
            let t0 = Date()
            let recordAs = latestBuild.map { (build: $0, tag: rel.tag) }
            guard await client.download(rel.assetURL, recordAs: recordAs) != nil else {
                // The two failure paths above fall back to the cache; a failed download of a
                // NEWER release must not strand a phone that still holds the previous one.
                phase = offlineFallback(or: .checkFailed)
                return
            }
            await UpdateClient.holdAtLeast(UpdateClient.downloadMinDisplay, since: t0)
```

(and delete the now-redundant `if let b = latestBuild { UpdateClient.recordCache(...) }` line — recording moved inside `download`).

- At the top of `startupCheck()`, before `phase = .checkInternet`, add:

```swift
        UpdateClient.migrateCacheIfNeeded()
```

In `app/AJMiddleCar/FirmwareView.swift`, `download()`'s call becomes:

```swift
        let recordAs = UpdateRules.buildNumber(r.tag).map { (build: $0, tag: r.tag) }
        if let url = await client.download(r.assetURL, recordAs: recordAs) {
```

- [ ] **Step 3: Verify**

Run: `./tools/test-all.sh 2>&1 | tail -3` — expected `== all green ==` (the changed code has no host suite; Task 9's xcodebuild is the compile gate, but run a quick syntax pass now):

```bash
cd app && xcodegen generate >/dev/null && xcodebuild -quiet build -scheme AJMiddleCar \
  -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-ota && cd ..
```

Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add app/AJMiddleCar/UpdateClient.swift app/AJMiddleCar/AppFlow.swift app/AJMiddleCar/FirmwareView.swift
git commit -m "fix(app): downloads are validated before the cache, and the cache stops being purgeable

HTTP status, image magic and the 4 KB floor are checked before anything
touches the firmware cache; metadata is recorded by the download itself so
kBuild/kTag can never describe a different file; the cache moves from
Caches/ to Application Support so iOS cannot purge the offline gate's
lifeline (decision 6).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: CarLink — a three-state radio status, and the rollback flag

**Files:**
- Modify: `app/AJMiddleCar/CarLink.swift` (RadioStatus, rollback, fetchRadio, sessionOpened)
- Modify: `app/AJMiddleCar/GalleryView.swift` (preview call site)
- Modify: `app/AJMiddleCar/FirmwareView.swift` (radioLine switches on the new type — minimal, Task 5 finishes the copy)

**Interfaces:**
- Consumes: `/status`'s `radio` object and (once the core plan lands) its top-level `rollback` bool.
- Produces: `CarLink.RadioStatus` (`.known(fw: String, ok: Bool)` | `.unavailable`), `@Published var radio: RadioStatus?`, `@Published var rollback: Bool?` — Tasks 5 and 6 consume both. `CarLink.preview(_:fw:radio:)`'s `radio` parameter becomes `RadioStatus?`.

Why (audit findings, decisions 1, 17): a missing or malformed `ok` defaulted to healthy; when all four `/status` fetches failed the radio line disappeared — silence indistinguishable from health on the one screen built to surface a mismatch; and nothing anywhere could see a bootloader rollback.

**Design choice — the rollback read path:** the flag rides the SAME `/status` fetch `fetchRadio()` already makes. It fires on `.sessionOpened` — exactly the moment a car returns from an OTA reboot — retries with backoff, and `FirmwareView` already observes `CarLink`. `rollback` is reset to `nil` on every `.sessionOpened` BEFORE the fetch, so a pre-reboot value can never masquerade as the new boot's answer; it is parsed independently of the radio object so an old firmware (no key) simply leaves it `nil`.

- [ ] **Step 1: Implement in CarLink**

**(a)** Replace the `Radio` struct and the two published properties:

```swift
    /// The radio co-processor's firmware, from `/status`. Three states: unknown (still
    /// fetching), known, and unavailable — every `/status` attempt failed. Unavailable is a
    /// state of its own because hiding the line made silence indistinguishable from health
    /// on the one screen that exists to surface a mismatch.
    enum RadioStatus: Equatable {
        case known(fw: String, ok: Bool)
        case unavailable
    }

    @Published private(set) var radio: RadioStatus?
    /// The car's report that the bootloader reverted the last update (decision 1): `/status`
    /// `rollback`, nil until a fresh post-adoption fetch answers (or on firmware that
    /// predates the key). Reset on every adoption so a pre-reboot value cannot leak forward.
    @Published private(set) var rollback: Bool?
```

**(b)** In `handle(_:)`'s `.sessionOpened` own-car branch, before `fetchRadio()`:

```swift
                rollback = nil
```

**(c)** Replace `fetchRadio()`:

```swift
    /// `/status` is one GET against a single-request server that is busy with the geometry
    /// prefetch fired in the same instant — one miss must not hide a radio mismatch for the
    /// whole session. Four tries, backing off; if every try fails the status becomes
    /// `.unavailable` rather than silence. `rollback` is parsed independently of the radio
    /// object: it must survive a malformed radio block, and its absence (older firmware)
    /// stays nil.
    private func fetchRadio() {
        radioFetch?.cancel()
        radioFetch = Task { [weak self, transport] in
            for delay: Double in [0, 1, 2, 4] {
                if delay > 0 { try? await Task.sleep(for: .seconds(delay)) }
                if Task.isCancelled { return }
                if let data = try? await transport.get("/status", timeout: 2),
                   let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                    if let rb = j["rollback"] as? Bool { self?.rollback = rb }
                    if let r = j["radio"] as? [String: Any],
                       let fw = r[CarContract.fwField] as? String {
                        // Missing/malformed `ok` is NOT health (decision 17): unknown means
                        // the one flag this line exists for could not be read.
                        self?.radio = .known(fw: fw, ok: r["ok"] as? Bool ?? false)
                        return
                    }
                }
            }
            if self?.radio == nil { self?.radio = .unavailable }
        }
    }
```

**(d)** `preview`'s signature: `radio: Radio? = nil` becomes `radio: RadioStatus? = nil` (body unchanged).

- [ ] **Step 2: Update the two call sites that name the old type**

- `app/AJMiddleCar/GalleryView.swift`, in `mockLink`: `radio: CarLink.Radio(fw: "3.0.6", ok: true)` becomes `radio: .known(fw: "3.0.6", ok: true)`.
- `app/AJMiddleCar/FirmwareView.swift`, `radioLine` compiles against the new type (final copy lands in Task 5):

```swift
    @ViewBuilder private var radioLine: some View {
        switch link.radio {
        case .known(let fw, true):
            Text(L.fwRadio(fw)).font(.system(size: 12)).foregroundStyle(p.muted)
        case .known(let fw, false):
            Text(L.fwRadioMismatch(fw)).font(.system(size: 12)).foregroundStyle(p.warn)
                .fixedSize(horizontal: false, vertical: true).frame(maxWidth: 260, alignment: .leading)
        case .unavailable:
            Text(L.fwRadioUnknown).font(.system(size: 12)).foregroundStyle(p.muted)
        case nil:
            EmptyView()
        }
    }
```

- [ ] **Step 3: Add the placeholder string (final copy in Task 5)**

`app/AJMiddleCar/L.swift`, next to the other fw accessors:

```swift
    static var fwRadioUnknown: String { s("fw.radioUnknown") }
```

`app/AJMiddleCar/Resources/ru.lproj/Localizable.strings`, next to `fw.radio`:

```
"fw.radioUnknown"    = "Радиомодуль: нет данных";
```

- [ ] **Step 4: Verify**

Run: `./tools/test-all.sh 2>&1 | tail -3` (green), then the compile:

```bash
cd app && xcodegen generate >/dev/null && xcodebuild -quiet build -scheme AJMiddleCar \
  -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-ota && cd ..
```

Expected: build succeeds — the compiler is the sweep for missed `CarLink.Radio` references.

- [ ] **Step 5: Commit**

```bash
git add app/AJMiddleCar/CarLink.swift app/AJMiddleCar/GalleryView.swift \
        app/AJMiddleCar/FirmwareView.swift app/AJMiddleCar/L.swift \
        app/AJMiddleCar/Resources/ru.lproj/Localizable.strings
git commit -m "fix(app): the radio line stops equating silence with health, and rollback becomes readable

RadioStatus grows an .unavailable state (all fetches failed), a missing ok
flag reads as not-ok, and /status's new rollback key is parsed on the same
fetch — reset on every adoption so a pre-reboot value cannot leak forward
(decisions 1, 17).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: The mismatch copy loses its dead-end, and the gallery learns to render every radio state

**Files:**
- Modify: `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings` (fw.radioMismatch)
- Modify: `app/AJMiddleCar/GalleryView.swift` (three new frames)

**Interfaces:** consumes Task 4's `RadioStatus`; produces nothing new.

Why (audit finding, decision 18): the mismatch warning told the user to "см. firmware/c6/README.md" — a repo path no phone user can follow — and no harness (mock or gallery) could ever render the string, so nobody had seen it.

- [ ] **Step 1: Rewrite the string**

In `Localizable.strings`, replace the `fw.radioMismatch` line with:

```
"fw.radioMismatch"   = "⚠ Радиомодуль: %@ — не та версия, которую ждёт прошивка. Нужна перепрошивка C6 на стенде";
```

- [ ] **Step 2: Add the gallery frames**

In `GalleryView.swift`'s `makeFrames`, after the `("Firmware forced", …)` entry, add:

```swift
            ("Firmware radio mismatch",  AnyView(NavigationStack { FirmwareView(
                palette: p, debugPhase: .upToDate,
                link: CarLink.preview(.live(Telemetry()), fw: "v1.0+584",
                                      radio: .known(fw: "2.11.7", ok: false))) })),
            ("Firmware radio unknown",   AnyView(NavigationStack { FirmwareView(
                palette: p, debugPhase: .upToDate,
                link: CarLink.preview(.live(Telemetry()), fw: "v1.0+584",
                                      radio: .unavailable)) })),
```

(The third new frame — the `.flashed` phase — arrives with Task 6, which creates the phase.)

- [ ] **Step 3: Verify**

Run the compile:

```bash
cd app && xcodegen generate >/dev/null && xcodebuild -quiet build -scheme AJMiddleCar \
  -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-ota && cd ..
```

Expected: build succeeds. Manual note (not automatable): `-gallery` now shows both radio states.

- [ ] **Step 4: Commit**

```bash
git add app/AJMiddleCar/Resources/ru.lproj/Localizable.strings app/AJMiddleCar/GalleryView.swift
git commit -m "fix(app): the radio-mismatch warning is actionable and finally renderable

The copy stops pointing at a repo README no phone can open, and the debug
gallery gains frames for the mismatch and no-data radio states so the
strings are reachable in rehearsal (decision 18).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: flash() — an acknowledged upload is committed, rollback is failure, and forced mode gets an escape

**Files:**
- Modify: `app/AJMiddleCar/FirmwareCarView.swift` (FwPhase gains `.flashed`)
- Modify: `app/AJMiddleCar/FirmwareView.swift` (flash(), stateBlock, skip button)
- Modify: `app/AJMiddleCar/L.swift`, `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings`
- Modify: `app/AJMiddleCar/GalleryView.swift` (two frames)

**Interfaces:**
- Consumes: `CarLink.rollback` (Task 4).
- Produces: `FwPhase.flashed`; `FirmwareView` state vars `flashAttempted`, `rolledBack` — Task 8's error surfacing shares the `.failed` sub-line logic.

Why (audit findings, decision 5): the 25 s window reported a committed flash as failure when iOS hopped to home WiFi; a boots-but-rolls-back release read as success (the bounce fired) and looped the forced gate forever with no escape.

- [ ] **Step 1: The phase**

In `FirmwareCarView.swift`:
- `enum FwPhase` gains `flashed` after `rebooting`:

```swift
enum FwPhase { case checking, upToDate, available, downloading, downloaded, uploading, rebooting, flashed, done, failed }
```

- In `mode`: add `.flashed` to the `.upToDate` line → `case .upToDate, .flashed: return .deco`.
- In `chipIcon`: add `.flashed` to the checkmark line → `case .upToDate, .done, .flashed: return "checkmark"`.

- [ ] **Step 2: Strings**

`L.swift`, next to the fw block:

```swift
    static var fwFlashedTitle: String { s("fw.flashedTitle") }
    static var fwFlashedSub: String { s("fw.flashedSub") }
    static var fwRollbackSub: String { s("fw.rollbackSub") }
    static var fwSkip: String { s("fw.skip") }
```

`Localizable.strings`, after `fw.doneSub`:

```
"fw.flashedTitle"   = "Прошито";
"fw.flashedSub"     = "Переподключись к машинке, чтобы проверить версию";
"fw.rollbackSub"    = "Машинка вернулась на прежнюю версию — прошивка откатилась. Повторная заливка того же образа, скорее всего, повторит откат";
"fw.skip"           = "Продолжить без обновления";
```

- [ ] **Step 3: flash() and the state block**

In `FirmwareView.swift`:

**(a)** Add two state vars next to `phase`:

```swift
    @State private var flashAttempted = false
    @State private var rolledBack = false
```

**(b)** Replace `flash()`:

```swift
    private func flash() async {
        guard let url = binURL else { return }
        flashAttempted = true
        rolledBack = false
        phase = .uploading
        guard await client.upload(url) else { phase = .failed; return }
        // The car acknowledged the upload: the image is written, set as boot target, and the
        // reboot is unconditional. From here the flash is COMMITTED — the question is only
        // whether this phone gets to watch the confirmation.
        phase = .rebooting
        let oldFw = link.fw
        var sawOffline = false
        let deadline = Date.now.addingTimeInterval(25)
        while Date.now < deadline {
            try? await Task.sleep(nanoseconds: 500_000_000)
            if let nf = link.fw, oldFw != nil, nf != oldFw { phase = .done; return }
            if !link.isLive { sawOffline = true }
            else if sawOffline {
                // The car came back — with the SAME firmware. That is a bootloader rollback
                // (or a flash that never took), never success: calling it done is what looped
                // the forced gate forever against a rolling-back release.
                rolledBack = true
                phase = .failed
                return
            }
        }
        // Deadline without a reconnect: iOS routinely hops to another known network when the
        // softAP vanishes and does not come back on its own. The flash is committed either
        // way — report that, not failure.
        phase = .flashed
    }
```

(Task 8 rewires the `upload` call's return type; at this task it is still `Bool`.)

**(c)** In `stateBlock`, add the `.flashed` case after `.rebooting`:

```swift
            case .flashed:
                title(L.fwFlashedTitle); sub(L.fwFlashedSub)
                if forced { skipButton }
```

**(d)** Replace the `.failed` case:

```swift
            case .failed:
                title(L.fwFailTitle)
                sub(rolledBack && link.rollback != false ? L.fwRollbackSub : L.fwFailSub)
                fwButton(L.fwRetry, prominent: true) { Task { await check() } }
                if forced && flashAttempted { skipButton }
```

**(e)** Add the escape button next to `fwButton`:

```swift
    /// Decision 5's escape hatch: a forced gate that failed (or could not confirm) a flash
    /// must not be a locked room. Ghost styling — continuing unupdated is the fallback, not
    /// the offer.
    private var skipButton: some View {
        fwButton(L.fwSkip, prominent: false) { onDone?() }
    }
```

**(f)** In the `.task` debug seeding, after `phase = dp`, add `flashAttempted = true` (so the gallery's forced-failed frame shows the escape):

```swift
            if let dp = debugPhase { phase = dp; flashAttempted = true; return }
```

- [ ] **Step 4: Gallery frames**

In `GalleryView.swift`, after the radio frames from Task 5:

```swift
            ("Firmware flashed",         fw(.flashed)),
            ("Firmware failed forced",   fw(.failed, forced: true)),
```

- [ ] **Step 5: Verify**

Compile:

```bash
cd app && xcodegen generate >/dev/null && xcodebuild -quiet build -scheme AJMiddleCar \
  -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-ota && cd ..
```

Expected: success (the exhaustive `switch phase` in both views is the sweep for the new case). `./tools/test-all.sh` stays green.

- [ ] **Step 6: Commit**

```bash
git add app/AJMiddleCar/FirmwareCarView.swift app/AJMiddleCar/FirmwareView.swift \
        app/AJMiddleCar/L.swift app/AJMiddleCar/Resources/ru.lproj/Localizable.strings \
        app/AJMiddleCar/GalleryView.swift
git commit -m "fix(app): a committed flash is never 'failed', rollback is named, forced mode can exit

An acknowledged upload past the 25 s window becomes .flashed (the image IS
written — only the confirmation was missed); a same-fw reconnect is the
rollback it is, with its own message instead of a success that looped the
forced gate forever; and a failed flash in forced mode offers «продолжить
без обновления» (decision 5).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: check() falls back to the cached image when GitHub is unreachable

**Files:**
- Modify: `app/AJMiddleCar/FirmwareView.swift` (check, download, .available copy)
- Modify: `app/AJMiddleCar/L.swift`, `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings`

**Interfaces:**
- Consumes: `UpdateRules.buildNumber` (Task 1), `UpdateClient.cachedBinURL/cachedBuild/cachedTag/hasCachedFile` (Task 3's location).
- Produces: `FirmwareView` state var `offlineCache` — internal only.

Why (audit's top app finding, decision 4b): the forced screen always made two fresh GitHub round-trips over the car's internet-less AP while the image the gate downloaded for exactly this moment sat unread on disk — a WiFi-only iPad dead-ended forever; an iPhone waited out the ~40 s WiFi demotion and re-downloaded the same bytes over cellular.

- [ ] **Step 1: The string**

`L.swift`:

```swift
    static var fwFromCache: String { s("fw.fromCache") }
```

`Localizable.strings`, after `fw.transition`:

```
"fw.fromCache"      = "из кэша — GitHub недоступен";
```

- [ ] **Step 2: Implement**

In `FirmwareView.swift`:

**(a)** Add next to the other state vars:

```swift
    @State private var offlineCache = false
```

**(b)** Replace `check()`:

```swift
    private func check() async {
        phase = .checking
        offlineCache = false
        if let r = await client.latestRelease() {
            release = r
            phase = UpdateClient.isUpdateAvailable(running: link.fw, latest: r.tag)
                ? .available : .upToDate
            return
        }
        release = nil
        // GitHub unreachable — the normal state on the car's internet-less AP. The launch
        // gate already downloaded the release it knew about; a cached image NEWER than the
        // car is flashable without any network (decision 4b). The car's build must be known:
        // with no car identity there is nothing to compare against.
        if UpdateClient.hasCachedFile,
           let cached = UpdateClient.cachedBuild,
           let car = UpdateRules.buildNumber(link.fw),
           cached > car {
            offlineCache = true
            binURL = UpdateClient.cachedBinURL
            phase = .available
            return
        }
        phase = .failed
    }
```

**(c)** In `stateBlock`'s `.available` case, the sub-line learns the cache origin:

```swift
            case .available:
                title(forced ? L.gateUpdateTitle : L.fwAvailable)
                let target = offlineCache ? (UpdateClient.cachedTag ?? "—") : (release?.tag ?? "—")
                sub(forced ? L.gateUpdateSub
                           : L.fwTransition(current, target)
                             + (offlineCache ? " · " + L.fwFromCache : ""))
                fwButton(L.fwUpdate, prominent: true) { Task { await download() } }
```

**(d)** `download()` short-circuits for the cache:

```swift
    private func download() async {
        if offlineCache {
            // The image is already on disk, validated at download time (decision 6); the
            // download phase would be a fetch of what we are standing on.
            phase = .downloaded
            return
        }
        guard let r = release else { return }
        phase = .downloading
        let t0 = Date()
        let recordAs = UpdateRules.buildNumber(r.tag).map { (build: $0, tag: r.tag) }
        if let url = await client.download(r.assetURL, recordAs: recordAs) {
            binURL = url
            await UpdateClient.holdAtLeast(UpdateClient.downloadMinDisplay, since: t0)
            phase = .downloaded
        } else { phase = .failed }
    }
```

**(e)** The `.downloading` caption's `release?.tag ?? ""` stays as-is (the offline path never enters `.downloading`), and `.uploading`'s sub-line becomes cache-aware:

```swift
                sub("\(offlineCache ? (UpdateClient.cachedTag ?? "") : (release?.tag ?? "")) · \(Int(client.uploadProgress * 100))%")
```

- [ ] **Step 3: Verify**

Compile (as in prior tasks) + `./tools/test-all.sh` — both green. Manual note: against the mock with WiFi off on the Mac this is not exercisable; the rehearsal is on-device (forced gate at the car with no cellular).

- [ ] **Step 4: Commit**

```bash
git add app/AJMiddleCar/FirmwareView.swift app/AJMiddleCar/L.swift \
        app/AJMiddleCar/Resources/ru.lproj/Localizable.strings
git commit -m "fix(app): the update screen flashes the cached image when GitHub is out of reach

check() falls back to the gate's own downloaded release when latestRelease
fails and the cache is newer than the car — the forced screen stops
dead-ending on the car's internet-less AP with a valid image on disk
(decision 4b).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: Upload — cancellable, 45 s, and the car's reason surfaces

**Files:**
- Modify: `app/AJMiddleCar/CarTransport.swift` (HTTPRequest gains cancellation)
- Modify: `app/AJMiddleCar/UpdateClient.swift` (upload → UploadOutcome)
- Modify: `app/AJMiddleCar/FirmwareView.swift` (cancel button, failure reason)
- Modify: `app/AJMiddleCar/L.swift`, `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings`

**Interfaces:**
- Consumes: `CarError.http(status:body:)` (existing — the envelope is already in the error).
- Produces: `UpdateClient.UploadOutcome` (`.ok | .cancelled | .failed(String)`); `HTTPRequest.perform` becomes cancellation-aware (every transport HTTP call inherits it — `get`/`post` behavior for non-cancelled callers is unchanged).

Why (audit findings, decisions 14, 15): every /ota failure rendered as the generic «проверь связь», though the car ships a JSON envelope naming the reason; and a mid-upload path loss froze the (forced) screen for up to 180 s with no way out.

- [ ] **Step 1: HTTPRequest cancellation**

In `CarTransport.swift`'s `HTTPRequest`:

**(a)** `done` becomes attachable (the OneShot pattern this file already uses): replace the `private let done:` property with

```swift
    private var completion: ((Result<(status: Int, body: Data), Error>) -> Void)?
    private var pendingResult: Result<(status: Int, body: Data), Error>?
```

remove `done` from `init` (drop its parameter). Delivery happens in exactly one place — `finish(_:)`'s `done(result)` line (verified against the current file; `finish` is already idempotent via `finished`, already cancels `conn`, and already releases `selfRetain`). Replace that one line with:

```swift
        if let completion {
            completion(result)
        } else {
            pendingResult = result
        }
```

**(b)** Add, on `queue` semantics:

```swift
    /// Attach the continuation's resume. If the request already finished (external cancel
    /// racing start), deliver the stored result immediately — same idempotence contract as
    /// OneShot above.
    func attach(_ c: @escaping (Result<(status: Int, body: Data), Error>) -> Void) {
        queue.async {
            if let r = self.pendingResult {
                self.pendingResult = nil
                c(r)
            } else {
                self.completion = c
            }
        }
    }

    /// External (task) cancellation: finish with CancellationError. `finish` is idempotent
    /// and cancels the connection, so a cancel that races completion is a no-op.
    func cancelExternally() {
        queue.async { self.finish(.failure(CancellationError())) }
    }
```

**(c)** Replace `perform`:

```swift
    static func perform(method: String,
                        path: String,
                        body: Data?,
                        contentType: String?,
                        timeout: TimeInterval,
                        progress: (@Sendable (Double) -> Void)?) async throws -> (status: Int, body: Data) {
        let req = HTTPRequest(method: method, path: path, body: body, contentType: contentType,
                              timeout: timeout, progress: progress)
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { cont in
                req.attach { cont.resume(with: $0) }
                req.start()
            }
        } onCancel: {
            req.cancelExternally()
        }
    }
```

(`start()` stops taking/holding a `done` closure; adapt `init` and any `self.done` references — the diff must leave every OTHER delivery path flowing through `finish` exactly as today.)

- [ ] **Step 2: UploadOutcome**

In `UpdateClient.swift`, replace `upload(_:)`:

```swift
    enum UploadOutcome: Equatable { case ok, cancelled, failed(String) }

    /// Uploads over `CarTransport`, WiFi-pinned like every request to the car. 45 s, not
    /// 180: the car itself abandons a stalled upload after ~30 s, so anything beyond that
    /// is the phone watching a corpse (decision 15). The car's error envelope is surfaced,
    /// not swallowed (decision 14).
    func upload(_ binURL: URL) async -> UploadOutcome {
        guard let data = try? Data(contentsOf: binURL) else { return .failed("нет файла прошивки") }
        do {
            _ = try await CarTransport.shared.post("/ota", body: data,
                                                   contentType: "application/octet-stream",
                                                   timeout: 45) { [weak self] p in
                Task { @MainActor in self?.uploadProgress = p }
            }
            return .ok
        } catch is CancellationError {
            return .cancelled
        } catch let CarError.http(status, body) {
            let msg = ((try? JSONSerialization.jsonObject(with: body)) as? [String: Any])?["error"] as? String
            return .failed(msg ?? "HTTP \(status)")
        } catch let e as CarError {
            return .failed(e.logDescription)
        } catch {
            return .failed(String(describing: error))
        }
    }
```

- [ ] **Step 3: FirmwareView — the cancel button and the reason**

**(a)** State vars:

```swift
    @State private var uploadTask: Task<UpdateClient.UploadOutcome, Never>?
    @State private var failReason: String?
```

**(b)** In `flash()`, the upload block (replacing Task 6's `guard await client.upload(url)` line):

```swift
        failReason = nil
        let task = Task { await client.upload(url) }
        uploadTask = task
        let outcome = await task.value
        uploadTask = nil
        switch outcome {
        case .cancelled:
            // Back to flash-ready, not to failure: the user changed their mind, nothing broke.
            phase = .downloaded
            return
        case .failed(let reason):
            failReason = reason
            phase = .failed
            return
        case .ok:
            break
        }
```

**(c)** `.uploading` gains the cancel:

```swift
            case .uploading:
                title(L.fwUploadTitle)
                sub("\(offlineCache ? (UpdateClient.cachedTag ?? "") : (release?.tag ?? "")) · \(Int(client.uploadProgress * 100))%")
                ProgressView(value: client.uploadProgress).tint(p.accent).frame(width: 160)
                fwButton(L.fwCancel, prominent: false) { uploadTask?.cancel() }
```

**(d)** `.failed`'s sub-line (extending Task 6's version) becomes:

```swift
                sub(rolledBack && link.rollback != false ? L.fwRollbackSub
                    : failReason.map { L.fwFailReason($0) } ?? L.fwFailSub)
```

**(e)** Strings — `L.swift`:

```swift
    static var fwCancel: String { s("fw.cancel") }
    static func fwFailReason(_ r: String) -> String { s("fw.failReason", r) }
```

`Localizable.strings`:

```
"fw.cancel"         = "Отмена";
"fw.failReason"     = "Машинка ответила: %@";
```

- [ ] **Step 4: Verify**

Compile + `./tools/test-all.sh` green. Live check against the mock (start it, upload a <4 KB file through the app is not scriptable — instead assert the transport layer): the change is compile-verified here; the mock-side error text path is exercised by the core plan's conformance. Manual note recorded.

- [ ] **Step 5: Commit**

```bash
git add app/AJMiddleCar/CarTransport.swift app/AJMiddleCar/UpdateClient.swift \
        app/AJMiddleCar/FirmwareView.swift app/AJMiddleCar/L.swift \
        app/AJMiddleCar/Resources/ru.lproj/Localizable.strings
git commit -m "fix(app): uploads are cancellable, bounded at 45 s, and failures name the car's reason

HTTPRequest honors task cancellation (the whole transport inherits it), the
/ota deadline drops from 180 s to 45 s with a visible cancel, and the car's
error envelope reaches the screen instead of the generic 'check the link'
(decisions 14, 15).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: Full verification

**Files:** none new.

- [ ] **Step 1: Full host suite**

Run: `./tools/test-all.sh`
Expected: `== all green ==` with `test_update: OK` and the extended `test_gate: OK` among the swift suites.

- [ ] **Step 2: App build from a regenerated project**

```bash
cd app && xcodegen generate && \
xcodebuild -quiet build -scheme AJMiddleCar \
  -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-ota; cd ..
grep -c 'UpdateRules.swift' app/AJMiddleCar.xcodeproj/project.pbxproj
```

Expected: build succeeds; the grep finds the new file in the generated project (≥1).

- [ ] **Step 3: Tree check**

`git status --short` — clean (the xcodeproj is gitignored). If a tracked file changed, stop and report.

---

## Deliberately out of scope

- The `/status` `rollback` key's SERVER side (firmware + mock + protocol.md) — the core plan owns it; this plan's parsing tolerates its absence.
- First-run-offline UX copy and the monotonic-build-number hazard — excluded by the decisions spec.
- A full end-to-end forced-gate rehearsal — needs a device at the car (no cellular), recorded as a manual checklist item.

## Self-review notes (author)

- Decision coverage: 4a→T2, 4b→T7, 4c→T2, 5→T6, 6→T3 (+T1's rule), 14→T8, 15→T8, 16→T2, 17→T4, 18→T5; rollback consumption (1, app side)→T4+T6.
- Type consistency: `UpdateRules.*` names identical across T1/T3/T7; `RadioStatus` spelled the same in T4/T5; `UploadOutcome` in T8 matches T6's call-site rewrite note; `skipButton`/`flashAttempted`/`rolledBack` consistent in T6/T8; `download(_:recordAs:)` signature identical in T3 and T7.
- Ordering: T1 before T2/T3/T7 (rules), T4 before T5/T6 (RadioStatus/rollback), T6 before T7/T8 (flash()/state vars they extend), T8 last before verification (touches flash() again).
- Known compile-sweep reliance: T4's Radio→RadioStatus and T6's new FwPhase case are exhaustive-switch-driven; the per-task xcodebuild is mandatory, not optional.
