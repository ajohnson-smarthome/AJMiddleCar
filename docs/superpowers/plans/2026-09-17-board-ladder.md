# План: лестница плат — одна стадия на адаптер и машинку

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** свести два цикла запуска (`dongleGate()` / `carGate()`) к одному: массив плат `[адаптер, машинка]` (или `[машинка]` без адаптера) и одна стадия `reach → /version → выпуск → правило → шаг`, с одним семейством экранов и стражами после гейта, перезапускающими лестницу.

**Architecture:** две чистые единицы с хост-тестами — `StageRule.decide` (решение одного опроса) и `CarReach` (сетевой автомат адаптера с бюджетом). `AppFlow` становится проводкой: фазы `Phase.stage(device, GateStep)`, паузы, парковка на обновлении, раннер `runLadder`/`restart`/`updateFinished`. `ConnectView` получает одну ситуацию `.stage`; `WrongCarView` удаляется; стражи `Link` перезапускают лестницу с нужной ступени.

**Tech Stack:** Swift/SwiftUI (XcodeGen), хост-тесты `swiftc` без XCTest, симулятор iPhone 17 против `tools/mock_car`.

**Spec:** `docs/superpowers/specs/2026-09-17-board-ladder-design.md` (читать вместе с планом; при расхождении план уступает спеке).

## Global Constraints

- Проза в чате и документах — по-русски; код, комментарии и сообщения коммитов — по-английски; заголовки задач — «### Задача N:».
- Не трогаем прошивки, контракт, мок, конформанс, `docs/protocol.md`, `FirmwareFlow`/`FirmwareView`/`UpdateRules`/`VersionRule`/`DongleLink.next`/`LinkRule`/`CarLink` (кроме вызова `link.retryAfterWrongCar()` при возврате в `.awaitingCar`) и транспорты.
- Тайм-ауты и окна перезагрузки — данные платы, не унифицируем (`/version` адаптера 3 с, машинки 2 с; окно 30/60 с; опрос 1,5 с).
- Никогда не редактировать генерируемые файлы (`app/AJMiddleCar/Generated/*`).
- Тексты экранов — сегодняшние, слово в слово; единственный новый — «Машинка отвечает с ошибкой».
- Сборка приложения: `cd app && xcodegen generate && xcodebuild build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle -quiet`
- Хост-тесты: `tools/test-all.sh` (он же прогоняет `swiftc` по каждому `app/tests/<name>/`).
- Симулятор против мока: `nohup tools/mock_car/.venv/bin/python -u tools/mock_car/mock_car.py >/tmp/mock.log 2>&1 &`, установить `/tmp/ddata-middle/Build/Products/Debug-iphonesimulator/AJMiddleCar.app`, `xcrun simctl launch booted com.adamjohnson.ajmiddlecar`, скриншот `xcrun simctl io booted screenshot` (повернуть `sips -r -90`); `MOCK_DEVICE=esp32-car` и `MOCK_NO_VERSION=1` — как в спеке §6.
- Каждый коммит заканчивается трейлером:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ
  ```

---

### Задача 1: `StageRule` — типы лестницы и чистое решение одного опроса

**Files:**
- Create: `app/AJMiddleCar/StageRule.swift`
- Create: `app/tests/stagerule/main.swift`
- Create: `app/tests/stagerule/sources`

**Interfaces:**
- Consumes: `VersionReply`, `DeviceVersion`, `VersionStep`, `VersionRule.step` (Identity.swift, VersionRule.swift); `RollbackChoice` (VersionRule.swift); `UpdateRules.Device` (UpdateRules.swift).
- Produces: `enum GateStep: Equatable`; `enum Reach: Equatable`; `struct Board` (с `struct Identity`); `enum StageRule` с `enum Verdict` и `static func decide(reach:version:board:latestTag:rollback:) -> Verdict`.

- [ ] **Шаг 1: тест `app/tests/stagerule/sources`**

```
StageRule.swift
Identity.swift
VersionRule.swift
UpdateRules.swift
CarError.swift
```

- [ ] **Шаг 2: провальный тест `app/tests/stagerule/main.swift`**

```swift
// Host test for StageRule.decide — one poll of one board's stage: what to show, whether to
// fetch the release first, advance, or fall back. Pure over reach + /version reply + tag.
// Run with swiftc; no XCTest, no simulator.
import Foundation
import Network

var failures = 0
func check(_ ok: Bool, _ what: String) { if !ok { print("FAIL: \(what)"); failures += 1 } }

let latest = "v1.0+200"
let dongle = Board.Identity(device: .dongle, expectedDevice: "ajdongle", proto: 1, silentStep: .absent)
let car    = Board.Identity(device: .car,    expectedDevice: "ajmiddlecar", proto: 2, silentStep: .seeking)

func doc(_ device: String, fw: String, proto: Int, rolledBack: Bool = false) -> VersionReply {
    .version(DeviceVersion(device: device, fw: fw, build: UpdateRules.buildNumber(fw) ?? -1,
                           proto: proto, rolled_back: rolledBack))
}
func decide(_ reach: Reach, _ version: VersionReply?, _ b: Board.Identity,
            tag: String? = latest, rollback: RollbackChoice = .unanswered) -> StageRule.Verdict {
    StageRule.decide(reach: reach, version: version, board: b, latestTag: tag, rollback: rollback)
}

// reach speaks first, before /version is even read.
check(decide(.lost, nil, car) == .lost, "reach lost: fall back")
check(decide(.hold(.searching), nil, car) == .show(.searching), "reach hold: show that step")
check(decide(.hold(.joinFailed), nil, car) == .show(.joinFailed), "reach hold join-failed")

// A board that answered (a document or 404) with no tag yet needs the release first.
check(decide(.reached, .absent, dongle, tag: nil) == .needRelease, "404 + no tag: fetch release")
check(decide(.reached, doc("ajdongle", fw: "v1.0+100", proto: 1), dongle, tag: nil) == .needRelease,
      "document + no tag: fetch release")
// Silence is not "answered": no release fetch, straight to the board's silent step.
check(decide(.reached, .silent, dongle, tag: nil) == .show(.absent), "silence + no tag: adapter absent, no fetch")
check(decide(.reached, .silent, car, tag: nil) == .show(.seeking), "silence + no tag: car seeking, no fetch")

// The rule's verdicts map to steps, per board's silent step.
check(decide(.reached, .silent, dongle) == .show(.absent), "silence: adapter .absent")
check(decide(.reached, .silent, car) == .show(.seeking), "silence: car .seeking")
check(decide(.reached, .faulty, car) == .show(.fault), "faulty: .fault")
check(decide(.reached, .denied, dongle) == .show(.denied), "denied: .denied")
check(decide(.reached, doc("esp32-car", fw: "v1.0+200", proto: 2), car) == .show(.wrongDevice("esp32-car")),
      "foreign device: .wrongDevice with its name")
check(decide(.reached, doc("ajmiddlecar", fw: "v1.0+200", proto: 2, rolledBack: true), car) == .show(.rolledBack),
      "rolled back: .rolledBack")
check(decide(.reached, doc("ajmiddlecar", fw: "v1.0+100", proto: 2), car) == .show(.updating),
      "behind the tag: .updating")
check(decide(.reached, .absent, car) == .show(.updating), "404: .updating (board older than /version)")
check(decide(.reached, doc("ajmiddlecar", fw: "v1.0+200", proto: 3), car) == .show(.appBehind(proto: 3)),
      "current but foreign proto: .appBehind carries the board's proto")
