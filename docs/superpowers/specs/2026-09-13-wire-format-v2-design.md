# Формат v2 — один словарь для машинки и адаптера

**Статус:** дизайн, утверждён 2026-09-13; план — `docs/superpowers/plans/2026-09-13-wire-format-v2.md`; реализовано 2026-09-13 (ветка wire-format-v2)
**Заменяет:** раскладки proto 1 в `docs/protocol.md`, `contract/car-api.json` и
`contract/dongle-api.json`; список полей `/status` и `/net` из
`docs/superpowers/specs/2026-08-30-dongle-api-design.md`.

## Проблема

Форматы росли по одному полю за раз, и это видно. Чтобы прочитать датаграмму или тело
`/status`, сегодня нужен исходник рядом:

- **Сокращения на проводе.** `t`, `y`, `rx_fps`, `wdt_trips`, `ctl`, `calib`,
  `to_car_x10`, `gear_x100`, `errno_age`. У каждого была причина быть коротким; ни одна из
  причин не звучала как «читатель это поймёт».
- **Тип сообщения угадывается по набору ключей.** Датаграмма с `hello` — привет, с `bye` —
  прощание, с обеими осями — команда. Парсер и классификатор сессии повторяют это правило
  каждый по-своему, а клиент, который чуть ошибся, молча отбрасывается.
- **Версия протокола едет только на hello.** Все остальные датаграммы безверсионны: в
  захвате трафика, в логе реле, в паре разных версий нечем определить, на каком диалекте
  написана строка байт.
- **`/status` — свалка.** Личность, качество связи, владелец актуатора, память, здоровье
  радио и события хранилища лежат рядом на одном уровне, а машинка и адаптер пишут одно и
  то же по-разному: `uptime_s` и `uptime`, `heap` у обоих означает свободную кучу, `rssi`
  меряется с противоположных концов линка.
- **Состояние — флаг плюс магическое значение.** `radio.ok:false` с
  `radio.fw:"unavailable"`; `rssi:0` как «не измерено»; `errno:0` как «никогда не
  падало»; `calibrated`, `bus_ok`, `rollback` как россыпь булевых. Каждое магическое
  значение — правило, которое читатель обязан знать.
- **Ошибки — проза.** `{"error":"out of range","field":"ramp_ms"}` даёт приложению
  предложение, которое нельзя локализовать, и ни одного кода, по которому можно
  переключиться.
- **Калибровка позиционная.** `wheels[]` — это FL, FR, RL, RR по индексу, а `sign: 1|-1`
  означает «перевёрнуто или нет».
- **Пять адресов для одной конфигурации**, у каждого свой GET и POST, тогда как
  приложение читает и пишет их одним экраном.

Ничего из этого не сломано. Это причина, по которой новому читателю нужен код, и причина,
по которой двум устройствам в приложении нужны два декодера для одной идеи.

## Решения, уже принятые (2026-09-13)

Предложены три подхода: только переименование; переименование плюс группировка;
конверт JSON-RPC поверх одного адреса. Выбрана **группировка**, и тем же разговором
решено:

- оси управления — `throttle` и `turn`;
- владелец актуатора, когда рулит приложение, — `remote`;
- конфигурация — один объект на одном адресе `/config`.

## Что такое v2

Сначала правила, потом каждое сообщение. Правила — то, что делает сообщения угадываемыми.

### Правила

1. **`proto` в каждом JSON, который отдаёт устройство, и в каждой датаграмме, которую
   приложение шлёт по UDP.** Тела HTTP-запросов его не несут — URL и ответ устройства уже
   несут. Машинка отбрасывает любую UDP-датаграмму, чей `proto` не её собственный, hello
   включительно; на hello всё равно *отвечает* (своим `proto`, чтобы несовпадение было
   видно), но не принимает — ровно сегодняшнее правило, распространённое с hello на все
   датаграммы.
2. **`type` на каждой UDP-датаграмме** — слово строчными, называющее сообщение.
   Датаграмма с неизвестным `type` или без него отбрасывается. У HTTP нет `type`: тип — это
   URL.
3. **snake_case**, единицы — суффиксами (`_ms`, `_s`, `_mm`, `_pct`, `_dbm`, `_hz`), без
   суффиксов фиксированной точки на проводе (`x10`, `x100` становятся дробями; см. `fixed`
   в разделе про схему). Ключ никогда не повторяет имя родителя (`ramp.rise_ms`, а не
   `ramp.ramp_ms`).
4. **Состояния — слова, в поле `state`** (или в именованном поле с перечислимыми
   словами, например `owner`). Булево используется только для настоящего да/нет и
   называется прилагательным или причастием: `calibrated`, `enabled`, `configured`,
   `rolled_back`.
5. **`null`, а не магическое значение, для измерения, которого нет**: RSSI, который не
   читали; канал без подключения; последняя ошибка, которой не было.
