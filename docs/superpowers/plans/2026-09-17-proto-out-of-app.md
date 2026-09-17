# План: `proto` вон из решений приложения

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** приложение перестаёт принимать любые решения по `proto` (гейт, рантайм-сессия, экраны), опираясь на совместимость исключительно по выпуску (`fw/build` == тег релиза); байт `proto` на проводе не трогаем — приложение по-прежнему пишет его в исходящие датаграммы.

**Architecture:** две независимые цепочки удаления одной оси. Гейт: `VersionRule`/`StageRule`/`AppFlow` теряют `proto`, экран S32 «Приложение устарело» удаляется. Рантайм-сессия: `RTFrame`/`SessionPolicy`/`CarTransport`/`CarLink`/`LinkState` перестают сравнивать входящий `proto`, состояния `protoMismatch`/`wrongProto` удаляются. Плюс документы и TODO-инвариант.

**Tech Stack:** Swift/SwiftUI (XcodeGen), хост-тесты `swiftc` без XCTest, симулятор iPhone 17 против `tools/mock_car`.

**Spec:** `docs/superpowers/specs/2026-09-17-proto-out-of-app-design.md` (читать вместе с планом; при расхождении план уступает спеке).

## Global Constraints

- Проза в чате и документах — по-русски; код, комментарии и сообщения коммитов — по-английски; заголовки задач — «### Задача N:».
- **Провод и контракт не трогаем:** поле `proto` в `/version`, `hello_ack`, каждой rt- и видео-датаграмме, в конверте REST — остаётся. `contract/*.json`, генераторы, обе прошивки, мок, конформанс, `app/AJMiddleCar/Generated/*` **не меняются**.
- Приложение **продолжает писать** `proto` в исходящие фреймы (`RTFrame.hello/drive/bye/view`) — убираем только **чтение/сравнение** входящего `proto`.
- Константы `CarContract.proto` / `DongleContract.proto` **остаются** (ими пишутся исходящие фреймы). Поле `proto` в `DeviceVersion` **остаётся** (документ декодируется целиком, поле просто не читается).
- Никогда не редактировать генерируемые файлы.
- Сборка приложения: `cd app && xcodegen generate && xcodebuild build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle -quiet`
- Хост-тесты: `tools/test-all.sh`.
- Симулятор против мока: `nohup tools/mock_car/.venv/bin/python -u tools/mock_car/mock_car.py >/tmp/mock.log 2>&1 &`; установить `/tmp/ddata-middle/Build/Products/Debug-iphonesimulator/AJMiddleCar.app`; `xcrun simctl terminate booted com.adamjohnson.ajmiddlecar; xcrun simctl launch booted com.adamjohnson.ajmiddlecar`; скриншот `xcrun simctl io booted screenshot` (повернуть `sips -r -90`).
- Каждый коммит заканчивается трейлером:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ
  ```

---

### Задача 1: гейт без `proto` — `VersionRule`, `StageRule`, `AppFlow`, экран S32

**Files:**
- Modify: `app/AJMiddleCar/VersionRule.swift` (убрать `appProto`, proto-проверку, `VersionStep.appBehind`)
- Modify: `app/AJMiddleCar/StageRule.swift` (`Board.Identity` без `proto`; `decide` без `appProto`; `GateStep.appBehind` вон)
- Modify: `app/AJMiddleCar/AppFlow.swift` (`Board.Identity(...)` без `proto:`)
- Modify: `app/AJMiddleCar/ConnectView.swift` (ветки `.appBehind`)
- Modify: `app/AJMiddleCar/L.swift` (`.appBehind` в `stageTitle`/`stageSub`; аксессоры `appBehindTitle`/`appBehindSub`)
- Modify: `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings` (три строки `appBehind.*`)
- Modify: `app/AJMiddleCar/GalleryView.swift` (два кадра «App behind …»)
- Test: `app/tests/versionrule/main.swift`, `app/tests/stagerule/main.swift`

**Interfaces:**
- Consumes: `VersionReply`, `DeviceVersion`, `RollbackChoice`, `UpdateRules`.
- Produces: `VersionRule.step(reply:expectedDevice:latestTag:rollback:) -> VersionStep` (без `appProto`); `VersionStep` без `.appBehind`; `GateStep` без `.appBehind`; `Board.Identity(device:expectedDevice:silentStep:)` (без `proto`); `StageRule.decide(reach:version:board:latestTag:rollback:)` (без изменения сигнатуры — `proto` жил внутри `board`).

- [ ] **Шаг 1: обновить тест `app/tests/versionrule/main.swift`**

Убрать проверки `.appBehind` и параметр `appProto`. Локальный хелпер `step(...)` и раздел протокола становятся так (заменить строки 17–19 и раздел «-- protocol, only once …»):

```swift
func step(_ reply: VersionReply, rollback: RollbackChoice = .unanswered) -> VersionStep {
    VersionRule.step(reply: reply, expectedDevice: "ajdongle", latestTag: latest, rollback: rollback)
}
```

Удалить весь блок из трёх `check(...)` про `.appBehind(proto:)` (строки, начинающиеся с
`// -- protocol, only once …` и `check(step(doc(fw: current, proto: 2)) == .appBehind(proto: 2), …)`
и следующие два). `doc(...)` оставить как есть — поле `proto` в документе остаётся, оно просто
больше не влияет на вердикт; можно добавить одну строку, фиксирующую это:

```swift
check(step(doc(fw: current, proto: 2)) == .ok, "current build, any proto: ok — proto is not a gate input anymore")
```

- [ ] **Шаг 2: обновить тест `app/tests/stagerule/main.swift`**

`Board.Identity` теперь без `proto`; убрать кейс `.appBehind`. Заменить строки 11–12:

```swift
let dongle = Board.Identity(device: .dongle, expectedDevice: "ajdongle", silentStep: .absent)
let car    = Board.Identity(device: .car,    expectedDevice: "ajmiddlecar", silentStep: .seeking)
```

Удалить проверку `.appBehind` (строки 48–49: `check(decide(.reached, doc("ajmiddlecar", fw: "v1.0+200", proto: 3), car) == .show(.appBehind(proto: 3)), …)`). Добавить взамен:

```swift
check(decide(.reached, doc("ajmiddlecar", fw: "v1.0+200", proto: 3), car) == .ok,
      "current build, any proto: .ok — proto is no longer a gate input")
```

- [ ] **Шаг 3: убедиться, что тесты не компилируются**

Run: `tools/test-all.sh`
Expected: провал сборки хост-тестов (`versionrule`/`stagerule`) — `.appBehind` не существует / лишний `appProto`. (Это RED.)

- [ ] **Шаг 4: `VersionRule.swift` — убрать proto из правила**

В `enum VersionStep` удалить кейс:

```swift
    /// Not behind, but speaking a protocol this app does not: the board is newer than the app.
    /// Nothing this app can do about it except say so.
    case appBehind(proto: Int)
```

В `step(...)` убрать параметр `appProto` и последнюю проверку. Сигнатура и хвост становятся:

```swift
    public static func step(reply: VersionReply, expectedDevice: String, latestTag: String,
                            rollback: RollbackChoice) -> VersionStep {
```

и в конце (было `if UpdateRules.mustUpdate… ; guard v.proto == appProto … ; return .ok`):

```swift
        if UpdateRules.mustUpdate(carFw: v.fw, latestTag: latestTag) { return .updating }
        // Protocol is no longer a gate input: one release ships the app and both firmwares
        // together, so "build == the release tag" is the whole of compatibility. `v.proto` is
        // decoded but not read. See spec 2026-09-17-proto-out-of-app.
        return .ok
```

- [ ] **Шаг 5: `StageRule.swift` — `Board.Identity` без `proto`, `GateStep` без `.appBehind`**