check(decide(.reached, doc("ajmiddlecar", fw: "v1.0+200", proto: 2), car) == .ok, "current + our proto: .ok")

if failures == 0 { print("stagerule: all checks passed") } else { print("stagerule: \(failures) FAILED"); exit(1) }
```

- [ ] **Шаг 3: убедиться, что тест не компилируется/падает**

Run: `swiftc -o /tmp/t app/AJMiddleCar/Generated/CarAPI.swift app/AJMiddleCar/Generated/DongleAPI.swift app/AJMiddleCar/Identity.swift app/AJMiddleCar/VersionRule.swift app/AJMiddleCar/UpdateRules.swift app/AJMiddleCar/CarError.swift app/tests/stagerule/main.swift && /tmp/t`
Expected: ошибка компиляции — `StageRule`, `GateStep`, `Reach`, `Board` не найдены.

- [ ] **Шаг 4: реализация `app/AJMiddleCar/StageRule.swift`**

```swift
import Foundation

/// One board's step of the launch ladder — the same words for both boards, the copy chosen by
/// the device. Screen ids in the "AJMiddleCar screens" artifact are in parentheses.
public enum GateStep: Equatable {
    case seeking                 // asking, nothing answered yet (adapter S1; car S30 while it boots)
    case absent                  // nothing answers and a person is needed (adapter S2)
    case checking                // answered (a document or 404), being looked over — once per stage (S3 / S30)
    case fault                   // answered with something that is not the document (S7 / S33)
    case denied                  // iOS refused local-network access (S8)
    case wrongDevice(String)     // /version.device is not ours (S9 / S23) — carries what it called itself
    case rolledBack              // /version.rolled_back (S10 / S31)
    case updating                // behind the release, or 404: FirmwareView(forced) (S11 / S27)
    case appBehind(proto: Int)   // current, but speaks a protocol this app does not (S32)
    // Reaching a board through another one — the car through the adapter (S12 / S13 / S29 / S14).
    case sendingNetwork, searching, joining, joinFailed
}

/// What `Board.reach()` said about getting to the board this poll.
public enum Reach: Equatable {
    case reached            // ask it: `/version` may be read
    case hold(GateStep)     // not there yet — show this step, poll again
    case lost               // the board this one is reached through is gone: fall back a rung
}

/// One rung of the ladder: everything the shared stage needs about a board. The pure decision
/// (`StageRule`) reads only `Identity`; the closures are `AppFlow`'s side (HTTP, the relay).
public struct Board {
    public struct Identity: Equatable {
        public let device: UpdateRules.Device
        public let expectedDevice: String     // DongleContract.device / CarContract.device
        public let proto: Int                  // DongleContract.proto / CarContract.proto
        /// What silence on `/version` means here: the adapter needs a person (.absent), the car
        /// behind a joined adapter is booting (.seeking).
        public let silentStep: GateStep
        public init(device: UpdateRules.Device, expectedDevice: String, proto: Int, silentStep: GateStep) {
            self.device = device; self.expectedDevice = expectedDevice
            self.proto = proto; self.silentStep = silentStep
        }
    }
    public let identity: Board.Identity
    /// The rung this board is reached through — `0` for the car behind the adapter, `nil` for a
    /// board reached directly. `reach` returning `.lost` sends the runner here.
    public let reachedThrough: Int?
    public let readVersion: () async throws -> Data
    public let reach: () async -> Reach
    public init(identity: Board.Identity, reachedThrough: Int?,
                readVersion: @escaping () async throws -> Data, reach: @escaping () async -> Reach) {
        self.identity = identity; self.reachedThrough = reachedThrough
        self.readVersion = readVersion; self.reach = reach
    }
}

/// The decision of one poll of one board's stage, pure over what the poll saw.
public enum StageRule {
    public enum Verdict: Equatable {
        case show(GateStep)   // set the phase to this step, pause, poll again
        case needRelease      // the board answered and the tag is unknown: fetch it, then poll again
        case ok               // this board is current and ours: advance to the next rung
        case lost             // the board this one is reached through is gone: fall back a rung
    }

    /// `version` is nil exactly when `reach` was not `.reached` (nothing was asked). When
    /// `reach == .reached`, `version` must be non-nil.
    public static func decide(reach: Reach, version: VersionReply?, board: Board.Identity,
                              latestTag: String?, rollback: RollbackChoice) -> Verdict {
        switch reach {
        case .lost: return .lost
        case .hold(let step): return .show(step)
        case .reached: break
        }
        guard let version else { return .show(board.silentStep) }  // defensive: reached ⇒ version
        // A board that answered — a document or a 404 — needs the release established before the
        // rule can compare it. Silence is not "answered": it goes straight to the silent step.
        if answered(version), latestTag == nil { return .needRelease }
        switch VersionRule.step(reply: version, expectedDevice: board.expectedDevice,
                                latestTag: latestTag ?? "", appProto: board.proto, rollback: rollback) {
        case .plugIn: return .show(board.silentStep)
        case .faulty: return .show(.fault)
        case .accessDenied: return .show(.denied)
        case .wrongDevice(let name): return .show(.wrongDevice(name))
        case .rolledBack: return .show(.rolledBack)
        case .updating: return .show(.updating)
        case .appBehind(let proto): return .show(.appBehind(proto: proto))
        case .ok: return .ok
        }
    }

    /// A read that carried a board's own bytes — a `/version` document or a 404. `.silent`,
    /// `.faulty` and `.denied` did not.
    private static func answered(_ reply: VersionReply) -> Bool {
        switch reply { case .version, .absent: return true; case .silent, .faulty, .denied: return false }
    }
}
```

- [ ] **Шаг 5: тест зелёный**

Run: тот же `swiftc … app/tests/stagerule/main.swift && /tmp/t`
Expected: `stagerule: all checks passed`.

- [ ] **Шаг 6: коммит**

```bash
git add app/AJMiddleCar/StageRule.swift app/tests/stagerule
git commit  # feat(app): StageRule — board-ladder types and one pure poll decision
```

---

### Задача 2: `CarReach` — сетевой автомат адаптера с бюджетом

**Files:**
- Create: `app/AJMiddleCar/CarReach.swift`
- Modify: `app/AJMiddleCar/AppFlow.swift` (удалить приватный `enum DongleStatusReply` в конце файла — он переезжает сюда публичным)
- Create: `app/tests/carreach/main.swift`
- Create: `app/tests/carreach/sources`

**Interfaces:**
- Consumes: `Reach`, `GateStep` (StageRule.swift); `DongleLink.next`, `DongleStep` (DongleLink.swift); `DongleStatus` (generated); `VersionReply.of` (Identity.swift).
- Produces: `enum DongleStatusReply` (публичный, с `decode`/`of`/`status`/`silent`/`faulty`/`denied`); `struct CarReach` с `enum Ask {case configure, retry}`, `next(_:expectedSSID:) -> (reach: Reach, ask: Ask?)`, `mutating func retry()`, `static let maxJoinAttempts = 1`, `attempts`, `gaveUp`.

- [ ] **Шаг 1: тест `app/tests/carreach/sources`**

```
CarReach.swift
StageRule.swift
DongleLink.swift
Identity.swift
UpdateRules.swift
CarError.swift
```

- [ ] **Шаг 2: провальный тест `app/tests/carreach/main.swift`**

```swift
// Host test for CarReach — reaching the car through the adapter: the adapter's network state
// machine with the join budget. Pure over the adapter's /status reply; the POSTs it asks for
// are the caller's to send. Run with swiftc.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) { if !ok { print("FAIL: \(what)"); failures += 1 } }