6. **Группы по подсистемам.** `device` (личность), `system` (аптайм, память), `link`,
   `motors`, `radio`, `storage` у машинки; `device`, `system`, `usb`, `wifi`, `relay` у
   адаптера. Группа — один и тот же объект, где бы ни встречалась: `device` в ответе на
   hello — это `device` в `/status`; поле, которое есть в группе у обоих устройств, у обоих
   имеет одно имя, тип и смысл; устройство может добавить в группу поле (`device.idf` у
   адаптера), но не переназвать.
7. **Одна форма ошибки**: `{"proto":N,"error":{"code":"…","message":"…","field":"…"}}`.
   `code` — слово из списка контракта, по нему переключается приложение; `message` — одно
   английское предложение для лога; `field` — путь через точку к виноватому ключу,
   отсутствует, когда виновато тело целиком. HTTP-статусы остаются: 400 — запрос, который
   устройство не примет; 409 — который не может обслужить сейчас; 500 — который пыталось и
   не смогло.
8. **Запись отвечает тем, что устройство теперь хранит**, а не `{"ok":true}`:
   `POST /config` возвращает всю конфигурацию, `POST /wifi` — сеть и состояние радио.
   Только действия без ресурса — spin, OTA — отвечают `{"proto":N,"ok":true}`.
9. **Каждая запись — POST.** HTTP-глаголы здесь не несут смысла; URL говорит «что», POST
   говорит «записать». (`PUT`/`PATCH` добавили бы только вторую вещь, которую надо знать, и
   правку транспорта в приложении.)
10. **Неизвестный ключ в теле HTTP-запроса — ошибка** (`unknown_field`). Это API двух
    сторон, где опечатка — баг, а не клиент из будущего. Неизвестные ключи в *ответе*
    приложение игнорирует, как сегодня, — прошивка может вырастить поле раньше, чем
    приложение о нём узнает. UDP-парсер по-прежнему игнорирует ключи, которых не ищет: он
    никогда не перечисляет датаграмму и не должен начинать на пути управления.

### Канал реального времени — UDP 4210

Не меняется: порт, частоты (команды 10 Гц, телеметрия 5 Гц, hello ~5 Гц до ответа),
сторожевой таймер 300 мс, простой сессии 10 с, `max_command` 96 и `max_datagram` 320,
ворота последовательности `(int32_t)(seq − last) > 0`, «последний hello побеждает», кольцо
мёртвых sid и правило, что стоп — это команда с нулями, а не тишина.

```jsonc
// приложение → машинка
{"proto":2,"type":"hello","session":"7f3a91c2"}
{"proto":2,"type":"drive","seq":1234,"throttle":0.50,"turn":-0.25}
{"proto":2,"type":"bye","seq":1235}

// машинка → приложение
{"proto":2,"type":"hello_ack","session":"7f3a91c2",
 "device":{"id":"ajmiddlecar","fw":"v1.0+784","build":784,"rolled_back":false}}
{"proto":2,"type":"telemetry","seq":88,
 "link":   {"rx_hz":10,"rssi_dbm":-58,"timeouts":0},
 "motors": {"bus":"ok","calibrated":true,"owner":"remote"},
 "system": {"uptime_s":812,"free_heap":200000}}
```

| v1 | v2 | примечание |
|---|---|---|
| `hello` (ключ с sid) | `type:"hello"` + `session` | правила sid те же: отправитель шлёт 8 hex-символов, приёмник берёт 1–15 буквенно-цифровых |
| ответ на hello (без маркера) | `type:"hello_ack"` | личность — группа `device`, тот же объект, что в `/status`: `build` и `rolled_back` приходят с рукопожатием, и приложение больше не ходит в `/status` за флагом отката |
| `t`, `y` | `throttle`, `turn` | −1…1, два знака, точка C-локали |
| `bye:1` + нулевые оси | `type:"bye"` + `seq` | оси были нулями, которые стоп и так пишет; ушли |
| телеметрия (без маркера) | `type:"telemetry"` | |
| `rx_fps` | `link.rx_hz` | принятых `drive` в секунду |
| `rssi` (0 = нет) | `link.rssi_dbm` (`null` = нет) | |
| `wdt_trips` | `link.timeouts` | срабатываний сторожевого таймера с включения |
| `bus_ok` | `motors.bus`: `ok` / `down` | |
| `calibrated` | `motors.calibrated` | остаётся булевым: оно и есть булево |
| `ctl` | `motors.owner` | значения ниже |
| `uptime_s`, `heap` | `system.uptime_s`, `system.free_heap` | |

`motors.owner`: `idle`, `recovering`, `console`, `remote`, `calibration`, `update`,
`safe_stop` — вместо v1 `none`, `recover`, `console`, `rt`, `calib`, `ota`, `safe`.
Перечислители `link_src_t` в `link.h` не меняются; меняются только слова, которые пишет
контракт.

