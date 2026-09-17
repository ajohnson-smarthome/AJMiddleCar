# «Другая машинка» — только в гейте, без рантайм-стража

**Статус:** утверждён в чате 2026-09-17; план — `docs/superpowers/plans/2026-09-17-wrong-car-runtime-out.md`.
**Опирается на:** `docs/superpowers/specs/2026-09-17-board-ladder-design.md` (гейт по `/version`,
`VersionRule` первым проверяет `device`), `docs/superpowers/specs/2026-09-17-proto-out-of-app-design.md`
(тот же приём: убрать рантайм-страховку, довериться проверке при коннекте).

## Зачем

Личность машинки (`device.id`) сейчас проверяется **дважды**: один раз при коннекте — в гейте по
`GET /version` через реле (`VersionRule.step` первым делом сверяет `device == ajmiddlecar` → **S23
«Другая машинка»**), и повторно в рантайме — по `hello_ack` в UDP-сессии (`CarTransport`
отказывается стримить чужой машинке, `CarLink` держит `SessionState.foreign`/`Link.wrongCar` и снова
рисует S23). Второй, рантайм, слой защищает от кейса «правильная машинка отвалилась, а на её месте на
**той же сети** ответила другая» — что требует **двух машинок с одинаковым SSID, но разными id**, то
есть коллизии/мисконфига, посреди сессии, без пропажи провода. Это оверкил: проверки при коннекте
достаточно, а рантайм-слой — мёртвый груз для почти недостижимого случая.

Убираем **рантайм-полицию личности** (`CarTransport`-гейт сессии, `CarLink`-ветку чужой машинки,
`SessionState.foreign`, `Link.wrongCar`, страж `.wrongCar`, `retryAfterWrongCar`, удержание
`identityHold`). Оставляем **гейтную** проверку `/version` (S23 при коннекте и на каждом перезапуске
лестницы) и весь экран `.wrongDevice` как есть.

## Безопасность, которую это НЕ ломает

Главная защита — «нельзя залить нашу прошивку в чужую машинку» — держится не на hello, а на гейте, и
она **остаётся**: прошивка заливается только на `.stage(.car, .updating)`, а туда ведёт `VersionRule`,
который читает `/version` и **первым** сверяет `device`. Даже если рантайм усыновит чужую машинку и
`carIdentified` решит «отстала» → `restart(from: .car)` → `/version` → чужой `device` → **S23**, до
`FirmwareView` дело не дойдёт. Флешить чужую плату по-прежнему невозможно.

## Единственный остаточный кейс (принят)

Чужая машинка с **актуальной** сборкой, подменившая нашу посреди сессии без пропажи провода
(коллизия SSID + тайминг): раньше её ловил hello как S23; теперь `/version` посреди сессии не
перечитывается (перезапуск лестницы бывает только на пропаже пути или устаревшей fw по телеметрии),
поэтому такую машинку **поедем** (не зальём — поедем) с чужой калибровкой. Чужую машинку с **старой**
сборкой — коротко поедем один-два кадра телеметрии, пока `carIdentified` не увидит «отстала» и не
уведёт в `restart → /version → S23`. Оба — принятая цена за снятие оверкила.

## 1. Что меняется — снимаем рантайм-полицию личности

### 1.1 `CarTransport.swift`
- В `session()` удалить блок отказа чужой машинке:
  ```swift
  guard identity.id == CarContract.device else {
      await sayGoodbye(on: socket)
      await holdIdentity()
      throw CarError.malformed("foreign device \(identity.id)")
  }
  ```
  После `emit(.sessionOpened(identity, sid: sid))` сессия усыновляется всегда:
  `sessionAdopted = true; everAdopted = true; …` (стрим-группа как есть).
- Удалить `holdIdentity()`, свойство `identityHold`, метод `retryNow()` — они обслуживали только
  удержание чужой личности. `sayGoodbye(on:)` **оставить** (его зовёт `requestStop(graceful:)`).

### 1.2 `CarLink.swift`
- В `handle(.sessionOpened)` убрать ветвление по `device` — усыновлять любой ответ:
  ```swift
  self.device = info.id
  self.fw = info.fw
  lastTelemetrySeq = nil
  session = .adopted(device: info.id, fw: info.fw)
  fetchRadio()
  config?.prefetchDriveGeometry()
  video.session(sid: sid)
  ```
  (`self.fw = info.fw` теперь безусловно — прежнее «`fw = nil` для чужой» было частью снимаемого
  слоя; флеш-безопасность держит гейт, см. выше.)
