# План: «другая машинка» — только в гейте, без рантайм-стража

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** снять рантайм-полицию личности машинки (гейт сессии в `CarTransport`, ветка чужой в `CarLink`, `SessionState.foreign`/`Link.wrongCar`, страж и удержание) — любой `hello` усыновляется; личность проверяет только гейт по `/version`.

**Architecture:** одна цепочка удаления в рантайм-слое связи. Флеш-безопасность остаётся: чужую плату отсекает `VersionRule` (сверяет `device` до `.updating`). Плюс правка документов.

**Tech Stack:** Swift/SwiftUI (XcodeGen), хост-тесты `swiftc` без XCTest, симулятор iPhone 17 против `tools/mock_car`.

**Spec:** `docs/superpowers/specs/2026-09-17-wrong-car-runtime-out-design.md` (читать вместе с планом; при расхождении план уступает спеке).

## Global Constraints

- Проза в чате/документах — по-русски; код, комментарии, сообщения коммитов — по-английски; заголовки задач — «### Задача N:».
- **Оставляем нетронутым:** гейтную проверку личности — `VersionRule.step` сверяет `device` первым; экран `ConnectView.Situation` `.stage(_, .wrongDevice)` (S23 машинки, S9 адаптера); строки `wrongCar.title/sub/hint`, `dongle.wrongTitle/wrongSub` и их аксессоры `L.wrongCarTitle/wrongCarSub/wrongCarHint`, `L.dongleWrongTitle/dongleWrongSub`. Метод `CarTransport.sayGoodbye(on:)` (его зовёт `requestStop(graceful:)`).
- **Не трогаем** провод, контракт, прошивки, мок, генераторы (`app/AJMiddleCar/Generated/*`). Поля `device` в `hello_ack`/`/version` остаются — приложение просто не судит по ним в рантайме.
- Принятый остаточный кейс: чужая машинка с актуальной сборкой, подменившая нашу посреди сессии, будет ехать (не заливаться) с чужой калибровкой.
- Сборка приложения: `cd app && xcodegen generate && xcodebuild build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle -quiet`
- Хост-тесты: `tools/test-all.sh`.
- Симулятор против мока: `nohup tools/mock_car/.venv/bin/python -u tools/mock_car/mock_car.py >/tmp/mock.log 2>&1 &`; установить `/tmp/ddata-middle/Build/Products/Debug-iphonesimulator/AJMiddleCar.app`; `xcrun simctl terminate booted com.adamjohnson.ajmiddlecar; xcrun simctl launch booted com.adamjohnson.ajmiddlecar`; скриншот `xcrun simctl io booted screenshot` (повернуть `sips -r -90`).
- Каждый коммит заканчивается трейлером:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ
  ```

---

### Задача 1: снять рантайм-стража «другая машинка»

**Files:**
- Modify: `app/AJMiddleCar/CarTransport.swift` (гейт сессии; `holdIdentity`/`identityHold`/`retryNow`)
- Modify: `app/AJMiddleCar/CarLink.swift` (ветка чужой машинки; два `survivingSessionEnd`; `retryAfterWrongCar`)
- Modify: `app/AJMiddleCar/LinkState.swift` (`SessionState.foreign`, `survivingSessionEnd`, `Link.wrongCar`, строка `LinkRule.compose`)
- Modify: `app/AJMiddleCar/SessionPolicy.swift` (`identityHoldSeconds`)
- Modify: `app/AJMiddleCar/AJMiddleCarApp.swift` (страж `.wrongCar`; вызов `retryAfterWrongCar`)
- Test: `app/tests/carlink/main.swift`, `app/tests/sessionpolicy/main.swift`

**Interfaces:**
- Consumes: `DeviceInfo`, `CarContract.device`, `LinkRule.compose`, `PathState`, `Telemetry`.
- Produces: `SessionState` без `.foreign` и без `survivingSessionEnd`; `Link` без `.wrongCar`; `CarTransport` без `retryNow`/`holdIdentity`/`identityHold`; `CarLink` без `retryAfterWrongCar`; `SessionPolicy` без `identityHoldSeconds`.

- [ ] **Шаг 1: тест `app/tests/carlink/main.swift`**

Заменить блок про чужую машинку и `survivingSessionEnd` на «чужой hello усыновляется». Удалить строки:

```swift
check(compose(.localNetworkDenied, .foreign(device: "esp32-car"), nil, nil) == .localNetworkDenied,
      "denial outranks a foreign car")

