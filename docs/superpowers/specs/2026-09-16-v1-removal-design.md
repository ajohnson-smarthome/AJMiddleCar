# Снятие мостиков v1 — одна базовая версия формата

**Статус:** реализовано 2026-09-16 (план `docs/superpowers/plans/2026-09-16-v1-removal.md`).
**Опирается на:** `docs/superpowers/specs/2026-09-13-wire-format-v2-design.md`, раздел
«День-флаг и два мостика через него» — этот документ исполняет обещание, данное там:
«[`LegacyIdentity`] удаляется, когда ни одна плата в поле не говорит на v1 — для этого
проекта после одного удачного круга обновления».

## Зачем

Круг прошёл. Обе платы ушли с v1 (v1.0+784 машинка / +789 адаптер) на v2-релиз через
адаптер, без кабеля, и с тех пор обновлялись ещё не раз — сейчас на обеих v1.0+879
(`docs/bringup.md`). Плат, говорящих на proto 1, в поле нет и не будет: прошивки
совместимости с v1 никогда не несли, релиз всегда содержит оба образа, и приложение
принудительно обновляет обе платы до своего релиза прежде, чем позволить ехать.

Что осталось от v1 — только в приложении, и только ради того круга: декодер
`LegacyIdentity`, ветка `.legacy` в `DongleReply`/`DongleLink`, зонд `/status` в `CarLink`
с публикацией `probedFw`, и всё, что от него питается (`AppFlow.carProbed`,
`FirmwareFlow.forCar`, `UpdateRules.carReachable`). Этот код исполнялся по назначению ровно
один раз, а с тех пор — каждую секунду, пока приложение здоровается с машинкой, и держит
в лестнице запуска второй, ничем не проверяемый путь. Убираем целиком: формат один, v2 —
не «вторая версия», а единственная. Слово «v2» в коде и документах остаётся только там,
где оно называет число на проводе (`proto: 2`, `CarContract.proto`) — это не версия
относительно чего-то, это значение поля.

Прошивки машинки и адаптера, мок и контракт не меняются: там v1 нет.

## 1. Приложение — что уходит

Всё в `app/AJMiddleCar`. Столбец «после» — состояние, к которому приводит план.

| Файл | Что удаляется | После |
|---|---|---|
| `LegacyIdentity.swift` | весь файл: `struct LegacyIdentity` и `parse(_:)` — декодер `device`/`fw` на верхнем уровне (v1) либо `device.id`/`device.fw` (v2) | файла нет |
| `DongleLink.swift` | `DongleReply.legacy(LegacyIdentity)`; свойство `carriesIdentity`; в `decode` — попытка `LegacyIdentity.parse` после неудачи `DongleStatus`; в `next` — ветка `case .legacy` целиком (сравнение `id.device`, `guard let latestTag else { return .waiting }`, «v1 не отстал от релиза → `.faulty`») | `DongleReply` — четыре случая: `.status`, `.silent`, `.faulty`, `.denied`. `decode`: v2-документ или `.faulty`. `next`: `.status` берёт документ, три остальных — как сейчас |
| `CarLink.swift` | `@Published probedFw`; `probe`, `lastProbeAt`, `probeAfter` (700 мс), `probeSpacing` (5 с); `scheduleProbe()`; его вызовы в `start()` (после `beginPumping`) и в обработке `.sessionClosed`; `probe?.cancel()` и `probedFw = nil` в `.sessionOpened` и в `requestStop` | машинка узнаётся только по ответу на hello. Пока его нет — `state` остаётся не-live, и лестница показывает радар (S25 «Здороваюсь с машинкой») |
| `AppFlow.swift` | `carProbed(fw:)`; в цикле опроса адаптера — два условия `reply.carriesIdentity` (шаг «адаптер найден» и запуск поиска релиза); в `readStatus` — текст лога «body decoded as neither v2 nor a v1 identity»; комментарии про мостик | оба условия — `if case .status = reply`; лог — «body is not a /status document (N bytes)» |
| `AJMiddleCarApp.swift` | `.onChange(of: link.probedFw) { _, fw in flow.carProbed(fw: fw) }` | |
| `FirmwareFlow.swift` | в `forCar(link:)`: `runningFw: { link?.fw ?? link?.probedFw }`, `isReachable: { UpdateRules.carReachable(live:probedFw:) }`; комментарий о зонде над ними | `runningFw: { [weak link] in link?.fw }`, `isReachable: { [weak link] in link?.isLive ?? false }` |
| `FirmwareFlow.swift` (`DongleState.refresh`) | версия адаптера во время его обновления читается через `LegacyIdentity.parse` — «на обеих сторонах перезагрузки», старой и новой раскладке | `if case .status(let s) = DongleReply.decode(data)` → `s.device.fw`; обе стороны перезагрузки теперь одной раскладки |
| `UpdateRules.swift` | `carReachable(live:probedFw:)` — без `probedFw` вырождается в `live` | функции нет; «машинка готова принять `POST /ota`» = `isLive`, что и читает `FirmwareFlow` |
| `DongleLink.swift` (`DongleStep.waiting`, doc) | фраза «Also the answer for a v1 dongle before any release is known…» | |
| `DongleClient.swift` | в комментарии к `statusData()` — «…or, failing that, as a v1 identity — the one piece of the old format this app still understands» | «the raw `/status` body, for `DongleReply.decode`» |
| `CarError.swift` | в комментарии к `apiCode` — «nil for a v1 body or…» | «nil for a body without the envelope or…» |

