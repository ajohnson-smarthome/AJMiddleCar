# `/version` — незаменяемый эндпоинт личности, одно правило обновления на две платы

**Статус:** спека, 2026-09-17.
**Опирается на:** `docs/superpowers/specs/2026-09-16-one-release-gate-design.md` (один запрос
релиза за запуск, машинка проверяется после адаптера);
`docs/superpowers/specs/2026-09-16-v1-removal-design.md`, §7 (дисциплина «новое поле — только
optional» — здесь она заменяется контрактом); `docs/superpowers/specs/2026-09-13-wire-format-v2-design.md`
(`proto` на каждой датаграмме, `device` в `hello_ack` и `/status`).

## Зачем

Сегодня версию адаптера гейт читает из `/status`, версию машинки — из ответа на `hello` по UDP.
Обе дороги упираются в одно и то же: документ, из которого читается версия, сам зависит от
версии протокола. У адаптера `/status` разбирается строгим генерированным `Codable` целиком —
адаптер с одним лишним обязательным полем становится «неисправным» (S7), не дойдя до
обновления. У машинки ответ на `hello` с чужим `proto` приложение не разбирает вовсе — `fw`
не узнаёт, принудительное обновление не запускается, экран S24 «Другая версия протокола» —
тупик, из которого выход только кабелем. То есть при следующем бампе `proto` обе платы
обновляются кабелем, а гейт, который существует ради «каждое подключение — проверить и
обновить», в этот единственный важный момент не работает.

Решение: **личность и версия платы живут в отдельном эндпоинте `/version`, формат которого
не меняется никогда**, а `/status` остаётся протоколо-зависимым документом состояния. Гейт
обеих плат читает `/version` первым, сравнивает и обновляет, и только потом читает то, что
зависит от протокола. Это одно правило на две платы, и оно же переживает любой день-флаг:
плата на старой прошивке отвечает на `/version` `404`, что и есть «отстала».

## 1. Контракт

### `/version` — в обоих контрактах, одинаково

`endpoints.version = "/version"` в `contract/car-api.json` и `contract/dongle-api.json`;
генераторы выдают `PATH_VERSION` / `DONGLE_PATH_VERSION` для прошивок и
`CarContract.versionPath` / `DongleContract.versionPath` для приложения.

Новый раздел верхнего уровня `version` — в обоих контрактах, **побайтно одинаковый**:

```json
"version": {
  "doc": "GET /version: who this board is and what it runs. The one document whose shape never changes: no field is ever added, renamed or removed; anything else belongs in /status, which lives under proto. A board that answers 404 predates this endpoint and is updated.",
  "fields": [
    {"name": "device",      "type": "str",  "doc": "the device name; ajmiddlecar for the car, ajdongle for the adapter"},
    {"name": "fw",          "type": "str",  "doc": "firmware version as the build prints it: v<semver>+<build>[-<n>-g<sha>[-dirty]]"},
    {"name": "build",       "type": "int",  "doc": "the number after + in fw, parsed by the firmware; 0 when fw carries none"},
    {"name": "proto",       "type": "int",  "doc": "the protocol number of everything else this board serves — this contract's proto"},
    {"name": "rolled_back", "type": "bool", "doc": "the bootloader reverted the last update; sticky until the next successful OTA"}
  ]
}
```

Живые документы:

```json
// машинка — GET http://192.168.7.1/version (через реле, порт 80)
{"device":"ajmiddlecar","fw":"v1.0+879","build":879,"proto":2,"rolled_back":false}
// адаптер — GET http://192.168.7.1:8080/version
{"device":"ajdongle","fw":"v1.0+879","build":879,"proto":1,"rolled_back":false}
```

`proto` здесь — номер протокола **всего остального** API платы: у машинки `car-api.proto`
(hello/drive/телеметрия/видео/`/status`/`/config`…), у адаптера `dongle-api.proto`
(`/status`, `/wifi`, `/ota`, реле). Смысл для приложения один: не совпал с вшитым — эта плата
новее приложения, дальше её не трогаем.