- В `handle(.sessionClosed)` заменить `session = session.survivingSessionEnd` на `session = .none`.
- Удалить метод `retryAfterWrongCar()`.

### 1.3 `LinkState.swift`
- Удалить `case foreign(device: String)` из `SessionState`.
- Удалить `var survivingSessionEnd` целиком (его использовал только `.foreign`; после прото- и
  этой правки он тривиален).
- Удалить `case wrongCar(device: String)` из `Link`.
- В `LinkRule.compose` удалить строку `if case .foreign(let device) = session { return .wrongCar(device: device) }`.

### 1.4 `SessionPolicy.swift`
- Удалить `static let identityHoldSeconds` — он питал только `holdIdentity`.

### 1.5 `AJMiddleCarApp.swift`
- В `.onChange(of: link.state)` удалить `case .wrongCar: flow.restart(from: .car)`; switch остаётся
  исчерпывающим по `noDongle`/`localNetworkDenied`/`live`/`searching`.
- В `.onChange(of: flow.phase) == .awaitingCar` убрать вызов `link.retryAfterWrongCar()`; остаётся
  `if link.isLive { flow.carIdentified(fw: link.fw) }`. Комментарий переписать (страж чужой машинки
  снят).

## 2. Что остаётся как есть

- **Гейтная проверка личности:** `VersionRule.step` по-прежнему сверяет `device` первым; **S23
  «Другая машинка»** (`.stage(.car, .wrongDevice)`) и **S9 «Не тот адаптер»** (`.stage(.dongle,
  .wrongDevice)`) — без изменений. Экран `ConnectView.Situation` `.stage(_, .wrongDevice)`, строки
  `wrongCar.title/sub/hint` и аксессоры `L.wrongCarTitle/Sub/Hint`, `dongleWrongTitle/Sub` — **не
  трогаем** (их использует гейт).
- **Флеш-безопасность:** заливка только через `/version`-путь, где `device` сверяется до `.updating`.
- **`sayGoodbye`** (вежливое закрытие сессии при уходе в фон/стопе) — остаётся.
- Провод, контракт, прошивки, мок, генераторы — не трогаем.

## 3. Тесты

- `tests/carlink`: удалить проверки `.foreign`/`.wrongCar`/`survivingSessionEnd` (строки ~38–54);
  добавить, что чужой `hello` теперь **усыновляется** (`compose(.dongleUp, .adopted(device:
  "esp32-car", fw: "v1.0+1"), fresh, 0.1)` == `.live(fresh)`), — идентичность в рантайме больше не
  судится, её судит гейт. Оставить проверки `noDongle`/`localNetworkDenied`/`searching`/`live`.
- `tests/sessionpolicy`: удалить строку `check(SessionPolicy.identityHoldSeconds == 10, …)`.
- Прочие хост-тесты не затрагиваются.

## 4. Документы

- `CLAUDE.md` (раздел iOS): одна строка — «личность машинки проверяется один раз при коннекте (гейт
  `/version` → S23); рантайм-сессия личность не переспрашивает — усыновляет ответивший `hello`;
  залить прошивку в чужую плату всё равно нельзя (гейт сверяет `device` до OTA)».
- `docs/protocol.md`: у `hello_ack`/`device` — фраза «после гейта приложение по `device` не судит:
  сессию открывает любой ответ; чужую машинку отсекает гейт по `/version` до открытия сессии».
- Спеки `board-ladder` и `proto-out-of-app`: пометка, что рантайм-страж «другая машинка» снят этой
  спекой.
- Память `launch-ladder-2026-09` и артефакты (строка «стражи по связи») — после ветки.

## 5. Чего не делаем

- Не убираем гейтную проверку `device` (S23 при коннекте) — это единственная и достаточная проверка
  личности; она же держит флеш-безопасность.
- Не трогаем провод/контракт/прошивки: `device` в `hello_ack` и `/version` остаётся, приложение
  просто перестаёт судить по нему в рантайме.
- Принимаем остаточный кейс «поехать на чужой после mid-session swap на актуальной прошивке» —
  ради снятия оверкила.