Размеры: `drive` — 66 байт с четырёхзначным `seq` и 72 при самом широком счётчике против
лимита 96; `telemetry` ~190 байт против 320; `hello_ack` ~130.

**Парсер.** `control_proto.c` остаётся плоским, без аллокаций, сканером ключей глубины 1.
Он получает ключ `type` (`proto` уже был) и теряет один (`bye` становится типом). Правило
«есть что исполнять» становится «известный тип, у которого есть нужные ключи»: `hello`
нужен `session`; `drive` — `seq`, `throttle`, `turn`; `bye` — `seq`. Классификатор в
`rt_link.h` переключается по типу, а не по наличию ключей. Ничто в парсере не учится
спускаться внутрь объекта — телеметрия *печатается* вложенной, но машинкой никогда не
разбирается.

### Машинка — HTTP, порт 80

```jsonc
// GET /  → текст, без изменений: "ajmiddlecar v1.0+784"

// GET /status — только чтение
{"proto":2,
 "device": {"id":"ajmiddlecar","fw":"v1.0+784","build":784,"rolled_back":false},
 "link":   {"rx_hz":10,"rssi_dbm":-58,"timeouts":0},
 "motors": {"bus":"ok","calibrated":true,"owner":"remote"},
 "radio":  {"fw":"3.0.6","expected":"3.0.6","state":"ok"},
 "storage":{"reset_at_boot":false},
 "system": {"uptime_s":812,"free_heap":200000}}
```

- `device.build` — число после `+` в `fw`, целым, чтобы ворота обновления сравнивали два
  целых, а не разбирали тег.
- `radio.state`: `ok`, `mismatch`, `unavailable` — вместо `ok:bool` плюс магического
  `fw:"unavailable"`. `radio.fw` — `null`, когда недоступно.
- `storage.reset_at_boot` вместо `nvs_wiped`: формат NVS сменился, и все настройки на этой
  загрузке вернулись к умолчаниям.
- `link`, `motors`, `system` — байт в байт группы телеметрии: один принтер, два вызова, как
  сегодня. `link.rx_hz` в `/status` сохраняет окно «от опроса до опроса».

```jsonc
// GET /config → все домены; POST /config ← любое подмножество доменов, каждый целиком
{"proto":2,
 "ramp":     {"rise_ms":300},
 "trim":     {"balance_pct":0},
 "recovery": {"enabled":true,"window_ms":5000},
 "wheel":    {"diameter_mm":65,"encoder_ppr":11,"gear_ratio":9.0,"quadrature":4},
 "chassis":  {"track_mm":130,"wheelbase_mm":210}}
```

| v1 | v2 |
|---|---|
| `GET/POST /ramp {"ramp_ms"}` | `ramp.rise_ms` |
| `GET/POST /trim {"trim_pct"}` | `trim.balance_pct` (+ притормаживает левый борт, − правый) |
| `GET/POST /recover {"enabled","window_ms"}` | `recovery.enabled`, `recovery.window_ms` |
| `GET/POST /wheel {"diameter_mm","ppr","gear_x100","quad"}` | `wheel.diameter_mm`, `wheel.encoder_ppr`, `wheel.gear_ratio` (дробь, 1.00…300.00), `wheel.quadrature` |
| `GET/POST /dims {"track_mm","wheelbase_mm"}` | `chassis.track_mm`, `chassis.wheelbase_mm` |

Диапазоны и умолчания не двигаются. `POST /config` сначала проверяет всё тело и не
применяет ничего, если хоть что-то не прошло — ответ либо полная конфигурация, как она
теперь хранится (200), либо одна ошибка с именем первого плохого поля (400). Каждый
присутствующий домен должен быть полным; отсутствующий не трогается. Проверка «не
изменилось — не пишем во флеш» по доменам остаётся.

```jsonc
// GET /calibration
{"proto":2,"calibrated":true,
 "wheels":[{"corner":"front_left", "pair":0,"inverted":false},
           {"corner":"front_right","pair":1,"inverted":false},
           {"corner":"rear_left",  "pair":2,"inverted":true},
           {"corner":"rear_right", "pair":3,"inverted":false}]}
// POST /calibration ← тот же массив "wheels"; четыре угла, каждый один раз; пары 0–3, каждая один раз
//                    → тело GET /calibration, как теперь хранится
// POST /calibration/spin ← {"pair":0,"direction":"forward"}   → {"proto":2,"ok":true} | 409 busy
// POST /ota ← образ, как есть                                  → {"proto":2,"ok":true}
```

`GET /calibration` теперь возвращает таблицу, а не только флаг — мастер может показать, что
хранит машинка. `corner` вместо позиции в массиве; `inverted` вместо `sign` (`sign:-1` ≡
`inverted:true`); `direction` `forward`/`reverse` вместо `dir` 1/0. `calibrated:false`
приходит с пустым массивом `wheels`.

