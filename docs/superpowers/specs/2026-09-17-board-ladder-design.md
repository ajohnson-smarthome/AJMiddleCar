# Лестница плат — одна стадия на адаптер и машинку

**Статус:** утверждён в брейншторме 2026-09-17; план — `docs/superpowers/plans/2026-09-17-board-ladder.md`; `GateStep.appBehind`/S32 и `Link.wrongProto` удалены `2026-09-17-proto-out-of-app-design.md`.
**Опирается на:** `docs/superpowers/specs/2026-09-17-version-endpoint-design.md` (замороженный
`GET /version`, одно правило `VersionRule` на обе платы, S30–S32 у машинки);
`docs/superpowers/specs/2026-09-16-one-release-gate-design.md` (один запрос релиза за запуск);
`docs/superpowers/specs/2026-06-14-forced-update-gate-design.md` (принудительное обновление без
выхода) и заголовок `FirmwareFlow.swift` («одна машина фаз на обе платы, плата — четыре
замыкания») — образец, который эта спека распространяет на лестницу.
**Заменяет:** ладдер из `2026-09-17-version-endpoint-design.md` §3 (два цикла `dongleGate()` /
`carGate()`, фазы с приставкой платы, `WrongCarView` после гейта). Провод, контракт, прошивки,
мок и конформанс не меняются.

## Зачем

После `/version` у обеих плат один документ личности и одно правило — а лестница запуска всё ещё
написана дважды: `dongleGate()` (~130 строк) и `carGate()` (~50) повторяют одну структуру
«прочитал `/version` → выпуск, если тега нет → правило → фаза → пауза»; фазы `AppFlow` носят
приставку платы (`.dongleWrong` / `.carWrong`, `.dongleRolledBack` / `.carRolledBack`,
`.dongleUpdating` / `.updateRequired` …); один и тот же вердикт правила рисуется двумя семьями
экранов (`ConnectView` у адаптера, `WrongCarView` у машинки), а у машинки нет экрана «ответила не
документом» — сломанная машинка выглядит как загружающаяся. Сравнение по шагам — артефакт
«Адаптер и машинка рядом» (строки «несимметрично»).

Между тем лестница у обеих плат одна: **найти → добраться → проверить версию → узнать выпуск →
обновить → дальше**. У адаптера «найти/добраться» — ответил ли `/version` по USB; у машинки —
адаптер вошёл в её сеть (S12/S13/S29/S14 — это адаптер *ищет машинку*, экраны так и называются).
Дальше всё совпадает. `FirmwareFlow` это уже доказал: одна машина фаз, плата — описание из
замыканий. Эта спека доводит тот же приём до лестницы целиком.

Три решения, принятые в брейншторме и здесь обязательные:

1. **Сетевой шаг адаптера — начало стадии машинки**, не конец стадии адаптера. После
   принудительного обновления машинки лестница честно показывает «ищу / подключаю», пока
   адаптер переподключается к перезагрузившейся машинке, а не «проверяю машинку» поверх тишины.
2. **Стражи после гейта перезапускают лестницу с нужной ступени**, а не рисуют свои экраны:
   провод пропал → стадия адаптера; чужой hello / чужой proto / fw младше тега → стадия машинки,
   где правило решает по `/version`. `WrongCarView` удаляется; S24 «другая версия протокола»
   исчезает в пользу вердикта правила (`.updating` — машинка старше, `.appBehind` — новее).
3. **Одно семейство экранов**: `ConnectView(.stage(device, step))`; кнопки одинаковые у обеих плат
   (`wrongDevice` и `rolledBack` — «Повторить», `joinFailed` — «Повторить», `denied` — «Открыть
   настройки», `appBehind` — без кнопки).

## 1. Модель

### 1.1 Плата

