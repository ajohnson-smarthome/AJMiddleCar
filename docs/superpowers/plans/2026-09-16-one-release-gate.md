# Один гейт релиза — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** релиз узнаётся один раз за запуск (в шаге адаптера, или тем же шагом сам по себе без
адаптера), машинка проверяется после hello и качает свой образ только внутри принудительного
обновления — как адаптер; экраны S15/S16/S18/S19 уходят из лестницы.

**Architecture:** в `AppFlow` блок «спросить GitHub» из `dongleGate()` выносится в
`fetchRelease(for:)`; тег один — `latestTag`; без адаптера тот же шаг крутит `releaseGate()`.
`carGate()` и пять его фаз удаляются, три фазы шага релиза переименовываются в нейтральные
(`.releaseCheck/.releaseOffline/.releaseMissing`), их экраны — те же `ConnectView`-ситуации с
новыми словами. `FirmwareFlow`/`FirmwareView` не меняются: форс машинки и так сам качает.

**Tech Stack:** Swift/SwiftUI (сборка `xcodebuild` под симулятор; host-тесты `swiftc` через
`tools/test-all.sh` — их эта работа не меняет); XcodeGen (`xcodegen generate` после удаления
файлов); симулятор + мок (`tools/mock_car`) для прогона лестницы.

**Spec:** `docs/superpowers/specs/2026-09-16-one-release-gate-design.md`

## Global Constraints

- Проза (план, документация, пояснительная часть коммитов) — по-русски; код, комментарии в
  коде, сообщения коммитов — по-английски. Строки локализации — по-русски, ключи — как в спеке §4.
- Прошивки, контракт, генераторы, мок — **не трогать**. `FirmwareFlow.swift`, `FirmwareView.swift`,
  `GateRule.swift`, `UpdateRules.swift` и их тесты — **не трогать** (спека §3, §8).
- `UpdateClient.needsDownload(…)`-обёртку **оставить** (спека §3 предлагала убрать; её читает
  `app/AJMiddleCarTests/ControlModelTests.swift:114–118`, который `xcodebuild build` не
  собирает, но ломать XCTest-цель незачем). `cachedBinURL` без `for:` — оставить
  (`migrateCacheIfNeeded`).
- Ничего не добавлять взамен удалённого: ни кнопки «Повторить» на S5/S6 (они держат и
  переспрашивают сами, как сейчас), ни предзагрузки в другом месте.
- Каждая задача заканчивается зелёным `xcodebuild` — `switch` по `Phase` и по
  `ConnectView.Situation` исчерпывающие, и переименование/удаление случая ловит только
  компилятор:

  ```bash
  cd app && xcodegen generate >/dev/null && xcodebuild build -quiet -scheme AJMiddleCar \
    -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle 2>&1 | tail -5
  ```

  Ожидается пустой вывод или `** BUILD SUCCEEDED **` без `error:`.
- Прогон лестницы в симуляторе против мока (задачи 1 и 2; Mac должен быть в интернете):

  ```bash
  # мок (один раз; если уже запущен — пропустить)
  cd tools/mock_car && nohup .venv/bin/python -u mock_car.py >/tmp/mock.log 2>&1 & cd ../..
  xcrun simctl boot "iPhone 17" 2>/dev/null; open -a Simulator
  xcrun simctl install booted /tmp/ddata-middle/Build/Products/Debug-iphonesimulator/AJMiddleCar.app
  xcrun simctl terminate booted com.adamjohnson.ajmiddlecar 2>/dev/null
  xcrun simctl launch booted com.adamjohnson.ajmiddlecar -viaMock
  sleep 8 && xcrun simctl io booted screenshot /tmp/ladder.png && sips -r -90 /tmp/ladder.png >/dev/null
  ```

  `-viaMock` сбрасывает липкий `-viaDongle` (`CarHost`). Через 8 с на скриншоте должен быть
  пульт (или радар «Здороваюсь с машинкой», если мок только что стартовал) — **не** экран
  «Проверяю обновления» и не «Нет интернета».
- Коммитить только файлы, перечисленные в задаче (`git add <файлы>`), никогда `git add -A`.
  `CLAUDE.md` не трогать. В дереве есть посторонний неотслеживаемый `docs/research/…` — не
  трогать.
- Сообщения коммитов заканчиваются строками:

  ```
  Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ
  ```

---

### Задача 1: Один тег, один шаг релиза; гейт машинки и его экраны уходят

**Files:**
- Modify: `app/AJMiddleCar/AppFlow.swift` (фазы 24–48, 96; `opensLink` 108–117; свойства 130, 179–184; `startupCheck` 196–208; `dongleReturned` doc 220–224; `dongleGate` doc 240–246 и блок 269–302, вызов `DongleLink.next` 304; `recheckDongleRollback` 475–490; `carGate` 513–566; doc над `carIdentified` 562–567; `retry` 594)
- Modify: `app/AJMiddleCar/AJMiddleCarApp.swift:108-116, 139-147`
- Modify: `app/AJMiddleCar/ConnectView.swift` (ситуации 22–43; арт 142–160; `title` 179–184; `message` 200–205; список без кнопки 249–252)
- Modify: `app/AJMiddleCar/GalleryView.swift:96-115`
- Modify: `app/AJMiddleCar/DeviceArt.swift:54-55` (комментарий)
- Modify: `app/AJMiddleCar/L.swift` (строки 41–42, 188–190)
- Modify: `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings` (строки 141, 143–144, 198–199)
- Delete: `app/AJMiddleCar/NoInternetView.swift`, `app/AJMiddleCar/UpdateCheckView.swift`