В `enum GateStep` удалить кейс `case appBehind(proto: Int) …`.

В `Board.Identity` удалить поле `proto` и параметр из инициализатора:

```swift
    public struct Identity: Equatable {
        public let device: UpdateRules.Device
        public let expectedDevice: String
        public let silentStep: GateStep
        public init(device: UpdateRules.Device, expectedDevice: String, silentStep: GateStep) {
            self.device = device; self.expectedDevice = expectedDevice; self.silentStep = silentStep
        }
    }
```

В `decide(...)` убрать `appProto:` из вызова `VersionRule.step` и удалить ветку `.appBehind`:

```swift
        switch VersionRule.step(reply: version, expectedDevice: board.expectedDevice,
                                latestTag: latestTag ?? "", rollback: rollback) {
        case .plugIn: return .show(board.silentStep)
        case .faulty: return .show(.fault)
        case .accessDenied: return .show(.denied)
        case .wrongDevice(let name): return .show(.wrongDevice(name))
        case .rolledBack: return .show(.rolledBack)
        case .updating: return .show(.updating)
        case .ok: return .ok
        }
```

- [ ] **Шаг 6: `AppFlow.swift` — `Board.Identity(...)` без `proto:`**

В `dongleBoard()` и `carBoard(...)` убрать `proto:` из `Board(identity: .init(...))`:

```swift
    // dongleBoard:
    Board(identity: .init(device: .dongle, expectedDevice: DongleContract.device, silentStep: .absent),
    // carBoard:
    Board(identity: .init(device: .car, expectedDevice: CarContract.device, silentStep: .seeking),
```

- [ ] **Шаг 7: `ConnectView.swift` — убрать S32**

Удалить ветку `.appBehind` в `stageScene`, `title` (метод), `message` и `actionButton`. В `stageScene` строки:

```swift
    case .appBehind:
        DeviceScene(palette: p, rings: .deco, ringTint: p.warn,
                    chip: (glyph: "exclamationmark.arrow.circlepath", tint: p.warn)) { stageBody(d) }
```
— удалить. В `title`/`message` строки `case .appBehind: …` через `L.stageTitle`/`L.stageSub` уходят вместе с их удалением в `L.swift` (шаг 8); ветки `.appBehind` в switch по `GateStep` убрать. В `actionButton` `.appBehind` попадал в `default: EmptyView()` — отдельной ветки нет, менять не нужно.

Note: после удаления кейса `GateStep.appBehind` компилятор потребует убрать `.appBehind` из всех switch по `GateStep`; их четыре в `ConnectView`/`L` — убрать каждый.

- [ ] **Шаг 8: `L.swift` — убрать `.appBehind` и осиротевшие аксессоры**

В `stageTitle(_:_:)` удалить `case .appBehind: return appBehindTitle`. В `stageSub(_:_:)` удалить `case .appBehind(let proto): return appBehindSub(d, proto, …)`. Удалить аксессоры:

```swift
static var appBehindTitle: String { s("appBehind.title") }
static func appBehindSub(_ d: UpdateRules.Device, _ theirs: Int, _ ours: Int) -> String { s("appBehindSub.\(d.rawValue)", theirs, ours) }
```

(Grep-подтверждение: `grep -rn 'appBehindTitle\|appBehindSub' app/AJMiddleCar` должен показать только их определения — иначе оставить.)

- [ ] **Шаг 9: строки** — удалить из `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings`:

```
"appBehind.title"        = "Приложение устарело";
"appBehindSub.dongle"    = "Адаптер говорит на протоколе %d, а это приложение — на %d. Обнови приложение.";
"appBehindSub.car"       = "Машинка говорит на протоколе %d, а это приложение — на %d. Обнови приложение.";
```

- [ ] **Шаг 10: `GalleryView.swift`** — удалить два кадра:

```swift
("App behind (dongle)",      AnyView(ConnectView(situation: .stage(.dongle, .appBehind(proto: 2))))),
("App behind (car)",         AnyView(ConnectView(situation: .stage(.car, .appBehind(proto: 3))))),
```

