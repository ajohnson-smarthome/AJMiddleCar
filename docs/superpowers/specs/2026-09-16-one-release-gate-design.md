# Один гейт релиза — машинка проверяется так же, как адаптер

**Статус:** реализовано 2026-09-16 (план `docs/superpowers/plans/2026-09-16-one-release-gate.md`).
**Опирается на:** `docs/superpowers/specs/2026-06-14-forced-update-gate-design.md` (гейт
принудительного обновления машинки — то, что здесь перестраивается);
`docs/superpowers/specs/2026-09-16-v1-removal-design.md` (версия машинки приходит только с
hello); каталог экранов приложения (артефакт «AJMiddleCar screens», номера S1…S29 ниже — оттуда).

## Зачем

Сегодня две платы проходят проверку прошивки в разном порядке:

- **адаптер**: найти (S1/S2) → узнать релиз (S4) → сравнить → *если отстал* — скачать и залить (S9);
- **машинка**: узнать релиз (S15) → **скачать заранее** (S19) → найти (S25) → сравнить → залить (S27).

Предзагрузка образа машинки — наследие спеки 2026-06-14: тогда телефон подключался к softAP
машинки напрямую и терял интернет, поэтому всё общение с GitHub делалось *до* встречи с машинкой,
а форс-прошивка шла из кэша. С адаптером (2026-08-29) телефон общается с машинкой по USB и свой
интернет не теряет — причина ушла, порядок остался. Последствия: образ машинки (~1,9 МБ)
качается при первом запуске после каждого релиза независимо от того, отстала ли машинка и
включена ли она вообще; S19 стоит на критическом пути каждого такого запуска; в лестнице
второй запрос `releases/latest`, вторая проверка интернета и два экрана («Нет интернета»,
«Не удалось проверить»), дублирующие S5/S6 адаптера, только с кнопкой вместо автоповтора.

Релиз у плат **один** — один тег на оба образа (`UpdateRules.mustUpdate`: «one release tags
both images identically»), образы — два разных файла (`ajmiddlecar.bin`, `ajdongle.bin`).
Значит, номер последнего релиза достаточно узнать один раз, а сравнивать и качать — для каждой
платы отдельно, когда она назвала свою версию. Для адаптера это уже так. Эта спека делает так же
для машинки.

## 1. Лестница после

```
S1/S2   ищем адаптер                       (без изменений)
S3      адаптер найден, проверяем            (без изменений)
S4      узнаём последний релиз — ОДИН раз за запуск; держим S5/S6, пока не узнаем
S9…S11  адаптер отстал → скачать → залить → перезагрузка   (без изменений)
S12–S14 адаптер подключается к сети машинки  (без изменений)
S25     ищем машинку (hello)                 ← сразу после адаптера; S15/S16/S18/S19 больше нет
        версия из hello сравнивается с тегом из S4 (carIdentified, как сейчас)
S27     машинка отстала → FirmwareView сам скачивает ajmiddlecar.bin и заливает — как S9 у адаптера
S28     пульт
```

Без адаптера (симулятор против мока: `CarHost.viaDongle == false`) адаптерного гейта нет, а тег
нужен — шаг S4 (с теми же S5/S6-удержаниями) выполняется **сам по себе**, перед S25. Экраны те
же, что у адаптерного пути; их слова становятся нейтральными (§4), потому что теперь они
описывают релиз, а не адаптер.

## 2. Приложение — `AppFlow`

**Один тег.** `latestTag` и `dongleLatestTag` сливаются в один `@Published latestTag: String?`.
Его читают `DongleLink.next` (адаптер) и `carIdentified` (машинка). `recheckDongleRollback()`
сбрасывает его в `nil`, как сейчас сбрасывает `dongleLatestTag` — тег переспрашивается на
следующем опросе. `dongleRelease` (записывается, никогда не читается) удаляется.

**Шаг релиза — одна функция, два вызова.** Блок внутри `dongleGate()` (после
`setPhase(.dongleUpdateCheck)`: `latestReleaseLookup(for: .dongle)` → `found`/`noImage`/
`unreachable`) выносится в

```swift
/// One attempt to learn the newest release. Sets `latestTag` and returns true, or sets the
/// holding phase (`.releaseOffline` / `.releaseMissing`) and returns false — the caller sleeps
/// a poll interval and asks again.
private func fetchRelease(for device: UpdateRules.Device) async -> Bool
```