```jsonc
// любая ошибка, с HTTP 400 / 409 / 500
{"proto":2,"error":{"code":"out_of_range","message":"ramp.rise_ms must be 0..2000","field":"ramp.rise_ms"}}
```

Коды ошибок машинки: `bad_json`, `missing_field`, `unknown_field`, `wrong_type`,
`out_of_range`, `not_allowed` (значение enum вне списка; повтор угла или пары), `busy`
(409), `too_small`, `not_firmware`, `write_failed` (500), `internal` (500).

### Адаптер — HTTP, 192.168.7.1:8080

```jsonc
// GET /status
{"proto":1,
 "device":{"id":"ajdongle","fw":"v1.0+789","build":789,"rolled_back":false,"idf":"v6.0.2"},
 "usb":   {"state":"up"},
 "wifi":  {"ssid":"AJMiddleCar","configured":true,"state":"connected",
           "rssi_dbm":-53,"channel":1,"attempts":{"used":0,"max":5}},
 "relay": {"to_car_hz":10.0,"to_phone_hz":5.0,"udp_sessions":1,"tcp_connections":2,
           "last_error":{"errno":118,"message":"No route to host","count":3,"age_s":41}},
 "system":{"uptime_s":412,"free_heap":8551152}}

// POST /wifi ← {"ssid":"AJMiddleCar","password":"drive1234"}
//           → {"proto":1,"ssid":"AJMiddleCar","state":"searching"}
// POST /ota  ← образ, как есть → {"proto":1,"ok":true}
```

| v1 | v2 |
|---|---|
| `device`, `fw`, `idf`, `rollback` | `device.id`, `device.fw`, `device.build`, `device.idf`, `device.rolled_back` |
| `usb` | `usb.state` |
| `net.ssid`, `net.state`, `net.rssi`, `channel`, `attempts`, `attempts_max` | `wifi.ssid`, `wifi.state`, `wifi.rssi_dbm` (`null`, если не подключён), `wifi.channel` (`null`, если не подключён), `wifi.attempts.used` (сегодняшний `attempts`, 0–5), `wifi.attempts.max` |
| `GET /net {"ssid","configured"}` | нет — `wifi.ssid` и `wifi.configured` в `/status` |
| `POST /net` → `{"ok":true}` | `POST /wifi` → `{"ssid","state"}` |
| `relay.to_car_x10`, `to_phone_x10` | `relay.to_car_hz`, `relay.to_phone_hz` (один знак) |
| `relay.slots_udp`, `slots_tcp` | `relay.udp_sessions`, `relay.tcp_connections` |
| `relay.errno`, `errno_count`, `errno_age` (0 = никогда) | объект `relay.last_error` или `null` |
| `uptime`, `heap` | `system.uptime_s`, `system.free_heap` |

У API адаптера свой `proto`, с 1: это отдельный контракт устройства, которое ничего не
знает о машинке, и его номер двигается, когда меняется *его* формат. У машинки 1 → 2,
потому что номер у неё уже был.

`wifi.state` сохраняет пять слов. `POST /wifi` сохраняет семантику из 13d9a87: новая сеть
запоминается в RAM и ищется заново; та же сеть, пока подключён или ищет, — ничего не
делать (200, текущее состояние); та же сеть после `failed` — начать поиск заново.

Коды ошибок адаптера: `bad_json`, `missing_field`, `unknown_field`, `wrong_type`,
`bad_length` (SSID или пароль вне границ), `bad_chars`, `radio_refused` (500),
`too_small`, `not_firmware`, `write_failed` (500), `busy` (409), `internal` (500).

### Что не меняется

Порты, адреса, частоты, таймауты, лимиты байт, правила сессии, тело OTA (сырой образ, тот же
нижний порог размера, тот же бюджет простоя), именование релизов (`v<семвер>+<сборка>`, два
файла), пересылка обоих портов адаптером без разбора, консольный `mix <t> <y>` и
**хранилище** машинки в NVS (следующий раздел).

## Хранилище остаётся v1

Машинка хранит каждый домен одной JSON-строкой, и ключи внутри неё пишет сам модуль домена
— `ramp.c` пишет `{"ramp_ms":…}`, `wheel.c` пишет `{"diameter_mm","ppr","gear_x100","quad"}`
и так далее, — а не таблица дескрипторов. Поэтому имена на проводе могут меняться, а
хранимые — нет: `cfg_api.c` передаёт значения домена в сеттер модуля массивом `int32_t` в
порядке дескриптора, а модуль сохраняет их под своими прежними ключами. В NVS ничего не
переименовывается, миграция не нужна, и машинка после обновления на v2 загружается с теми
настройками, что были. Целое поля `fixed` (`gear_x100: 900`) — ровно то, что `wheel.c` и
так хранит.