- [ ] **Шаг 11: тесты и сборка зелёные**

Run: `tools/test-all.sh`
Expected: всё зелёное (`versionrule`, `stagerule` в т.ч.).
Run: сборка приложения (Global Constraints).
Expected: exit 0.

- [ ] **Шаг 12: коммит**

```bash
git add app/AJMiddleCar/VersionRule.swift app/AJMiddleCar/StageRule.swift app/AJMiddleCar/AppFlow.swift \
        app/AJMiddleCar/ConnectView.swift app/AJMiddleCar/L.swift \
        app/AJMiddleCar/Resources/ru.lproj/Localizable.strings app/AJMiddleCar/GalleryView.swift \
        app/tests/versionrule app/tests/stagerule
git commit  # refactor(app): the gate no longer reads proto; the S32 app-behind screen is gone
```

---

### Задача 2: рантайм-сессия без `proto` — `RTFrame`, `SessionPolicy`, `CarTransport`, `CarLink`, `LinkState`

**Files:**
- Modify: `app/AJMiddleCar/RTFrame.swift` (`parse` без proto-сравнения; `Inbound.protoMismatch` вон)
- Modify: `app/AJMiddleCar/SessionPolicy.swift` (`HandshakeOutcome.protoMismatch` вон)
- Modify: `app/AJMiddleCar/CarTransport.swift` (`Event.protoMismatch`, `Handshake.protoMismatch`, ветка handshake вон)
- Modify: `app/AJMiddleCar/CarLink.swift` (ветка `handle` `.protoMismatch` вон)
- Modify: `app/AJMiddleCar/LinkState.swift` (`SessionState.protoMismatch`, `Link.wrongProto`, `LinkRule.compose` строка)
- Modify: `app/AJMiddleCar/AJMiddleCarApp.swift` (страж `case .wrongProto`)
- Test: `app/tests/rtframe/main.swift`, `app/tests/sessionpolicy/main.swift`, `app/tests/carlink/main.swift`

**Interfaces:**
- Consumes: `DeviceInfo`, `CarContract.proto` (только для записи исходящих), `Telemetry`.
- Produces: `RTFrame.Inbound` без `.protoMismatch`; `SessionPolicy.HandshakeOutcome` без `.protoMismatch`; `CarTransport.Event`/`Handshake` без `.protoMismatch` (handshake отдаёт `DeviceInfo`); `SessionState` без `.protoMismatch`; `Link` без `.wrongProto`.

- [ ] **Шаг 1: тест `app/tests/rtframe/main.swift`**

Заменить проверку proto-mismatch (строки 51–52) на «любой proto разбирается как identity»:

```swift
check(RTFrame.parse(#"{"proto":3,"type":"hello_ack","session":"7f3a91c2","device":{"id":"ajmiddlecar","fw":"v9","build":9,"rolled_back":false}}"#)
        == .helloReply(sid: "7f3a91c2", device: DeviceInfo(id: "ajmiddlecar", fw: "v9", build: 9, rolled_back: false)),
      "a foreign proto is ignored — hello_ack parses as identity")
```