Логика — та, что есть: `found` + `GateRule.canVerify` → `latestTag = rel.tag`, `true`;
`found` без номера сборки или `noImage(tag)` → `.releaseMissing(tag: tag, device: device)`,
`false`; `unreachable` → `.releaseOffline`, `false`. Правило «S4 не показывать повторно, пока
держатся S5/S6» (`holding`) остаётся у вызывающего, как сейчас.

- `dongleGate()` вызывает `fetchRelease(for: .dongle)` на каждом опросе, пока `latestTag == nil`
  — ровно как сегодняшний inline-блок.
- `startupCheck()` без адаптера:

  ```swift
  if CarHost.viaDongle && !dongleHandedOver {
      await dongleGate(); dongleHandedOver = true          // тег получен внутри
  } else if !CarHost.viaDongle {
      await releaseGate()                                   // тот же шаг, сам по себе
  }
  setPhase(.awaitingCar)
  ```

  где `releaseGate()` — цикл `while latestTag == nil { if !holding { setPhase(.releaseCheck) };
  if await fetchRelease(for: .car) { break }; sleep(donglePollInterval) }`. `for: .car`, потому
  что без адаптера проверять наличие `ajdongle.bin` в релизе не за что; тег — тот же.

`UpdateClient.migrateCacheIfNeeded()` (первая строка нынешнего `carGate()`) переезжает в начало
`startupCheck()`.

**Фазы.** Удаляются `checkInternet`, `noInternet`, `checkUpdate`, `checkFailed`, `downloading`
(и их ветки в `opensLink` и в `root`; `PhasePacer` фаз по именам не знает). Переименовываются, потому
что больше не принадлежат адаптеру:

| было | стало |
|---|---|
| `.dongleUpdateCheck` | `.releaseCheck` |
| `.dongleOffline` | `.releaseOffline` |
| `.dongleNoRelease(tag:)` | `.releaseMissing(tag:, device:)` |

`carGate()` удаляется целиком; `retry()` (его единственные вызывающие — удаляемые экраны)
удаляется; `dongleReturned()` — комментарий про «car's own gate already answered» упрощается:
тег один, он держится.

## 3. Что уходит из приложения

| Место | Что | После |
|---|---|---|
| `AppFlow.swift` | `carGate()`, `retry()`, пять фаз, `dongleLatestTag`, `dongleRelease` | см. §2 |
| `AJMiddleCarApp.swift` | ветки `case .checkInternet, .checkUpdate`, `.downloading, .checkFailed`, `.noInternet` в `root` | ветки для трёх переименованных фаз |
| `ConnectView.swift` | ситуация `.carUpdateCheck` (S15) | нет; `.adapterUpdateCheck` → `.releaseCheck`, `.offline` → `.releaseOffline`, `.noRelease(tag:)` → `.releaseMissing(tag:device:)` |
| `NoInternetView.swift` (S16), `UpdateCheckView.swift` (S18/S19) | файлы целиком | удалены; `DeviceArt.swift` — комментарий про `NoInternetView` |
| `L.swift`, `Localizable.strings` | `gate.noInternetTitle/Sub`, `gate.checkFailedTitle/Sub`, `carUpdCheckTitle/Sub` | удалены; `gate.noReleaseTitle/Sub`, `dongle.updCheckTitle/Sub`, `dongle.offlineSub` — новые слова, §4 |
| `UpdateClient.swift` | `internetReachable()`, статические `cachedBuild`/`hasCachedFile` без `for:` (машинные умолчания, читал только `carGate`) | удалены; `needsDownload`-обёртка остаётся — её читает XCTest-цель (`AJMiddleCarTests/ControlModelTests.swift`); `for device:` варианты остаются (их читает `FirmwareFlow`); `cachedBinURL` без `for:` остаётся — его читает `migrateCacheIfNeeded` |
| `GalleryView.swift` | кадры «Step 6 car update», «NoInternet», «UpdateCheck checking / downloading / failed» | удалены (5 кадров) |

`GateRule` (`mayDrive`, `canVerify`), `UpdateRules` (`needsDownload`, `flashPlan`, `mustUpdate`,
`isUpdateAvailable`) и их хост-тесты — без изменений.

## 4. Слова экранов S4/S5/S6

Экраны те же (арт, раскладка, отсутствие кнопки, автоповтор каждые 1,5 с). Меняются только
подписи — они теперь про релиз, и их видит и путь без адаптера:

| экран | заголовок | подпись |
|---|---|---|
| S4 `.releaseCheck` | «Проверяю обновления» | «Смотрю, какой выпуск последний. Обеим платам нужна одна и та же версия — обновлю до поездки, а не посреди неё.» |
| S5 `.releaseOffline` | «Нет интернета» | «Чтобы проверить прошивки, нужен интернет. Как только он появится, продолжу сам.» |
| S6 `.releaseMissing` | «Нет выпуска для адаптера» / «…для машинки» (по `device`) | «В последнем выпуске (%@) нет прошивки адаптера/машинки, поэтому сверить версию не с чем. Пока такой выпуск не опубликован, ехать нельзя.» |

Ключи: `gate.releaseCheckTitle/Sub`, `gate.noInternetTitle` (остаётся), `gate.offlineSub`,
`gate.noReleaseTitle.car/.dongle`, `gate.noReleaseSub.car/.dongle`. Старые `dongle.updCheck*`,
`dongle.offlineSub`, `gate.noReleaseTitle/Sub` удаляются.

## 5. Принудительное обновление машинки (S27) — без изменений в механике

`FirmwareView(.car, forced)` уже делает всё сам: `FirmwareFlow.check()` спрашивает
`latestRelease(for: .car)`, `download()` через `UpdateRules.flashPlan` берёт образ из кэша, если
он там есть, иначе качает, потом заливает. Предзагрузка в S19 была для него только оптимизацией
— после неё `flashPlan` выбирал `.useCache`. Теперь на первом форсе после релиза он выберет
`.download` и покажет «Скачивание» — ровно как S9 у адаптера.

Одно поведение меняется и принимается: **форс машинки без интернета** (GitHub упал между S4 и
S27) — раньше проходил из кэша, теперь `check()` даёт `.failed` с «Повторить», если в кэше нет
образа новее машинки. Адаптер живёт так с рождения; лестница и так не пропускает без интернета
(S5 держит).

## 6. Тесты и проверка

- Хост-тесты: новых чистых правил нет — решение «тег или удержание» уже есть в
  `GateRule.canVerify` + `ReleaseLookup`; остальное — оркестрация в `AppFlow`, которая
  хост-тестами не покрывается и сейчас. `tools/test-all.sh` — зелёный; `app/tests/update`
  трогает только `needsDownload`/`flashPlan`, которые остаются.
- `xcodebuild` — обязателен: удаление фаз и файлов ловит только компилятор (все `switch` по
  `Phase` исчерпывающие).
- Симулятор против мока (без `-viaDongle`): лестница — S4 (нейтральные слова) → S25 → S28;
  при выключенном Wi-Fi на Mac — S5 держит и отпускает сам, когда интернет вернулся.
- Симулятор `-viaDongle` при подключённом адаптере: S1 → S3 → S4 → (S9…) → S12… → S25 → S28;
  запуск после релиза с актуальным адаптером и отставшей машинкой: S27 качает и заливает —
  сценарий «релиз общий, а качать надо только одной плате».
- Галерея: три переименованных кадра S4/S5/S6 с новыми словами; пяти кадров нет.

## 7. Документы

- Артефакт «AJMiddleCar screens»: строки S15, S16, S18, S19 удаляются; S4/S5/S6 — новые названия
  фаз/ситуаций и слова; стрелка «connected → S15» из S13/S14 ведёт в S25; в таблице таймингов
  строка «Показ скачивания ≥ 1,2 с» остаётся (это `FirmwareFlow`).
- `docs/superpowers/specs/2026-06-14-forced-update-gate-design.md` — в шапке одна строка:
  «*Порядок гейтов машинки заменён 2026-09-16 — см. `2026-09-16-one-release-gate-design.md`.
  Предзагрузка образа до встречи с машинкой снята вместе с причиной (softAP без интернета).*»
- `CLAUDE.md` — правок нет: «the whole launch ladder including the adapter's own update, the
  car's forced update» остаётся верным.

## 8. Чего не делаем

- Не трогаем `FirmwareFlow.check()`: он по-прежнему сам спрашивает `latestRelease(for:)` при
  открытии `FirmwareView` — это проверка свежести экрана обновления, она же работает из
  настроек без форса. Передавать ему тег из гейта — отдельный вопрос, не этот.
- Не убираем кэш образов и `UpdateRules.flashPlan`: образ, скачанный один раз, по-прежнему не
  качается второй.
- Не добавляем машинке аналога `.dongleRolledBack` — про откат машинки вне сценария заливки
  (см. разговор об откате) — отдельная тема.