**Interfaces:**
- Consumes: `UpdateClient.latestReleaseLookup(for: UpdateRules.Device) async -> ReleaseLookup` (`.found(Release)`, `.noImage(tag:)`, `.unreachable`); `GateRule.canVerify(latestBuild:)`; `DongleLink.next(reply:latestTag:expectedSSID:rollback:)`; `CarHost.viaDongle`.
- Produces: `AppFlow.Phase` без `checkInternet/noInternet/checkUpdate/checkFailed/downloading`, с `releaseCheck`, `releaseOffline`, `releaseMissing(tag: String, device: UpdateRules.Device)`; `ConnectView.Situation` с `releaseCheck`, `releaseOffline`, `releaseMissing(tag: String, device: UpdateRules.Device)` (без `carUpdateCheck`); `AppFlow.latestTag` — единственный тег; `private func fetchRelease(for:) async -> Bool`, `private func releaseGate() async`. Слова экранов в этой задаче **старые** (ключи `dongle.updCheck*`, `dongle.offlineSub`, `gate.noRelease*`) — их меняет задача 2.

- [ ] **Шаг 1: Фазы**

В `app/AJMiddleCar/AppFlow.swift`, внутри `enum Phase: Equatable`:

(а) заменить

```swift
        /// Step 3. Asking GitHub whether the adapter's own firmware is current. Had no phase at
        /// all before, so this wait happened behind whatever screen preceded it.
        case dongleUpdateCheck
```

на

```swift
        /// Step 3. Asking GitHub for the newest release — once per launch, for both boards: one
        /// release tags both images, so the tag learned here is what the adapter is compared
        /// against now and the car after its hello. Had no phase at all before, so this wait
        /// happened behind whatever screen preceded it.
        case releaseCheck
```

(б) заменить

```swift
        /// The adapter's newest release could not be established — no internet, or a release
        /// with no build number in it. A hold, not a failure: `dongleGate()` keeps asking, so
        /// this clears itself the moment the network returns. That is also why it is not
        /// `.noInternet`, whose screen offers a Retry: while this loop is running `retry()` is
        /// refused by `gateRunning`, and a button that does nothing is worse than no button.
        case dongleOffline
        /// A release exists and carries no image for the adapter, so its version cannot be
        /// established and the gate will not hand over. Not the user's to fix — only publishing
        /// a release with the adapter's image clears it — which is why the screen says that
        /// instead of blaming the network.
        case dongleNoRelease(tag: String)
```

на

```swift
        /// The newest release could not be established: GitHub did not answer. A hold, not a
        /// failure — the gate keeps asking (`fetchRelease`), so this clears itself the moment
        /// the network returns, which is why the screen has no button.
        case releaseOffline
        /// A release exists and carries no image for `device`, or no build number, so nothing
        /// can be compared against it and the gate will not proceed. Not the user's to fix —
        /// only publishing a usable release clears it — which is why the screen says that
        /// instead of blaming the network.
        case releaseMissing(tag: String, device: UpdateRules.Device)
```

(в) удалить строку

```swift
        case checkInternet, noInternet, checkUpdate, checkFailed, downloading
```

(г) в `opensLink` заменить

```swift
            case .checkDongle, .dongleAbsent, .dongleChecking, .dongleUpdateCheck, .carFinding,
                 .dongleOffline, .dongleNoRelease,
                 .dongleFault, .dongleDenied, .dongleWrong,
                 .dongleUpdating, .dongleRolledBack,
                 .dongleSendingNet, .dongleConfiguring, .dongleJoinFailed,
                 .checkInternet, .noInternet, .checkUpdate, .checkFailed, .downloading: return false
```

на

```swift
            case .checkDongle, .dongleAbsent, .dongleChecking, .releaseCheck, .carFinding,
                 .releaseOffline, .releaseMissing,
                 .dongleFault, .dongleDenied, .dongleWrong,
                 .dongleUpdating, .dongleRolledBack,
                 .dongleSendingNet, .dongleConfiguring, .dongleJoinFailed: return false
```

- [ ] **Шаг 2: Один тег**

Там же: заменить

```swift
    @Published var latestTag: String?
```

на

```swift
    /// The newest release's tag — one for both boards, learned once per launch by
    /// `fetchRelease` (inside `dongleGate()`, or on its own in `releaseGate()` when there is no
    /// adapter). `DongleLink.next` compares the adapter against it; `carIdentified` the car.
    /// Cleared by `recheckDongleRollback()`, which is what makes the next poll ask again.
    @Published var latestTag: String?
```