(Если в тесте есть проверка, что телеметрия с чужим `proto` даёт `nil` — заменить на: телеметрия с любым `proto` разбирается в `.telemetry`. Сверить `DeviceInfo`'s точную форму по `RTFrame`/`CarAPI`.)

- [ ] **Шаг 2: тест `app/tests/sessionpolicy/main.swift`**

Удалить обе проверки `protoMismatch` (строки 19–22). `ack(...)` больше не варьирует `proto` для этого; оставить фикстуру с `proto: 2`. Добавить, что чужой `proto` в hello_ack всё равно даёт identity:

```swift
check(SessionPolicy.handshakeOutcome(RTFrame.parse(ack(sid, proto: 3)), sid: sid)
        == .identity(DeviceInfo(id: "ajmiddlecar", fw: "v1.0+517", build: 517, rolled_back: false)),
      "a foreign proto is still our car by identity — proto is not judged here")
```

- [ ] **Шаг 3: тест `app/tests/carlink/main.swift`**

Удалить блок проверок `protoMismatch`/`wrongProto` (строки ~45–53 и раздел `survivingSessionEnd` для `.protoMismatch`, ~57–61). Оставить эквивалентные проверки для `.foreign`/`.wrongCar`, если их нет — добавить минимальную:

```swift
check(SessionState.foreign(device: "esp32-car").survivingSessionEnd == .foreign(device: "esp32-car"),
      "a foreign identity survives the session that found it")
```

- [ ] **Шаг 4: убедиться, что тесты падают**

Run: `tools/test-all.sh`
Expected: провал (`rtframe`/`sessionpolicy`/`carlink`) — `.protoMismatch`/`.wrongProto` ещё существуют или сигнатуры не совпали. (RED — часть проверок уже требует нового поведения `.helloReply`.)

- [ ] **Шаг 5: `RTFrame.swift` — `parse` игнорирует входящий `proto`**

В `enum Inbound` удалить `case protoMismatch(sid: String, theirs: Int)`. В `parse(...)` для `helloAck` и `telemetry` убрать proto-гейты (исходящие фреймы, что пишут `proto`, не трогать):

```swift
        switch type {
        case RTType.helloAck:
            guard let sid = j[CarContract.sessionField] as? String,
                  let ack = try? JSONDecoder().decode(HelloAck.self, from: data) else { return nil }
            // proto is on the wire but not judged here — see spec 2026-09-17-proto-out-of-app.
            return .helloReply(sid: sid, device: ack.device)
        case RTType.telemetry:
            guard let t = try? JSONDecoder().decode(Telemetry.self, from: data) else { return nil }
            return .telemetry(t)
        default:
            return nil
        }
```

(Строку `let theirs = j[CarContract.protoField] as? Int ?? 0` удалить — она больше не нужна.)

- [ ] **Шаг 6: `SessionPolicy.swift` — `HandshakeOutcome` без `.protoMismatch`**

```swift
    enum HandshakeOutcome: Equatable {
        case identity(DeviceInfo)
        case ignore
    }
    static func handshakeOutcome(_ inbound: RTFrame.Inbound?, sid: String) -> HandshakeOutcome {
        switch inbound {
        case .helloReply(let replySid, let device) where replySid == sid:
            return .identity(device)
        default:
            return .ignore
        }
    }
```

- [ ] **Шаг 7: `CarTransport.swift` — убрать proto-ветку handshake**

Удалить `case protoMismatch(theirs: Int)` из `Event` и из `Handshake`. `Handshake` теперь одноимённый одному кейсу — свернуть: `handshake(sid:)` и `awaitHello(sid:)` возвращают `DeviceInfo?`/`DeviceInfo`, `awaitHello` отдаёт `device` напрямую:

```swift
    private func awaitHello(sid: String) async throws -> DeviceInfo? {
        while !Task.isCancelled {
            guard let text = try await receiveOne() else { continue }
            switch SessionPolicy.handshakeOutcome(RTFrame.parse(text), sid: sid) {
            case .identity(let device): return device
            case .ignore: continue
            }
        }
        return nil
    }
```

и в вызывающем коде (было `switch try await handshake(sid:) { case .identity … case .protoMismatch …}`) — прямое присваивание, весь `.protoMismatch`-блок (`emit(.protoMismatch)`, `sayGoodbye`, `holdIdentity`, `throw CarError.malformed("protocol …")`) удаляется:

```swift
        guard let identity = try await handshake(sid: sid) else { throw CarError.refused }
        emit(.sessionOpened(identity, sid: sid))
```

(Сверить фактические имена: если `handshake(sid:)` — обёртка над `awaitHello`, привести обе к `DeviceInfo?`. `holdIdentity()`/`sayGoodbye(on:)` могут остаться используемыми `.foreign`-путём — не удалять, если на них есть другие ссылки; grep перед удалением.)

- [ ] **Шаг 8: `CarLink.swift` — убрать `handle` `.protoMismatch`**

Удалить весь `case .protoMismatch(let theirs):` в `handle(_:)` (присваивание `session = .protoMismatch(...)`, `video.sessionClosed()`). Комментарии, называющие `.protoMismatch` рядом (в `retryAfterWrongCar` и `.sessionClosed`), переформулировать на «чужая машинка» (`.foreign`) — `retryAfterWrongCar` остаётся (сбрасывает `.foreign`).

- [ ] **Шаг 9: `LinkState.swift` — убрать `protoMismatch`/`wrongProto`**

- Удалить `case protoMismatch(theirs: Int)` из `SessionState`; в `survivingSessionEnd` — `case .foreign: return self` (без `.protoMismatch`):

```swift
    var survivingSessionEnd: SessionState {
        switch self {
        case .foreign: return self
        case .none, .adopted: return .none
        }
    }
```
- Удалить `case wrongProto(theirs: Int)` из `Link`.
- В `LinkRule.compose` удалить строку `if case .protoMismatch(let theirs) = session { return .wrongProto(theirs: theirs) }`.

- [ ] **Шаг 10: `AJMiddleCarApp.swift` — страж без `.wrongProto`**

В `.onChange(of: link.state)`:

```swift
            .onChange(of: link.state) { _, new in
                switch new {
                case .noDongle, .localNetworkDenied: flow.restart(from: .dongle)
                case .wrongCar:                       flow.restart(from: .car)
                case .live:                           flow.carIdentified(fw: link.fw)
                case .searching:                      break
                }
            }
```

(Если `carRoot` ссылается на `.wrongProto` — он на `default`, менять не нужно; сверить.)

- [ ] **Шаг 11: тесты и сборка зелёные**

Run: `tools/test-all.sh`
Expected: всё зелёное.
Run: сборка приложения.
Expected: exit 0.

- [ ] **Шаг 12: дым на симуляторе**

Запустить мок, установить сборку, запустить приложение, скриншот → экран езды (S28). `MOCK_DEVICE=esp32-car` → S23 «Другая машинка». (Проверка, что чужая-машинка и езда живы без proto-веток.) Погасить мок. Если симулятор капризничает — сказать прямо, не выдавать за успех.

- [ ] **Шаг 13: коммит**

```bash
git add app/AJMiddleCar/RTFrame.swift app/AJMiddleCar/SessionPolicy.swift app/AJMiddleCar/CarTransport.swift \
        app/AJMiddleCar/CarLink.swift app/AJMiddleCar/LinkState.swift app/AJMiddleCar/AJMiddleCarApp.swift \
        app/tests/rtframe app/tests/sessionpolicy app/tests/carlink
git commit  # refactor(app): the runtime session no longer reads proto; protoMismatch/wrongProto gone
```

---

### Задача 3: документы и TODO-инвариант

**Files:**
- Modify: `CLAUDE.md` (раздел iOS)
- Modify: `docs/protocol.md` (одна фраза у поля `proto`)
- Modify: `docs/superpowers/specs/2026-09-17-version-endpoint-design.md`, `docs/superpowers/specs/2026-09-17-board-ladder-design.md` (пометки)
- Modify: `docs/bringup.md` (TODO-инвариант)

**Interfaces:** только проза; код не трогаем.

- [ ] **Шаг 1: `CLAUDE.md`** — в разделе iOS, рядом с абзацем про лестницу, добавить:

```
Совместимость держится на дисциплине выпуска: один релиз поставляет приложение и обе прошивки
вместе, поэтому «билд платы == тег релиза» и есть совместимость. Приложение НЕ решает по `proto` —
ни на гейте, ни в рантайм-сессии; `proto` остаётся байтом на проводе (приложение пишет его в
исходящие датаграммы, прошивка дропает чужой), но как версия формата, а не вход в решение.
```

- [ ] **Шаг 2: `docs/protocol.md`** — у поля/раздела `proto` дописать одну фразу:

```
Зачем `proto` на проводе: это версия формата. Её работа — на будущее: при изменении раскладки
датаграммы старый бинарь по `proto` её отвергнет, а не неверно разберёт. Приложение по `proto`
не решает (совместимость — по выпуску); прошивка дропает датаграмму с чужим `proto`.
```

- [ ] **Шаг 3: две прошлые спеки** — дописать в их «Статус»/начало по строке:

- `2026-09-17-version-endpoint-design.md`: «proto-ось из решений приложения снята
  `2026-09-17-proto-out-of-app-design.md` (`VersionRule` без `appProto`, S32 удалён; поле `proto`
  в `/version` остаётся)».
- `2026-09-17-board-ladder-design.md`: «`GateStep.appBehind`/S32 и `Link.wrongProto` удалены
  `2026-09-17-proto-out-of-app-design.md`».

- [ ] **Шаг 4: `docs/bringup.md`** — добавить TODO-инвариант в подходящий список:

```
- [ ] **Самопроверка версии приложения при старте** (когда приложение попадёт в App Store):
      сейчас совместимость держится на инварианте «приложение всегда последнее» (собирается из
      исходников). Когда появится распространяемый бинарь, приложение должно гейтить СВОЮ версию
      на старте — это настоящее место защиты «устаревшее приложение + новая прошивка», которую
      раньше слабо ловил `proto`/S32 (спека `2026-09-17-proto-out-of-app-design.md`).
```

- [ ] **Шаг 5: проверка согласованности**

Run: `grep -rn "appBehind\|wrongProto\|protoMismatch\|appProto" app/AJMiddleCar CLAUDE.md docs/protocol.md`
Expected: пусто (кроме, возможно, `docs/protocol.md`'s общего упоминания `proto` — но не `appBehind`/`wrongProto`/`protoMismatch`).

- [ ] **Шаг 6: коммит**

```bash
git add CLAUDE.md docs/protocol.md docs/superpowers/specs/2026-09-17-version-endpoint-design.md \
        docs/superpowers/specs/2026-09-17-board-ladder-design.md docs/bringup.md
git commit  # docs: compatibility rests on the release; proto stays on the wire as a format tag
```

---

## После задач (шаги контроллера, не задачи плана)

- Пересобрать оба артефакта (S32 «Приложение устарело» уходит из каталога и из строки «протокол») и перепубликовать.
- Обновить память `launch-ladder-2026-09` (proto больше не вход в решение; S32 удалён).

## Self-review (заполняется автором плана)

- **Покрытие спеки:** §1.1 гейт → Задача 1; §1.2 лестница → Задача 1; §1.3 рантайм-сессия → Задача 2; §1.4 экраны → Задача 1 (S32) + Задача 2 (страж `.wrongProto`); §2 «не трогаем» → Global Constraints + явные «не удалять» пометки; §3 строки/галерея → Задача 1; §4 тесты → Задачи 1–2; §5 документы/TODO → Задача 3; §6 «не делаем» → Global Constraints. Пробелов нет.
- **Типы:** `VersionStep`/`GateStep` теряют `.appBehind` (Задача 1) — консистентно во всех switch; `Board.Identity` без `proto` (Задача 1) — вызовы в `AppFlow` и тестах правятся там же; `Inbound`/`HandshakeOutcome`/`Event`/`Handshake`/`SessionState`/`Link` теряют proto-кейсы (Задача 2) — все консьюмеры в той же задаче; `handshake`/`awaitHello` → `DeviceInfo?` консистентно.
- **Заглушек нет:** каждый шаг несёт конкретный код или точную команду; «сверить grep» — там, где точный список зависит от других ссылок (аксессоры, `holdIdentity`/`sayGoodbye`), с явным правилом «удалять только при единственной ссылке».