// Identity outranks liveness: a car that answers with someone else's name is never driven.
check(compose(.dongleUp, .foreign(device: "esp32-car"), fresh, 0.1) == .wrongCar(device: "esp32-car"),
      "wrong car, however fresh")

// What survives a session ending. Two callers ask this — the session closing under the transport,
// and `stop(graceful:)` when the scene leaves `.active` — a foreign identity is not a transient
// failure to retry behind a radar sweep, so it must survive both.
check(SessionState.foreign(device: "esp32-car").survivingSessionEnd == .foreign(device: "esp32-car"),
      "a foreign identity survives the session that found it")
check(adopted.survivingSessionEnd == .none, "an adopted session does not")
check(SessionState.none.survivingSessionEnd == .none, "nor does nothing at all")
// Which is the whole point: the survivor keeps its own screen across the restart.
check(compose(.dongleUp, SessionState.foreign(device: "esp32-car").survivingSessionEnd, nil, nil)
        == .wrongCar(device: "esp32-car"), "the wrong-car screen holds across a session end")
```

и добавить вместо них:

```swift
// Identity is no longer judged in the live session — the gate's /version check (S23) is the one
// identity gate. A hello from any car adopts the session; a foreign car is driven, not screened
// (a mid-session swap on a colliding SSID is the accepted residual — spec wrong-car-runtime-out).
let foreignAdopted = SessionState.adopted(device: "esp32-car", fw: "v1.0+1")
check(compose(.dongleUp, foreignAdopted, fresh, 0.1) == .live(fresh),
      "any hello adopts: a foreign car is live, not a wrong-car screen")
```

- [ ] **Шаг 2: тест `app/tests/sessionpolicy/main.swift`**

Удалить строку:

```swift
check(SessionPolicy.identityHoldSeconds == 10, "the wrong-car hold is ten seconds")
```

- [ ] **Шаг 3: убедиться, что тесты падают**

Run: `tools/test-all.sh`
Expected: провал сборки хост-тестов (`carlink`/`sessionpolicy`) — `.foreign`/`.wrongCar`/`survivingSessionEnd`/`identityHoldSeconds` ещё существуют или новый кейс требует `.live`. (RED.)

- [ ] **Шаг 4: `LinkState.swift` — убрать `.foreign`, `survivingSessionEnd`, `Link.wrongCar`**

В `enum SessionState` удалить кейс `case foreign(device: String)` (с его комментарием) и весь `var survivingSessionEnd { … }`. Итог:

```swift
enum SessionState: Equatable {
    case none
    case adopted(device: String, fw: String)
}
```

В `enum Link` удалить `case wrongCar(device: String)`.

В `LinkRule.compose` удалить строку:

```swift
        if case .foreign(let device) = session { return .wrongCar(device: device) }
```

(порядок остаётся: `path` → `guard case .adopted else return .searching` → live).

- [ ] **Шаг 5: `SessionPolicy.swift` — убрать `identityHoldSeconds`**

Удалить `static let identityHoldSeconds: Double = 10` (и его doc-комментарий, если есть).

- [ ] **Шаг 6: `CarTransport.swift` — усыновлять любой ответ, снять удержание**

В `session()` удалить блок отказа чужой машинке (после `emit(.sessionOpened(identity, sid: sid))`):

```swift
        guard identity.id == CarContract.device else {
            await sayGoodbye(on: socket)
            await holdIdentity()
            throw CarError.malformed("foreign device \(identity.id)")
        }