```swift
/// One rung of the launch ladder: everything the shared stage needs to know about a board.
struct Board {
    let device: UpdateRules.Device            // .dongle / .car — copy, artwork, reboot window
    let expectedDevice: String                // DongleContract.device / CarContract.device
    let proto: Int                            // DongleContract.proto / CarContract.proto
    /// What silence on `/version` means here: the adapter needs a person (.absent), the car
    /// behind a joined adapter is booting (.seeking).
    let silentStep: GateStep
    let readVersion: () async throws -> Data  // DongleClient.versionData() / CarTransport.get(versionPath, timeout: 2)
    let reach: () async -> Reach              // how to get to it before asking; see §2
}

/// What `Board.reach()` says about getting to the board this poll.
enum Reach: Equatable {
    case reached            // ask it
    case hold(GateStep)     // not yet: show this step, poll again
    case lost               // the board this one is reached through is gone: back one rung
}
```

Лестница: `CarHost.viaDongle ? [adapter, car(via: adapter)] : [car(direct)]`. Адаптер и машинка
напрямую (мок): `reach = { .reached }`. Машинка через адаптер: `reach` строится из `CarReach` (§2).

Тайминги остаются данными платы и не унифицируются: `/version` адаптера — 3 с
(`DongleClient.get` по умолчанию), машинки — 2 с; окно перезагрузки — `UpdateRules.rebootWindow`
(30 / 60 с); опрос — `donglePollInterval` 1,5 с, один на всех.

### 1.2 Шаг и фаза

```swift
/// One board's step of the ladder — the same words for both boards, the copy chosen by device.
enum GateStep: Equatable {
    case seeking                 // asking, nothing answered yet (adapter S1; car S30 while it boots)
    case absent                  // nothing answers and a person is needed (adapter S2)
    case checking                // answered (a document or 404), being looked over — once per stage (S3 / S30)
    case fault                   // answered with something that is not the document (S7 / S33 new)
    case denied                  // iOS refused local-network access (S8)
    case wrongDevice(String)     // /version.device is not ours (S9 / S23)
    case rolledBack              // /version.rolled_back (S10 / S31)
    case updating                // behind the release, or 404: FirmwareView(forced) (S11 / S27)
    case appBehind(proto: Int)   // current, but speaks a protocol this app does not (S32)
    // Reaching a board through another one — the car through the adapter (S12 / S13 / S29 / S14).
    case sendingNetwork, searching, joining, joinFailed
}

enum Phase: Equatable {
    case stage(UpdateRules.Device, GateStep)
    case releaseCheck                                          // S4
    case releaseOffline                                        // S5
    case releaseMissing(tag: String, device: UpdateRules.Device) // S6
    case awaitingCar                                           // the ladder is done; CarLink owns the screen
    case ready                                                 // carIdentified said drive
}
```

`Phase.opensLink` — только `.awaitingCar` и `.ready`, как сейчас. Шесть случаев вместо
двадцати пяти; ни одного с приставкой платы.

### 1.3 Стадия — один цикл на все платы

Каждая итерация, в этом порядке:

1. **Запрос перезапуска.** Если раннеру велено уйти на ступень раньше (§4) — `return .lost`.
2. **Парковка.** Если фаза `.stage(device, .updating)` — `FirmwareView` владеет платой: не
   опрашивать, ждать `updateFinished(device)` или запроса перезапуска (§4). Опрос во время
   парковки запрещён: перезагрузка машинки по OTA роняет её точку доступа, адаптер уходит в
   `searching`, и опрос снёс бы экран посреди прошивки.
3. **`reach()`** — пока плата не достигнута: в начале стадии и снова после каждой тишины
   `/version` (§2.2); плата, ответившая в прошлый раз, считается достигнутой и не переспрашивается.
   `.hold(step)` → `setPhase(.stage(device, step))`, пауза, снова; `.lost` → `return .lost`;
   `.reached` → дальше.
4. **`/version`** через `board.readVersion` → `VersionReply` (`.version / .absent / .silent /
   .faulty / .denied`, как сейчас).
5. **Ответила (документ или 404) — один раз за стадию `setPhase(.stage(device, .checking))`**
   (сегодняшний `sawDongle`; `PhasePacer` даёт кадру его 0,4 с).