Что из раздела генерируется: прошивкам — макросы ключей (`KEY_VERSION_DEVICE`… /
`DONGLE_KEY_VERSION_*`); моку и конформансу — таблица полей для валидатора. Приложению —
**ничего**: одна рукописная структура `DeviceVersion` (пять полей, `Codable`) в
`app/AJMiddleCar/Identity.swift`, с хост-тестом. Строгий разбор здесь безопасен именно потому,
что формат заморожен; единственный вариант «не разобралось» — `404` старой платы, который
декодера не касается. Тест генератора (`tools/test_gen_contract.py`) проверяет, что разделы
`version` двух контрактов равны побайтно.

### `/status` теряет `device` — у обеих плат, целиком

Группа `device` уходит из `groups` и из `status.groups` обоих контрактов. Машинка:
`/status` = `proto`, затем `link`, `motors`, `radio`, `storage`, `system`, `video` (шесть групп).
Адаптер: `proto`, затем `usb`, `wifi`, `relay`, `system`; поле `idf` переезжает в `system`
(`system.idf`, str). `proto` наверху `/status` остаётся — документ протоколо-зависимый, и это
его номер.

### Что не меняется

`hello_ack` по-прежнему несёт объект `device` (`id`, `fw`, `build`, `rolled_back`) — это
личность UDP-сессии, она сторожит «чужая машинка / чужой `proto` посреди сессии» и
перезагрузку в другую сборку. Решение по версии она больше не принимает. `GET /` машинки
(текст `ajmiddlecar v1.0+879`) остаётся как стендовое удобство.

## 2. Прошивки

**Машинка** (`firmware/car/core/main/status_api.c`): новый хендлер `GET /version` рядом с
`/status`. Принтер `device_group_json` (`device_json.h`) переделывается в `version_json`:
плоский объект из `CAR_DEVICE_ID`, `esp_app_get_description()->version`,
`fw_build_number()`, `RT_PROTO`, `status_api_rolled_back()`; `/status` его больше не зовёт и
начинается с `proto` и `link`. Лимит `max_uri_handlers = 12`, занято 9.

**Адаптер** (`firmware/dongle/main/status_api.c`, `status_json.c`): то же — `GET /version` из
`DONGLE_DEVICE`, версии, `DONGLE_PROTO`, `s_rollback`; `status_json_render` теряет группу
`device`, `idf` печатается в `system`. Лимит `max_uri_handlers = 6`, занято 3.

Два принтера, не общий файл (прошивки друг на друга не ссылаются — как `ota_api.c`,
«deliberate twin»), приколоченные к одному разделу контракта: макросы ключей из генератора и
хост-тест каждой платы, сверяющий отпечаток с эталонной строкой
`{"device":"…","fw":"v1.0+879","build":879,"proto":N,"rolled_back":false}`.

`build` считает прошивка (`fw_build_number`), как и сейчас: `v1.0+881-3-gabc-dirty` → `881`,
без `+` → `0`.

Порядок при загрузке не меняется: `/version` регистрируется там же, где `/status`, и живёт
или умирает вместе с HTTP-сервером; поведение по откату (`mark_app_valid` у машинки после
подъёма AP, у адаптера — последним) прежнее.

## 3. Приложение

### Ответ и правило

Ответ на `GET /version` классифицируется так же, как сегодня `/status` адаптера
(`DongleReply` → `VersionReply`, в `Identity.swift`):

| `VersionReply` | когда |
|---|---|
| `.version(DeviceVersion)` | 200 и пять полей разобрались |
| `.absent` | 404 — плата старше эндпоинта |
| `.silent` | нет кабеля / отказ соединения / таймаут |
| `.faulty` | ответ есть, но не документ (HTTP-ошибка, обрыв, не разобралось) |
| `.denied` | iOS запретил локальную сеть |