и удалить

```swift
    /// The dongle's release and the tag `DongleLink` compares against — fetched once, lazily,
    /// the first time `/status` answers, and held on the flow rather than inside `dongleGate()`
    /// so `recheckDongleRollback()` can reopen the fetch (that is what "check again" means).
    private var dongleRelease: UpdateClient.Release?
    private var dongleLatestTag: String?
```

- [ ] **Шаг 3: `fetchRelease(for:)` и `releaseGate()`**

Заменить `startupCheck()` целиком (от `func startupCheck() async {` до его `}`) на:

```swift
    func startupCheck() async {
        guard !gateRunning else { return }
        gateRunning = true
        defer { gateRunning = false }
        UpdateClient.migrateCacheIfNeeded()
        // The dongle half runs wherever there is a dongle to ask: every device, and a simulator
        // launched with `-viaDongle` (CarHost) — the adapter on the Mac's USB, the simulator as
        // the phone. It learns the newest release on the way (step 3). Against the mock there
        // is no dongle and nothing stands in for one, so the release step runs on its own; the
        // car itself is met on the far side of `.awaitingCar`, in its reply to the hello.
        if CarHost.viaDongle {
            if !dongleHandedOver {
                await dongleGate()
                dongleHandedOver = true
            }
        } else {
            await releaseGate()
        }
        setPhase(.awaitingCar)
    }

    /// Step 3, one attempt: ask GitHub for the newest release and adopt its tag. The release is
    /// one for both boards — one tag, two images — so this runs once per launch, and both the
    /// adapter's comparison (`DongleLink.next`) and the car's (`carIdentified`) read the tag it
    /// leaves in `latestTag`. `device` only says which image's presence to insist on.
    ///
    /// Returns true once `latestTag` is set. Otherwise sets the holding phase — `.releaseOffline`
    /// when GitHub could not be reached, `.releaseMissing` when the release carries no image for
    /// `device` or no build number — and returns false; the caller sleeps a poll interval and
    /// asks again. Announces `.releaseCheck` only when not already holding: re-announcing on
    /// every failed poll made "checking" and the hold alternate — with `PhasePacer` guaranteeing
    /// each screen its 400 ms, that is a strobe rather than a sequence.
    private func fetchRelease(for device: UpdateRules.Device) async -> Bool {
        var holding = phase == .releaseOffline
        if case .releaseMissing = phase { holding = true }
        if !holding { setPhase(.releaseCheck) }
        switch await client.latestReleaseLookup(for: device) {
        case .found(let rel):
            // The tag is only adopted once it can be compared against. Setting it first and
            // validating after left an unusable tag in place, and the next poll then skipped
            // this whole block and drove on it.
            guard GateRule.canVerify(latestBuild: UpdateClient.buildNumber(rel.tag)) else {
                setPhase(.releaseMissing(tag: rel.tag, device: device))
                return false
            }
            latestTag = rel.tag
            return true
        case .noImage(let tag):
            setPhase(.releaseMissing(tag: tag, device: device))
            return false
        case .unreachable:
            setPhase(.releaseOffline)
            return false
        }
    }

    /// The release step on its own, for a launch with no adapter to find it behind (the mock).
    /// Same step, same screens, same holds as inside `dongleGate()`; `.car` because there is no
    /// adapter whose image the release would have to carry.
    private func releaseGate() async {
        while latestTag == nil {
            if await fetchRelease(for: .car) { return }
            try? await Task.sleep(for: Self.donglePollInterval)
        }
    }
```

- [ ] **Шаг 4: `dongleGate()` зовёт `fetchRelease`**

В doc-комментарии над `private func dongleGate()` заменить

```swift
    /// next at each step, and return. What follows is the caller's: `startupCheck()` hands over
    /// to `carGate()`, and `dongleReturned()` — a re-entry after the wire came back — does not,
    /// because the car's gate has already answered. One release tags both images identically
    /// (`UpdateRules.Device`), so the tag fetched here for the dongle's own comparison is a
    /// separate call from the one `carGate()` makes for the car's — decoupled on purpose, so
    /// neither device's gate reads a tag fetched for the other's asset URL.
```

на

```swift
    /// next at each step, and return. What follows is the caller's: both `startupCheck()` and
    /// `dongleReturned()` — a re-entry after the wire came back — move on to `.awaitingCar`.
    /// The newest release is learned here, once, for both boards (`fetchRelease`): one release
    /// tags both images, and the car is compared against the same tag after its hello.
```

Внутри цикла заменить блок от строки

```swift
            // Step 3, and a gate rather than a formality: the adapter's newest release must be
```

до закрывающей `}` этого `if` (после `case .unreachable: … continue }` и ещё одной `}`) — на:

```swift
            // Step 3, and a gate rather than a formality: the newest release must be established
            // before anything is decided about the adapter. Retried on every poll until it is —
            // a launch that could not reach GitHub must not proceed on the assumption that
            // nothing has changed, which is exactly what it used to do.
            if case .status = reply, latestTag == nil {
                if !(await fetchRelease(for: .dongle)) {
                    try? await Task.sleep(for: Self.donglePollInterval)
                    continue
                }
            }
```