Блоб калибровки (`calib`: `{"deadzone","wheels":[{"pair","sign"}]}`) так же не зависит от
API — `calib_api.c` преобразует на границе — и остаётся как есть.

Адаптер о сети ничего не хранит (13d9a87), хранить нечего.

## Контракт

Обе схемы вырастают, чтобы описывать то, что сейчас лишь называют. Их читают
`tools/gen_contract.py` и `tools/gen_dongle.py`; ничего ниже по течению руками не пишется.

### `contract/car-api.json`

```jsonc
{
  "proto": 2,
  "device": "ajmiddlecar",
  "network": {…без изменений…},
  "rt": {
    "port": 4210, "max_datagram": 320, "max_command": 96,
    "command_hz": 10, "telemetry_hz": 5, "watchdog_ms": 300, "session_idle_ms": 10000,
    "keys": {"proto":"proto","type":"type","session":"session","seq":"seq",
             "throttle":"throttle","turn":"turn"},
    "types": {"hello":"hello","hello_ack":"hello_ack","drive":"drive","bye":"bye",
              "telemetry":"telemetry"},
    "doc": "…правила sid, без изменений…"
  },
  "envelope": {"proto":"proto","ok":"ok","error":"error","code":"code","message":"message","field":"field"},
  "endpoints": {"root":"/","status":"/status","config":"/config",
                "calibration":"/calibration","spin":"/calibration/spin","ota":"/ota"},
  "groups": {
    "device": {"swift":"DeviceInfo","fields":[
               {"name":"id","type":"str"}, {"name":"fw","type":"str"}, {"name":"build","type":"int"},
               {"name":"rolled_back","type":"bool"}]},
    "link":   {"swift":"LinkInfo","fields":[
               {"name":"rx_hz","type":"int"}, {"name":"rssi_dbm","type":"int","nullable":true},
               {"name":"timeouts","type":"int"}]},
    "motors": {"swift":"MotorsInfo","fields":[
               {"name":"bus","type":"state","swift":"MotorsBus","values":["ok","down"]},
               {"name":"calibrated","type":"bool"},
               {"name":"owner","type":"state","swift":"MotorsOwner",
                "values":["idle","recovering","console","remote","calibration","update","safe_stop"]}]},
    "radio":  {"swift":"RadioInfo","fields":[
               {"name":"fw","type":"str","nullable":true}, {"name":"expected","type":"str"},
               {"name":"state","type":"state","swift":"RadioState","values":["ok","mismatch","unavailable"]}]},
    "storage":{"swift":"StorageInfo","fields":[{"name":"reset_at_boot","type":"bool"}]},
    "system": {"swift":"SystemInfo","fields":[
               {"name":"uptime_s","type":"int"}, {"name":"free_heap","type":"int"}]}
  },
  "telemetry": {"swift":"Telemetry","groups": ["link","motors","system"]},
  "status":    {"swift":"CarStatus","groups": ["device","link","motors","radio","storage","system"]},
  "config": {
    "path": "/config",
    "swift": "CarConfig",
    "domains": [
      {"key":"ramp","nvs_key":"ramp","swift":"Ramp","doc":"…",
       "fields":[{"name":"rise_ms","type":"int","min":0,"max":2000,"default":300,"doc":"…"}]},
      {"key":"trim", …"fields":[{"name":"balance_pct",…}]},
      {"key":"recovery","nvs_key":"recover",…},
      {"key":"wheel", …"fields":[…,{"name":"encoder_ppr",…},
                                  {"name":"gear_ratio","type":"fixed","scale":100,
                                   "min":100,"max":30000,"default":900,"doc":"gear ratio; 1:9 is 9.0"},
                                  {"name":"quadrature","type":"enum","values":[1,2,4],"default":4}]},
      {"key":"chassis","nvs_key":"dims",…}
    ]
  },
  "calibration": {
    "corners": ["front_left","front_right","rear_left","rear_right"],
    "directions": ["forward","reverse"],
    "pairs": 4,
    "keys": {"calibrated":"calibrated","wheels":"wheels","corner":"corner",
             "pair":"pair","inverted":"inverted","direction":"direction"}
  },
  "errors": ["bad_json","missing_field","unknown_field","wrong_type","out_of_range","not_allowed",
             "busy","too_small","not_firmware","write_failed","internal"]
}
```