Чистое правило `VersionRule.step` (`VersionRule.swift`, хост-тест) — одно на обе платы:

```swift
enum VersionStep: Equatable {
    case plugIn, faulty, accessDenied
    case wrongDevice(name: String)   // device ≠ expected — ни одно другое поле не читается
    case rolledBack                  // rolled_back, и «Повторить» не дал выпуска новее
    case updating                    // .absent, или build < сборки релиза
    case appBehind(proto: Int)       // build ≥ релиза, но proto ≠ вшитого: плата новее приложения
    case ok
}
static func step(reply: VersionReply, expectedDevice: String, latestTag: String,
                 appProto: Int, rollback: RollbackChoice) -> VersionStep
```

Порядок проверок — нынешний из `DongleLink.next`: личность → откат → версия → протокол.
`.absent` → `.updating` без проверки личности (решение раздела 7). `latestTag` — не
optional: гейт, как сейчас, сначала получает релиз (`fetchRelease`) и только потом зовёт
правило. `RollbackChoice` — существующий (`.unanswered` / `.recheck(from:)`), с той же
логикой «выпуск новее того, что предлагался при откате, **и** новее стоящего».

`DongleLink.next` остаётся только с сетевой частью — `sendCredentials / searchingCar /
waiting / retryJoin / readyForCar` — и получает `DongleStatus` уже после `.ok`:

```swift
static func next(status: DongleStatus, expectedSSID: String) -> DongleStep
```

Строгий разбор `/status` (генерированный `Codable`) происходит только у платы, чья версия
сошлась. `DongleReply` и `carriesIdentity`-подобные вещи уходят; классификатор ошибок
транспорта (`DongleReply.of`) переезжает в `VersionReply.of` без изменений.

### Лестница адаптера

Меняется в одном месте: перед `/status` — `/version`.

```
S1 ищу адаптер ── GET /version ──▶ S2 тишина · S7 не документ · S8 доступ запрещён
                                 ▶ S3 проверяю адаптер ──▶ S4 релиз (один раз) ──▶ VersionRule
   VersionRule: S9 не тот адаптер · S10 откатился · S11 обновление · S32 обнови приложение · ok
   ok ──▶ GET /status ──▶ DongleLink.next: S12 · S13 · S29 · S14 · readyForCar
```

S1/S2/S7/S8 теперь про `/version` — он первый запрос, `/status` идёт после `.ok`. Опрос,
удержания, бюджет попыток — как сейчас (`donglePollInterval` 1,5 с). Один опрос = один
`GET /version` + (после `.ok`) один `GET /status`.

### Лестница машинки

Шаг «hello и посмотрим» заменяется симметричным шагом проверки:

```
readyForCar ──▶ S30 проверяю машинку ── GET /version через реле, каждые 1,5 с, таймаут 2 с ──▶ VersionRule
   VersionRule: S23 другая машинка (по device, до hello) · S31 обновление машинки откатилось
                · S27 принудительное обновление · S32 обнови приложение · ok
   ok ──▶ .awaitingCar ──▶ hello ──▶ S28
```

Фазы `AppFlow`: новые `.carChecking` (S30), `.carWrong(device:)` (S23 по `/version`),
`.carRolledBack` (S31), `.appBehind(device: proto:)` (S32, общая для обеих плат). `opensLink` у
всех четырёх — `false`. Шаг живёт в
`carGate()` — отдельной функции после `dongleGate()` (и после `releaseGate()` на пути без
адаптера), по образцу `dongleGate()`: цикл опроса, `setPhase` только на смену. `.silent` на
этом шаге = машинка не отвечает через реле (перезагружается, AP поднимается) — держим S30,
переспрашиваем; `.faulty` — то же с логом, как `readStatus()` сейчас.

`dongleReturned()` после повторного прогона `dongleGate()` идёт в `carGate()`, а не сразу в
`.awaitingCar`: адаптер вернулся — машинку тоже проверяем заново.