и в вызове ниже заменить `latestTag: dongleLatestTag` на `latestTag: latestTag`.

- [ ] **Шаг 5: `dongleReturned`, `recheckDongleRollback`, `carGate`, `retry`, осиротевшие комментарии**

(а) В doc-комментарии над `func dongleReturned()` заменить абзац

```swift
    /// Only the dongle half re-runs. The car's own gate already answered this session and
    /// `latestTag` is still held, so re-running it would re-probe GitHub and could strand a
    /// live session on `.noInternet` over a cable that was out for two seconds. Handing back to
    /// `.awaitingCar` is enough: `carIdentified` restores `.ready`/`.updateRequired` on the
    /// next hello, which is where the phase was before the wire went.
```

на

```swift
    /// Only the dongle half re-runs, and `latestTag` is still held, so the release is not asked
    /// again. Handing back to `.awaitingCar` is enough: `carIdentified` restores
    /// `.ready`/`.updateRequired` on the next hello, which is where the phase was before the
    /// wire went.
```

(б) Удалить осиротевший doc-комментарий — четыре строки, за которыми идёт пустая строка, а не
объявление — вместе с этой пустой строкой:

```swift
    /// The user chose to proceed on the dongle's current, reverted firmware rather than being
    /// stuck on `.dongleRolledBack` forever (the car's own forced-update gate keeps the same
    /// escape hatch — `FirmwareView`'s skip button). Read by `dongleGate()`'s very next poll,
    /// which is at most `donglePollInterval` away.
```

(в) В `recheckDongleRollback()` заменить тело на

```swift
        rollbackChoice = .recheck(from: latestTag)
        // Clearing the tag is what makes the next poll re-ask GitHub: `fetchRelease` runs while
        // `latestTag == nil`, so this is the recheck actually happening.
        latestTag = nil
```

(г) Удалить `carGate()` целиком — от doc-комментария `/// The car's own pre-connect gate
(internet probe → latest release → download if needed).` до закрывающей `}` функции
включительно (с пустой строкой после).

(д) Над `func carIdentified(fw:)` удалить три первые строки его doc-комментария — осиротевший
абзац другой, давно удалённой функции:

```swift
    /// GitHub unreachable or unusable: a cached image is enough to drive — and enough to
    /// force with. Seeding `latestTag` from the cache is what keeps the forced gate armed
    /// offline (decision 4a); without it `mustUpdate` compared against nil and every car,
    /// pre-versioning ones included, drove unforced whenever the launch had no internet.
```

Doc начинается теперь с `/// The car said who it is, in its hello reply.`

(е) Удалить `func retry() { Task { await startupCheck() } }` (и пустую строку перед ним, чтобы
между `updateFinished()` и закрывающей `}` класса осталась одна).

- [ ] **Шаг 6: `root` в `AJMiddleCarApp.swift`**

Заменить

```swift
        case .dongleUpdateCheck:
            ConnectView(situation: .adapterUpdateCheck)
```

на

```swift
        case .releaseCheck:
            ConnectView(situation: .releaseCheck)
```

заменить

```swift
        case .dongleOffline:
            ConnectView(situation: .offline)
        case .dongleNoRelease(let tag):
            ConnectView(situation: .noRelease(tag: tag))
```

на

```swift
        case .releaseOffline:
            ConnectView(situation: .releaseOffline)
        case .releaseMissing(let tag, let device):
            ConnectView(situation: .releaseMissing(tag: tag, device: device))
```

и удалить целиком

```swift
        // Step 6 of the ladder: the car's own release check, which now says what it is doing
        // and to whom. `UpdateCheckView` keeps the two states that are not a step — a download
        // with a progress bar, and a failure with a button.
        case .checkInternet, .checkUpdate:
            ConnectView(situation: .carUpdateCheck)
        case .downloading, .checkFailed:
            UpdateCheckView(palette: p, phase: flow.shown, client: flow.client) { flow.retry() }
        case .noInternet:
            NoInternetView(palette: p) { flow.retry() }
```

- [ ] **Шаг 7: `ConnectView.Situation`**

(а) В enum заменить

```swift
        /// The newest release could not be established, so nothing may proceed. No button: the
        /// gate loop is still asking and clears this itself the moment the network returns.
        case offline
        /// A release exists and carries no image for the adapter. Carries the tag, because the
        /// only person who can act on this is the one who publishes releases, and the tag is
        /// what tells them which one to look at.
        case noRelease(tag: String)
```

на

```swift
        /// The newest release could not be established, so nothing may proceed. No button: the
        /// gate loop is still asking and clears this itself the moment the network returns.
        case releaseOffline
        /// A release exists and carries no image for `device` (or no build number). Carries the
        /// tag, because the only person who can act on this is the one who publishes releases,
        /// and the tag is what tells them which one to look at.
        case releaseMissing(tag: String, device: UpdateRules.Device)
```

заменить