func status(ssid: String, state: String) -> DongleStatusReply {
    let json = #"""
    {"proto":1,"usb":{"state":"up"},
     "wifi":{"ssid":"\#(ssid)","configured":\#(!ssid.isEmpty),"state":"\#(state)","rssi_dbm":-50,"channel":1,
             "attempts":{"used":0,"max":5}},
     "relay":{"to_car_hz":0.0,"to_phone_hz":0.0,"udp_sessions":0,"tcp_connections":0,"last_error":null},
     "system":{"uptime_s":1,"free_heap":1,"idf":"v6.0.2"}}
    """#
    return .status(try! JSONDecoder().decode(DongleStatus.self, from: Data(json.utf8)))
}
let car = CarContract.ssid

// A cold start: not configured → send the credentials, show "sending", spend the one budget.
var r = CarReach()
var out = r.next(status(ssid: "", state: "idle"), expectedSSID: car)
check(out.reach == .hold(.sendingNetwork) && out.ask == .configure, "cold: send credentials")
// The next poll while still unconfigured: budget spent → join failed, no more asks.
out = r.next(status(ssid: "", state: "idle"), expectedSSID: car)
check(out.reach == .hold(.joinFailed) && out.ask == nil, "budget spent: join failed, no ask")

// Association steps show without spending anything.
var r2 = CarReach()
check(r2.next(status(ssid: car, state: "searching"), expectedSSID: car).reach == .hold(.searching), "searching")
check(r2.next(status(ssid: car, state: "joining"), expectedSSID: car).reach == .hold(.joining), "joining")
let ok = r2.next(status(ssid: car, state: "connected"), expectedSSID: car)
check(ok.reach == .reached && ok.ask == nil, "connected: reached")
check(r2.attempts == 0 && !r2.gaveUp, "connected resets the budget")

// A radio that reports failed: retry once (still "searching", not "no link"), then hold failed.
var r3 = CarReach()
let retry = r3.next(status(ssid: car, state: "failed"), expectedSSID: car)
check(retry.reach == .hold(.searching) && retry.ask == .retry, "failed: retry, shown as searching")
check(r3.next(status(ssid: car, state: "failed"), expectedSSID: car).reach == .hold(.joinFailed), "failed again: join failed")
r3.retry()
check(r3.next(status(ssid: car, state: "failed"), expectedSSID: car).ask == .retry, "retry() gives a fresh budget")

// The adapter itself gone or refusing is the adapter stage's verdict, not the car's — hand back.
check(CarReach().next(.silent, expectedSSID: car).reach == .lost, "adapter silent: lost")
check(CarReach().next(.faulty, expectedSSID: car).reach == .lost, "adapter faulty: lost")
check(CarReach().next(.denied, expectedSSID: car).reach == .lost, "adapter denied: lost")

if failures == 0 { print("carreach: all checks passed") } else { print("carreach: \(failures) FAILED"); exit(1) }
```

- [ ] **Шаг 3: убедиться, что тест падает**

Run: `swiftc -o /tmp/t app/AJMiddleCar/Generated/CarAPI.swift app/AJMiddleCar/Generated/DongleAPI.swift app/AJMiddleCar/StageRule.swift app/AJMiddleCar/DongleLink.swift app/AJMiddleCar/Identity.swift app/AJMiddleCar/UpdateRules.swift app/AJMiddleCar/CarError.swift app/tests/carreach/main.swift && /tmp/t`
Expected: ошибка — `CarReach`, `DongleStatusReply` не найдены.

- [ ] **Шаг 4: реализация `app/AJMiddleCar/CarReach.swift`**

```swift
import Foundation

/// What one read of the adapter's `/status` produced. Public, because `CarReach` classifies it
/// and the host test builds it. Read only after `VersionRule` said `.ok` (its shape depends on
/// the protocol `/version` just vouched for); a 404 here is a fault, not an older board.
public enum DongleStatusReply: Equatable {
    case status(DongleStatus)
    case silent    // nothing answered: no cable, refused connection, a deadline with no bytes
    case faulty    // answered and unusable: an HTTP error, a truncated stream, an undecodable body
    case denied    // iOS refused to let the request leave the phone

    /// Read a `/status` body as a document, else a fault.
    public static func decode(_ data: Data) -> DongleStatusReply {
        if let s = try? JSONDecoder().decode(DongleStatus.self, from: data) { return .status(s) }
        return .faulty
    }
    /// Classify what `DongleClient.statusData()` threw — `VersionReply.of`'s rule, with 404
    /// folded into `.faulty` (a `/status` fault, never an older board).
    public static func of(_ error: Error) -> DongleStatusReply {
        switch VersionReply.of(error) {
        case .version, .absent, .faulty: return .faulty
        case .silent: return .silent
        case .denied: return .denied
        }
    }
}

/// Reaching the car through the adapter: the adapter's network state machine with the join
/// budget, as one pure structure. `next` says what to show and what POST to send; the caller
/// (`AppFlow`) sends it. Identity, rollback and version are `VersionRule`'s, decided from the
/// car's own `/version` once this returns `.reached`.
public struct CarReach {
    public enum Ask: Equatable { case configure, retry }
    /// ONE, not three: each ask spends a full five-attempt budget on the adapter's side. The app
    /// asks once, the adapter tries five times, then both say so and wait for a person. See the
    /// removed `AppFlow.maxDongleJoinAttempts` doc for the whole history.
    public static let maxJoinAttempts = 1
    public private(set) var attempts = 0
    public private(set) var gaveUp = false
    public init() {}

    public mutating func next(_ reply: DongleStatusReply, expectedSSID: String) -> (reach: Reach, ask: Ask?) {
        guard case .status(let s) = reply else {
            // Silence / a bad answer / a denial from the ADAPTER is the adapter stage's verdict
            // (S2 / S7 / S8), not the car's — hand back a rung.
            return (.lost, nil)
        }
        switch DongleLink.next(status: s, expectedSSID: expectedSSID) {
        case .sendCredentials: return charge(.configure, showing: .sendingNetwork)
        case .searchingCar: return (.hold(.searching), nil)
        case .waiting: return (.hold(.joining), nil)
        case .retryJoin: return charge(.retry, showing: .searching)
        case .readyForCar:
            attempts = 0; gaveUp = false
            return (.reached, nil)
        }
    }

    /// «Повторить» on the join-failed screen: a fresh budget, spent from the next ask.
    public mutating func retry() { attempts = 0; gaveUp = false }

    /// Spend one budget for an ask, or hold at join-failed once it is gone. Mirrors the old
    /// `askDongleToJoin`: `guard !gaveUp`, then count this attempt.
    private mutating func charge(_ ask: Ask, showing step: GateStep) -> (reach: Reach, ask: Ask?) {
        guard !gaveUp else { return (.hold(.joinFailed), nil) }
        attempts += 1
        if attempts >= Self.maxJoinAttempts { gaveUp = true }
        return (.hold(step), ask)
    }
}
```

- [ ] **Шаг 5: удалить приватный дубль из `AppFlow.swift`**

Удалить блок `private enum DongleStatusReply { … }` в самом конце `app/AJMiddleCar/AppFlow.swift` (сейчас строки после `updateFinished()`, начинающиеся с `/// What one read of the adapter's` и до конца файла). `readDongleStatus()` продолжит работать — `DongleStatusReply.decode/.of/.status` теперь резолвятся в публичный тип из `CarReach.swift`.

- [ ] **Шаг 6: тест зелёный + приложение компилируется**

Run: `swiftc … app/tests/carreach/main.swift && /tmp/t`
Expected: `carreach: all checks passed`.
Run: сборка приложения (см. Global Constraints).
Expected: успех, exit 0.

- [ ] **Шаг 7: коммит**

```bash
git add app/AJMiddleCar/CarReach.swift app/AJMiddleCar/AppFlow.swift app/tests/carreach
git commit  # feat(app): CarReach — the adapter's network reach as one pure state machine
```

---

### Задача 3: `ConnectView` — ситуация `.stage`, тексты и рисунок

**Files:**
- Modify: `app/AJMiddleCar/ConnectView.swift` (добавить `case stage`, обработать в `leftPanel`/`title`/`message`/`actionButton`, добавить параметр `onRetry`)
- Modify: `app/AJMiddleCar/L.swift` (добавить `stageTitle`, `stageSub`, `carFaultTitle`, `carFaultSub`)
- Modify: `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings` (добавить две строки `car.fault*`)

**Interfaces:**
- Consumes: `GateStep`, `UpdateRules.Device`; существующие `DeviceScene`, `LinkScene`, `ConnectCarView`, `CarBody`, `AdapterBody`; существующие строки.
- Produces: `ConnectView.Situation.stage(UpdateRules.Device, GateStep)`; `ConnectView.onRetry: (() -> Void)?`.

Примечание: ситуация `.stage` добавляется рядом со старыми — они пока остаются (удаляются в Задаче 5), сборка зелёная.

- [ ] **Шаг 1: строки в `Localizable.strings`** (после блока `car.checking*`)

```
"car.faultTitle"         = "Машинка отвечает с ошибкой";
"car.faultSub"           = "Машинка в сети адаптера, но отвечает не так, как ждёт приложение. Перезагрузи её.";
```

- [ ] **Шаг 2: аксессоры в `L.swift`** (рядом с `carCheckingTitle`)

```swift
static var carFaultTitle: String { s("car.faultTitle") }
static var carFaultSub: String { s("car.faultSub") }

/// The one board-ladder step's title, chosen by device — today's copy, word for word.
static func stageTitle(_ step: GateStep, _ d: UpdateRules.Device) -> String {
    switch step {
    case .seeking:       return d == .car ? carCheckingTitle : dongleFindingTitle
    case .absent:        return linkNoDongleTitle
    case .checking:      return d == .car ? carCheckingTitle : dongleCheckingTitle
    case .fault:         return d == .car ? carFaultTitle : dongleFaultTitle
    case .denied:        return linkDeniedTitle
    case .wrongDevice:   return d == .car ? wrongCarTitle : dongleWrongTitle
    case .rolledBack:    return rolledBackTitle(d)
    case .updating:      return gateUpdateTitle          // rendered by FirmwareView, not here
    case .appBehind:     return appBehindTitle
    case .sendingNetwork:return dongleSendingNetTitle
    case .searching:     return carFindingTitle
    case .joining:       return dongleConfiguringTitle
    case .joinFailed:    return dongleJoinFailedTitle
    }
}
/// The step's subtitle, chosen by device.
static func stageSub(_ step: GateStep, _ d: UpdateRules.Device) -> String {
    switch step {
    case .seeking:       return d == .car ? carCheckingSub : dongleFindingSub
    case .absent:        return linkNoDongleSub
    case .checking:      return d == .car ? carCheckingSub : dongleCheckingSub
    case .fault:         return d == .car ? carFaultSub : dongleFaultSub
    case .denied:        return linkDeniedSub
    case .wrongDevice(let found):
        return d == .car ? wrongCarSub(found, CarContract.device) + "\n\n" + wrongCarHint
                         : dongleWrongSub(found)
    case .rolledBack:    return rolledBackSub(d)
    case .updating:      return gateUpdateSub
    case .appBehind(let proto):
        return appBehindSub(d, proto, d == .car ? CarContract.proto : DongleContract.proto)
    case .sendingNetwork:return dongleSendingNetSub
    case .searching:     return carFindingSub
    case .joining:       return dongleConfiguringSub
    case .joinFailed:    return dongleJoinFailedSub
    }
}
```

Note: `gateUpdateSub` уже есть (`gate.updateSub`); если аксессора нет — добавить `static var gateUpdateTitle: String { s("gate.updateTitle") }` / `gateUpdateSub`.

- [ ] **Шаг 3: параметр и ветка в `ConnectView.swift`**

Добавить в `enum Situation`:

```swift
/// One rung of the launch ladder, the board and the step. The single situation the runner
/// emits; every board-prefixed situation below is being folded into it.
case stage(UpdateRules.Device, GateStep)
```

Добавить свойство рядом с `onRetryJoin`:

```swift
/// `.stage` with a `.wrongDevice`, `.rolledBack` or `.joinFailed` step — the one button those
/// carry. `wakePoll` / `recheckRollback` / `retryJoin` respectively; wired by `RootView`.
var onRetry: (() -> Void)? = nil
```

В `leftPanel` добавить ветку (перед `case .searching, .findingCar`):

```swift
case .stage(let d, let step):
    stageScene(d, step)
```

Добавить метод (внутри `ConnectView`):

```swift
@ViewBuilder private func stageScene(_ d: UpdateRules.Device, _ step: GateStep) -> some View {
    @ViewBuilder func body() -> some View { if d == .car { CarBody(palette: p) } else { AdapterBody(palette: p) } }
    switch step {
    case .seeking where d == .dongle, .absent:
        DeviceScene(palette: p, rings: .wait(), presence: 0.34) { AdapterBody(palette: p) }
    case .seeking, .checking:            // car .seeking and either board's .checking: found, being asked
        DeviceScene(palette: p, rings: .wait(), chip: (glyph: "cpu", tint: p.accent)) { body() }
    case .fault:
        DeviceScene(palette: p, rings: .wait(), chip: (glyph: "exclamationmark", tint: p.warn)) { body() }
    case .denied:
        DeviceScene(palette: p, rings: .deco, ringTint: p.warn, chip: (glyph: "lock", tint: p.warn)) { body() }
    case .wrongDevice:
        DeviceScene(palette: p, rings: .deco, ringTint: p.warn, chip: (glyph: "questionmark", tint: p.warn)) { body() }
    case .rolledBack:
        DeviceScene(palette: p, rings: .deco, ringTint: p.warn, chip: (glyph: "arrow.uturn.backward", tint: p.warn)) { body() }
    case .appBehind:
        DeviceScene(palette: p, rings: .deco, ringTint: p.warn,
                    chip: (glyph: "exclamationmark.arrow.circlepath", tint: p.warn)) { body() }
    case .updating:                      // never reached: FirmwareView renders .updating
        DeviceScene(palette: p, rings: .wait(), chip: (glyph: "arrow.down", tint: p.accent)) { body() }
    case .sendingNetwork:
        DeviceScene(palette: p, rings: .inward, chip: (glyph: "wifi", tint: p.accent)) { AdapterBody(palette: p) }
    case .searching:
        ConnectCarView(palette: p)
    case .joining:
        LinkScene(palette: p)
    case .joinFailed:
        LinkScene(palette: p, failed: true)
    }
}
```

В `title` добавить: `case .stage(let d, let step): return L.stageTitle(step, d)`.
В `message` добавить: `case .stage(let d, let step): return L.stageSub(step, d)`.
В `actionButton` добавить ветку:

```swift
case .stage(_, let step):
    switch step {
    case .denied:
        pillButton(L.openSettings, tint: p.accent) {
            if let url = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(url) }
        }
    case .wrongDevice, .rolledBack, .joinFailed:
        if let onRetry { pillButton(L.fwRetry, tint: p.warn, action: onRetry) }
    default:
        EmptyView()
    }
```

- [ ] **Шаг 4: сборка**

Run: сборка приложения (Global Constraints).
Expected: успех, exit 0.

- [ ] **Шаг 5: коммит**

```bash
git add app/AJMiddleCar/ConnectView.swift app/AJMiddleCar/L.swift app/AJMiddleCar/Resources/ru.lproj/Localizable.strings
git commit  # feat(app): ConnectView.stage — one situation family for the board ladder
```

---

### Задача 4: `AppFlow` — раннер, стадия, стражи; `RootView` на `.stage`

**Files:**
- Modify: `app/AJMiddleCar/AppFlow.swift` (новый `Phase`, `Board`-массив, `runLadder`/`stage`/`restart`/`updateFinished(_:)`, удаление старых гейтов)
- Modify: `app/AJMiddleCar/AJMiddleCarApp.swift` (`root` на `.stage`, стражи `link.state`, `carRoot`)

**Interfaces:**
- Consumes: `StageRule.decide`, `Board`, `Board.Identity`, `GateStep`, `Reach` (Задача 1); `CarReach`, `DongleStatusReply` (Задача 2); `ConnectView.Situation.stage`, `onRetry` (Задача 3); существующие `fetchRelease`, `recheckRollback`, `consumeRollbackRecheck`, `pollPause`, `wakePoll`, `setPhase`, `GateRule.mayDrive`, `UpdateClient.buildNumber`.
- Produces: `AppFlow.Phase` (`.stage/.releaseCheck/.releaseOffline/.releaseMissing/.awaitingCar/.ready`); `func restart(from:)`, `func updateFinished(_:)`, `func retryJoin()`, `func retryAction(for:) -> (() -> Void)?`.

Это ядро. Старые ситуации `ConnectView` и `WrongCarView` после этой задачи мертвы, но остаются в файлах (их удаляет Задача 5); `GalleryView` не трогаем — сборка зелёная.

- [ ] **Шаг 1: заменить `enum Phase` и `opensLink`** в `AppFlow.swift`

Удалить весь текущий `enum Phase { … }` (со всеми `case .checkDongle …` и `var opensLink`), заменить на:

```swift
enum Phase: Equatable {
    /// One rung of the ladder — the board and its current step. The only screen-bearing phase
    /// there is now, in place of the twenty-odd board-prefixed cases this replaced.
    case stage(UpdateRules.Device, GateStep)
    case releaseCheck                                            // S4
    case releaseOffline                                          // S5
    case releaseMissing(tag: String, device: UpdateRules.Device) // S6
    /// The ladder is done; `CarLink` owns the screen (radar or drive).
    case awaitingCar
    /// `carIdentified` said the car may drive.
    case ready

    /// The phases whose screen opens the UDP link. Everything mid-ladder is on the false side:
    /// until the ladder hands over there is no session to open, and `.stage(_, .updating)` runs
    /// the forced update over HTTP — a session there would only shout `wrongProto` at the very
    /// car being updated.
    var opensLink: Bool {
        switch self {
        case .awaitingCar, .ready: return true
        case .stage, .releaseCheck, .releaseOffline, .releaseMissing: return false
        }
    }
}
```

Обновить инициализаторы `@Published var phase` и `shown`: `= .stage(.dongle, .seeking)` (первый кадр — «ищу адаптер»; для мока раннер сразу перезапишет).

- [ ] **Шаг 2: свойства раннера** — заменить блок старых полей

Удалить: `dongleHandedOver`, `dongleRerunWanted`, `sawDongle`, `dongleJoinAttempts`, `dongleJoinGaveUp`, `lastStatusFailure` (оставить — его читает `readDongleStatus`), `maxDongleJoinAttempts`. Добавить:

```swift
/// The ladder for this launch: `[adapter, car]` via the dongle, `[car]` against the mock.
private var boards: [Board] = []
/// The rung the runner is on — read by `restart` to decide whether to fall back.
private var currentRung = 0
/// A guard asked the ladder to be at this rung or earlier. Consumed at the top of `stage`.
private var wantRung: Int?
/// The car's reach state — the join budget — recreated at each entry to the car's stage.
private var carReach = CarReach()
```

Оставить: `latestTag`, `client`, `dongle`, `donglePollInterval`, `pollSleep`, `gateRunning`, `rollbackChoice`, `lastStatusFailure`, `lastVersionFailure`, весь блок `phase/shown/queued/pacing/shownAt`.

- [ ] **Шаг 3: `startupCheck` и раннер** — заменить `startupCheck`, `runGates`, `releaseGate`

```swift
func startupCheck() async {
    guard !gateRunning else { return }
    gateRunning = true
    defer { gateRunning = false }
    UpdateClient.migrateCacheIfNeeded()
    await runLadder(from: 0)
    setPhase(.awaitingCar)
}

/// The ladder, rung by rung. `.advance` climbs; `.jump(w)` (a guard, or a lost reach) drops to
/// rung `w`. `gateRunning` is claimed by every caller before the first `await`, so two runners
/// never overlap.
private func runLadder(from start: Int) async {
    boards = buildBoards()
    var i = min(start, boards.count - 1)
    while i < boards.count {
        currentRung = i
        switch await stage(boards[i]) {
        case .advance: i += 1
        case .jump(let w): i = max(0, min(w, boards.count - 1))
        }
    }
}

/// `[adapter, car-through-adapter]` when there is a dongle to reach the car through; `[car]`
/// read directly when there is not (the mock). The rung indices `restart`/`reach` use come from
/// `rung(of:)`, not from this array, so they are valid before the first build.
private func buildBoards() -> [Board] {
    if CarHost.viaDongle { return [dongleBoard(), carBoard(reachedThrough: 0)] }
    return [carBoard(reachedThrough: nil)]
}
private func rung(of device: UpdateRules.Device) -> Int {
    switch device { case .dongle: return 0; case .car: return CarHost.viaDongle ? 1 : 0 }
}

private func dongleBoard() -> Board {
    Board(identity: .init(device: .dongle, expectedDevice: DongleContract.device,
                          proto: DongleContract.proto, silentStep: .absent),
          reachedThrough: nil,
          readVersion: { [dongle] in try await dongle.versionData() },
          reach: { .reached })
}
private func carBoard(reachedThrough: Int?) -> Board {
    Board(identity: .init(device: .car, expectedDevice: CarContract.device,
                          proto: CarContract.proto, silentStep: .seeking),
          reachedThrough: reachedThrough,
          readVersion: { try await CarTransport.shared.get(CarContract.versionPath, timeout: 2) },
          reach: reachedThrough == nil ? { .reached }
                                       : { [weak self] in await self?.reachCarViaDongle() ?? .lost })
}

/// One poll of "is the adapter in the car's network yet": its `/status` → `CarReach` → show the
/// step and send the POST it asks for. The car's version is read only once this is `.reached`.
private func reachCarViaDongle() async -> Reach {
    let reply = await readDongleStatus()
    let (reach, ask) = carReach.next(reply, expectedSSID: CarContract.ssid)
    if let ask { await sendJoin(ask) }
    return reach
}
```

- [ ] **Шаг 4: `stage`** — добавить

```swift
private enum StageExit { case advance; case jump(Int) }

/// One board's whole stage: reach it, read its `/version`, learn the release if needed, apply
/// the rule, park on a forced update. Returns when the board is current and ours (`.advance`)
/// or a guard/lost reach sends the runner elsewhere (`.jump`).
private func stage(_ board: Board) async -> StageExit {
    if board.identity.device == .car, CarHost.viaDongle { carReach = CarReach() }
    var sawChecking = false
    while true {
        if let w = wantRung { wantRung = nil; return .jump(w) }
        // Parked: FirmwareView owns the board during a forced update. Poll nothing — the car's
        // OTA reboot drops its AP and the adapter would read that as "gone" — just wait for
        // `updateFinished` (which moves the phase off `.updating`) or a guard.
        if case .stage(board.identity.device, .updating) = phase {
            await pollPause(); continue
        }
        let reach = await board.reach()
        let version: VersionReply? = reach == .reached ? await readCarOrDongleVersion(board) : nil
        if reach == .reached, let version, !sawChecking, answered(version) {
            sawChecking = true
            setPhase(.stage(board.identity.device, .checking))
        }
        switch StageRule.decide(reach: reach, version: version, board: board.identity,
                                latestTag: latestTag, rollback: rollbackChoice) {
        case .lost:
            return .jump(board.reachedThrough ?? max(0, currentRung - 1))
        case .needRelease:
            _ = await fetchRelease(for: board.identity.device)   // sets latestTag or a hold phase
            await pollPause(); continue                          // re-decide next poll with the tag
        case .ok:
            return .advance
        case .show(let step):
            if step == .rolledBack || step == .updating { consumeRollbackRecheck() }
            setPhase(.stage(board.identity.device, step))
            await pollPause(); continue
        }
    }
}

/// `readVersion(_:_:)` over a board's own `readVersion` closure — the classified read, logged
/// once per distinct failure (`lastVersionFailure`).
private func readCarOrDongleVersion(_ board: Board) async -> VersionReply {
    await readVersion(board.identity.device.rawValue, board.readVersion)
}
private func answered(_ reply: VersionReply) -> Bool {
    switch reply { case .version, .absent: return true; case .silent, .faulty, .denied: return false }
}
```

- [ ] **Шаг 5: `sendJoin`, `restart`, `updateFinished`, `retryJoin`, `retryAction`, `carIdentified`**

Заменить `askDongleToJoin(retry:)` на:

```swift
/// One POST asking the adapter to join the car's network. `configure` the first time,
/// `retry` after — `DongleClient` keeps them apart on purpose. Failures are logged, never
/// swallowed; the credentials never reach a log.
private func sendJoin(_ ask: CarReach.Ask) async {
    do {
        let reply: DongleWifiReply
        switch ask {
        case .configure: reply = try await dongle.join(ssid: CarContract.ssid, password: CarContract.password)
        case .retry:     reply = try await dongle.retryJoin(ssid: CarContract.ssid, password: CarContract.password)
        }
        print("dongle \(DongleContract.wifiPath): \(reply.state)")
    } catch {
        print("dongle \(DongleContract.wifiPath) (\(ask == .configure ? "configure" : "retry")) failed: " + Self.describe(error))
    }
}
```

Заменить `dongleReturned()`, `retryDongleJoin()`, `dongleUpdateFinished()`, `updateFinished()` на:

```swift
/// A post-gate guard fired: the ladder should be at `device`'s rung or earlier. Not running →
/// run from there. Running past it → the current stage falls back until it is there. Running at
/// it or earlier → nothing (it will reach it). `restart(from: .car)` re-enters the car's stage
/// in place; `restart(from: .dongle)` walks back to the adapter.
func restart(from device: UpdateRules.Device) {
    guard CarHost.viaDongle || device == .car else { return }
    let target = rung(of: device)
    guard gateRunning else {
        gateRunning = true
        Task { @MainActor in
            defer { self.gateRunning = false }
            await self.runLadder(from: target)
            self.setPhase(.awaitingCar)
        }
        return
    }
    wantRung = min(wantRung ?? currentRung, target)
    wakePoll()
}

/// «Повторить» on the join-failed screen: a fresh budget, spent from the next reach.
func retryJoin() { carReach.retry(); wakePoll() }

/// `FirmwareView` finished a forced update of `device`. Move the phase off `.updating` so the
/// parked stage re-reaches and re-decides from a fresh `/version`; if the ladder is not running
/// (a forced update raised by `carIdentified` mid-session), run it from that board's rung.
func updateFinished(_ device: UpdateRules.Device) {
    guard case .stage(device, .updating) = phase else { return }
    setPhase(.stage(device, .checking))
    if gateRunning {
        wakePoll()          // unpark the running stage
    } else {
        gateRunning = true
        Task { @MainActor in
            defer { self.gateRunning = false }
            await self.runLadder(from: self.rung(of: device))
            self.setPhase(.awaitingCar)
        }
    }
}

/// The button each `.stage` step can carry, if any — wired into `ConnectView.onRetry`.
func retryAction(for step: GateStep) -> (() -> Void)? {
    switch step {
    case .wrongDevice: return { [weak self] in self?.wakePoll() }
    case .rolledBack:  return { [weak self] in self?.recheckRollback() }
    case .joinFailed:  return { [weak self] in self?.retryJoin() }
    default:           return nil
    }
}
```

Заменить тело `carIdentified(fw:)`:

```swift
func carIdentified(fw: String?) {
    guard let fw else { return }
    guard phase == .awaitingCar || phase == .ready else { return }
    if GateRule.mayDrive(deviceBuild: UpdateClient.buildNumber(fw),
                         latestBuild: UpdateClient.buildNumber(latestTag)) {
        setPhase(.ready)
    } else {
        restart(from: .car)   // re-enter the car's stage; the rule raises .updating from /version
    }
}
```

- [ ] **Шаг 6: удалить `dongleGate()` и `carGate()` и `adapterStillJoined()`** целиком из `AppFlow.swift`. Убедиться, что `readDongleStatus()`, `readVersion(_:_:)`, `describe(_:)`, `fetchRelease(for:)`, `recheckRollback()`, `consumeRollbackRecheck()`, `pollPause()`, `wakePoll()`, `setPhase(_:)` остались.

- [ ] **Шаг 7: `AJMiddleCarApp.swift` — `root`, стражи, `carRoot`**

Заменить `root` switch (`switch flow.shown { … }`) на:

```swift
switch flow.shown {
case .stage(let dev, .updating):
    FirmwareView(palette: p, flow: dev == .car ? .forCar() : .forDongle(client: flow.dongle),
                 forced: true, onDone: { flow.updateFinished(dev) })
case .stage(let dev, let step):
    ConnectView(situation: .stage(dev, step), onRetry: flow.retryAction(for: step))
case .releaseCheck:
    ConnectView(situation: .releaseCheck)
case .releaseOffline:
    ConnectView(situation: .releaseOffline)
case .releaseMissing(let tag, let dev):
    ConnectView(situation: .releaseMissing(tag: tag, device: dev))
case .awaitingCar, .ready:
    carRoot.onAppear { link.start() }
}
```

Заменить `.onChange(of: link.state)` на стражи:

```swift
.onChange(of: link.state) { _, new in
    switch new {
    case .noDongle, .localNetworkDenied: flow.restart(from: .dongle)
    case .wrongCar, .wrongProto:         flow.restart(from: .car)
    case .live:                          flow.carIdentified(fw: link.fw)
    case .searching:                     break
    }
}
```

Оставить `.onChange(of: link.fw) { _, fw in flow.carIdentified(fw: fw) }`, `.onChange(of: scenePhase)` (с `flow.shown.opensLink`) и `.onChange(of: flow.phase) { _, phase in if phase == .awaitingCar { link.retryAfterWrongCar(); if link.isLive { flow.carIdentified(fw: link.fw) } } }` (добавить `link.retryAfterWrongCar()` — раньше он вызывался из `WrongCarView`).

Заменить `carRoot`:

```swift
@ViewBuilder private var carRoot: some View {
    switch link.state {
    case .live:
        if flow.phase == .ready { DriveView(link: link, intent: intent) }
        else { ZStack { p.bg.ignoresSafeArea(); ConnectView() } }   // live, gate not yet done (S26)
    default:
        // Every other link state — searching, wrongCar, wrongProto, noDongle, denied — has
        // already restarted the ladder through a guard, so this is a one-frame fallback.
        ZStack { p.bg.ignoresSafeArea(); ConnectView() }
    }
}
```

- [ ] **Шаг 8: сборка**

Run: сборка приложения (Global Constraints).
Expected: успех, exit 0. (Старые ситуации `ConnectView`/`WrongCarView` мертвы, но `GalleryView` их ещё держит — сборка проходит.)

- [ ] **Шаг 9: хост-тесты**

Run: `tools/test-all.sh`
Expected: всё зелёное (`stagerule`, `carreach`, остальные).

- [ ] **Шаг 10: дым на симуляторе**

Запустить мок, установить сборку, `xcrun simctl launch booted com.adamjohnson.ajmiddlecar`, подождать ~8 с, скриншот (повернуть `sips -r -90`).
Expected: экран езды (S28).
Перезапустить мок `MOCK_DEVICE=esp32-car`, перезапустить приложение → скриншот.
Expected: `.stage(.car, .wrongDevice)` — «Это другая машинка» с «Повторить».
Перезапустить мок `MOCK_NO_VERSION=1` → скриншот.
Expected: `.stage(.car, .updating)` → прошивка → S28 (день-флаг).
Остановить мок посреди езды, затем снова → приложение возвращается на радар и обратно на S28.
Погасить мок.

- [ ] **Шаг 11: коммит**

```bash
git add app/AJMiddleCar/AppFlow.swift app/AJMiddleCar/AJMiddleCarApp.swift
git commit  # feat(app): one ladder runner and stage; post-gate guards restart it
```

---

### Задача 5: удалить `WrongCarView` и мёртвые ситуации; матрица галереи

**Files:**
- Delete: `app/AJMiddleCar/WrongCarView.swift`
- Modify: `app/AJMiddleCar/ConnectView.swift` (удалить старые ситуации и их ветки)
- Modify: `app/AJMiddleCar/L.swift` (удалить осиротевшие аксессоры)
- Modify: `app/AJMiddleCar/GalleryView.swift` (матрица шаг × плата)

**Interfaces:**
- Consumes: `ConnectView.Situation.stage`, `onRetry` (Задача 3); `FirmwareView(forced:)`.
- Produces: галерея без `WrongCar`/`WrongProto`, с кадрами `.stage` для обеих плат и forced-кадрами адаптера.

Проверить перед удалением строки: она нужна только `ConnectView`/`GalleryView`/`WrongCarView`, вне этого не используется. `wrongCar.*`/`wrongProto.*` остаются в `.strings` (их использует `stageSub`/будущее) — удаляем ТОЛЬКО `wrongProto.*` если на них нет ссылок; `wrongCar.sub`/`.hint` использует `stageSub`, оставить.

- [ ] **Шаг 1: удалить старые ситуации из `ConnectView.Situation`**

Удалить кейсы: `findingAdapter`, `releaseCheck` (НЕТ — `.releaseCheck` ещё используется `RootView`; оставить), — то есть удалить только те, что раннер больше не эмитит: `findingAdapter`, `findingCar`, `checkingDongle`, `noDongle`, `localNetworkDenied`, `dongleFault`, `wrongDongle`, `sendingNetwork`, `dongleConfiguring`, `dongleJoinFailed`, `rolledBack`, `checkingCar`, `appBehind`. Оставить: `searching`, `releaseOffline`, `releaseMissing`, `releaseCheck`, `stage`. Удалить свойства `onRecheckRollback`, `onRetryJoin` (их заменил `onRetry`). Удалить соответствующие ветки в `leftPanel`, `title`, `message`, `actionButton`. Убедиться, что `leftPanel` для `.searching`/`.releaseOffline`/`.releaseMissing`/`.releaseCheck` сохранён (эти рисуются вне `.stage`).

Проверка exhaustiveness: после удаления `leftPanel`/`title`/`message`/`actionButton` покрывают ровно `{searching, releaseOffline, releaseMissing, releaseCheck, stage}`.

- [ ] **Шаг 2: удалить `WrongCarView.swift`**

```bash
git rm app/AJMiddleCar/WrongCarView.swift
```

- [ ] **Шаг 3: почистить `L.swift`** — удалить аксессоры, на которые больше нет ссылок: `wrongProtoTitle`, `wrongProtoSub`, `wrongProtoHint`, `dongleFindingTitle`/`Sub` (если `stageTitle` их использует для `.seeking` дongle — НЕ удалять), `linkNoDongleTitle`/`Sub`, `dongleCheckingTitle`/`Sub`, `carCheckingTitle`/`Sub`, `dongleFaultTitle`/`Sub`, `dongleWrongTitle`/`Sub`, `dongleSendingNetTitle`/`Sub`, `dongleConfiguringTitle`/`Sub`, `dongleJoinFailedTitle`/`Sub`, `carFindingTitle`/`Sub`, `rolledBackTitle`/`Sub`, `appBehindTitle`/`Sub`, `wrongCarTitle`/`Sub`/`Hint` — **но только те, что не вызываются из `stageTitle`/`stageSub`.** Практически: `stageTitle`/`stageSub` (Задача 3) вызывают почти все из них, поэтому реально осиротевшими будут лишь `wrongProto*` и, возможно, `wrongCarTitle` (заголовок теперь через `stageTitle`, но `wrongCarTitle` вызывается там же — оставить). **Правило: удалять аксессор только после `grep -rn "имя" app/AJMiddleCar`, показавшего единственную ссылку — определение.** Соответствующие строки в `.strings` удалять по тому же правилу (`grep` по ключу во всём `app/`).

- [ ] **Шаг 4: матрица галереи** — заменить блок кадров `ConnectView`/`WrongCarView` в `GalleryView.makeFrames`

Удалить строки от `("Checking dongle", …)` до `("WrongProto", …)` включительно. Вставить (лейблы = каталог артефакта; дают пересъёмку):

```swift
("Connect (radar)",          AnyView(ConnectView())),
// Adapter stage
("Step 1 finding adapter",   AnyView(ConnectView(situation: .stage(.dongle, .seeking)))),
("No dongle",                AnyView(ConnectView(situation: .stage(.dongle, .absent)))),
("Checking dongle",          AnyView(ConnectView(situation: .stage(.dongle, .checking)))),
("Dongle fault",             AnyView(ConnectView(situation: .stage(.dongle, .fault)))),
("Local network denied",     AnyView(ConnectView(situation: .stage(.dongle, .denied), onRetry: {}))),
("Wrong dongle",             AnyView(ConnectView(situation: .stage(.dongle, .wrongDevice("some-other-adapter")), onRetry: {}))),
("Dongle rolled back",       AnyView(ConnectView(situation: .stage(.dongle, .rolledBack), onRetry: {}))),
("App behind (dongle)",      AnyView(ConnectView(situation: .stage(.dongle, .appBehind(proto: 2))))),
("Step 3 release check",     AnyView(ConnectView(situation: .releaseCheck))),
("Offline, cannot verify",   AnyView(ConnectView(situation: .releaseOffline))),
("No release for adapter",   AnyView(ConnectView(situation: .releaseMissing(tag: "v1.0+483", device: .dongle)))),
("No release for car",       AnyView(ConnectView(situation: .releaseMissing(tag: "v1.0+483", device: .car)))),
// Reaching the car through the adapter
("Dongle sending network",   AnyView(ConnectView(situation: .stage(.car, .sendingNetwork)))),
("Step 4 finding car",       AnyView(ConnectView(situation: .stage(.car, .searching)))),
("Dongle configuring",       AnyView(ConnectView(situation: .stage(.car, .joining)))),
("Dongle join failed",       AnyView(ConnectView(situation: .stage(.car, .joinFailed), onRetry: {}))),
// Car stage
("Car checking",             AnyView(ConnectView(situation: .stage(.car, .seeking)))),
("Car fault",                AnyView(ConnectView(situation: .stage(.car, .fault)))),
("WrongCar",                 AnyView(ConnectView(situation: .stage(.car, .wrongDevice("esp32-car")), onRetry: {}))),
("Car rolled back",          AnyView(ConnectView(situation: .stage(.car, .rolledBack), onRetry: {}))),
("App behind (car)",         AnyView(ConnectView(situation: .stage(.car, .appBehind(proto: 3))))),
```

Добавить недостающие forced-кадры адаптера в блок `fw(...)` (после `("Adapter fw failed", …)` или рядом):

```swift
("Adapter fw forced",        fw(.available, forced: true, device: .dongle)),
("Adapter fw downloaded",    fw(.downloaded, device: .dongle)),
("Adapter fw flashed",       fw(.flashed, device: .dongle)),
("Adapter fw failed forced", fw(.failed, forced: true, device: .dongle)),
```

Убедиться, что каждый лейбл уникален и что удалены `("WrongProto", …)`.

- [ ] **Шаг 5: сборка + галерея**

Run: сборка приложения (Global Constraints).
Expected: успех, exit 0.
Установить, снять 3–4 новых кадра галереи: `xcrun simctl launch booted com.adamjohnson.ajmiddlecar --args -gallery -galleryIndex N` для «Car fault», «Wrong dongle», «Adapter fw forced», «WrongCar»; повернуть `sips -r -90`.
Expected: экраны рисуются, тексты на месте, у `wrongDevice`/`rolledBack`/`joinFailed` — кнопка «Повторить».

- [ ] **Шаг 6: хост-тесты**

Run: `tools/test-all.sh`
Expected: всё зелёное.

- [ ] **Шаг 7: коммит**

```bash
git add -A app/AJMiddleCar
git commit  # refactor(app): drop WrongCarView and the board-prefixed situations; gallery is a step×board matrix
```

---

### Задача 6: документы

**Files:**
- Modify: `CLAUDE.md` (раздел iOS)
- Modify: `docs/superpowers/specs/2026-09-17-version-endpoint-design.md` (пометка о замене §3)
- Modify: `docs/bringup.md` (пункты стенда)

**Interfaces:** только проза; код не трогаем.

- [ ] **Шаг 1: `CLAUDE.md`** — в разделе iOS, рядом с описанием запуска, добавить абзац:

```
Запуск — это лестница плат: массив `[адаптер, машинка]` (или `[машинка]` без адаптера) и одна
стадия `reach → /version → выпуск → правило → шаг` (`StageRule`, чистая, хост-тест
`app/tests/stagerule`). До машинки добираются через адаптер — `CarReach` (`app/tests/carreach`),
сетевой автомат с бюджетом попыток. `AppFlow` — проводка: фазы `Phase.stage(device, GateStep)`,
раннер `runLadder`/`restart`/`updateFinished`. Стражи связи после гейта (провод пропал, чужой
hello, чужой proto) не рисуют своих экранов, а перезапускают лестницу с нужной ступени
(`restart(from:)`). Один экран на всё — `ConnectView(.stage(device, step))`; `WrongCarView`
больше нет.
```

- [ ] **Шаг 2: спека `/version`** — в строке `**Статус:**` файла `2026-09-17-version-endpoint-design.md` дописать: `; §3 (лестница) заменена docs/superpowers/specs/2026-09-17-board-ladder-design.md`.

- [ ] **Шаг 3: `docs/bringup.md`** — в «Bench sequence» заменить два пункта про день-флаг/replug (сейчас они ссылаются на `S27`/`carGate`) на формулировки в терминах лестницы: «день-флаг без кабеля: обе платы 404 → адаптер `.stage(.dongle,.updating)` → машинка `.stage(.car,.updating)` → S28»; «адаптер выдернут посреди `.stage(.car,.updating)` → `.stage(.dongle,.absent)` → воткнуть → `.stage(.car,.searching/.joining)` → снова `.updating` → S28»; добавить пункт «после обновления машинки лестница показывает `.searching/.joining` (адаптер переподключается) до `.checking`».

- [ ] **Шаг 4: сборка контракта не нужна; проверить, что документы согласованы**

Run: `grep -rn "carGate\|dongleGate\|WrongCarView" CLAUDE.md docs/bringup.md`
Expected: пусто (старые имена не остались в этих двух файлах).

- [ ] **Шаг 5: коммит**

```bash
git add CLAUDE.md docs/superpowers/specs/2026-09-17-version-endpoint-design.md docs/bringup.md
git commit  # docs: the launch ladder — one stage per board, guards restart it
```

---

## После задач (шаги контроллера, не задачи плана)

- Пересобрать оба артефакта («Экраны AJMiddleCar», «Адаптер и машинка рядом») по новым кадрам галереи и перепубликовать.
- Обновить память `launch-ladder-2026-09` под лестницу плат.
- Стенд (`-viaDongle`): полная лестница, день-флаг обеих плат, выдёргивание адаптера посреди обновления машинки (пункты уже в `docs/bringup.md`).

## Self-review (заполняется автором плана)

- **Покрытие спеки:** §1 модель → Задачи 1,4; §2 CarReach/reach → Задачи 2,4; §3 экраны → Задачи 3,5; §4 проводка/стражи → Задача 4; §5 тесты → Задачи 1,2 (+ дым в 4,5); §6 проверка → Задачи 4,5; §7 документы → Задача 6; §8 «чего не делаем» → Global Constraints. Пробелов нет.
- **Типы:** `GateStep`/`Reach`/`Board.Identity`/`StageRule.Verdict` определены в Задаче 1 и используются с теми же сигнатурами в 2–5; `CarReach.Ask`/`next`/`retry` — Задача 2 → 4; `Phase.stage`/`restart(from:)`/`updateFinished(_:)`/`retryAction(for:)`/`retryJoin()` — Задача 4 → 5; `ConnectView.Situation.stage`/`onRetry` — Задача 3 → 4,5.
- **Заглушек нет:** каждый шаг несёт реальный код или точную команду. Единственное «по grep» — удаление осиротевших строк/аксессоров в Задаче 5, где точный список зависит от `stageTitle`/`stageSub`; правило удаления задано явно (удалять только при единственной ссылке).