### Экраны

| id | фаза → экран | слова |
|---|---|---|
| S30 | `.carChecking` → `ConnectView(.checkingCar)` | «Проверяю машинку» · «Адаптер в сети машинки. Спрашиваю у неё версию — обновлю до поездки, а не посреди неё.» Арт: машинка, кольца «спрашиваю» (как S3 у адаптера) |
| S31 | `.carRolledBack` → `ConnectView(.rolledBack(device: .car), onRecheckRollback:)` | «Обновление машинки откатилось» · «Машинка вернулась на прежнюю версию — обновление откатилось. Тот же образ, скорее всего, откатится снова: «Повторить» проверит, вышла ли новая версия.» Кнопка «Повторить» — как у S10: сброс тега, переспрос релиза |
| S32 | `.appBehind(device, proto)` → `ConnectView(.appBehind(device:proto:))` | «Приложение устарело» · «Адаптер / Машинка говорит на протоколе %d, а это приложение — на %d. Обнови приложение.» Без кнопки: приложение здесь ничего сделать не может; опрос продолжается, чтобы экран ушёл сам, если плату перепрошили |

S10 и S31 — одна ситуация `ConnectView` с параметром платы (`.rolledBack(device:)`, S10 = `.dongle`),
как `.releaseMissing(tag:device:)`; строки — по `device`, как `gate.noReleaseTitle.<device>`.
`WrongCarView` не трогаем: `.carWrong(device:)` рендерится тем же `WrongCarView(.foreignDevice)`,
его «Повторить» здесь — «переспросить `/version`» (опрос и так идёт; кнопка лишь снимает
ожидание); S24 остаётся страховкой от hello.

### `hello` остаётся вторым стражем

`carIdentified(fw:)` продолжает сравнивать `fw` из `hello` с релизом на каждом кадре —
ловит перезагрузку в другую сборку посреди сессии. `proto` из `hello` по-прежнему даёт S24, но
до `hello` доходит только машинка, чей `/version` уже сошёлся, так что S24 — страховка на
случай, когда машинку перепрошили кабелем посреди сессии. `CarLink.rollback` (публикуется,
не читается) удаляется: откат машинки теперь видит гейт через `/version`.

### `FirmwareFlow` без особого случая

У обеих плат `refresh()` = `GET /version` (машинке — через реле, `CarTransport.get(
CarContract.versionPath, timeout: 2)`; адаптеру — `DongleClient`), `runningFw` — из последнего
ответа, `isReachable` — «последний `refresh` ответил». `DongleState` становится `VersionState`
и обслуживает обе платы; `forCar(link:)` больше не читает `link.fw`/`link.isLive`.
Наблюдение за перезагрузкой (`rebootWindow` 60 с / 30 с, опрос 0,5 с) и `flashWhenReachable`
работают по `/version`, без UDP-сессии. Поэтому **`.updateRequired` больше не открывает
связь**: `link.start()` — только с `.awaitingCar` / `.ready` (`opensLink`); заливка идёт по HTTP,
сессия на время прошивки не нужна и только шумела бы `wrongProto`-событиями от машинки со
старым `proto`.

## 4. Мок и конформанс

`tools/mock_car/mock_car.py`: `GET /version` из `car.device`, `car.fw`, `build_number(car.fw)`,
`PROTO`, `car.rollback`; `/status` без `device`. `MOCK_DEVICE=esp32-car` (чужая машинка)
теперь проявляется на S23 через `/version`, до hello. Мока адаптера нет и не появляется:
путь без адаптера — `releaseGate()` → `carGate()` против мока → S28.

`tools/conformance.py`: новая проверка `/version` — пять полей и типы по контракту,
`device == DEVICE`, `build == build_number(fw)`, `proto == PROTO`, `rolled_back` — bool;
проверка `GET /` («строка начинается с device») берёт id из `/version`; валидатор `/status` —
без `device`, `404` на `/version` — провал конформанса.