Семантика, которая меняется, одна: **`CarLink` больше не читает `/status` машинки сам по
себе.** Сегодня это делалось через 700 мс молчания на hello и раз в 5 с потом; единственный
потребитель результата — принудительное обновление v1-машинки. Второго назначения у зонда
нет: v2-машинка отвечает на hello за десятки миллисекунд, hello повторяется каждые 200 мс,
и молчание означает «машинки нет» (или адаптер не в её сети) — что лестница уже показывает
радаром и что зонд никак не улучшал.

Вместе с зондом уходит известная дыра из `wire-format-v2`-заметок: «`.rebooting` в
`FirmwareFlow` может пропустить `.done` для зондированной машинки». Теперь `runningFw`
у машинки — только `fw` из ответа на hello, и версия после прошивки появляется тем же
путём, что и до неё.

### Что остаётся, хоть и называется «legacy»

`UpdateRules.legacyCacheFileName` / `legacyCacheURLs` и `UpdateClient.migrateCacheIfNeeded`
— перенос кэша образа прошивки *внутри телефона* (`firmware-latest.bin` → имя по
устройству). Это история хранилища приложения, не формата провода; к v1 отношения не имеет
и остаётся как есть.

`DongleStep.waiting` остаётся: у него есть второй источник — `net.state` `joining`/`unknown`
в `DongleLink.next`. Уходит только его v1-ветка.

## 2. Что увидит приложение, встретив v1-плату

Ничего особого — и это осознанно. Специальных экранов, подсказок и веток кода не
добавляем:

- **v1-адаптер.** Его `/status` не декодируется как `DongleStatus` → `DongleReply.faulty` →
  S7 «Ошибка адаптера». Честно: для этой сборки приложения он и есть неисправен.
- **v1-машинка.** Молчит на v2-hello → сессии нет → S25 «Здороваюсь с машинкой» без конца.

Обе лечатся кабелем (`idf.py flash`) либо `curl -X POST --data-binary @…bin` на `/ota`
платы — оба пути описаны в `docs/bringup.md` и `firmware/dongle/README.md` и от
приложения не зависят.

## 3. Тесты

Хост-тесты (`tools/test-all.sh` перебирает `app/tests/*/`):

- `app/tests/legacyidentity/` — каталог удаляется целиком (`main.swift`, `sources`).
- `app/tests/donglelink/sources` — строка `LegacyIdentity.swift` убирается; заголовок
  `main.swift` (комментарий, перечисляющий источники и мостик) правится.
- `app/tests/donglelink/main.swift` — удаляются три блока: «the v1 bridge: an old dongle is
  recognised…», «the v1 bridge before any release is known…», «what triggers the release
  lookup: an identity, in either spelling» (проверки `carriesIdentity`). Остаётся проверка
  `DongleReply.decode(Data("junk".utf8)).isFaulty` — она про v2. К ней добавляется одна
  новая: тело в v1-раскладке (`{"device":"ajdongle","fw":"v1.0+789",…}`) декодируется как
  `.faulty` — это и есть зафиксированное решение §2, и тест, который упадёт, если кто-то
  вернёт мостик «по-тихому».
- `app/tests/update/main.swift` — три проверки `carReachable` удаляются вместе с функцией.

`xcodebuild` (симулятор) — обязательный шаг: удаление файла и `@Published`-свойства ловится
только компилятором, хост-тесты не собирают `CarLink`/`AppFlow`.

Стенд, один прогон: симулятор `-viaDongle` при подключённом адаптере проходит лестницу до
пульта с видео. Ничего в лестнице от зонда не зависело, так что это проверка отсутствия
регрессии, не новой функции. Отдельно, для протокола: время от «Здороваюсь с машинкой» до
пульта не должно вырасти — hello и раньше не ждал зонда.

## 4. Документы

- `docs/superpowers/specs/2026-09-13-wire-format-v2-design.md` — в начале раздела
  «День-флаг и два мостика через него» одна строка: «*Мостики сняты 2026-09-16 — см.
  `2026-09-16-v1-removal-design.md`.*» Сам раздел остаётся: это запись о том, как круг был
  пройден.
- `docs/protocol.md` — v1 не упоминает; правок нет.
- `tools/mock_car/test_state.py:79` — комментарий `# the v1 key` у датаграммы с ключом
  `hello` → `# a key the format does not have`. Проверка остаётся: hello без `session`
  отвергается — это правило формата, не совместимости.
- `CLAUDE.md` — правок нет: v1 там не описан.
- Артефакт с экранами приложения (`scratchpad/appshots/build_app_page.py`, страница
  «AJMiddleCar screens») — у S7 две заметки про v1-адаптер («тело не v1/v2», «после S3/S4:
  v1-адаптер „не отстаёт“ от релиза») переписываются: S7 — «ответ без документа `/status`»,
  а после S3/S4 он бывает только от HTTP-ошибки или обрыва на очередном опросе. Перевыпуск
  артефакта — после реализации, тем же скриптом.

Слово «v2» в комментариях приложения, где оно означает «этот формат» (например, «a live v2
session is the ordinary proof»), правится на нейтральное там же, где правится код; охоты за
ним по всему дереву не устраиваем.

## 5. Проверка завершённости

`grep -rn "LegacyIdentity\|probedFw\|carProbed\|carReachable\|carriesIdentity\|\.legacy\b" app/`
— пусто. `grep -rn "\bv1\b" app/AJMiddleCar` — только версии `v1.0+…` в галерее и ничего
про формат.

## 6. Чего не делаем

- Не трогаем прошивки, контракт, генераторы, мок: v1 в них нет.
- Не добавляем экрана «эта плата на старом формате»: плат нет, а экран — это ещё одна
  ветка на будущее, которое не наступит.
- Не переименовываем `proto: 2` в `proto: 1` и не сбрасываем нумерацию: число на проводе —
  часть контракта, и его смена — это и есть новый день-флаг.
- Не удаляем миграцию кэша прошивки (`legacyCache*`) — см. §1.