6. **Выпуск.** Ответила и `latestTag == nil` → `fetchRelease(for: device)`; не получилось —
   пауза, снова (фазы S4/S5/S6 — как сейчас, общие). У мока порядок становится тем же, что у
   адаптера: сначала ответ платы, потом выпуск (сегодня `releaseGate()` идёт до S30).
7. **Правило** `VersionRule.step(reply:expectedDevice:latestTag:appProto:rollback:)` → шаг:

   | Вердикт | Шаг | Примечание |
   |---|---|---|
   | `.plugIn` | `board.silentStep` | адаптер `.absent` (и бюджет `CarReach` сбрасывается — вернувшийся адаптер всё забыл); машинка `.seeking`, на следующей итерации `reach()` спросит адаптер снова |
   | `.faulty` | `.fault` | у машинки — новый экран S33 |
   | `.accessDenied` | `.denied` | |
   | `.wrongDevice(name)` | `.wrongDevice(name)` | |
   | `.rolledBack` | `.rolledBack` | `consumeRollbackRecheck()` |
   | `.updating` | `.updating` | `consumeRollbackRecheck()`; парковка |
   | `.appBehind(proto)` | `.appBehind(proto:)` | |
   | `.ok` | — | `return .ok` |

8. `setPhase(.stage(device, step))`, `pollPause()`.

Решающая часть — пп. 3, 6, 7 — чистая функция:

```swift
enum StageRule {
    enum Verdict: Equatable {
        case show(GateStep)   // set the phase, pause, poll again
        case needRelease      // the board answered and the tag is unknown: fetch it, then poll again
        case ok
        case lost
    }
    /// One poll's decision. `version` is nil when `reach` did not say `.reached` (nothing was asked).
    static func decide(reach: Reach, version: VersionReply?, board: Board.Identity,
                       latestTag: String?, rollback: RollbackChoice) -> Verdict
}
```

`Board.Identity` — `device`, `expectedDevice`, `proto`, `silentStep` (данные платы без
замыканий, чтобы тест не строил транспорт). `AppFlow` остаётся проводкой: фазы, паузы,
парковка, «`.checking` один раз», сброс бюджета.

## 2. Стадия машинки: «добраться» через адаптер

### 2.1 `CarReach` — чистый автомат с бюджетом

Сегодняшний хвост `dongleGate()` (после `.ok`: `/status` → `DongleLink.next` → `askDongleToJoin`)
переезжает в стадию машинки как **её `reach()`**, без изменения поведения. Логика — чистая
структура; HTTP остаётся в замыкании.

```swift
/// Reaching the car through the adapter: the adapter's network state machine, with the
/// join budget. Pure over what the adapter's /status said; the asks it returns are the
/// caller's to send.
struct CarReach {
    enum Ask: Equatable { case configure, retry }     // POST /wifi ... / ... retry
    private(set) var attempts = 0
    private(set) var gaveUp = false

    /// One poll of the adapter's /status → what to show and what to send.
    mutating func next(_ reply: DongleStatusReply, expectedSSID: String) -> (reach: Reach, ask: Ask?)
    /// «Повторить» on .joinFailed: a fresh budget, spent from the next sendCredentials/retryJoin.
    mutating func retry()
}
```

| `/status` адаптера | `DongleLink.next` | `next` возвращает |
|---|---|---|
| документ; сеть не машинки или пустая | `.sendCredentials` | бюджет есть → `(.hold(.sendingNetwork), .configure)`; кончился → `(.hold(.joinFailed), nil)` |
| документ; `searching` | `.searchingCar` | `(.hold(.searching), nil)` |
| документ; `joining` / незнакомое | `.waiting` | `(.hold(.joining), nil)` |
| документ; `failed` / `idle` | `.retryJoin` | бюджет есть → `(.hold(.searching), .retry)` (как сейчас: «ищу», не «нет связи», пока попытка ещё должна); кончился → `(.hold(.joinFailed), nil)` |
| документ; `connected` | `.readyForCar` | `(.reached, nil)`, бюджет сброшен |
| тишина | — | `(.lost, nil)` — адаптер выдернут |
| не документ / запрет сети | — | `(.lost, nil)` — это вердикт стадии адаптера (S7 / S8), пусть она его и покажет |