```

— так что сразу за `emit(...)` идёт `sessionAdopted = true; everAdopted = true; …`.

Удалить метод `holdIdentity()`, свойство `identityHold` и метод `retryNow()` (они обслуживали только удержание чужой личности). `sayGoodbye(on:)` **оставить** — его зовёт `requestStop(graceful:)`.

- [ ] **Шаг 7: `CarLink.swift` — усыновлять любой hello; убрать `survivingSessionEnd` и `retryAfterWrongCar`**

В `handle(.sessionOpened)` заменить ветвление по `device` на безусловное усыновление:

```swift
        case .sessionOpened(let info, let sid):
            self.device = info.id
            lastTelemetrySeq = nil
            self.fw = info.fw
            session = .adopted(device: info.id, fw: info.fw)
            fetchRadio()
            config?.prefetchDriveGeometry()
            video.session(sid: sid)
```

(Сохранить существующие имена вокруг: `lastTelemetrySeq = nil`, `fetchRadio()`, `config?.prefetchDriveGeometry()`, `video.session(sid: sid)` — как в прежней «своей» ветке.)

В `handle(.sessionClosed)` заменить `session = session.survivingSessionEnd` на `session = .none`.

В обработчике остановки (там второй `session = session.survivingSessionEnd`, рядом с комментарием про уход в фон / Control Center) — тоже заменить на `session = .none`; комментарий переписать (личность больше не «переживает» конец сессии).

Удалить метод `retryAfterWrongCar()` целиком.

- [ ] **Шаг 8: `AJMiddleCarApp.swift` — убрать страж `.wrongCar` и вызов `retryAfterWrongCar`**

В `.onChange(of: link.state)` удалить `case .wrongCar: flow.restart(from: .car)`; switch остаётся исчерпывающим:

```swift
            .onChange(of: link.state) { _, new in
                switch new {
                case .noDongle, .localNetworkDenied: flow.restart(from: .dongle)
                case .live:                          flow.carIdentified(fw: link.fw)
                case .searching:                     break
                }
            }
```

В `.onChange(of: flow.phase) == .awaitingCar` убрать `link.retryAfterWrongCar()`:

```swift
            .onChange(of: flow.phase) { _, phase in
                if phase == .awaitingCar {
                    if link.isLive { flow.carIdentified(fw: link.fw) }
                }
            }
```

Комментарий над этим обработчиком, где упомянут `retryAfterWrongCar()`/`WrongCarView`, переписать: «страж чужой машинки снят — личность судит гейт по `/version`».

- [ ] **Шаг 9: тесты, сборка, дым**

Run: `tools/test-all.sh` → всё зелёное (`carlink`/`sessionpolicy` в т.ч.).
Run: сборка приложения (Global Constraints) → exit 0.
Дым: запустить мок, установить, запустить приложение → экран езды (S28). Затем перезапустить мок с `MOCK_DEVICE=esp32-car`, перезапустить приложение → ожидаем **S23 «Другая машинка» из гейта** (по `/version`, до открытия сессии — экран не пропал). Погасить мок. Если симулятор капризничает — сказать прямо.

- [ ] **Шаг 10: проверка согласованности**

Run: `grep -rn "\.foreign\|wrongCar(\|Link.wrongCar\|holdIdentity\|identityHold\|survivingSessionEnd\|retryAfterWrongCar\|identityHoldSeconds\|retryNow" app/AJMiddleCar`
Expected: пусто **кроме** `wrongCar.title/sub/hint`-строк и аксессоров `L.wrongCarTitle/wrongCarSub/wrongCarHint` (гейтные — остаются). То есть в выводе допустимы только упоминания `wrongCarTitle`/`wrongCarSub`/`wrongCarHint`/строк `wrongCar.` ; ни `SessionState.foreign`, ни `Link.wrongCar`, ни `retryAfterWrongCar`, ни `holdIdentity`/`identityHold`/`retryNow`/`identityHoldSeconds`/`survivingSessionEnd` быть не должно.

- [ ] **Шаг 11: коммит**

```bash
git add app/AJMiddleCar/CarTransport.swift app/AJMiddleCar/CarLink.swift app/AJMiddleCar/LinkState.swift \
        app/AJMiddleCar/SessionPolicy.swift app/AJMiddleCar/AJMiddleCarApp.swift \
        app/tests/carlink app/tests/sessionpolicy