Типы полей: `int`, `bool`, `str`, `number` (дробь, которую устройство только сообщает),
`enum` (целое из списка — как сегодня), `state` (слово из списка, с именем Swift-enum'а),
`fixed` (на проводе дробь, внутри `значение × scale` целым; `min`, `max`, `default` — в
целой области, так что C-таблица остаётся `int32`; округление — от нуля, как `lround`:
9.125 → 913), `object` (один уровень вложенности внутри группы, со своими `fields` и
именем `swift`), плюс `"nullable": true` на любом типе. Каждая группа называет свою
Swift-структуру; у машинки без префикса (`DeviceInfo`, `LinkInfo`…), у адаптера с
`Dongle` (`DongleDevice`, `DongleWifi`…), потому что оба сгенерированных файла
компилируются в каждый хост-тест.

### `contract/dongle-api.json`

Получает `"proto": 1`, тот же `envelope`, форму `groups`/`status` (`device`, `usb`, `wifi`,
`relay`, `system` — `wifi.attempts` и `relay.last_error` — поля типа `object`),
`"endpoints": {"status":"/status","wifi":"/wifi","ota":"/ota"}`, `wifi_request` (`ssid`,
`password`) для тела POST, `wifi_reply` (`ssid`, `state`) для его ответа и свой список
`errors`. `bounds` остаётся; словари состояний живут на полях `state`.

### Что выдают генераторы

- **C (`cfg_table.inc`, `dongle_contract.inc`).** Как сегодня плюс: `RT_TYPE_*` для типов
  сообщений; `KEY_GROUP_<GROUP>` для имён групп и `KEY_<GROUP>_<FIELD>` для каждого поля
  группы (`KEY_<GROUP>_<FIELD>_<SUB>` внутри `object`); слова состояний как
  `<GROUP>_<FIELD>_<VALUE>` (`MOTORS_OWNER_REMOTE`, `RADIO_STATE_OK`) с `_COUNT`; `ERR_*`
  для кодов ошибок; `KEY_PROTO`, `KEY_OK`, `KEY_ERROR`, `KEY_ERROR_CODE`,
  `KEY_ERROR_MESSAGE`, `KEY_ERROR_FIELD` для конверта; `PATH_*`; `CORNER_*`,
  `DIRECTION_*`, `KEY_CALIB_*`; таблица дескрипторов с колонкой `scale` (`1` для всего,
  кроме `fixed`) и JSON-ключом домена `key` там, где был `path`; `RT_PROTO` /
  `DONGLE_PROTO`. У адаптера, как сегодня, префикс `DONGLE_`. Принтеры в `telemetry.h`,
  `device_json.h`, `status_json.c`/`status_api.c` и `calib_api.c` собирают свои форматные
  строки из этих макросов, так что переименованный ключ не переживёт в одном месте и не в
  другом.
- **Swift (`CarAPI.swift`, `DongleAPI.swift`).** Структуры конфигурации как сегодня, с
  `gear_ratio: Double`, плюс `CarConfig` (каждый домен опционален, чтобы POST мог нести
  подмножество). Новые `Codable`-структуры, имена свойств которых — имена на проводе:
  snake_case, без `CodingKeys`, по соглашению, которому уже следуют сгенерированные
  структуры конфигурации: по структуре на группу (`DeviceInfo`, `LinkInfo`, `MotorsInfo`,
  `RadioInfo`, `StorageInfo`, `SystemInfo`; `DongleDevice`, `DongleUsb`, `DongleWifi`,
  `DongleWifiAttempts`, `DongleRelay`, `DongleRelayError`, `DongleSystem`), `Telemetry`,
  `CarStatus`, `DongleStatus`, `DongleWifiReply`, `Calibration`/`CalibWheel` и
  `CarAPIError`/`DongleAPIError` для конверта. Поля состояний — enum'ы со случаями
  контракта плюс `unknown(String)` (`MotorsBus`, `MotorsOwner`, `RadioState`,
  `DongleUsbState`, `DongleWifiState`), чтобы прошивка, вырастившая слово, не ломала
  декодирование; `CalibCorner`, `CalibDirection`, `CarErrorCode`, `DongleErrorCode` так же.
  `RTType` называет типы датаграмм.
- **Python (`generated.py`).** Валидатор вложенного `/config` (`validate_config`,
  `to_wire`, `from_wire`, `lround`); таблицы групп, конверта, адресов, кодов ошибок,
  ключей и типов RT, калибровки.
- **`docs/protocol.md`.** Врезаемая таблица конфигурации группируется по доменам;
  рукописные части переписываются под v2.

## Изменения по компонентам

### Прошивка машинки (`firmware/car/core/main`)

- `control_proto.{c,h}`: ключ `type`; `session` вместо `hello`; ключ `bye` уходит; кадр
  несёт `type` как enum, правило приёма — по типу. `has_ty` → `has_axes`.
- `rt_link.{c,h}`: классификатор переключается по типу; `send_hello_reply` печатает
  `type` `hello_ack`, `session` и группу `device` (один принтер, общий со `status_api.c`);
  `proto` проверяется на каждой датаграмме, не только на hello.
- `telemetry.{c,h}`: принтер трёх групп; `rssi` 0 печатается как `null`; `ctl` → слова
  `owner` через `link_src_name()` (отображение в `link.h`, новые слова).
- `status_api.c`: `device`, общие группы, `radio` (`state` из `s_radio_ok`/«недоступно»),
  `storage`. `build` — из того же номера `git rev-list`, что в строке версии.
- `cfg_api.c`, `cfg_contract.h`: один маршрут `/config`; GET обходит все домены; POST
  разбирает каждый присутствующий домен по его дескриптору в два прохода (проверить всё,
  затем применить всё); `scale` в дескрипторе; `fixed` из числа cJSON как
  `lround(v × scale)`, печать с числом знаков по `scale`. Пять биндингов в `BINDINGS[]`
  остаются; меняется только ключ поиска — с пути на ключ домена. `cfg_json.c` и пять
  модулей доменов не меняются: хранимые имена — их.
- `calib_api.c`: три маршрута; `corner`/`inverted`/`direction` на границе;
  `calibration.c` не трогается.
- `api_util.c`: `api_reply_error(req, status, code, field, message)` печатает конверт v2 с
  `proto`; `api_reply_ok` печатает `{"proto":2,"ok":true}`; новый `api_reply_json`
  добавляет `"proto":2,` перед телом, собранным вызывающим.
- `http_server.c`: маршруты `/config`, `/calibration`, `/calibration/spin`; `/ramp` …
  `/dims`, `/calib`, `/calib/spin`, `/calib/save` удаляются.
- `link.h`: `link_src_name()` возвращает новые слова (`MOTORS_OWNER_*`); `_Static_assert`
  на количество остаётся.
- `main.c`: вывод консоли не меняется; `mix` не меняется.

### Прошивка адаптера (`firmware/dongle/main`)

- `status_api.c`: пять групп; `rssi_dbm`/`channel` печатаются `null`, если не подключён;
  `attempts` объектом; `to_car_hz` из целого ×10 как `%u.%u`; `last_error` объектом с
  `strerror(errno)` в `message` или `null`, когда `relay_stats` ничего не записал
  (`last_errno == 0`). Сам рендер — чистый `status_json.c`, тестируемый на хосте.
- `net_api.c`: маршрут `/wifi` (только POST); тело ответа из текущего `wifi_state`;
  `net_cfg_render_public` уступает место `net_cfg_render_wifi_reply` (`configured`
  переезжает в `/status`).
- `api_util.c`, `ota_api.c`: конверт ошибок v2 и коды; `proto` на каждом ответе.
- `net_cfg.{c,h}`: `net_cfg_err_code()` отображает существующий `net_cfg_err_t` в слова
  контракта (`bad_length`, `bad_chars`) рядом с `net_cfg_err_field()` и
  `net_cfg_err_msg()`.
- `screens.c`/`display.c`: ничего — панель читает состояние через `wifi_state`, а не JSON.

### Приложение (`app/AJMiddleCar`)

- `RTFrame.swift`: `hello`, `command`, `bye` из `CarContract`/`RTType`; `parse`
  переключается по `type`; `Telemetry` становится сгенерированной структурой.
- `DongleStatus.swift`: заменяется сгенерированным `DongleStatus`; `DongleNet` удаляется;
  `DongleClient` получает `POST /wifi` с ответом `DongleWifiReply` и теряет `GET /net`.
- `DongleLink.swift`, `AppFlow.swift`: читают `wifi.ssid`/`wifi.configured` из `/status`
  там, где читали `GET /net`; слова `state` не меняются.
- `CarLink.swift`: `rolled_back` приходит в группе `device` ответа на hello, так что
  `/status` читается только ради `radio`; декодируется `CarStatus`, `radio.state` вместо
  `ok`.
- `ConfigStore.swift`, `ConfigState.swift` и пять экранов конфигурации: один
  `GET /config` и один `POST /config` с подмножеством, которое изменил экран;
  `WheelParamsView` показывает `gear_ratio` дробью.
- `CalibClient.swift`, `CalibrationView.swift`: углы по имени; `inverted`.
- `CarError.swift`: читает код из конверта (`apiCode`); локализованный текст выбирается по
  `code`.
- `UpdateRules.mustUpdate` по-прежнему сравнивает номер сборки из тега (`fw`) — так же
  работает и для личности, прочитанной зондом (следующий раздел); `device.build` доступен
  для будущего упрощения.

### Мок (`tools/mock_car`)

Маршруты и тела следуют сгенерированным таблицам; `rt_link.py` говорит типизированными
датаграммами; `state.py` по существу не меняется.

### Документация

`docs/protocol.md` переписывается под v2 (таблицы сообщений выше — его скелет);
`firmware/dongle/README.md` и `README.md` — примеры обновляются; абзац про контракт в
`CLAUDE.md` упоминает группы.

## День-флаг и два мостика через него

*Мостики сняты 2026-09-16 — см. `2026-09-16-v1-removal-design.md`. Раздел оставлен как
запись о том, как круг был пройден.*

Proto 2 несовместим с proto 1, и так и задумано: приложение принудительно обновляет обе
платы до релиза, с которым поставляется, так что смешанные версии существуют только те
минуты, что идёт обновление. Две вещи должны пережить эти минуты, и сегодняшний код не
умеет ни одну:

1. **v2-приложение должно узнать v1-машинку.** v1-машинка молча отбрасывает v2-hello
   (`session` — ключ, которого она не знает, а датаграмма без ничего знакомого
   отвергается), так что версия машинки не может дойти до ворот обновления через ответ на
   hello. Мостик: когда hello остаются без ответа 2 с, приложение читает `/status` через
   реле и берёт личность **наследственным зондом** — `device`+`fw` на верхнем уровне (v1)
   или `device.id`+`device.fw` (v2). Сборка старше релиза уходит на экран обновления,
   ровно как если бы это сказал ответ на hello; недоступный `/status` оставляет радар
   крутиться, как сегодня.
2. **v2-приложение должно узнать v1-адаптер.** v2-декодер `DongleStatus` падает на
   v1-теле. Тот же зонд читает `device`/`fw` из сырого JSON, и `DongleLink` трактует
   «наследственная личность, старая сборка» как `.updating`, а не как «не адаптер».

Оба мостика — один декодер `LegacyIdentity` (~30 строк) в приложении, с хост-тестом; он
удаляется, когда ни одна плата в поле не говорит на v1 — для этого проекта после одного
удачного круга обновления. Прошивки совместимости с v1 не несут вовсе.

Выкатка: срезать релиз с обоими v2-образами; поставить v2-приложение из Xcode; подключить;
приложение обновляет адаптер, затем машинку, затем едет. Стендовое доказательство этой
спеки — один такой круг, с v1.0+784/+789 до v2-релиза, без кабеля.

## Тестирование

На хосте, всё через `tools/test-all.sh`:

- `test_control_proto`: типизированные датаграммы; датаграмма без `type` или с
  неизвестным отвергается; `drive` без оси, `bye` без `seq`, `hello` без `session`
  отвергаются; `proto` на `drive` не судится парсером; лимит 96 байт с самым длинным
  допустимым `drive`.
- `test_rt_session`: классификация по типу; hello с чужим proto всё равно получает
  `RT_REPLY`; `drive`/`bye` с чужим или отсутствующим proto — `RT_DROP`.
- `test_telemetry` / `test_contract_wire` / `test_device_json`: напечатанные телеметрия и
  группа `device` разбираются как JSON, набор ключей и вложенность совпадают с `groups`
  схемы; `rssi` 0 → `null`; слова владельца — из схемы.
- `test_cfg_table` / `test_cfg_value`: дескриптор несёт `scale` 100 у `gear_ratio` и 1 у
  остальных; домены по JSON-ключу; `fixed` округляет от нуля (9.125 → 913), дробь для
  `int` — ошибка типа, а не усечение.
- `test_calib_wire`: слова углов и направлений ↔ позиции; таблица печатается в порядке
  FL, FR, RL, RR; пустая, когда не откалибровано.
- Адаптер: `test_net_cfg` (коды, тело ответа `/wifi`), `test_status_json` (всё тело
  `/status`, `null` без связи, `last_error: null`, экранирование SSID, худший размер).
- `tools/test_gen_contract.py`: обе схемы; оба генератора; `scale`; Swift-enum'ы несут
  `unknown`; сгенерированный Swift обоих устройств компилируется вместе.
- Приложение: `rtframe` (типизированные кадры, размеры), `sessionpolicy`, `carlink`,
  `legacyidentity` (v1-тело, v2-тело, мусор), `donglestatus` (сгенерированный декодер),
  `donglelink` (включая мостик), `configstate` и `carapi` (один объект, `pick`/`wrap`),
  `intent` (колёса по углам).
- Мок: `test_state.py`, `test_rtlink.py` — типизированные датаграммы, сгруппированная
  телеметрия, атомарный `/config`; конформанс-инструменты `tools/conformance.py` и
  `tools/conformance_rt.py` — поле за полем против контракта.

Стенд: круг выкатки выше; затем один проезд, один проход мастера калибровки, по одному
`POST /config` с каждого экрана настроек и `GET /status` обоих устройств, прочитанный
глазами против этого документа.

## Вне рамок

- Бинарный формат реального времени. JSON на 66 байт × 10 Гц — не цена.
- Версионирование консоли. Это стендовый инструмент, а не протокол.
- Живые v1-адреса в прошивках. Принудительное обновление делает их мёртвым кодом с
  первого удачного круга.
- `PUT`/`PATCH`; `application/problem+json`; ошибки с URI-типами. Ничего из этого не даёт
  выигрыша для API двух сторон на устройстве.