Бюджет — `maxDongleJoinAttempts` (сегодня 1), `attempts`/`gaveUp` как сейчас; `retry()` — то, что
делает `retryDongleJoin()`. `DongleStatusReply` (`.status / .silent / .faulty / .denied`) из
приватного типа `AppFlow` становится публичным рядом с `CarReach` (тот же `DongleLink.swift`
или новый `CarReach.swift`; в хост-тест `donglelink` он не входит — у `CarReach` свой).

`CarReach` создаётся заново при входе в стадию машинки (поэтому после любого возврата на стадию
адаптера бюджет свежий — сегодняшнее «`.plugIn` сбрасывает бюджет» получается само) и хранится в
`AppFlow`, пока стадия идёт: «Повторить» на `.joinFailed` (`retryJoin()`) должна дотянуться до
живого экземпляра.

### 2.2 Когда `reach()` зовётся

- В начале стадии — до `.reached` (сетевой шаг, S12/S13/S29/S14).
- Дальше — только на тишину `/version`: машинка ответила → адаптер очевидно в её сети, лишний
  `/status` на каждый опрос не нужен. Тишина при `.reached` → `.seeking` (S30, «грузится»);
  следующая итерация снова `reach()`: адаптер потерял сеть → честные `.searching` / `.joining`;
  адаптер молчит → `.lost`.
- После `updateFinished(.car)` — стадия с начала: `reach()` покажет S13/S29, пока адаптер
  переподключается к перезагрузившейся машинке, потом `.checking` → правило → `.ok`.

Это заменяет сегодняшний `adapterStillJoined()` и делает `dongleRerunWanted` ненужным (§4).

### 2.3 Без адаптера (мок)

Одна плата: `reach = { .reached }`, `silentStep = .seeking`, `readVersion` —
`CarTransport.shared.get(CarContract.versionPath, timeout: 2)`. Выпуск спрашивается после первого
ответа машинки (п. 6 §1.3).

## 3. Экраны

### 3.1 Ситуации

```swift
enum Situation: Equatable {
    case stage(UpdateRules.Device, GateStep)
    case releaseCheck, releaseOffline
    case releaseMissing(tag: String, device: UpdateRules.Device)
    case searching                       // the radar, S25: CarLink's own, between hello and live
}
```

Уходят: `findingAdapter`, `checkingDongle`, `noDongle(reason)`, `localNetworkDenied`,
`dongleFault`, `wrongDongle`, `sendingNetwork`, `dongleConfiguring`, `dongleJoinFailed`,
`findingCar`, `checkingCar`, `rolledBack(device:)`, `appBehind(device:proto:)` — все они
становятся `.stage(_, _)`. `NWPath.UnsatisfiedReason` у S2 больше не различается: сегодняшний
гейт и так показывал `.notAvailable`, а после гейта провод пропал → это стадия адаптера (§4).

`WrongCarView.swift` удаляется. Его заголовок, подтекст и подсказка становятся текстами
`.wrongDevice` для машинки; `.appBehind` для машинки берёт сегодняшний `appBehindSub.car`.

### 3.2 Тексты — матрица «шаг × плата»

Ключи `stage.<step>.title` (общий) или `stage.<step>.title.<device>` (по плате) и так же `sub`;
`L.stageTitle(_ step:, _ device:)`, `L.stageSub(_:_:)`. **Сами тексты — сегодняшние, слово в
слово**, перекладываются из `dongle.*`, `car.*`, `gate.*`, `wrongCar.*`, `rolledBack*`,
`appBehind*`:

| Шаг | Заголовок | Подтекст |
|---|---|---|
| `seeking` | по плате: «Ищу адаптер» / «Проверяю машинку» | по плате |
| `absent` | по плате (у машинки не показывается: `silentStep = .seeking`) | по плате |
| `checking` | по плате: «Проверяю адаптер» / «Проверяю машинку» | по плате |
| `fault` | по плате: «Адаптер отвечает с ошибкой» / **новый** «Машинка отвечает с ошибкой» | по плате; для машинки: «Машинка в сети адаптера, но отвечает не так, как ждёт приложение. Перезагрузи её.» |
| `denied` | общий | общий |
| `wrongDevice` | по плате: «Не тот адаптер» / «Это другая машинка» | по плате (у машинки — сегодняшние `wrongCar.sub` + `wrongCar.hint`) |
| `rolledBack` | по плате (`rolledBackTitle.<device>`) | по плате |
| `appBehind` | общий («Приложение устарело») | по плате |
| `sendingNetwork / searching / joining / joinFailed` | общий (`dongle.sendingNet*`, `car.finding*`, `dongle.configuring*`, `dongle.joinFailed*`) | общий |

### 3.3 Кнопки и рисунок

| Шаг | Кнопка | Действие (одно на обе платы) |
|---|---|---|
| `denied` | «Открыть настройки» | как сейчас |
| `wrongDevice` | «Повторить» | `flow.wakePoll()` — опрос и так идёт; у адаптера кнопка появляется, у машинки остаётся |
| `rolledBack` | «Повторить» | `flow.recheckRollback()` |
| `joinFailed` | «Повторить» | `flow.retryJoin()` |
| остальные | — | удержания, снимаются сами |

`ConnectView` получает один необязательный `onRetry: (() -> Void)?` вместо сегодняшних
`onRecheckRollback` / `onRetryJoin` (у ситуации не бывает двух кнопок; настройки — как сейчас).

Рисунок — по сегодняшним правилам: тело по `device` (`CarBody` / `AdapterBody`), чип по шагу
(«?» — `wrongDevice`, «!» — `fault`, «↺» — `rolledBack`, часы — `appBehind`, кольца —
`seeking/checking/searching/joining`). Шаги `sendingNetwork/searching/joining/joinFailed`
рисуют адаптер, хотя стадия машинкина: работу делает он.

### 3.4 Галерея и артефакты

`GalleryView`: кадры `ConnectView` — матрица шаг × плата (все шаги, обе платы, кроме `absent`
машинки); кадры `FirmwareView` — все фазы для обеих плат, включая `forced` (сегодня у адаптера
нет `forced`, `downloaded`, `flashed`, `failed forced`). Оба артефакта («Экраны AJMiddleCar»,
«Адаптер и машинка рядом») пересобираются после ветки; номера S1…S32 сохраняются, S23/S24-по-hello
уходят из каталога как отдельные записи, «Машинка отвечает с ошибкой» — S33.

## 4. Стражи после гейта и проводка

### 4.1 Раннер

```swift
/// The ladder, top to bottom — or from `rung`, when a guard says a board has changed.
func run(from rung: UpdateRules.Device? = nil) async
/// A guard fired: the ladder should be at `rung` or earlier. Running and already there or
/// earlier → nothing; running past it → the current stage returns .lost until the runner is
/// at `rung`; not running → run(from: rung).
func restart(from rung: UpdateRules.Device)
func updateFinished(_ device: UpdateRules.Device)   // FirmwareView is done: the stage restarts from reach()
```

Раннер: индекс по лестнице; `.ok` → следующая плата; `.lost` → на ступень назад (не ниже
`restart`-запроса и не ниже первой); после последней — `setPhase(.awaitingCar)`. `gateRunning` —
единственный мьютекс, берётся **синхронно до первого `await`** во всех трёх входах (`.task`,
`restart`, `updateFinished`), как сегодня.