## 5. Тесты

- Контракт (`tools/test_gen_contract.py`): разделы `version` двух контрактов побайтно равны;
  `endpoints.version` есть в обоих; генераторы эмитят `PATH_VERSION` / `DONGLE_PATH_VERSION`,
  `versionPath` в обоих Swift-контрактах и макросы ключей; `groups.device` отсутствует.
- Прошивки: `firmware/car/core/test/test_device_json.c` → отпечаток `version_json` равен
  эталону; `/status` без `device`. `firmware/dongle/test/test_status_json.c` → то же для
  адаптера, `system.idf` на месте.
- Приложение (host, `swiftc`): новый `app/tests/versionrule` — все восемь исходов, в том числе
  `.absent → .updating`, «откат + `.recheck` + выпуск новее → `.updating`», «откат +
  `.recheck` + новее нет → `.rolledBack`», «`build ≥ релиза`, `proto ≠` → `.appBehind`»,
  «чужой `device` при любых остальных полях → `.wrongDevice`»; новый `app/tests/identity` —
  `DeviceVersion` разбирает оба живых документа, `VersionReply.of` классифицирует ошибки
  транспорта (переезд теста из `donglelink`); `donglelink` переписывается под
  `next(status:expectedSSID:)`; `carlink`, `sessionpolicy`, `rtframe` — без изменений.
- `xcodebuild`; симулятор против мока: S30 → S28 за секунды; `MOCK_DEVICE=esp32-car` → S23.
- Стенд (раздел 7): день-флаг без кабеля, обе платы.

## 6. Документы

`docs/protocol.md` — таблица эндпоинтов (генерируется) и проза: `/version` как единственный
незаменяемый эндпоинт, `/status` без личности, `hello_ack` без изменений;
`firmware/dongle/README.md` — пример `/status` и `/version`; `CLAUDE.md` — «семь
status/telemetry групп» → шесть и упоминание `/version` в абзаце о контракте; спека v1-removal
§7 — строка «дисциплина закреплена контрактом: `/version`, см. эту спеку»; артефакт экранов —
S30/S31/S32, S24 как страховка, кадры галереи трёх новых ситуаций.

## 7. Выкатка и день-флаг

Порядок: контракт + генераторы → обе прошивки (хост-тесты) → мок + конформанс
(`tools/test-all.sh` зелёный) → приложение (`xcodebuild`, симулятор) → документы. Релиз —
обычный, с обоими образами.

Первый запуск приложения с HEAD против плат на прежнем релизе: адаптер отвечает на `/version`
`404` → `.absent` → S11 → обновлён по USB → `/version` отвечает; адаптер подключается к машинке
→ S30 → `404` → S27 → обновлена через реле → `/version` отвечает → S28. **Кабель не нужен ни
одной плате** — это и есть стендовое доказательство спеки.

Решение, принятое осознанно: при `404` личность плата не подтвердила, и приложение считает,
что за `192.168.7.1(:8080)` наша плата. Для этого одного перехода риск нулевой (чужой
USB-Ethernet-адаптер, отвечающий `404` на `/version` и принимающий `POST /ota` с образом ESP,
не существует); после него `404` у наших плат больше не встречается, а у чужой платы правило
останавливается на `.wrongDevice` до всякой заливки.

## 8. Чего не делаем

- Не трогаем `hello_ack`, `GET /`, `proto` наверху `/status`, S23/S24 от hello (остаются страховками).
- Не оставляем в приложении никакой совместимости со старым `/status.device` — старая плата
  проходит через `404 → обновить`, и этого достаточно.
- Не добавляем в `/version` ничего сверх пяти полей — ни `idf`, ни `hw`, ни аптайма; всё это —
  `/status`.
- Не генерируем `DeviceVersion` из схемы: формат заморожен, рукописная структура с тестом —
  и есть его замораживание.
- Не делаем мок адаптера.