git commit  # refactor(app): the live session no longer polices car identity; the gate's /version is the one check
```

---

### Задача 2: документы

**Files:**
- Modify: `CLAUDE.md` (раздел iOS)
- Modify: `docs/protocol.md` (фраза у `hello_ack`/`device`)
- Modify: `docs/superpowers/specs/2026-09-17-board-ladder-design.md`, `docs/superpowers/specs/2026-09-17-proto-out-of-app-design.md` (пометки)

**Interfaces:** только проза; код не трогаем.

- [ ] **Шаг 1: `CLAUDE.md`** — в раздел iOS, рядом с абзацем про совместимость-по-выпуску, добавить:

```
Личность машинки проверяется один раз — при коннекте, в гейте по `/version` (`VersionRule`
сверяет `device` первым → S23 «Другая машинка»). Живая UDP-сессия личность не переспрашивает:
`CarLink` усыновляет любой ответивший `hello`. Залить прошивку в чужую плату всё равно нельзя —
заливка идёт только через `/version`-путь, где `device` сверяется до OTA.
```

- [ ] **Шаг 2: `docs/protocol.md`** — у `hello_ack`/поля `device` дописать:

```
После гейта приложение по `device` не судит: живую сессию открывает любой ответивший `hello`;
чужую машинку отсекает гейт по `GET /version` до открытия сессии (там `device` сверяется первым).
```

- [ ] **Шаг 3: две спеки** — дописать по строке:

- `2026-09-17-board-ladder-design.md`: «рантайм-страж «другая машинка» (`SessionState.foreign`/`Link.wrongCar`) снят `2026-09-17-wrong-car-runtime-out-design.md`; личность судит только гейт по `/version`».
- `2026-09-17-proto-out-of-app-design.md`: «в том же духе снят и рантайм-страж личности — `2026-09-17-wrong-car-runtime-out-design.md`».

- [ ] **Шаг 4: проверка**

Run: `grep -rn "wrongCar\|foreign\|retryAfterWrongCar\|identityHold" CLAUDE.md docs/protocol.md`
Expected: нет упоминаний снятых рантайм-символов как существующих (в `protocol.md` допустима лишь новая фраза про то, что приложение по `device` не судит).

- [ ] **Шаг 5: коммит**

```bash
git add CLAUDE.md docs/protocol.md docs/superpowers/specs/2026-09-17-board-ladder-design.md \
        docs/superpowers/specs/2026-09-17-proto-out-of-app-design.md
git commit  # docs: car identity is checked once at connect; the live session no longer re-checks it
```

---

## После задач (шаги контроллера, не задачи плана)

- Пересобрать оба артефакта: строка «После гейта: стражи по связи» — у машинки убрать «другая машинка» из **рантайм**-стражей (S23 остаётся только гейтным, при коннекте), пометить, что рантайм личность не переспрашивает.
- Обновить память `launch-ladder-2026-09` (рантайм-страж личности снят).

## Self-review (заполняется автором плана)

- **Покрытие спеки:** §1.1 CarTransport → Задача 1 шаг 6; §1.2 CarLink → шаг 7 (оба `survivingSessionEnd` + `retryAfterWrongCar`); §1.3 LinkState → шаг 4; §1.4 SessionPolicy → шаг 5; §1.5 AJMiddleCarApp → шаг 8; §2 «что остаётся» → Global Constraints + шаг 10 grep-исключения; §3 тесты → шаги 1–2; §4 документы → Задача 2; §5 «не делаем» → Global Constraints. Пробелов нет.
- **Типы:** `SessionState` теряет `.foreign`+`survivingSessionEnd`, `Link` теряет `.wrongCar` (шаг 4) — все консьюмеры (`LinkRule.compose`, `CarLink`, `AJMiddleCarApp`, тесты) правятся в той же задаче; `CarTransport` теряет `retryNow`/`holdIdentity`/`identityHold` (шаг 6), их единственный внешний вызов — из `retryAfterWrongCar` (удаляется шаг 7); `SessionPolicy.identityHoldSeconds` (шаг 5), единственный вызов — в `holdIdentity` (удаляется шаг 6).
- **Заглушек нет:** каждый шаг несёт конкретный код/команду; grep-исключения для гейтных строк заданы явно (шаг 10).