Три сегодняшних входа сливаются: `startupCheck()` → `run()`; `dongleReturned()` уходит
(провод вернулся при идущей лестнице — стадия адаптера сама увидит ответ; при завершённой —
`restart(from: .dongle)` уже сработал на пропаже); `updateFinished()` и `dongleUpdateFinished()`
→ `updateFinished(device)`: `setPhase(.stage(device, .checking))`, флаг «`.checking` показан»
сброшен, парковка снята; если лестница не идёт (принудительное обновление, поднятое `carIdentified`
посреди сессии) — `run(from: device)`.

`restart(from: .dongle)` во время собственной стадии адаптера — в том числе на парковке
`.stage(.dongle, .updating)`, когда USB переподключается по замыслу, — ничего не делает («уже там
или раньше»). Это сегодняшнее «флаг не ставится на `.dongleUpdating`».

### 4.2 Стражи

`RootView`, `.onChange(of: link.state)`:

| `Link` | Действие |
|---|---|
| `.noDongle` | `flow.restart(from: .dongle)` |
| `.localNetworkDenied` | `flow.restart(from: .dongle)` — стадия адаптера покажет `.denied` |
| `.wrongCar` | `flow.restart(from: .car)` — `/version` → `.wrongDevice`, «Повторить» будит опрос |
| `.wrongProto` | `flow.restart(from: .car)` — правило решит: `.updating` или `.appBehind` |
| `.live` | `flow.carIdentified(fw: link.fw)` — как сейчас |
| `.searching` | — |

`carIdentified(fw:)` (по `.live` и по телеметрии, как сейчас): `GateRule.mayDrive` → `.ready`,
иначе → `restart(from: .car)` (вместо прямого `.updateRequired`). Фазы `.awaitingCar/.ready` —
единственные, в которых стражи что-то значат; в остальных лестница уже идёт, и `restart` — no-op
или переход на ступень раньше.

`.onChange(of: flow.phase) == .awaitingCar`: `link.retryAfterWrongCar()` (сброс удержания по
чужому id/proto — hello сразу) и, при `link.isLive`, `carIdentified(fw:)` — как сейчас.

`carRoot` — только состояния связи, которые лестница не перехватывает: `.searching` → радар;
`.live ∧ .ready` → `DriveView`; `.live ∧ ¬.ready` → радар (S26); всё остальное → радар (эти
состояния уже перезапустили лестницу, и фаза ушла из `.awaitingCar/.ready` в том же тике).
`link.start()` идемпотентен (`guard pump == nil`) — повторный вход в `.awaitingCar` безопасен;
связь при перезапуске стадии не останавливается (как сегодня при `.updateRequired`).

### 4.3 `RootView.root`

```swift
switch flow.shown {
case .stage(let dev, .updating):
    FirmwareView(palette: p, flow: dev == .car ? .forCar() : .forDongle(client: flow.dongle),
                 forced: true, onDone: { flow.updateFinished(dev) })
case .stage(let dev, let step):
    ConnectView(situation: .stage(dev, step), onRetry: flow.retryAction(for: step))
case .releaseCheck: ConnectView(situation: .releaseCheck)
case .releaseOffline: ConnectView(situation: .releaseOffline)
case .releaseMissing(let tag, let dev): ConnectView(situation: .releaseMissing(tag: tag, device: dev))
case .awaitingCar, .ready: carRoot.onAppear { link.start() }
}
```

`retryAction(for:)` — `wrongDevice → wakePoll`, `rolledBack → recheckRollback`,
`joinFailed → retryJoin`, иначе nil.

### 4.4 Что удаляется из `AppFlow`

`dongleGate`, `carGate`, `runGates`, `releaseGate`, `dongleReturned`, `dongleHandedOver`,
`dongleRerunWanted`, `sawDongle`, `adapterStillJoined`, `askDongleToJoin` и бюджет,
`readDongleStatus`/приватный `DongleStatusReply` (в `CarReach`), `dongleUpdateFinished`,
`retryDongleJoin` → `retryJoin`, все фазы с приставкой платы. Остаются: `phase/shown/PhasePacer`,
`latestTag/fetchRelease`, `rollbackChoice/recheckRollback/consumeRollbackRecheck`,
`pollPause/wakePoll`, `gateRunning`, `dongle: DongleClient`, `migrateCacheIfNeeded` на запуске.
Ожидаемый размер — около 350 строк вместо 820.