```swift
        /// Step 3: the adapter is ours and healthy; asking GitHub whether it is current. Had no
        /// screen at all before — `dongleGate()` did this silently, so a launch that stopped
        /// here looked like a launch that had stopped for no reason.
        case adapterUpdateCheck
```

на

```swift
        /// Step 3: asking GitHub for the newest release — the one tag both boards are compared
        /// against. Had no screen at all before — `dongleGate()` did this silently, so a launch
        /// that stopped here looked like a launch that had stopped for no reason.
        case releaseCheck
```

и удалить целиком

```swift
        /// Step 6: asking GitHub about the car's own firmware — over the internet, not over the
        /// link to the car. Nothing has spoken to the car at this point; `carGate()` only fetches
        /// the release and, if needed, downloads it. The car's own version arrives one screen
        /// later, in its reply to the hello, which is why this step must not claim a connection:
        /// saying "связь есть" here and then greeting the car on the next screen is what made the
        /// sequence read backwards.
        case carUpdateCheck
```

(б) В арт-`switch`: `case .offline:` → `case .releaseOffline:`; `case .noRelease:` →
`case .releaseMissing:`; `case .adapterUpdateCheck:` → `case .releaseCheck:`; удалить

```swift
        case .carUpdateCheck:
            DeviceScene(palette: p, rings: .inward,
                        chip: (glyph: "arrow.down", tint: p.accent)) { CarBody(palette: p) }
```

(в) В `title`: `case .offline: return L.gateNoInternetTitle` → `case .releaseOffline: return L.gateNoInternetTitle`;
`case .noRelease: return L.gateNoReleaseTitle` → `case .releaseMissing: return L.gateNoReleaseTitle`;
`case .adapterUpdateCheck: return L.dongleUpdCheckTitle` → `case .releaseCheck: return L.dongleUpdCheckTitle`;
удалить `case .carUpdateCheck: return L.carUpdCheckTitle`.

(г) В `message`: `case .offline: return L.dongleOfflineSub` → `case .releaseOffline: return L.dongleOfflineSub`;
`case .noRelease(let tag): return L.gateNoReleaseSub(tag)` → `case .releaseMissing(let tag, _): return L.gateNoReleaseSub(tag)`;
`case .adapterUpdateCheck: return L.dongleUpdCheckSub` → `case .releaseCheck: return L.dongleUpdCheckSub`;
удалить `case .carUpdateCheck: return L.carUpdCheckSub`.

(д) В списке ситуаций без кнопки заменить

```swift
             .findingAdapter, .adapterUpdateCheck, .findingCar, .carUpdateCheck, .offline,
             .noRelease:
```

на

```swift
             .findingAdapter, .releaseCheck, .findingCar, .releaseOffline, .releaseMissing:
```

- [ ] **Шаг 8: Галерея, `DeviceArt`, `L`, строки, файлы**

`app/AJMiddleCar/GalleryView.swift`: заменить

```swift
            ("Step 3 adapter update",   AnyView(ConnectView(situation: .adapterUpdateCheck))),
```

на

```swift
            ("Step 3 release check",    AnyView(ConnectView(situation: .releaseCheck))),
```

удалить строку `("Step 6 car update",       AnyView(ConnectView(situation: .carUpdateCheck))),`;
заменить

```swift
            ("Offline, cannot verify",  AnyView(ConnectView(situation: .offline))),
            ("No release for adapter",  AnyView(ConnectView(situation: .noRelease(tag: "v1.0+483")))),
```

на

```swift
            ("Offline, cannot verify",  AnyView(ConnectView(situation: .releaseOffline))),
            ("No release for adapter",  AnyView(ConnectView(situation: .releaseMissing(tag: "v1.0+483", device: .dongle)))),
```

удалить строку `("NoInternet", …)` и три строки `("UpdateCheck checking", …)`,
`("UpdateCheck downloading", …)`, `("UpdateCheck failed", …)`.

`app/AJMiddleCar/DeviceArt.swift`: заменить

```swift
    /// Almost always the accent. `NoInternetView` wants them warm, and used to draw a whole
    /// second set of rings — and a second car — for want of this one parameter.
```

на

```swift
    /// Almost always the accent. The offline hold (`ConnectView`'s `.releaseOffline`) wants them
    /// warm; the screen that preceded it used to draw a whole second set of rings — and a second
    /// car — for want of this one parameter.
```

`app/AJMiddleCar/L.swift`: удалить строки

```swift
    static var carUpdCheckTitle: String { s("car.updCheckTitle") }
    static var carUpdCheckSub: String { s("car.updCheckSub") }
```

и

```swift
    static var gateNoInternetSub: String { s("gate.noInternetSub") }
    static var gateCheckFailedTitle: String { s("gate.checkFailedTitle") }
    static var gateCheckFailedSub: String { s("gate.checkFailedSub") }
```

(`gateNoInternetTitle` остаётся — его читает `ConnectView`.)

`app/AJMiddleCar/Resources/ru.lproj/Localizable.strings`: удалить строки с ключами
`gate.noInternetSub`, `gate.checkFailedTitle`, `gate.checkFailedSub`, `car.updCheckTitle`,
`car.updCheckSub` (пять строк). `gate.noInternetTitle`, `gate.updateTitle`, `gate.updateSub`
остаются.

Файлы:

```bash
git rm -q app/AJMiddleCar/NoInternetView.swift app/AJMiddleCar/UpdateCheckView.swift
```

- [ ] **Шаг 9: Сборка и проверка отсутствия хвостов**

`xcodebuild` из Global Constraints → `BUILD SUCCEEDED`. Затем:

```bash
grep -rn "carGate\|dongleLatestTag\|dongleRelease\b\|checkInternet\|noInternet\|checkUpdate\b\|checkFailed\|carUpdateCheck\|adapterUpdateCheck\|dongleUpdateCheck\|dongleOffline\b\|dongleNoRelease\|NoInternetView\|UpdateCheckView\|flow.retry\|\.downloading\b" app/AJMiddleCar/AppFlow.swift app/AJMiddleCar/AJMiddleCarApp.swift app/AJMiddleCar/ConnectView.swift app/AJMiddleCar/GalleryView.swift app/AJMiddleCar/L.swift
```

Ожидается: пусто. (`.downloading` в `FirmwareFlow`/`FirmwareView`/`FirmwareCarView` — другой
enum, `FwPhase`, его не трогаем; поэтому grep ограничен пятью файлами.)

- [ ] **Шаг 10: Лестница против мока**

Команды из Global Constraints («Прогон лестницы»). Ожидается: через 8 с — пульт или радар;
в `xcrun simctl spawn booted log stream` не нужно смотреть. Если на скриншоте «Нет интернета»
— проверить, что Mac в сети; если «Проверяю обновления» дольше 8 с — GitHub медленный,
подождать ещё 10 с и снять снова. Приложить путь к скриншоту в отчёт.

- [ ] **Шаг 11: Коммит**

```bash
git add app/AJMiddleCar/AppFlow.swift app/AJMiddleCar/AJMiddleCarApp.swift app/AJMiddleCar/ConnectView.swift \
        app/AJMiddleCar/GalleryView.swift app/AJMiddleCar/DeviceArt.swift app/AJMiddleCar/L.swift \
        app/AJMiddleCar/Resources/ru.lproj/Localizable.strings
git commit -m "refactor(app): one release lookup per launch — the car is checked the way the adapter is

The car's own pre-connect gate (internet probe, a second releases/latest,
a pre-download of ajmiddlecar.bin) dates from the softAP days, when
joining the car cost the phone its internet. Through the dongle it does
not, so the release is learned once — in the adapter's step 3, or by the
same step on its own when there is no adapter — and the car's version,
which arrives with its hello, is compared against that one tag. The
forced update downloads the car's image itself, as the adapter's always
has. Five phases, two views and one ConnectView situation leave; the
three release phases lose their 'dongle' prefix.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ"
```

(`git rm` уже поставил два удаления в индекс.)

---

### Задача 2: Слова экранов S4/S5/S6 — про релиз, не про адаптер

**Files:**
- Modify: `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings` (строки `dongle.offlineSub`, `dongle.updCheckTitle`, `dongle.updCheckSub`, `gate.noReleaseTitle`, `gate.noReleaseSub`)
- Modify: `app/AJMiddleCar/L.swift` (`dongleOfflineSub` 23; `gateNoReleaseTitle`/`gateNoReleaseSub` 33–34; `dongleUpdCheckTitle`/`dongleUpdCheckSub` 37–38)
- Modify: `app/AJMiddleCar/ConnectView.swift` (`title`, `message` — три случая)

**Interfaces:**
- Consumes: `ConnectView.Situation.releaseCheck`, `.releaseOffline`, `.releaseMissing(tag:device:)` (задача 1); `UpdateRules.Device.rawValue` — `"car"` / `"dongle"` (так уже строятся ключи `fw.connectTitle.\(d.rawValue)` в `L.swift`).
- Produces: `L.gateReleaseCheckTitle`, `L.gateReleaseCheckSub`, `L.gateOfflineSub`, `L.gateNoReleaseTitle(_ d: UpdateRules.Device)`, `L.gateNoReleaseSub(_ d: UpdateRules.Device, _ tag: String)`.

- [ ] **Шаг 1: Строки**

В `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings`:

заменить строку `"dongle.offlineSub" = …;` на

```
"gate.offlineSub"          = "Чтобы проверить прошивки, нужен интернет. Как только он появится, продолжу сам.";
```

заменить две строки `"dongle.updCheckTitle" = …;` и `"dongle.updCheckSub" = …;` на

```
"gate.releaseCheckTitle"  = "Проверяю обновления";
"gate.releaseCheckSub"    = "Смотрю, какой выпуск последний. Обеим платам нужна одна и та же версия — обновлю до поездки, а не посреди неё.";
```

заменить две строки `"gate.noReleaseTitle" = …;` и `"gate.noReleaseSub" = …;` на