## 5. Тесты

Хост-тесты `swiftc`, как остальные в `app/tests/`; `tools/test-all.sh` подхватывает каталоги.

- **`tests/stagerule`** (новый): `StageRule.decide` для обеих плат — `reach = .hold(step)` →
  `.show(step)` без чтения версии; `.lost` → `.lost`; тишина → `silentStep` платы (`.absent` /
  `.seeking`); документ или 404 при `latestTag == nil` → `.needRelease`; все восемь вердиктов
  `VersionRule` → шаги по таблице §1.3 (404 → `.updating`; `rolled_back` с `.recheck` новее →
  `.updating`, иначе `.rolledBack`); `.ok` → `.ok`; порядок — reach раньше версии, версия раньше
  выпуска, выпуск раньше правила.
- **`tests/carreach`** (новый): последовательности статусов — `sendCredentials → searching →
  joining → connected` = `.reached` c `ask = .configure` только на первом; `failed` при бюджете →
  `(.hold(.searching), .retry)`, после — `(.hold(.joinFailed), nil)`; `retry()` даёт новый бюджет;
  `connected` сбрасывает `attempts`; тишина / не документ / запрет → `.lost`; чужая сеть в
  `wifi.ssid` → `.configure` (сравнение с именем, не с пустотой — уже в `DongleLink.next`).
- Существующие без изменений: `versionrule`, `donglelink`, `gate`, `phasepacer`, `donglestatus`,
  `identity`.

## 6. Проверка

**Симулятор против мока** (обязательно в плане, каждый пункт со скриншотом):

1. обычный запуск → S30 → S28;
2. `MOCK_DEVICE=esp32-car` → `.stage(.car, .wrongDevice)` с «Повторить»; перезапуск мока с нашим
   именем → лестница добегает сама;
3. `MOCK_NO_VERSION=1` → `.stage(.car, .updating)` → прошивка → S30 → S28 (день-флаг);
4. мок остановлен посреди езды → радар; мок снова → S28 (страж связи, не лестница);
5. галерея: каждый кадр матрицы.

**Стенд** (после ветки, `-viaDongle`, пункты дописать в `docs/bringup.md` рядом с существующими
двумя): полная лестница на железе; выдернуть адаптер на S27 → S2 → воткнуть → S13/S29 → S27
снова; после обновления машинки — S13/S29 до `.checking`; hello от другой машинки (второй
`MOCK_DEVICE` не воспроизвести на железе — пункт остаётся «по возможности»).

## 7. Документы

- `CLAUDE.md`, раздел iOS: лестница = массив плат, плата = описание (`Board`), одна стадия
  (`StageRule`), стражи после гейта перезапускают её; `WrongCarView` больше нет.
- `docs/superpowers/specs/2026-09-17-version-endpoint-design.md`, статус: «§3 (лестница)
  заменена `2026-09-17-board-ladder-design.md`».
- `docs/bringup.md` — пункты стенда из §6.
- Память `launch-ladder-2026-09` и оба артефакта — после ветки.

## 8. Чего не делаем

- Не трогаем прошивки, контракт, мок, конформанс, `docs/protocol.md` — провод тот же.
- Не трогаем `FirmwareFlow` / `FirmwareView` / `UpdateRules` / `VersionRule` / `DongleLink.next` /
  `LinkRule` / `CarLink` (кроме вызова `retryAfterWrongCar()` при возврате в `.awaitingCar`) /
  транспорты.
- Не унифицируем тайм-ауты и окна перезагрузки — это данные платы.
- Не добавляем третью плату — но после этой спеки она стоит одно описание, не третий цикл.
- Не меняем тексты экранов (кроме одного нового — `fault` машинки) и рисунки.
- Экран прошивки из настроек (не `forced`) — без изменений.