```
"gate.noReleaseTitle.dongle" = "Нет выпуска для адаптера";
"gate.noReleaseTitle.car"    = "Нет выпуска для машинки";
"gate.noReleaseSub.dongle"   = "В последнем выпуске (%@) нет прошивки адаптера, поэтому сверить его версию не с чем. Пока такой выпуск не опубликован, ехать нельзя.";
"gate.noReleaseSub.car"      = "В последнем выпуске (%@) нет прошивки машинки, поэтому сверить её версию не с чем. Пока такой выпуск не опубликован, ехать нельзя.";
```

- [ ] **Шаг 2: `L.swift`**

Заменить `static var dongleOfflineSub: String { s("dongle.offlineSub") }` на
`static var gateOfflineSub: String { s("gate.offlineSub") }`.

Заменить

```swift
    static var gateNoReleaseTitle: String { s("gate.noReleaseTitle") }
    static func gateNoReleaseSub(_ tag: String) -> String { s("gate.noReleaseSub", tag) }
```

на

```swift
    static func gateNoReleaseTitle(_ d: UpdateRules.Device) -> String { s("gate.noReleaseTitle.\(d.rawValue)") }
    static func gateNoReleaseSub(_ d: UpdateRules.Device, _ tag: String) -> String { s("gate.noReleaseSub.\(d.rawValue)", tag) }
```

Заменить

```swift
    static var dongleUpdCheckTitle: String { s("dongle.updCheckTitle") }
    static var dongleUpdCheckSub: String { s("dongle.updCheckSub") }
```

на

```swift
    static var gateReleaseCheckTitle: String { s("gate.releaseCheckTitle") }
    static var gateReleaseCheckSub: String { s("gate.releaseCheckSub") }
```

- [ ] **Шаг 3: `ConnectView`**

В `title`: `case .releaseMissing: return L.gateNoReleaseTitle` →
`case .releaseMissing(_, let device): return L.gateNoReleaseTitle(device)`;
`case .releaseCheck: return L.dongleUpdCheckTitle` → `case .releaseCheck: return L.gateReleaseCheckTitle`.

В `message`: `case .releaseOffline: return L.dongleOfflineSub` → `case .releaseOffline: return L.gateOfflineSub`;
`case .releaseMissing(let tag, _): return L.gateNoReleaseSub(tag)` →
`case .releaseMissing(let tag, let device): return L.gateNoReleaseSub(device, tag)`;
`case .releaseCheck: return L.dongleUpdCheckSub` → `case .releaseCheck: return L.gateReleaseCheckSub`.

- [ ] **Шаг 4: Сборка, хвосты, один кадр галереи**

`xcodebuild` из Global Constraints → `BUILD SUCCEEDED`. Затем:

```bash
grep -rn "dongleOfflineSub\|dongleUpdCheck\|dongle\.offlineSub\|dongle\.updCheck\|\"gate\.noReleaseTitle\"\|\"gate\.noReleaseSub\"" app/AJMiddleCar
```

Ожидается: пусто.

Кадр S6 в галерее — проверить, что строка с `%@` и устройством подставляется:

```bash
# Index = the tuple's position (from 0) in the array `makeFrames` returns (`return [` … `]`);
# every frame is one line starting with `("`.
N=$(awk '/return \[/{f=1;next} f&&/^[[:space:]]*\("/{ if ($0 ~ /"No release for adapter"/) {print i; exit}; i++ }' app/AJMiddleCar/GalleryView.swift)
echo "index $N"
xcrun simctl install booted /tmp/ddata-middle/Build/Products/Debug-iphonesimulator/AJMiddleCar.app
xcrun simctl terminate booted com.adamjohnson.ajmiddlecar 2>/dev/null
xcrun simctl launch booted com.adamjohnson.ajmiddlecar -gallery -galleryIndex "$N"
sleep 3 && xcrun simctl io booted screenshot /tmp/s6.png && sips -r -90 /tmp/s6.png >/dev/null
```

Если `awk` печатает пусто, найти индекс руками: порядковый номер кортежа
`("No release for adapter", …)` среди строк вида `("…", …)` внутри `return [ … ]` в `makeFrames`, считая с нуля. На скриншоте:
заголовок «Нет выпуска для адаптера», в подписи `v1.0+483` в скобках. Путь к скриншоту — в отчёт.

- [ ] **Шаг 5: Коммит**

```bash
git add app/AJMiddleCar/Resources/ru.lproj/Localizable.strings app/AJMiddleCar/L.swift app/AJMiddleCar/ConnectView.swift
git commit -m "feat(app): the release step's screens speak of the release, not the adapter

The same three screens now serve a launch with no adapter (the mock), so
their words stop presuming one: 'checking updates' for both boards, 'no
internet' for the firmware check, and 'no release for the adapter / for
the car' by the device whose image is missing.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ"
```

---

### Задача 3: `UpdateClient` без пробы интернета; документы; весь набор тестов

**Files:**
- Modify: `app/AJMiddleCar/UpdateClient.swift` (`internetReachable()` 54–~84 с комментарием; `static var cachedBuild` 182; `static var hasCachedFile` 192)
- Modify: `docs/superpowers/specs/2026-06-14-forced-update-gate-design.md` (шапка, после строки `**Статус:** …`)
- Modify: `docs/superpowers/specs/2026-09-16-one-release-gate-design.md` (строка `**Статус:**`; строка таблицы §3 про `UpdateClient`)

**Interfaces:**
- Consumes: ничего нового.
- Produces: `UpdateClient` без `internetReachable()`, без `cachedBuild`/`hasCachedFile` (варианты без `for:`); `needsDownload`, `mustUpdate`, `cachedBinURL`, `cachedTag`, все `(for:)`-варианты — остаются.

- [ ] **Шаг 1: Убедиться, что читателей нет**

```bash
grep -rn "internetReachable\|UpdateClient\.cachedBuild\b\|UpdateClient\.hasCachedFile\b" app/ | grep -v "UpdateClient.swift"
```

Ожидается: пусто (единственным читателем был `carGate()`, удалённый в задаче 1). Если что-то
находится — остановиться и сообщить (NEEDS_CONTEXT), не удалять.

- [ ] **Шаг 2: `UpdateClient.swift`**

Удалить `static func internetReachable() async -> Bool` целиком — от doc-комментария
`/// Lightweight reachability probe to GitHub (distinguishes "no internet" from "API failed").`
до закрывающей `}` функции включительно (внутри — длинный комментарий про Wi-Fi машинки и
`URLSessionConfiguration.ephemeral`). Заголовок `// MARK: - Internet reachability + firmware cache`
над ним заменить на `// MARK: - Firmware cache`.

Удалить строку `static var cachedBuild: Int? { cachedBuild(for: .car) }` и строку
`static var hasCachedFile: Bool { hasCachedFile(for: .car) }`.

- [ ] **Шаг 3: Документы**

В `docs/superpowers/specs/2026-06-14-forced-update-gate-design.md` после строки, начинающейся с
`**Статус:**`, вставить (с пустой строкой до и после):

```markdown
*Порядок гейтов машинки заменён 2026-09-16 — см. `2026-09-16-one-release-gate-design.md`.
Предзагрузка образа до встречи с машинкой снята вместе с причиной (softAP без интернета):
релиз узнаётся один раз в шаге адаптера, образ качается внутри принудительного обновления.*
```

В `docs/superpowers/specs/2026-09-16-one-release-gate-design.md`:

- `**Статус:** спека, 2026-09-16.` → `**Статус:** реализовано 2026-09-16 (план `docs/superpowers/plans/2026-09-16-one-release-gate.md`).`
- в таблице §3, строка `| \`UpdateClient.swift\` | …` — заменить текст ячейки «Что» на
  `\`internetReachable()\`, статические \`cachedBuild\`/\`hasCachedFile\` без \`for:\` (машинные умолчания, читал только \`carGate\`)`
  и ячейки «После» на
  `удалены; \`needsDownload\`-обёртка остаётся — её читает XCTest-цель (\`AJMiddleCarTests/ControlModelTests.swift\`); \`for device:\` варианты остаются (их читает \`FirmwareFlow\`); \`cachedBinURL\` без \`for:\` остаётся — его читает \`migrateCacheIfNeeded\``.

- [ ] **Шаг 4: Сборка и весь набор**

`xcodebuild` из Global Constraints → `BUILD SUCCEEDED`. Затем из корня:

```bash
tools/test-all.sh 2>&1 | tail -8
```

Ожидается: `== all green ==`. (Эта работа хост-тестов не касается; прогон — проверка, что
`app/tests/*` не ссылались на удалённое.)

- [ ] **Шаг 5: Коммит**

```bash
git add app/AJMiddleCar/UpdateClient.swift docs/superpowers/specs/2026-06-14-forced-update-gate-design.md \
        docs/superpowers/specs/2026-09-16-one-release-gate-design.md
git commit -m "refactor(app): drop the internet probe the car's gate alone used; mark the old gate spec

UpdateClient.internetReachable() and the car-default cache accessors had
one reader, carGate(), which is gone; the release lookup's own
.unreachable is the reachability check now. The 2026-06-14 gate spec
points at its replacement.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ"
```

---

## После задач — контроллер, не субагенты

- **Артефакт «AJMiddleCar screens»** (спека §7): в `scratchpad/appshots/build_app_page.py`
  удалить строки S15, S16, S18, S19; у S4/S5/S6 — новые имена фаз/ситуаций и слова; стрелки
  «connected → S15» из S13/S14 перевести в S25; переснять кадры галереи (индексы сдвинулись —
  скрипт ищет по подписям) и перевыпустить.
- **Стенд `-viaDongle`** при подключённом адаптере: S1 → S3 → S4 → … → S25 → S28; отдельно —
  сценарий «адаптер актуален, машинка отстала» (S27 качает `ajmiddlecar.bin` сам). Одна строка
  в `docs/bringup.md`.
- **Память**: `memory/fpv-video.md`/`wire-format-v2.md` — заметка, что лестница запуска стала
  симметричной (2026-09-16), S15–S19 нет.
