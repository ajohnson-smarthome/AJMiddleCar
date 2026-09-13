# План реализации формата v2 (Wire Format v2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Цель:** Заменить форматы proto 1 машинки и адаптера на сгруппированные, типизированные, самоописывающиеся форматы v2 из спеки — от контракта и генераторов до обеих прошивок, приложения, мока, конформанс-инструментов и документации — и оставить дерево зелёным под `tools/test-all.sh`.

**Архитектура:** Всё висит на двух файлах контракта. Фаза A переписывает их и генераторы, чтобы каждый символ, который нужен остальным фазам, сначала появился в сгенерированном коде; фазы B–E заставляют каждого потребителя говорить на v2, следуя этим символам. Прошивки не несут никакой совместимости с v1; приложение несёт один зонд `LegacyIdentity` (~30 строк), чтобы v2-приложение могло найти и обновить плату на v1.

**Технологии:** ESP-IDF 6.0.2 (`source tools/env-p4.sh`), хост-тесты на C11 обычным `cc` (`make -C firmware/<x>/test run`), генератор на Python 3 (stdlib) + unittest, мок на aiohttp, приложение SwiftUI с хост-тестами через `swiftc`, XcodeGen.

**Спека:** `docs/superpowers/specs/2026-09-13-wire-format-v2-design.md` — каждая форма сообщения, каждое имя поля и каждое правило ниже взяты из неё; если задача и спека расходятся, права спека, а задача ошибается.

## Глобальные ограничения

- Proto машинки `2`; proto адаптера `1` (его первый версионированный формат). `proto` — первый ключ каждого JSON, который отдаёт устройство, и каждой UDP-датаграммы, которую шлёт приложение.
- `type` на каждой UDP-датаграмме: `hello`, `hello_ack`, `drive`, `bye`, `telemetry`. Неизвестный или отсутствующий → датаграмма отбрасывается.
- Имена полей в snake_case с суффиксами единиц; состояния — слова; `null` для недоступного измерения; никаких суффиксов фиксированной точки на проводе.
- Лимиты UDP не меняются: `max_command` 96, `max_datagram` 320. `drive` ≤ 72 байт, `telemetry` ≈ 190, `hello_ack` ≈ 130.
- Конверт ошибки: `{"proto":N,"error":{"code":"…","message":"…","field":"…"}}`, `field` опускается, когда виновато тело целиком; HTTP 400 / 409 / 500 как сегодня.
- Каждая запись — POST. `POST /config` и `POST /wifi` отвечают ресурсом, как он теперь хранится; `spin` и `/ota` отвечают `{"proto":N,"ok":true}`.
- Неизвестный ключ в теле HTTP-запроса → `unknown_field` (400). UDP-парсер ключи никогда не перечисляет.
- Хранилище машинки не трогается: модули доменов (`ramp.c`, `car.c`, `recovery.c`, `wheel.c`, `dims.c`, `calibration.c`) продолжают сами писать свои ключи NVS.
- Сгенерированные файлы никогда не правятся руками. После любого изменения контракта или генератора: `python3 tools/gen_contract.py`, затем `bash tools/check_contract.sh`.
- Сообщения коммитов заканчиваются двумя строками атрибуции из сессии (`Co-Authored-By: …` и `Claude-Session: …`).
- Язык комментариев в коде и документации в репозитории: английский, в стиле проекта (объяснять «почему», а не «что»). Язык этого плана — русский.

## Порядок сборки и «красное окно»

Сгенерированные артефакты общие для всего нижестоящего, поэтому дерево не может быть зелёным на каждом коммите фазы A. Правило плана:

- **Фаза A** (Задачи 1–6) гоняет только `python3 tools/test_gen_contract.py` и `bash tools/check_contract.sh`. После Задачи 6 перегенерированные `cfg_table.inc`, `dongle_contract.inc`, `CarAPI.swift`, `DongleAPI.swift`, `generated.py` и таблица в `docs/protocol.md` закоммичены, а `make -C firmware/car/core/test run`, `make -C firmware/dongle/test run`, Swift-хост-тесты и тесты мока КРАСНЫЕ до своей фазы. Последняя задача каждой фазы делает её тесты зелёными; Задача 27 гоняет весь `tools/test-all.sh`.
- **Фаза B** (прошивка машинки) — `make -C firmware/car/core/test run` в каждой задаче и `idf.py build` в конце.
- **Фаза C** (прошивка адаптера) — `make -C firmware/dongle/test run` в каждой задаче и `idf.py build` в конце.
- **Фаза D** (приложение) — названный хост-тест `app/tests/<name>` в каждой задаче (той же строкой `swiftc`, что в `tools/test-all.sh`) и `xcodebuild` в конце.
- **Фаза E** (мок, конформанс, документация) заканчивается `CONFORMANCE=required tools/test-all.sh`.

Один Swift-хост-тест собирается так (точная строка из `tools/test-all.sh`, с `<name>` и его файлом `sources`):

```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car
name=rtframe; extra=(); while read -r s; do [ -n "$s" ] && extra+=("app/AJMiddleCar/$s"); done < app/tests/$name/sources
swiftc -o /tmp/hosttest_$name app/AJMiddleCar/Generated/CarAPI.swift app/AJMiddleCar/Generated/DongleAPI.swift "${extra[@]}" app/tests/$name/main.swift && /tmp/hosttest_$name
```

## Структура файлов

Создаются:
- `tools/gen_common.py` — эмиттеры групп/состояний/объектов, общие для генераторов машинки и адаптера (C `#define`, Swift-структуры и enum'ы, Python-таблицы).
- `firmware/car/core/main/device_json.h` — чистый: печатает группу `device` (`id`, `fw`, `build`, `rolled_back`) для ответа на hello и `/status`; `fw_build_number()`.
- `firmware/car/core/main/cfg_value.h` — чистый: разбор/печать `fixed` для `cfg_api.c`.
- `firmware/car/core/main/calib_wire.h` — чистый: слово угла/направления ↔ индекс для `calib_api.c`.
- `firmware/car/core/test/test_device_json.c`, `test_cfg_value.c`, `test_calib_wire.c`.
- `firmware/dongle/main/status_json.{c,h}` — чистый: рендер тела `/status` адаптера из простой структуры; тестируется на хосте.
- `firmware/dongle/test/test_status_json.c`.
- `app/AJMiddleCar/LegacyIdentity.swift` — зонд личности v1 (`device`/`fw` сверху или `device.id`/`device.fw`).
- `app/tests/legacyidentity/{main.swift,sources}`.

Изменяются (по фазам): `contract/car-api.json`, `contract/dongle-api.json`, `tools/gen_contract.py`, `tools/gen_dongle.py`, `tools/test_gen_contract.py`, `docs/protocol.md` (A); `control_proto.{c,h}`, `rt_link.{c,h}`, `link.h`, `telemetry.{c,h}`, `status_api.{c,h}`, `api_util.{c,h}`, `cfg_api.c`, `cfg_contract.h`, `calib_api.c`, `ota_api.c`, `http_server.c`, тесты машинки и их `Makefile` (B); `net_cfg.{c,h}`, `api_util.{c,h}`, `status_api.c`, `net_api.c`, `ota_api.c`, `main.c`, тесты адаптера и `Makefile` (C); `RTFrame.swift`, `SessionPolicy.swift`, `CarTransport.swift`, `CarLink.swift`, `LinkState.swift`, `DongleStatus.swift` (удаляется), `DongleClient.swift`, `DongleLink.swift`, `AppFlow.swift`, `ConfigStore.swift`, `ConfigState.swift`, `WheelParamsView.swift`, `CalibClient.swift`, `ControlModel.swift`, `CalibrationView.swift`, `CarError.swift`, `UpdateRules.swift`, `DriveView.swift`, `SettingsView.swift`, `GalleryView.swift`, `L.swift`, `AJMiddleCarApp.swift`, тесты приложения (D); `tools/mock_car/{state.py,rt_link.py,mock_car.py,test_state.py,test_rtlink.py}`, `tools/conformance.py`, `tools/conformance_rt.py`, `docs/protocol.md`, `README.md`, `firmware/dongle/README.md`, `CLAUDE.md`, строка статуса спеки (E).

---
## Фаза A — контракт и генераторы

### Задача 1: `contract/car-api.json` v2

**Файлы:**
- Изменить: `contract/car-api.json` (целиком)
- Тест: `tools/test_gen_contract.py` (`TestSchema`)

**Интерфейсы:**
- Даёт: схему, которую читает каждый эмиттер в Задачах 3–6. Ключи верхнего уровня: `proto`, `device`, `network`, `rt` (с `keys`, `types`), `envelope`, `endpoints`, `groups`, `telemetry`, `status`, `config` (с `path`, `swift`, `domains[].key/nvs_key/swift/fields`), `calibration`, `errors`.

- [ ] **Шаг 1: Заменить `TestSchema` в `tools/test_gen_contract.py` тестами формы v2**

Заменить весь `class TestSchema(unittest.TestCase): …` (всё до `import filecmp`) на:

```python
class TestSchema(unittest.TestCase):
    def test_top_level(self):
        s = load()
        self.assertEqual(s["proto"], 2)
        self.assertEqual(s["device"], "ajmiddlecar")
        self.assertEqual(s["network"]["ssid"], "AJMiddleCar")
        # No host: the app reaches the car only through the dongle.
        self.assertNotIn("host", s["network"])
        self.assertEqual(s["envelope"], {"proto": "proto", "ok": "ok", "error": "error",
                                         "code": "code", "message": "message", "field": "field"})
        self.assertEqual(s["endpoints"], {"root": "/", "status": "/status", "config": "/config",
                                          "calibration": "/calibration",
                                          "spin": "/calibration/spin", "ota": "/ota"})

    def test_rt_constants_and_vocabulary(self):
        rt = load()["rt"]
        self.assertEqual(rt["port"], 4210)
        self.assertEqual(rt["max_datagram"], 320)
        self.assertEqual(rt["max_command"], 96)
        self.assertLess(rt["max_command"], rt["max_datagram"])
        self.assertEqual(rt["command_hz"], 10)
        self.assertEqual(rt["telemetry_hz"], 5)
        self.assertEqual(rt["watchdog_ms"], 300)
        self.assertEqual(rt["session_idle_ms"], 10000)
        self.assertGreater(rt["session_idle_ms"], rt["watchdog_ms"] * 10)
        self.assertEqual(rt["keys"], {"proto": "proto", "type": "type", "session": "session",
                                      "seq": "seq", "throttle": "throttle", "turn": "turn"})
        self.assertEqual(rt["types"], {"hello": "hello", "hello_ack": "hello_ack",
                                       "drive": "drive", "bye": "bye", "telemetry": "telemetry"})

    def test_groups(self):
        g = load()["groups"]
        self.assertEqual(list(g), ["device", "link", "motors", "radio", "storage", "system"])
        names = {k: [f["name"] for f in v["fields"]] for k, v in g.items()}
        self.assertEqual(names["device"], ["id", "fw", "build", "rolled_back"])
        self.assertEqual(names["link"], ["rx_hz", "rssi_dbm", "timeouts"])
        self.assertEqual(names["motors"], ["bus", "calibrated", "owner"])
        self.assertEqual(names["radio"], ["fw", "expected", "state"])
        self.assertEqual(names["storage"], ["reset_at_boot"])
        self.assertEqual(names["system"], ["uptime_s", "free_heap"])
        owner = next(f for f in g["motors"]["fields"] if f["name"] == "owner")
        self.assertEqual(owner["type"], "state")
        self.assertEqual(owner["values"], ["idle", "recovering", "console", "remote",
                                           "calibration", "update", "safe_stop"])
        self.assertEqual(owner["swift"], "MotorsOwner")
        rssi = next(f for f in g["link"]["fields"] if f["name"] == "rssi_dbm")
        self.assertTrue(rssi.get("nullable"))
        for k, v in g.items():
            self.assertTrue(v["swift"], k)
            for f in v["fields"]:
                self.assertIn(f["type"], ("int", "bool", "str", "state"), f"{k}.{f['name']}")
                self.assertTrue(f["doc"].strip(), f"{k}.{f['name']}")
                if f["type"] == "state":
                    self.assertTrue(f["swift"], f"{k}.{f['name']}")
                    self.assertEqual(len(f["values"]), len(set(f["values"])))

    def test_telemetry_and_status_pick_groups_that_exist(self):
        s = load()
        self.assertEqual(s["telemetry"]["groups"], ["link", "motors", "system"])
        self.assertEqual(s["status"]["groups"],
                         ["device", "link", "motors", "radio", "storage", "system"])
        for name in s["telemetry"]["groups"] + s["status"]["groups"]:
            self.assertIn(name, s["groups"])
        self.assertEqual(s["telemetry"]["swift"], "Telemetry")
        self.assertEqual(s["status"]["swift"], "CarStatus")

    def test_config_domains(self):
        c = load()["config"]
        self.assertEqual(c["path"], "/config")
        self.assertEqual(c["swift"], "CarConfig")
        keys = [d["key"] for d in c["domains"]]
        self.assertEqual(keys, ["ramp", "trim", "recovery", "wheel", "chassis"])
        self.assertEqual([d["nvs_key"] for d in c["domains"]],
                         ["ramp", "trim", "recover", "wheel", "dims"])
        self.assertEqual([d["swift"] for d in c["domains"]],
                         ["Ramp", "Trim", "Recovery", "Wheel", "Chassis"])
        for d in c["domains"]:
            self.assertTrue(d["fields"], d["key"])
            for f in d["fields"]:
                where = f"{d['key']}.{f['name']}"
                self.assertTrue(re.fullmatch(r"[a-z][a-z0-9_]*", f["name"]), where)
                self.assertIn(f["type"], ("int", "bool", "enum", "fixed"), where)
                self.assertTrue(f["doc"].strip(), where)
                if f["type"] in ("int", "fixed"):
                    self.assertLess(f["min"], f["max"], where)
                    self.assertGreaterEqual(f["default"], f["min"], where)
                    self.assertLessEqual(f["default"], f["max"], where)
                    if f["type"] == "fixed":
                        self.assertGreater(f["scale"], 1, where)
                elif f["type"] == "enum":
                    self.assertIn(f["default"], f["values"], where)
                    self.assertEqual(len(f["values"]), len(set(f["values"])), where)
                else:
                    self.assertIsInstance(f["default"], bool, where)

    def test_ranges_match_the_firmware_today(self):
        """The schema must describe the firmware that exists, not one we imagined."""
        main = ROOT / "firmware" / "car" / "core" / "main"
        file_for_key = {"wheel": "wheel.h", "chassis": "dims.h", "recovery": "recovery.h",
                        "ramp": "ramp.c", "trim": "car.c"}
        src_by_file = {n: (main / n).read_text() for n in set(file_for_key.values())}
        expected = {
            ("wheel", "diameter_mm"): (20, 150), ("wheel", "encoder_ppr"): (1, 1000),
            ("wheel", "gear_ratio"): (100, 30000),
            ("chassis", "track_mm"): (60, 300), ("chassis", "wheelbase_mm"): (90, 360),
            ("recovery", "window_ms"): (1000, 10000),
            ("ramp", "rise_ms"): (0, 2000), ("trim", "balance_pct"): (-30, 30),
        }
        got = {}
        for d in load()["config"]["domains"]:
            for f in d["fields"]:
                if f["type"] in ("int", "fixed"):
                    got[(d["key"], f["name"])] = (f["min"], f["max"])
        self.assertEqual(got, expected)
        for (key, name), (lo, hi) in expected.items():
            src = src_by_file[file_for_key[key]]
            for bound in (lo, hi):
                pat = rf"(?<![\w.-]){re.escape(str(bound))}(?![\w.])"
                self.assertRegex(src, pat, f"{key} {name}: bound {bound} not in {file_for_key[key]}")

    def test_calibration_and_errors(self):
        s = load()
        c = s["calibration"]
        self.assertEqual(c["corners"], ["front_left", "front_right", "rear_left", "rear_right"])
        self.assertEqual(c["directions"], ["forward", "reverse"])
        self.assertEqual(c["pairs"], 4)
        self.assertEqual(c["keys"], {"calibrated": "calibrated", "wheels": "wheels",
                                     "corner": "corner", "pair": "pair",
                                     "inverted": "inverted", "direction": "direction"})
        self.assertEqual(s["errors"], ["bad_json", "missing_field", "unknown_field", "wrong_type",
                                       "out_of_range", "not_allowed", "busy", "too_small",
                                       "not_firmware", "write_failed", "internal"])
```

- [ ] **Шаг 2: Прогнать тесты схемы и увидеть, как они падают на файле v1**

Команда: `cd /Users/adamjohnson/VSCode/esp32-p4-car && python3 -m unittest tools.test_gen_contract.TestSchema -v 2>&1 | tail -20`
Ожидается: FAIL — `KeyError: 'envelope'`, `AssertionError: 1 != 2` и подобное. (Другие классы файла ещё ссылаются на `domains`/`ctl_values`; они переписываются в Задачах 3–5.)

- [ ] **Шаг 3: Написать схему v2**

Заменить `contract/car-api.json` ровно на:

```json
{
  "proto": 2,
  "device": "ajmiddlecar",
  "network": {
    "ssid": "AJMiddleCar",
    "password": "drive1234"
  },
  "rt": {
    "port": 4210,
    "max_datagram": 320,
    "max_command": 96,
    "command_hz": 10,
    "telemetry_hz": 5,
    "watchdog_ms": 300,
    "session_idle_ms": 10000,
    "keys": {
      "proto": "proto",
      "type": "type",
      "session": "session",
      "seq": "seq",
      "throttle": "throttle",
      "turn": "turn"
    },
    "types": {
      "hello": "hello",
      "hello_ack": "hello_ack",
      "drive": "drive",
      "bye": "bye",
      "telemetry": "telemetry"
    },
    "doc": "Every datagram carries proto and type. session is the session id: producers send 8 hex characters; acceptors take 1-15 alphanumerics. drive and bye carry seq; hello does not."
  },
  "envelope": {
    "proto": "proto",
    "ok": "ok",
    "error": "error",
    "code": "code",
    "message": "message",
    "field": "field"
  },
  "endpoints": {
    "root": "/",
    "status": "/status",
    "config": "/config",
    "calibration": "/calibration",
    "spin": "/calibration/spin",
    "ota": "/ota"
  },
  "groups": {
    "device": {
      "swift": "DeviceInfo",
      "doc": "Who is answering. The same object in the hello reply and in /status.",
      "fields": [
        {"name": "id", "type": "str", "doc": "the device name; ajmiddlecar for this car"},
        {"name": "fw", "type": "str", "doc": "firmware version, v<semver>+<build>"},
        {"name": "build", "type": "int", "doc": "the number after + in fw, as an integer"},
        {"name": "rolled_back", "type": "bool", "doc": "the bootloader reverted the last update"}
      ]
    },
    "link": {
      "swift": "LinkInfo",
      "doc": "The control link as the car sees it.",
      "fields": [
        {"name": "rx_hz", "type": "int", "doc": "drive datagrams received per second"},
        {"name": "rssi_dbm", "type": "int", "nullable": true, "doc": "the driving station's signal at the car, null when not measured"},
        {"name": "timeouts", "type": "int", "doc": "control-watchdog trips since boot"}
      ]
    },
    "motors": {
      "swift": "MotorsInfo",
      "doc": "The actuator: its bus, its calibration, and who is commanding it.",
      "fields": [
        {"name": "bus", "type": "state", "swift": "MotorsBus", "values": ["ok", "down"], "doc": "both PWM boards up and the last write accepted"},
        {"name": "calibrated", "type": "bool", "doc": "a valid wheel calibration is loaded"},
        {"name": "owner", "type": "state", "swift": "MotorsOwner", "values": ["idle", "recovering", "console", "remote", "calibration", "update", "safe_stop"], "doc": "which source owns the actuator"}
      ]
    },
    "radio": {
      "swift": "RadioInfo",
      "doc": "The C6 radio co-processor's firmware against what this build expects.",
      "fields": [
        {"name": "fw", "type": "str", "nullable": true, "doc": "the radio's version, null when it did not answer"},
        {"name": "expected", "type": "str", "doc": "the version this build was made for"},
        {"name": "state", "type": "state", "swift": "RadioState", "values": ["ok", "mismatch", "unavailable"], "doc": "ok when fw equals expected"}
      ]
    },
    "storage": {
      "swift": "StorageInfo",
      "doc": "What happened to the settings at this boot.",
      "fields": [
        {"name": "reset_at_boot", "type": "bool", "doc": "the NVS format changed and every setting went back to its default"}
      ]
    },
    "system": {
      "swift": "SystemInfo",
      "doc": "Uptime and memory.",
      "fields": [
        {"name": "uptime_s", "type": "int", "doc": "seconds since boot"},
        {"name": "free_heap", "type": "int", "doc": "free heap in bytes"}
      ]
    }
  },
  "telemetry": {
    "swift": "Telemetry",
    "groups": ["link", "motors", "system"],
    "doc": "The 5 Hz push: proto, type, seq, then these groups."
  },
  "status": {
    "swift": "CarStatus",
    "groups": ["device", "link", "motors", "radio", "storage", "system"],
    "doc": "GET /status: proto, then these groups."
  },
  "config": {
    "path": "/config",
    "swift": "CarConfig",
    "doc": "GET returns every domain; POST takes any subset of domains, each complete, validates the whole body before applying any of it, and answers with the full configuration as now held.",
    "domains": [
      {
        "key": "ramp",
        "nvs_key": "ramp",
        "swift": "Ramp",
        "doc": "Slew-rate limit on acceleration. Rise is bounded, fall is instant, so stopping is never delayed.",
        "fields": [
          {"name": "rise_ms", "type": "int", "min": 0, "max": 2000, "default": 300, "doc": "time from zero to full scale in ms; 0 disables the ramp"}
        ]
      },
      {
        "key": "trim",
        "nvs_key": "trim",
        "swift": "Trim",
        "doc": "Straight-line correction. Positive slows the left side, negative slows the right; it only ever attenuates.",
        "fields": [
          {"name": "balance_pct", "type": "int", "min": -30, "max": 30, "default": 0, "doc": "percentage by which the faster side is slowed"}
        ]
      },
      {
        "key": "recovery",
        "nvs_key": "recover",
        "swift": "Recovery",
        "doc": "Reverse-replay retreat on unexpected link loss. A deliberate goodbye suppresses it.",
        "fields": [
          {"name": "enabled", "type": "bool", "default": true, "doc": "retrace on unexpected silence; when false the car stops instead"},
          {"name": "window_ms", "type": "int", "min": 1000, "max": 10000, "default": 5000, "doc": "how far back the breadcrumb history reaches"}
        ]
      },
      {
        "key": "wheel",
        "nvs_key": "wheel",
        "swift": "Wheel",
        "doc": "Wheel and encoder geometry. Stored on the car; speed is not yet computed from it.",
        "fields": [
          {"name": "diameter_mm", "type": "int", "min": 20, "max": 150, "default": 65, "doc": "wheel diameter in mm"},
          {"name": "encoder_ppr", "type": "int", "min": 1, "max": 1000, "default": 11, "doc": "encoder pulses per motor-shaft revolution, one channel"},
          {"name": "gear_ratio", "type": "fixed", "scale": 100, "min": 100, "max": 30000, "default": 900, "doc": "gear ratio as a decimal; 1:9 is 9.0 (held as ratio x100 inside)"},
          {"name": "quadrature", "type": "enum", "values": [1, 2, 4], "default": 4, "doc": "quadrature edge multiplier"}
        ]
      },
      {
        "key": "chassis",
        "nvs_key": "dims",
        "swift": "Chassis",
        "doc": "Distances between wheel centres. The track feeds the app's manoeuvre geometry.",
        "fields": [
          {"name": "track_mm", "type": "int", "min": 60, "max": 300, "default": 130, "doc": "lateral distance between left and right wheel centres"},
          {"name": "wheelbase_mm", "type": "int", "min": 90, "max": 360, "default": 210, "doc": "longitudinal distance between front and rear wheel centres"}
        ]
      }
    ]
  },
  "calibration": {
    "swift": "Calibration",
    "wheel_swift": "CalibWheel",
    "corners": ["front_left", "front_right", "rear_left", "rear_right"],
    "directions": ["forward", "reverse"],
    "pairs": 4,
    "keys": {
      "calibrated": "calibrated",
      "wheels": "wheels",
      "corner": "corner",
      "pair": "pair",
      "inverted": "inverted",
      "direction": "direction"
    },
    "doc": "GET /calibration returns calibrated and the wheels table (empty when not calibrated). POST /calibration takes wheels: four corners, each once, pairs 0..3 each once, inverted per wheel. POST /calibration/spin takes pair and direction."
  },
  "errors": ["bad_json", "missing_field", "unknown_field", "wrong_type", "out_of_range", "not_allowed", "busy", "too_small", "not_firmware", "write_failed", "internal"]
}
```

- [ ] **Шаг 4: Прогнать тесты схемы**

Команда: `python3 -m unittest tools.test_gen_contract.TestSchema -v 2>&1 | tail -12`
Ожидается: все 7 PASS.

- [ ] **Шаг 5: Коммит**

```bash
git add contract/car-api.json tools/test_gen_contract.py
git commit -F- <<'MSG'
contract(car): proto 2 — typed datagrams, grouped status, one /config

The schema now describes what it used to only name: the datagram types, the six
status groups with their state vocabularies, the envelope, the endpoints, the
calibration words and the error codes. Domain keys are the JSON keys inside
/config; nvs_key stays what the modules store under.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 2: `contract/dongle-api.json` v2

**Файлы:**
- Изменить: `contract/dongle-api.json` (целиком)
- Тест: `tools/test_gen_contract.py` (`TestDongleSchema`, `TestDongleAgreesWithTheCar`)

**Интерфейсы:**
- Даёт: схему адаптера с `proto`, `envelope`, `endpoints` (`status`, `wifi`, `ota`), `groups` (`device`, `usb`, `wifi`, `relay`, `system`), `status.groups`, `wifi_request`, `wifi_reply`, `bounds`, `errors`.

- [ ] **Шаг 1: Переписать `TestDongleSchema` в `tools/test_gen_contract.py`**

Заменить тело класса (`TestDongleAgreesWithTheCar` оставить как есть — его проверки портов по-прежнему верны) на:

```python
class TestDongleSchema(unittest.TestCase):
    def load(self):
        return json.loads((ROOT / "contract" / "dongle-api.json").read_text())

    def test_identity_and_address_are_pinned(self):
        s = self.load()
        self.assertEqual(s["proto"], 1)
        self.assertEqual(s["device"], "ajdongle")
        self.assertEqual(s["network"], {"host": "192.168.7.1", "port": 8080,
                                        "doc": s["network"]["doc"]})
        self.assertEqual(s["endpoints"], {"status": "/status", "wifi": "/wifi", "ota": "/ota"})
        self.assertEqual(s["envelope"], {"proto": "proto", "ok": "ok", "error": "error",
                                         "code": "code", "message": "message", "field": "field"})

    def test_bounds_are_wpa2s(self):
        b = self.load()["bounds"]
        self.assertEqual((b["ssid_min"], b["ssid_max"]), (1, 32))
        self.assertEqual((b["pass_min"], b["pass_max"]), (8, 63))

    def test_it_names_no_car(self):
        text = (ROOT / "contract" / "dongle-api.json").read_text().lower()
        self.assertNotIn("ajmiddlecar", text)
        self.assertNotIn("drive1234", text)

    def test_groups(self):
        g = self.load()["groups"]
        self.assertEqual(list(g), ["device", "usb", "wifi", "relay", "system"])
        names = {k: [f["name"] for f in v["fields"]] for k, v in g.items()}
        self.assertEqual(names["device"], ["id", "fw", "build", "rolled_back", "idf"])
        self.assertEqual(names["usb"], ["state"])
        self.assertEqual(names["wifi"], ["ssid", "configured", "state", "rssi_dbm", "channel",
                                         "attempts"])
        self.assertEqual(names["relay"], ["to_car_hz", "to_phone_hz", "udp_sessions",
                                          "tcp_connections", "last_error"])
        self.assertEqual(names["system"], ["uptime_s", "free_heap"])
        for k, v in g.items():
            self.assertTrue(v["swift"].startswith("Dongle"), k)
        wifi_state = next(f for f in g["wifi"]["fields"] if f["name"] == "state")
        self.assertEqual(wifi_state["values"], ["idle", "searching", "joining", "connected", "failed"])
        self.assertEqual(wifi_state["swift"], "DongleWifiState")
        usb_state = next(f for f in g["usb"]["fields"] if f["name"] == "state")
        self.assertEqual(usb_state["values"], ["up", "down"])
        attempts = next(f for f in g["wifi"]["fields"] if f["name"] == "attempts")
        self.assertEqual(attempts["type"], "object")
        self.assertEqual([f["name"] for f in attempts["fields"]], ["used", "max"])
        err = next(f for f in g["relay"]["fields"] if f["name"] == "last_error")
        self.assertEqual(err["type"], "object")
        self.assertTrue(err["nullable"])
        self.assertEqual([f["name"] for f in err["fields"]], ["errno", "message", "count", "age_s"])
        for name in ("rssi_dbm", "channel"):
            f = next(f for f in g["wifi"]["fields"] if f["name"] == name)
            self.assertTrue(f.get("nullable"), name)

    def test_status_wifi_and_errors(self):
        s = self.load()
        self.assertEqual(s["status"]["groups"], ["device", "usb", "wifi", "relay", "system"])
        self.assertEqual(s["status"]["swift"], "DongleStatus")
        self.assertEqual(s["wifi_request"], {"ssid": "ssid", "password": "password"})
        self.assertEqual(s["wifi_reply"], {"swift": "DongleWifiReply", "fields": ["ssid", "state"]})
        self.assertEqual(s["errors"], ["bad_json", "missing_field", "unknown_field", "wrong_type",
                                       "bad_length", "bad_chars", "radio_refused", "too_small",
                                       "not_firmware", "write_failed", "busy", "internal"])
```

- [ ] **Шаг 2: Прогнать и увидеть красное**

Команда: `python3 -m unittest tools.test_gen_contract.TestDongleSchema -v 2>&1 | tail -12`
Ожидается: FAIL с `KeyError: 'proto'` и подобным.

- [ ] **Шаг 3: Написать схему адаптера v2**

Заменить `contract/dongle-api.json` ровно на:

```json
{
  "proto": 1,
  "device": "ajdongle",
  "doc": "The vocabulary the app and the dongle must spell identically. Rules live in firmware/dongle/main/net_cfg.{c,h}, which is host-tested; this file carries names, numbers, paths and shapes.",
  "network": {
    "host": "192.168.7.1",
    "port": 8080,
    "doc": "The dongle's own address on the USB wire, and the port it serves. Port 80 and the real-time port are reserved for the car, forwarded through untouched."
  },
  "relay": {
    "http_port": 80,
    "rt_port": 4210,
    "doc": "The ports the dongle listens on for the car's traffic, on its own address. What speaks on them is not the dongle's business."
  },
  "endpoints": {
    "status": "/status",
    "wifi": "/wifi",
    "ota": "/ota"
  },
  "envelope": {
    "proto": "proto",
    "ok": "ok",
    "error": "error",
    "code": "code",
    "message": "message",
    "field": "field"
  },
  "groups": {
    "device": {
      "swift": "DongleDevice",
      "doc": "Who is answering.",
      "fields": [
        {"name": "id", "type": "str", "doc": "the device name; ajdongle for this adapter"},
        {"name": "fw", "type": "str", "doc": "firmware version, v<semver>+<build>"},
        {"name": "build", "type": "int", "doc": "the number after + in fw, as an integer"},
        {"name": "rolled_back", "type": "bool", "doc": "the bootloader reverted the last update"},
        {"name": "idf", "type": "str", "doc": "the ESP-IDF version this image was built with"}
      ]
    },
    "usb": {
      "swift": "DongleUsb",
      "doc": "The wire to the phone.",
      "fields": [
        {"name": "state", "type": "state", "swift": "DongleUsbState", "values": ["up", "down"], "doc": "whether a host is attached"}
      ]
    },
    "wifi": {
      "swift": "DongleWifi",
      "doc": "The station: which network it was told, and how the join is going.",
      "fields": [
        {"name": "ssid", "type": "str", "doc": "the network held since boot; empty when none was sent"},
        {"name": "configured", "type": "bool", "doc": "a network has been sent since boot"},
        {"name": "state", "type": "state", "swift": "DongleWifiState", "values": ["idle", "searching", "joining", "connected", "failed"], "doc": "idle: no network; searching: not seen on the air; joining: seen, getting an address; connected; failed: the attempt budget ran out"},
        {"name": "rssi_dbm", "type": "int", "nullable": true, "doc": "the car's signal at the dongle, null unless connected"},
        {"name": "channel", "type": "int", "nullable": true, "doc": "Wi-Fi channel, null unless connected"},
        {"name": "attempts", "type": "object", "swift": "DongleWifiAttempts", "doc": "the join budget", "fields": [
          {"name": "used", "type": "int", "doc": "attempts spent on the current network"},
          {"name": "max", "type": "int", "doc": "the budget"}
        ]}
      ]
    },
    "relay": {
      "swift": "DongleRelay",
      "doc": "What is being forwarded, and the last time forwarding failed.",
      "fields": [
        {"name": "to_car_hz", "type": "number", "doc": "control datagrams per second toward the car"},
        {"name": "to_phone_hz", "type": "number", "doc": "datagrams per second toward the phone"},
        {"name": "udp_sessions", "type": "int", "doc": "real-time sessions in use, of 4"},
        {"name": "tcp_connections", "type": "int", "doc": "HTTP connections in use, of 4"},
        {"name": "last_error", "type": "object", "swift": "DongleRelayError", "nullable": true, "doc": "the most recent forwarding failure, null when none since boot", "fields": [
          {"name": "errno", "type": "int", "doc": "the POSIX errno"},
          {"name": "message", "type": "str", "doc": "strerror of it"},
          {"name": "count", "type": "int", "doc": "how many times in a row this errno repeated"},
          {"name": "age_s", "type": "int", "doc": "seconds since it last happened"}
        ]}
      ]
    },
    "system": {
      "swift": "DongleSystem",
      "doc": "Uptime and memory.",
      "fields": [
        {"name": "uptime_s", "type": "int", "doc": "seconds since boot"},
        {"name": "free_heap", "type": "int", "doc": "free heap in bytes"}
      ]
    }
  },
  "status": {
    "swift": "DongleStatus",
    "groups": ["device", "usb", "wifi", "relay", "system"],
    "doc": "GET /status: proto, then these groups."
  },
  "wifi_request": {
    "ssid": "ssid",
    "password": "password"
  },
  "wifi_reply": {
    "swift": "DongleWifiReply",
    "fields": ["ssid", "state"],
    "doc": "POST /wifi answers with proto and these two fields of the wifi group, as now held."
  },
  "bounds": {
    "ssid_min": 1,
    "ssid_max": 32,
    "pass_min": 8,
    "pass_max": 63,
    "doc": "WPA2's limits, not ours. A password is empty or pass_min..pass_max; net_cfg enforces that, and the character-class rule it also enforces has no expression here."
  },
  "errors": ["bad_json", "missing_field", "unknown_field", "wrong_type", "bad_length", "bad_chars", "radio_refused", "too_small", "not_firmware", "write_failed", "busy", "internal"]
}
```

- [ ] **Шаг 4: Прогнать оба класса схемы адаптера**

Команда: `python3 -m unittest tools.test_gen_contract.TestDongleSchema tools.test_gen_contract.TestDongleAgreesWithTheCar -v 2>&1 | tail -12`
Ожидается: PASS. (`TestDongleAgreesWithTheCar.test_relay_http_port_is_the_number_the_car_actually_serves` читает `http_server.c` машинки — тот здесь не трогается, поэтому тест остаётся зелёным.)

- [ ] **Шаг 5: Коммит**

```bash
git add contract/dongle-api.json tools/test_gen_contract.py
git commit -F- <<'MSG'
contract(dongle): proto 1 — grouped status, /wifi answers with state

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```
### Задача 3: `tools/gen_common.py` и C-эмиттер машинки

**Файлы:**
- Создать: `tools/gen_common.py`
- Изменить: `tools/gen_contract.py` (`_c_field`, `emit_c`, `field_range`)
- Тест: `tools/test_gen_contract.py` (`TestCommonEmitters`, `TestCEmitter`)

**Интерфейсы:**
- Даёт (Python): `gen_common.c_group_defines(schema, prefix) -> list[str]`, `c_envelope_defines(schema, prefix)`, `c_error_defines(schema, prefix)`, `c_endpoint_defines(schema, prefix)`, `swift_type(field) -> str`, `swift_state_enum(name, values, doc) -> list[str]`, `swift_struct(name, fields, doc) -> list[str]`, `swift_groups(schema) -> list[str]`, `swift_document(name, schema, groups, doc, extra_fields=()) -> list[str]`, `swift_error_envelope(name, code_enum, schema) -> list[str]`, `py_common(schema) -> list[str]`, `lround(x) -> int`.
- Даёт (C, в `cfg_table.inc`): строки `cfg_field_t` теперь из 8 членов (`scale` последний); `cfg_domain_t.key` вместо `.path`; `CFG_CONFIG_PATH`; `RT_KEY_{PROTO,TYPE,SESSION,SEQ,THROTTLE,TURN}`; `RT_TYPE_{HELLO,HELLO_ACK,DRIVE,BYE,TELEMETRY}`; `KEY_{PROTO,OK,ERROR,ERROR_CODE,ERROR_MESSAGE,ERROR_FIELD}`; `PATH_{ROOT,STATUS,CONFIG,CALIBRATION,SPIN,OTA}`; `KEY_GROUP_<G>`, `KEY_<G>_<F>`; `MOTORS_BUS_{OK,DOWN}`, `MOTORS_OWNER_{IDLE,…,SAFE_STOP}`, `MOTORS_OWNER_COUNT`, `RADIO_STATE_{OK,MISMATCH,UNAVAILABLE}`; `ERR_<CODE>`; `CORNER_{FRONT_LEFT,…}`, `CORNER_COUNT`, `DIRECTION_{FORWARD,REVERSE}`, `CALIB_PAIRS`, `KEY_CALIB_{CALIBRATED,WHEELS,CORNER,PAIR,INVERTED,DIRECTION}`; `RT_PROTO`, `CFG_DOMAIN_COUNT`, `CFG_MAX_FIELDS`.

- [ ] **Шаг 1: Добавить `TestCommonEmitters` и переписать `TestCEmitter` в `tools/test_gen_contract.py`**

Заменить `class TestCEmitter…` (до его `test_every_domain_appears_once` включительно) на:

```python
class TestCommonEmitters(unittest.TestCase):
    def setUp(self):
        import gen_common
        self.c = gen_common
        self.s = load()

    def test_group_defines_cover_keys_states_and_counts(self):
        out = "\n".join(self.c.c_group_defines(self.s, ""))
        self.assertIn('#define KEY_GROUP_LINK "link"', out)
        self.assertIn('#define KEY_LINK_RSSI_DBM "rssi_dbm"', out)
        self.assertIn('#define MOTORS_OWNER_REMOTE "remote"', out)
        self.assertIn('#define MOTORS_OWNER_SAFE_STOP "safe_stop"', out)
        self.assertIn("#define MOTORS_OWNER_COUNT 7", out)
        self.assertIn('#define RADIO_STATE_UNAVAILABLE "unavailable"', out)
        prefixed = "\n".join(self.c.c_group_defines(self.s, "X_"))
        self.assertIn('#define X_KEY_GROUP_LINK "link"', prefixed)

    def test_object_fields_get_sub_keys(self):
        d = json.loads((ROOT / "contract" / "dongle-api.json").read_text())
        out = "\n".join(self.c.c_group_defines(d, "DONGLE_"))
        self.assertIn('#define DONGLE_KEY_WIFI_ATTEMPTS "attempts"', out)
        self.assertIn('#define DONGLE_KEY_WIFI_ATTEMPTS_USED "used"', out)
        self.assertIn('#define DONGLE_KEY_RELAY_LAST_ERROR_AGE_S "age_s"', out)
        self.assertIn('#define DONGLE_WIFI_STATE_SEARCHING "searching"', out)
        self.assertIn('#define DONGLE_USB_STATE_DOWN "down"', out)

    def test_envelope_errors_and_endpoints(self):
        env = "\n".join(self.c.c_envelope_defines(self.s, ""))
        self.assertIn('#define KEY_ERROR_CODE "code"', env)
        self.assertIn('#define KEY_PROTO "proto"', env)
        errs = "\n".join(self.c.c_error_defines(self.s, ""))
        self.assertIn('#define ERR_OUT_OF_RANGE "out_of_range"', errs)
        paths = "\n".join(self.c.c_endpoint_defines(self.s, ""))
        self.assertIn('#define PATH_SPIN "/calibration/spin"', paths)

    def test_swift_state_enum_has_unknown_and_round_trips(self):
        out = "\n".join(self.c.swift_state_enum("MotorsBus", ["ok", "down"], "doc"))
        self.assertIn("public enum MotorsBus: Equatable, Sendable, Codable {", out)
        self.assertIn("    case ok", out)
        self.assertIn("    case unknown(String)", out)
        self.assertIn('        case "down": self = .down', out)
        self.assertIn("        default: self = .unknown(rawValue)", out)
        self.assertIn("    public static let all: [MotorsBus] = [.ok, .down]", out)

    def test_swift_struct_types(self):
        fields = [{"name": "rx_hz", "type": "int", "doc": "a"},
                  {"name": "rssi_dbm", "type": "int", "nullable": True, "doc": "b"},
                  {"name": "bus", "type": "state", "swift": "MotorsBus", "values": ["ok"], "doc": "c"},
                  {"name": "to_car_hz", "type": "number", "doc": "d"},
                  {"name": "last_error", "type": "object", "swift": "E", "nullable": True,
                   "fields": [], "doc": "e"}]
        out = "\n".join(self.c.swift_struct("S", fields, "doc"))
        self.assertIn("    public var rx_hz: Int", out)
        self.assertIn("    public var rssi_dbm: Int?", out)
        self.assertIn("    public var bus: MotorsBus", out)
        self.assertIn("    public var to_car_hz: Double", out)
        self.assertIn("    public var last_error: E?", out)
        self.assertIn("public init(rx_hz: Int, rssi_dbm: Int?, bus: MotorsBus, to_car_hz: Double, "
                      "last_error: E?)", out)

    def test_swift_document_puts_proto_first_then_groups(self):
        out = "\n".join(self.c.swift_document("CarStatus", self.s, self.s["status"]["groups"], "d"))
        lines = [l for l in out.splitlines() if l.startswith("    public var ")]
        self.assertEqual(lines[0], "    public var proto: Int")
        self.assertEqual(lines[1], "    public var device: DeviceInfo")
        self.assertEqual(lines[-1], "    public var system: SystemInfo")

    def test_lround_is_half_away_from_zero(self):
        self.assertEqual(self.c.lround(900.5), 901)
        self.assertEqual(self.c.lround(900.4999), 900)
        self.assertEqual(self.c.lround(-2.5), -3)
        self.assertEqual(self.c.lround(2.5), 3)


class TestCEmitter(unittest.TestCase):
    def setUp(self):
        import gen_contract
        self.g = gen_contract
        self.s = load()
        self.out = self.g.emit_c(self.s)

    def test_table_carries_names_ranges_defaults_and_scale(self):
        self.assertIn('{ "rise_ms", CFG_INT, 0, 2000, 300, NULL, 0, 1 }', self.out)
        self.assertIn('{ "enabled", CFG_BOOL, 0, 1, 1, NULL, 0, 1 }', self.out)
        self.assertIn('{ "gear_ratio", CFG_FIXED, 100, 30000, 900, NULL, 0, 100 }', self.out)
        self.assertIn('{ "quadrature", CFG_ENUM, 1, 4, 4, CFG_WHEEL_QUADRATURE_ALLOWED, 3, 1 }',
                      self.out)
        self.assertIn("static const int32_t CFG_WHEEL_QUADRATURE_ALLOWED[] = { 1, 2, 4 };", self.out)

    def test_domains_are_keyed_not_pathed(self):
        self.assertIn('    { "ramp", "ramp", CFG_RAMP_FIELDS, 1 },', self.out)
        self.assertIn('    { "recovery", "recover", CFG_RECOVER_FIELDS, 2 },', self.out)
        self.assertIn('    { "chassis", "dims", CFG_DIMS_FIELDS, 2 },', self.out)
        self.assertIn('#define CFG_CONFIG_PATH "/config"', self.out)
        self.assertEqual(self.out.count("CFG_DOMAINS[] = {"), 1)
        self.assertIn("#define CFG_DOMAIN_COUNT 5", self.out)
        self.assertIn("#define CFG_MAX_FIELDS 4", self.out)

    def test_rt_symbols(self):
        for line in ("#define RT_PORT 4210", "#define RT_MAX_DATAGRAM 320", "#define RT_MAX_COMMAND 96",
                     "#define RT_PROTO 2", '#define RT_KEY_TYPE "type"', '#define RT_KEY_SESSION "session"',
                     '#define RT_KEY_THROTTLE "throttle"', '#define RT_KEY_TURN "turn"',
                     '#define RT_TYPE_HELLO_ACK "hello_ack"', '#define RT_TYPE_DRIVE "drive"',
                     '#define RT_TYPE_TELEMETRY "telemetry"'):
            self.assertIn(line, self.out.splitlines(), line)
        self.assertNotIn("RT_KEY_HELLO", self.out)
        self.assertNotIn("RT_KEY_BYE", self.out)
        self.assertNotIn("CTL_", self.out)

    def test_groups_envelope_errors_paths_and_calibration(self):
        for line in ('#define KEY_GROUP_MOTORS "motors"', '#define KEY_MOTORS_OWNER "owner"',
                     '#define MOTORS_OWNER_IDLE "idle"', "#define MOTORS_OWNER_COUNT 7",
                     '#define KEY_ERROR_FIELD "field"', '#define ERR_BUSY "busy"',
                     '#define PATH_CONFIG "/config"', '#define CORNER_REAR_RIGHT "rear_right"',
                     "#define CORNER_COUNT 4", '#define DIRECTION_REVERSE "reverse"',
                     "#define CALIB_PAIRS 4", '#define KEY_CALIB_INVERTED "inverted"'):
            self.assertIn(line, self.out.splitlines(), line)
```

- [ ] **Шаг 2: Прогнать оба класса и увидеть красное**

Команда: `python3 -m unittest tools.test_gen_contract.TestCommonEmitters tools.test_gen_contract.TestCEmitter 2>&1 | tail -5`
Ожидается: FAIL — `ModuleNotFoundError: No module named 'gen_common'`, затем `KeyError: 'domains'`.

- [ ] **Шаг 3: Создать `tools/gen_common.py`**

```python
#!/usr/bin/env python3
"""Emitters shared by contract/car-api.json and contract/dongle-api.json.

A `group` is an object of named fields a device reports (`link`, `wifi`, …). A `state`
field is a word from a list; an `object` field is one level of nesting inside a group;
`nullable` means the value may be JSON null. The three languages get their names from
the same walk over the same schema, so a field renamed in one file is renamed in every
artifact or in none.
"""
import math
import pprint


def upper(name):
    return name.upper()


def lround(x):
    """C's lround: half away from zero. Python's round() is half-to-even, and a
    validator that disagrees with cfg_api.c about 9.005 is exactly the drift the
    generator exists to prevent."""
    return int(math.copysign(math.floor(abs(x) + 0.5), x))


# ---- C ---------------------------------------------------------------------------

def c_group_defines(schema, prefix):
    out = []
    for gname, g in schema["groups"].items():
        G = upper(gname)
        out.append(f'#define {prefix}KEY_GROUP_{G} "{gname}"')
        for f in g["fields"]:
            F = upper(f["name"])
            out.append(f'#define {prefix}KEY_{G}_{F} "{f["name"]}"')
            if f["type"] == "object":
                for sub in f["fields"]:
                    out.append(f'#define {prefix}KEY_{G}_{F}_{upper(sub["name"])} "{sub["name"]}"')
            if f["type"] == "state":
                for v in f["values"]:
                    out.append(f'#define {prefix}{G}_{F}_{upper(v)} "{v}"')
                out.append(f"#define {prefix}{G}_{F}_COUNT {len(f['values'])}")
        out.append("")
    return out


def c_envelope_defines(schema, prefix):
    e = schema["envelope"]
    return [
        f'#define {prefix}KEY_PROTO "{e["proto"]}"',
        f'#define {prefix}KEY_OK "{e["ok"]}"',
        f'#define {prefix}KEY_ERROR "{e["error"]}"',
        f'#define {prefix}KEY_ERROR_CODE "{e["code"]}"',
        f'#define {prefix}KEY_ERROR_MESSAGE "{e["message"]}"',
        f'#define {prefix}KEY_ERROR_FIELD "{e["field"]}"',
        "",
    ]


def c_error_defines(schema, prefix):
    return [f'#define {prefix}ERR_{upper(c)} "{c}"' for c in schema["errors"]] + [""]


def c_endpoint_defines(schema, prefix):
    return [f'#define {prefix}PATH_{upper(k)} "{v}"' for k, v in schema["endpoints"].items()] + [""]


# ---- Swift -----------------------------------------------------------------------

def swift_type(f):
    if f["type"] in ("state", "object"):
        base = f["swift"]
    elif f["type"] == "fixed" or f["type"] == "number":
        base = "Double"
    else:
        base = {"int": "Int", "bool": "Bool", "str": "String", "enum": "Int"}[f["type"]]
    return base + ("?" if f.get("nullable") else "")


def swift_state_enum(name, values, doc):
    """An enum with the contract's cases plus unknown(String): a firmware that grows a
    word must not make the app fail to decode a document it otherwise understands."""
    cases = ", ".join("." + v for v in values)
    lines = [f"/// {doc}", f"public enum {name}: Equatable, Sendable, Codable {{"]
    lines += [f"    case {v}" for v in values]
    lines += [
        "    case unknown(String)",
        "    public var rawValue: String {",
        "        switch self {",
    ]
    lines += [f'        case .{v}: return "{v}"' for v in values]
    lines += [
        "        case .unknown(let raw): return raw",
        "        }",
        "    }",
        "    public init(rawValue: String) {",
        "        switch rawValue {",
    ]
    lines += [f'        case "{v}": self = .{v}' for v in values]
    lines += [
        "        default: self = .unknown(rawValue)",
        "        }",
        "    }",
        "    public init(from decoder: Decoder) throws {",
        "        self.init(rawValue: try decoder.singleValueContainer().decode(String.self))",
        "    }",
        "    public func encode(to encoder: Encoder) throws {",
        "        var c = encoder.singleValueContainer()",
        "        try c.encode(rawValue)",
        "    }",
        f"    public static let all: [{name}] = [{cases}]",
        "}",
        "",
    ]
    return lines


def swift_struct(name, fields, doc):
    """Property names ARE the wire names — snake_case, no CodingKeys — the convention the
    generated config structs already follow. Synthesised Codable then needs no mapping."""
    lines = [f"/// {doc}", f"public struct {name}: Codable, Equatable, Sendable {{"]
    for f in fields:
        lines.append(f'    /// {f["doc"]}')
        lines.append(f'    public var {f["name"]}: {swift_type(f)}')
    args = ", ".join(f'{f["name"]}: {swift_type(f)}' for f in fields)
    assigns = "; ".join(f'self.{f["name"]} = {f["name"]}' for f in fields)
    lines.append(f"    public init({args}) {{ {assigns} }}")
    lines += ["}", ""]
    return lines


def swift_groups(schema):
    """Every state enum, every object struct, every group struct — nested types first,
    so a file that reads top to bottom meets each name before it is used."""
    out = []
    for g in schema["groups"].values():
        for f in g["fields"]:
            if f["type"] == "state":
                out += swift_state_enum(f["swift"], f["values"], f["doc"])
            if f["type"] == "object":
                for sub in f["fields"]:
                    if sub["type"] == "state":
                        out += swift_state_enum(sub["swift"], sub["values"], sub["doc"])
                out += swift_struct(f["swift"], f["fields"], f["doc"])
        out += swift_struct(g["swift"], g["fields"], g["doc"])
    return out


def swift_document(name, schema, groups, doc, extra_fields=()):
    """A struct made of groups: proto first, then any extras (telemetry's seq), then one
    property per group named exactly as the group is on the wire."""
    fields = [{"name": schema["envelope"]["proto"], "type": "int",
               "doc": "the protocol version the device speaks"}]
    fields += list(extra_fields)
    for gname in groups:
        g = schema["groups"][gname]
        fields.append({"name": gname, "type": "object", "swift": g["swift"], "doc": g["doc"]})
    return swift_struct(name, fields, doc)


def swift_error_envelope(name, code_enum, schema):
    e = schema["envelope"]
    return [
        "/// The error envelope every endpoint answers with, on 400 / 409 / 500.",
        f"public struct {name}: Codable, Equatable, Sendable {{",
        f"    public var {e['proto']}: Int",
        f"    public var {e['error']}: Body",
        "    public struct Body: Codable, Equatable, Sendable {",
        f"        public var {e['code']}: {code_enum}",
        f"        public var {e['message']}: String",
        f"        public var {e['field']}: String?",
        f"        public init({e['code']}: {code_enum}, {e['message']}: String, {e['field']}: String?) {{ "
        f"self.{e['code']} = {e['code']}; self.{e['message']} = {e['message']}; self.{e['field']} = {e['field']} }}",
        "    }",
        f"    public init({e['proto']}: Int, {e['error']}: Body) {{ self.{e['proto']} = {e['proto']}; self.{e['error']} = {e['error']} }}",
        "}",
        "",
    ]


# ---- Python ----------------------------------------------------------------------

def py_common(schema):
    """The tables both mocks and both conformance tools read."""
    return [
        f"PROTO = {schema['proto']}",
        f"DEVICE = {schema['device']!r}",
        f"ENVELOPE = {schema['envelope']!r}",
        f"ENDPOINTS = {schema['endpoints']!r}",
        f"ERRORS = {schema['errors']!r}",
        f"STATUS_GROUPS = {schema['status']['groups']!r}",
        # pformat, not json.dumps: this is a Python module, and JSON writes `true`
        # where Python needs `True`. sort_dicts=False keeps it deterministic.
        f"GROUPS = {pprint.pformat(schema['groups'], indent=4, sort_dicts=False, width=96)}",
    ]
```

- [ ] **Шаг 4: Переписать `field_range`, `_c_field` и `emit_c` в `tools/gen_contract.py`**

Добавить `from gen_common import (c_group_defines, c_envelope_defines, c_error_defines, c_endpoint_defines, swift_state_enum, swift_struct, swift_groups, swift_document, swift_error_envelope, swift_type, py_common, lround)` рядом с импортом `gen_dongle`. Затем заменить `field_range`, `_c_field` и `emit_c` на:

```python
def field_range(f):
    """Human-readable range for one field, escaped for a Markdown table cell."""
    if f["type"] == "int":
        return f"{f['min']}..{f['max']}"
    if f["type"] == "fixed":
        s = f["scale"]
        return f"{f['min'] / s:g}..{f['max'] / s:g}"
    if f["type"] == "enum":
        return " \\| ".join(str(v) for v in f["values"])
    return "true \\| false"


def _allowed_symbol(domain, f):
    return f"CFG_{domain['nvs_key'].upper()}_{f['name'].upper()}_ALLOWED"


def _c_field(domain, f):
    if f["type"] == "int":
        return f'{{ "{f["name"]}", CFG_INT, {f["min"]}, {f["max"]}, {f["default"]}, NULL, 0, 1 }}'
    if f["type"] == "fixed":
        return (f'{{ "{f["name"]}", CFG_FIXED, {f["min"]}, {f["max"]}, {f["default"]}, NULL, 0, '
                f'{f["scale"]} }}')
    if f["type"] == "bool":
        return f'{{ "{f["name"]}", CFG_BOOL, 0, 1, {1 if f["default"] else 0}, NULL, 0, 1 }}'
    sym = _allowed_symbol(domain, f)
    return (f'{{ "{f["name"]}", CFG_ENUM, {min(f["values"])}, {max(f["values"])}, '
            f'{f["default"]}, {sym}, {len(f["values"])}, 1 }}')


def emit_c(schema):
    domains = schema["config"]["domains"]
    out = [f"/* {BANNER} */", "", '#include "cfg_contract.h"', ""]
    for d in domains:
        for f in d["fields"]:
            if f["type"] == "enum":
                vals = ", ".join(str(v) for v in f["values"])
                out.append(f"static const int32_t {_allowed_symbol(d, f)}[] = {{ {vals} }};")
    out.append("")
    for d in domains:
        name = f"CFG_{d['nvs_key'].upper()}_FIELDS"
        out.append(f"static const cfg_field_t {name}[] = {{")
        for f in d["fields"]:
            out.append("    " + _c_field(d, f) + ",")
        out.append("};")
    out.append("")
    out.append("static const cfg_domain_t CFG_DOMAINS[] = {")
    for d in domains:
        name = f"CFG_{d['nvs_key'].upper()}_FIELDS"
        out.append(f'    {{ "{d["key"]}", "{d["nvs_key"]}", {name}, {len(d["fields"])} }},')
    out.append("};")
    out.append("")
    out.append(f'#define CFG_CONFIG_PATH "{schema["config"]["path"]}"')
    out.append(f"#define CFG_DOMAIN_COUNT {len(domains)}")
    out.append(f"#define CFG_MAX_FIELDS {max(len(d['fields']) for d in domains)}")
    out.append("")
    rt = schema["rt"]
    out.append(f"#define RT_PROTO {schema['proto']}")
    out.append(f'#define RT_PORT {rt["port"]}')
    out.append(f'#define RT_MAX_DATAGRAM {rt["max_datagram"]}')
    out.append(f'#define RT_MAX_COMMAND {rt["max_command"]}')
    out.append(f'#define RT_COMMAND_HZ {rt["command_hz"]}')
    out.append(f'#define RT_TELEMETRY_HZ {rt["telemetry_hz"]}')
    out.append(f'#define RT_WATCHDOG_MS {rt["watchdog_ms"]}')
    out.append(f'#define RT_SESSION_IDLE_MS {rt["session_idle_ms"]}')
    for k, v in rt["keys"].items():
        out.append(f'#define RT_KEY_{k.upper()} "{v}"')
    for k, v in rt["types"].items():
        out.append(f'#define RT_TYPE_{k.upper()} "{v}"')
    out.append("")
    out += c_envelope_defines(schema, "")
    out += c_endpoint_defines(schema, "")
    out += c_group_defines(schema, "")
    out += c_error_defines(schema, "")
    cal = schema["calibration"]
    for c in cal["corners"]:
        out.append(f'#define CORNER_{c.upper()} "{c}"')
    out.append(f"#define CORNER_COUNT {len(cal['corners'])}")
    for d in cal["directions"]:
        out.append(f'#define DIRECTION_{d.upper()} "{d}"')
    out.append(f"#define CALIB_PAIRS {cal['pairs']}")
    for k, v in cal["keys"].items():
        out.append(f'#define KEY_CALIB_{k.upper()} "{v}"')
    return "\n".join(out)
```

- [ ] **Шаг 5: Прогнать оба класса**

Команда: `python3 -m unittest tools.test_gen_contract.TestCommonEmitters tools.test_gen_contract.TestCEmitter -v 2>&1 | tail -16`
Ожидается: все PASS.

- [ ] **Шаг 6: Коммит**

```bash
git add tools/gen_common.py tools/gen_contract.py tools/test_gen_contract.py
git commit -F- <<'MSG'
gen: shared group/state/envelope emitters, and the car's C table for v2

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 4: Swift-эмиттер машинки

**Файлы:**
- Изменить: `tools/gen_contract.py` (`_swift_type`, `_swift_literal`, `emit_swift`)
- Тест: `tools/test_gen_contract.py` (`TestSwiftEmitter`)

**Интерфейсы:**
- Даёт (`CarAPI.swift`): `CarContract` (`proto`, `device`, `ssid`, `password`, `rtPort`, `maxDatagram`, `maxCommand`, `commandHz`, `telemetryHz`, `watchdogMs`, `sessionIdleMs`, `protoField`, `typeField`, `sessionField`, `seqField`, `throttleField`, `turnField`, `okField`, `errorField`, `rootPath`, `statusPath`, `configPath`, `calibrationPath`, `spinPath`, `otaPath`); `RTType` (`hello`, `helloAck`, `drive`, `bye`, `telemetry`); enum'ы `MotorsBus`, `MotorsOwner`, `RadioState`; структуры `DeviceInfo`, `LinkInfo`, `MotorsInfo`, `RadioInfo`, `StorageInfo`, `SystemInfo`, `Telemetry(proto:seq:link:motors:system:)`, `CarStatus(proto:device:link:motors:radio:storage:system:)`; конфигурация `Ramp`, `Trim`, `Recovery`, `Wheel` (с `gear_ratio: Double`), `Chassis`, у каждой `static let key`, `static let default`, `<field>Range` (`ClosedRange<Int>` или `ClosedRange<Double>` для `fixed`), `<field>Allowed`, `static func pick(from: CarConfig) -> Self?`, `static func wrap(_:) -> CarConfig`; `CarConfig(proto: Int? = nil, ramp: Ramp? = nil, …)`; `CalibCorner`, `CalibDirection`, `CalibWheel(corner:pair:inverted:)`, `Calibration(proto:calibrated:wheels:)`; `CarErrorCode`; `CarAPIError`.

- [ ] **Шаг 1: Переписать `TestSwiftEmitter`**

```python
class TestSwiftEmitter(unittest.TestCase):
    def setUp(self):
        import gen_contract
        self.out = gen_contract.emit_swift(load())

    def lines(self):
        return self.out.splitlines()

    def test_contract_constants(self):
        for line in ("    public static let proto = 2", '    public static let device = "ajmiddlecar"',
                     "    public static let rtPort: UInt16 = 4210", "    public static let maxCommand = 96",
                     '    public static let typeField = "type"', '    public static let sessionField = "session"',
                     '    public static let turnField = "turn"', '    public static let configPath = "/config"',
                     '    public static let spinPath = "/calibration/spin"',
                     '    public static let okField = "ok"', '    public static let errorField = "error"'):
            self.assertIn(line, self.lines(), line)
        self.assertNotIn("helloField", self.out)
        self.assertNotIn("yawField", self.out)
        self.assertIn('    public static let helloAck = "hello_ack"', self.lines())
        self.assertIn("public enum RTType {", self.out)

    def test_groups_and_documents(self):
        self.assertIn("public enum MotorsOwner: Equatable, Sendable, Codable {", self.out)
        self.assertIn("    case safe_stop", self.lines())
        self.assertIn("public struct LinkInfo: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public var rssi_dbm: Int?", self.lines())
        self.assertIn("public struct Telemetry: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public init(proto: Int, seq: Int, link: LinkInfo, motors: MotorsInfo, "
                      "system: SystemInfo) { self.proto = proto; self.seq = seq; self.link = link; "
                      "self.motors = motors; self.system = system }", self.lines())
        self.assertIn("public struct CarStatus: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public var radio: RadioInfo", self.lines())
        self.assertIn("    public var fw: String?", self.lines())   # RadioInfo.fw is nullable

    def test_config_structs(self):
        self.assertIn("public struct Wheel: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public var gear_ratio: Double", self.lines())
        self.assertIn("    public var quadrature: Int", self.lines())
        self.assertIn('    static let key = "wheel"', self.lines())
        self.assertIn("    static let `default` = Wheel(diameter_mm: 65, encoder_ppr: 11, gear_ratio: 9.0, "
                      "quadrature: 4)", self.lines())
        self.assertIn("    static let gear_ratioRange: ClosedRange<Double> = 1.0...300.0", self.lines())
        self.assertIn("    static let diameter_mmRange: ClosedRange<Int> = 20...150", self.lines())
        self.assertIn("    static let quadratureAllowed: [Int] = [1, 2, 4]", self.lines())
        self.assertIn("    static func pick(from c: CarConfig) -> Wheel? { c.wheel }", self.lines())
        self.assertIn("    static func wrap(_ v: Wheel) -> CarConfig { CarConfig(wheel: v) }", self.lines())
        self.assertIn("public struct CarConfig: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public var recovery: Recovery?", self.lines())
        self.assertIn("    public init(proto: Int? = nil, ramp: Ramp? = nil, trim: Trim? = nil, "
                      "recovery: Recovery? = nil, wheel: Wheel? = nil, chassis: Chassis? = nil) { "
                      "self.proto = proto; self.ramp = ramp; self.trim = trim; self.recovery = recovery; "
                      "self.wheel = wheel; self.chassis = chassis }", self.lines())
        self.assertNotIn("static let path", self.out)

    def test_calibration_and_errors(self):
        self.assertIn("public enum CalibCorner: Equatable, Sendable, Codable {", self.out)
        self.assertIn("    case front_left", self.lines())
        self.assertIn("public enum CalibDirection: Equatable, Sendable, Codable {", self.out)
        self.assertIn("public struct CalibWheel: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public var inverted: Bool", self.lines())
        self.assertIn("public struct Calibration: Codable, Equatable, Sendable {", self.out)
        self.assertIn("    public var wheels: [CalibWheel]", self.lines())
        self.assertIn("public enum CarErrorCode: Equatable, Sendable, Codable {", self.out)
        self.assertIn("    case out_of_range", self.lines())
        self.assertIn("public struct CarAPIError: Codable, Equatable, Sendable {", self.out)
        self.assertIn("        public var code: CarErrorCode", self.lines())

    def test_default_uses_the_schema_values(self):
        self.assertIn("    static let `default` = Recovery(enabled: true, window_ms: 5000)", self.lines())
```

- [ ] **Шаг 2: Прогнать и увидеть красное**

Команда: `python3 -m unittest tools.test_gen_contract.TestSwiftEmitter 2>&1 | tail -4`
Ожидается: FAIL (`KeyError` на старых ключах схемы).

- [ ] **Шаг 3: Переписать `_swift_type`, `_swift_literal`, `emit_swift`**

```python
def _swift_literal(f):
    if f["type"] == "bool":
        return "true" if f["default"] else "false"
    if f["type"] == "fixed":
        return f"{f['default'] / f['scale']:.1f}" if (f["default"] % f["scale"]) == 0 \
            else repr(f["default"] / f["scale"])
    return str(f["default"])


def _camel(name):
    return name.split("_")[0] + "".join(w.title() for w in name.split("_")[1:])


def emit_swift(schema):
    rt, net, env, ep = schema["rt"], schema["network"], schema["envelope"], schema["endpoints"]
    cfg, cal = schema["config"], schema["calibration"]
    out = [f"// {BANNER}", "", "import Foundation", "",
           "public enum CarContract {",
           f"    public static let proto = {schema['proto']}",
           f'    public static let device = "{schema["device"]}"',
           f'    public static let ssid = "{net["ssid"]}"',
           f'    public static let password = "{net["password"]}"',
           f"    public static let rtPort: UInt16 = {rt['port']}",
           f"    public static let maxDatagram = {rt['max_datagram']}",
           f"    public static let maxCommand = {rt['max_command']}",
           f"    public static let commandHz = {rt['command_hz']}",
           f"    public static let telemetryHz = {rt['telemetry_hz']}",
           f"    public static let watchdogMs = {rt['watchdog_ms']}",
           f"    public static let sessionIdleMs = {rt['session_idle_ms']}"]
    for k, v in rt["keys"].items():
        out.append(f'    public static let {k}Field = "{v}"')
    for k in ("ok", "error"):
        out.append(f'    public static let {k}Field = "{env[k]}"')
    for k, v in ep.items():
        out.append(f'    public static let {k}Path = "{v}"')
    out += ["}", "",
            "/// The `type` word on every real-time datagram.",
            "public enum RTType {"]
    for k, v in rt["types"].items():
        out.append(f'    public static let {_camel(k)} = "{v}"')
    out += ["}", ""]
    out += swift_groups(schema)
    out += swift_document(schema["telemetry"]["swift"], schema, schema["telemetry"]["groups"],
                          schema["telemetry"]["doc"],
                          extra_fields=[{"name": rt["keys"]["seq"], "type": "int",
                                         "doc": "the car's own push counter"}])
    out += swift_document(schema["status"]["swift"], schema, schema["status"]["groups"],
                          schema["status"]["doc"])
    for d in cfg["domains"]:
        n = d["swift"]
        out.append(f"/// {d['doc']}")
        out.append(f"public struct {n}: Codable, Equatable, Sendable {{")
        for f in d["fields"]:
            out.append(f"    /// {f['doc']}")
            out.append(f"    public var {f['name']}: {swift_type(f)}")
        args = ", ".join(f"{f['name']}: {swift_type(f)}" for f in d["fields"])
        assigns = "; ".join(f"self.{f['name']} = {f['name']}" for f in d["fields"])
        out.append(f"    public init({args}) {{ {assigns} }}")
        out += ["}", ""]
        out.append(f"public extension {n} {{")
        out.append(f'    static let key = "{d["key"]}"')
        lit = ", ".join(f"{f['name']}: {_swift_literal(f)}" for f in d["fields"])
        out.append(f"    static let `default` = {n}({lit})")
        for f in d["fields"]:
            if f["type"] == "int":
                out.append(f"    static let {f['name']}Range: ClosedRange<Int> = {f['min']}...{f['max']}")
            elif f["type"] == "fixed":
                s = f["scale"]
                out.append(f"    static let {f['name']}Range: ClosedRange<Double> = "
                           f"{f['min'] / s:.1f}...{f['max'] / s:.1f}")
            elif f["type"] == "enum":
                vals = ", ".join(str(v) for v in f["values"])
                out.append(f"    static let {f['name']}Allowed: [Int] = [{vals}]")
        out.append(f"    static func pick(from c: {cfg['swift']}) -> {n}? {{ c.{d['key']} }}")
        out.append(f"    static func wrap(_ v: {n}) -> {cfg['swift']} {{ {cfg['swift']}({d['key']}: v) }}")
        out += ["}", ""]
    out.append(f"/// {cfg['doc']}")
    out.append(f"public struct {cfg['swift']}: Codable, Equatable, Sendable {{")
    out.append(f"    public var {env['proto']}: Int?")
    for d in cfg["domains"]:
        out.append(f"    public var {d['key']}: {d['swift']}?")
    args = ", ".join([f"{env['proto']}: Int? = nil"] +
                     [f"{d['key']}: {d['swift']}? = nil" for d in cfg["domains"]])
    assigns = "; ".join([f"self.{env['proto']} = {env['proto']}"] +
                        [f"self.{d['key']} = {d['key']}" for d in cfg["domains"]])
    out.append(f"    public init({args}) {{ {assigns} }}")
    out += ["}", ""]
    out += swift_state_enum("CalibCorner", cal["corners"], "A wheel's corner, by name.")
    out += swift_state_enum("CalibDirection", cal["directions"], "Which way to spin a pair.")
    k = cal["keys"]
    out += swift_struct(cal["wheel_swift"], [
        {"name": k["corner"], "type": "state", "swift": "CalibCorner", "doc": "which corner this row describes"},
        {"name": k["pair"], "type": "int", "doc": "the channel pair driving it, 0..3"},
        {"name": k["inverted"], "type": "bool", "doc": "true when the pair's A channel drives it backwards"},
    ], "One wheel of the calibration table.")
    out += swift_struct(cal["swift"], [
        {"name": env["proto"], "type": "int", "doc": "the protocol version the device speaks"},
        {"name": k["calibrated"], "type": "bool", "doc": "a valid table is loaded"},
        {"name": k["wheels"], "type": "array", "swift": f"[{cal['wheel_swift']}]",
         "doc": "the table, empty when not calibrated"},
    ], cal["doc"])
    out += swift_state_enum("CarErrorCode", schema["errors"], "The code inside an error envelope.")
    out += swift_error_envelope("CarAPIError", "CarErrorCode", schema)
    return "\n".join(out)
```

`swift_type` в `gen_common.py` должен принимать `"array"` для поля `wheels`: добавить `if f["type"] == "array": base = f["swift"]` перед веткой `state`/`object` (сделать это сейчас, в `gen_common.swift_type`).

- [ ] **Шаг 4: Прогнать класс**

Команда: `python3 -m unittest tools.test_gen_contract.TestSwiftEmitter -v 2>&1 | tail -10`
Ожидается: 5 PASS.

- [ ] **Шаг 5: Убедиться, что сгенерированный Swift компилируется сам по себе**

Команда:
```bash
python3 -c "import sys; sys.path.insert(0,'tools'); import gen_contract as g; open('/tmp/CarAPI.swift','w').write(g.emit_swift(g.load_schema()))" && swiftc -parse-as-library -emit-library -o /tmp/libCarAPI.dylib /tmp/CarAPI.swift && echo compiled
```
Ожидается: `compiled`. Если Swift ругается на имя (`default` уже экранирован обратными кавычками; `proto`/`type` — не ключевые слова), чинить эмиттер, а не вывод.

- [ ] **Шаг 6: Коммит**

```bash
git add tools/gen_common.py tools/gen_contract.py tools/test_gen_contract.py
git commit -F- <<'MSG'
gen(swift): grouped structs, state enums, CarConfig and the error envelope

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 5: Эмиттеры Python и документации, оба эмиттера адаптера

**Файлы:**
- Изменить: `tools/gen_contract.py` (`VALIDATE_SRC`, `emit_python`, `emit_doc`), `tools/gen_dongle.py` (both emitters)
- Тест: `tools/test_gen_contract.py` (`TestPythonEmitter`, `TestDocEmitter`, `TestDongleEmitters`)

**Интерфейсы:**
- Даёт (`generated.py`): `PROTO`, `DEVICE`, `NETWORK`, `RT` (с `keys`, `types`), `ENVELOPE`, `ENDPOINTS`, `ERRORS`, `STATUS_GROUPS`, `TELEMETRY_GROUPS`, `GROUPS`, `CALIBRATION`, `CONFIG_PATH`, `DOMAINS` (словарь по ключу домена: `{"nvs_key", "defaults", "fields"}`), `lround(x)`, `validate_config(body) -> (True, None) | (False, (code, field, message))`, `to_wire(key, values) -> dict`, `from_wire(key, obj) -> dict`.
- Даёт (`dongle_contract.inc`): `DONGLE_PROTO`, `DONGLE_DEVICE`, `DONGLE_HOST`, `DONGLE_PORT`, `DONGLE_RELAY_HTTP_PORT`, `DONGLE_RELAY_RT_PORT`, `DONGLE_PATH_{STATUS,WIFI,OTA}`, `DONGLE_SSID_MIN/MAX`, `DONGLE_PASS_MIN/MAX`, `DONGLE_KEY_{PROTO,OK,ERROR,ERROR_CODE,ERROR_MESSAGE,ERROR_FIELD}`, `DONGLE_KEY_GROUP_<G>`, `DONGLE_KEY_<G>_<F>[_<SUB>]`, `DONGLE_WIFI_STATE_*`, `DONGLE_USB_STATE_*`, `DONGLE_WIFI_REQ_{SSID,PASSWORD}`, `DONGLE_ERR_*`.
- Даёт (`DongleAPI.swift`): `DongleContract` (`proto`, `device`, `host`, `port`, `relayHttpPort`, `relayRtPort`, `statusPath`, `wifiPath`, `otaPath`, `ssidMin`, `ssidMax`, `passMin`, `passMax`, `ssidField`, `passwordField`), enum'ы `DongleUsbState`, `DongleWifiState`, структуры `DongleDevice`, `DongleUsb`, `DongleWifiAttempts`, `DongleWifi`, `DongleRelayError`, `DongleRelay`, `DongleSystem`, `DongleStatus(proto:device:usb:wifi:relay:system:)`, `DongleWifiReply(proto:ssid:state:)`, `DongleErrorCode`, `DongleAPIError`.

- [ ] **Шаг 1: Переписать три тестовых класса**

```python
class TestDocEmitter(unittest.TestCase):
    def test_table_has_a_row_per_field_grouped_by_domain(self):
        import gen_contract
        out = gen_contract.emit_doc(load())
        self.assertIn("| Domain | Field | Type | Range | Default | Meaning |", out)
        self.assertIn("| `wheel` | `gear_ratio` | decimal | 1..300 | 9.0 |", out)
        self.assertIn("| `ramp` | `rise_ms` | int | 0..2000 | 300 |", out)
        self.assertIn("| `recovery` | `enabled` | bool | true \\| false | true |", out)
        self.assertEqual(out.count("| `wheel` |"), 4)

    def test_splice_replaces_only_the_marked_region(self):
        import gen_contract
        doc = "before\n" + gen_contract.MARK_BEGIN + "\nold\n" + gen_contract.MARK_END + "\nafter\n"
        self.assertEqual(gen_contract.splice(doc, "new"),
                         "before\n" + gen_contract.MARK_BEGIN + "\nnew\n" + gen_contract.MARK_END + "\nafter\n")

    def test_splice_refuses_a_document_without_markers(self):
        import gen_contract
        with self.assertRaises(ValueError):
            gen_contract.splice("no markers here", "x")


class TestPythonEmitter(unittest.TestCase):
    def setUp(self):
        import gen_contract, types
        self.src = gen_contract.emit_python(load())
        self.m = types.ModuleType("generated_under_test")
        exec(self.src, self.m.__dict__)

    def test_tables(self):
        m = self.m
        self.assertEqual(m.PROTO, 2)
        self.assertEqual(m.RT["keys"]["turn"], "turn")
        self.assertEqual(m.RT["types"]["hello_ack"], "hello_ack")
        self.assertEqual(m.CONFIG_PATH, "/config")
        self.assertEqual(list(m.DOMAINS), ["ramp", "trim", "recovery", "wheel", "chassis"])
        self.assertEqual(m.DOMAINS["wheel"]["defaults"]["gear_ratio"], 900)
        self.assertEqual(m.TELEMETRY_GROUPS, ["link", "motors", "system"])
        self.assertEqual(m.GROUPS["motors"]["fields"][2]["values"][3], "remote")
        self.assertEqual(m.CALIBRATION["corners"][0], "front_left")
        self.assertIn("busy", m.ERRORS)

    def test_validate_accepts_the_defaults_on_the_wire(self):
        m = self.m
        body = {k: m.to_wire(k, d["defaults"]) for k, d in m.DOMAINS.items()}
        self.assertEqual(body["wheel"]["gear_ratio"], 9.0)
        self.assertEqual(m.validate_config(body), (True, None))
        self.assertEqual(m.validate_config({"ramp": {"rise_ms": 300}}), (True, None))

    def test_validate_rejects_and_names_the_field(self):
        m = self.m
        self.assertEqual(m.validate_config({"ramp": {"rise_ms": 2001}})[1][:2], ("out_of_range", "ramp.rise_ms"))
        self.assertEqual(m.validate_config({"ramp": {}})[1][:2], ("missing_field", "ramp.rise_ms"))
        self.assertEqual(m.validate_config({"ramp": {"rise_ms": 300, "x": 1}})[1][:2], ("unknown_field", "ramp.x"))
        self.assertEqual(m.validate_config({"nope": {}})[1][:2], ("unknown_field", "nope"))
        self.assertEqual(m.validate_config({"ramp": 5})[1][:2], ("wrong_type", "ramp"))
        self.assertEqual(m.validate_config({"ramp": {"rise_ms": True}})[1][:2], ("wrong_type", "ramp.rise_ms"))
        self.assertEqual(m.validate_config({"ramp": {"rise_ms": 25.7}})[1][:2], ("wrong_type", "ramp.rise_ms"))
        self.assertEqual(m.validate_config({"wheel": {**m.to_wire("wheel", m.DOMAINS["wheel"]["defaults"]),
                                                      "quadrature": 3}})[1][:2], ("not_allowed", "wheel.quadrature"))
        self.assertEqual(m.validate_config({})[1][:2], ("missing_field", ""))
        self.assertEqual(m.validate_config([])[1][:2], ("bad_json", ""))

    def test_fixed_rounds_half_away_from_zero(self):
        m = self.m
        w = m.to_wire("wheel", m.DOMAINS["wheel"]["defaults"])
        # 9.125 x 100 is exactly 912.5: half away from zero says 913, and Python's own
        # round() would say 912 — this is the one place the two differ.
        self.assertEqual(m.validate_config({"wheel": {**w, "gear_ratio": 9.125}}), (True, None))
        self.assertEqual(m.from_wire("wheel", {**w, "gear_ratio": 9.125})["gear_ratio"], 913)
        self.assertEqual(m.from_wire("wheel", {**w, "gear_ratio": 300.004})["gear_ratio"], 30000)
        self.assertEqual(m.validate_config({"wheel": {**w, "gear_ratio": 300.0051}})[1][:2],
                         ("out_of_range", "wheel.gear_ratio"))


class TestDongleEmitters(unittest.TestCase):
    def setUp(self):
        import gen_dongle
        self.g = gen_dongle
        with open(ROOT / "contract" / "dongle-api.json") as f:
            self.s = json.load(f)
        self.c = self.g.emit_dongle_c(self.s)
        self.sw = self.g.emit_dongle_swift(self.s)

    def assertEmitsLine(self, line, out):
        self.assertIn(line, out.splitlines(), f"no emitted line is exactly {line!r}")

    def test_c_header_is_pure_defines(self):
        for line in ("#define DONGLE_PROTO 1", '#define DONGLE_DEVICE "ajdongle"',
                     '#define DONGLE_HOST "192.168.7.1"', "#define DONGLE_PORT 8080",
                     "#define DONGLE_RELAY_HTTP_PORT 80", "#define DONGLE_RELAY_RT_PORT 4210",
                     "#define DONGLE_SSID_MAX 32", "#define DONGLE_PASS_MIN 8",
                     '#define DONGLE_PATH_WIFI "/wifi"', '#define DONGLE_KEY_PROTO "proto"',
                     '#define DONGLE_KEY_ERROR_CODE "code"', '#define DONGLE_KEY_GROUP_WIFI "wifi"',
                     '#define DONGLE_KEY_WIFI_ATTEMPTS_MAX "max"', '#define DONGLE_KEY_RELAY_LAST_ERROR "last_error"',
                     '#define DONGLE_WIFI_STATE_IDLE "idle"', '#define DONGLE_USB_STATE_UP "up"',
                     '#define DONGLE_WIFI_REQ_PASSWORD "password"', '#define DONGLE_ERR_BAD_LENGTH "bad_length"',
                     '#define DONGLE_KEY_DEVICE_ID "id"'):
            self.assertEmitsLine(line, self.c)
        for banned in ("#include", "esp_err_t", "typedef", "struct "):
            self.assertNotIn(banned, self.c)
        self.assertNotIn("DONGLE_PATH_NET", self.c)
        self.assertNotIn("DONGLE_NETKEY", self.c)

    def test_swift_exposes_the_same_vocabulary(self):
        for line in ("    public static let proto = 1", '    public static let device = "ajdongle"',
                     "    public static let port: UInt16 = 8080", "    public static let relayHttpPort: UInt16 = 80",
                     "    public static let relayRtPort: UInt16 = 4210", '    public static let wifiPath = "/wifi"',
                     "    public static let ssidMax = 32", '    public static let ssidField = "ssid"',
                     '    public static let passwordField = "password"'):
            self.assertEmitsLine(line, self.sw)
        self.assertIn("public enum DongleWifiState: Equatable, Sendable, Codable {", self.sw)
        self.assertIn("    case searching", self.sw.splitlines())
        self.assertIn("public enum DongleUsbState: Equatable, Sendable, Codable {", self.sw)
        self.assertIn("public struct DongleWifiAttempts: Codable, Equatable, Sendable {", self.sw)
        self.assertIn("    public var last_error: DongleRelayError?", self.sw.splitlines())
        self.assertIn("    public var channel: Int?", self.sw.splitlines())
        self.assertIn("public struct DongleStatus: Codable, Equatable, Sendable {", self.sw)
        self.assertIn("    public init(proto: Int, device: DongleDevice, usb: DongleUsb, wifi: DongleWifi, "
                      "relay: DongleRelay, system: DongleSystem) { self.proto = proto; self.device = device; "
                      "self.usb = usb; self.wifi = wifi; self.relay = relay; self.system = system }",
                      self.sw.splitlines())
        self.assertIn("public struct DongleWifiReply: Codable, Equatable, Sendable {", self.sw)
        self.assertIn("    public init(proto: Int, ssid: String, state: DongleWifiState) { self.proto = proto; "
                      "self.ssid = ssid; self.state = state }", self.sw.splitlines())
        self.assertIn("public enum DongleErrorCode: Equatable, Sendable, Codable {", self.sw)
        self.assertIn("public struct DongleAPIError: Codable, Equatable, Sendable {", self.sw)
        self.assertNotIn("netPath", self.sw)
        self.assertNotIn("DongleStatusKey", self.sw)

    def test_both_emitters_are_deterministic(self):
        self.assertEqual(self.c, self.g.emit_dongle_c(self.s))
        self.assertEqual(self.sw, self.g.emit_dongle_swift(self.s))
```

- [ ] **Шаг 2: Прогнать и увидеть красное**

Команда: `python3 -m unittest tools.test_gen_contract.TestDocEmitter tools.test_gen_contract.TestPythonEmitter tools.test_gen_contract.TestDongleEmitters 2>&1 | tail -5`
Ожидается: FAIL.

- [ ] **Шаг 3: Переписать `emit_doc`, `VALIDATE_SRC`, `emit_python` в `tools/gen_contract.py`**

```python
def emit_doc(schema):
    lines = [
        "| Domain | Field | Type | Range | Default | Meaning |",
        "|---|---|---|---|---|---|",
    ]
    for d in schema["config"]["domains"]:
        for f in d["fields"]:
            kind = {"int": "int", "bool": "bool", "enum": "enum", "fixed": "decimal"}[f["type"]]
            default = _swift_literal(f)
            lines.append(f"| `{d['key']}` | `{f['name']}` | {kind} | {field_range(f)} | {default} | {f['doc']} |")
    return "\n".join(lines)


VALIDATE_SRC = '''

def lround(x):
    """C's lround: half away from zero, so 9.005 x 100 is 901 here and on the car."""
    return int(math.copysign(math.floor(abs(x) + 0.5), x))


def to_wire(key, values):
    """A domain's internal integers -> the JSON the car answers: fixed fields as decimals."""
    out = {}
    for f in DOMAINS[key]["fields"]:
        v = values[f["name"]]
        out[f["name"]] = v / f["scale"] if f["type"] == "fixed" else v
    return out


def from_wire(key, obj):
    """A validated domain object -> internal integers: fixed fields x scale, rounded."""
    out = {}
    for f in DOMAINS[key]["fields"]:
        v = obj[f["name"]]
        out[f["name"]] = lround(v * f["scale"]) if f["type"] == "fixed" else v
    return out


def validate_config(body):
    """Return (True, None) or (False, (code, field, message)). Mirrors cfg_api.c exactly:
    the body is an object of domain objects, each present domain complete, unknown keys
    refused at both levels, numbers typed the way cJSON types them (a JSON boolean is not
    a number; a fraction is not an integer)."""
    if not isinstance(body, dict):
        return False, ("bad_json", "", "expected a JSON object")
    if not body:
        return False, ("missing_field", "", "no configuration domain in the body")
    for key in body:
        if key not in DOMAINS:
            return False, ("unknown_field", key, f"{key} is not a configuration domain")
    for key, domain in DOMAINS.items():
        if key not in body:
            continue
        obj = body[key]
        if not isinstance(obj, dict):
            return False, ("wrong_type", key, f"{key} must be an object")
        names = {f["name"] for f in domain["fields"]}
        for k in obj:
            if k not in names:
                return False, ("unknown_field", f"{key}.{k}", f"{key} has no field {k}")
        for f in domain["fields"]:
            name = f["name"]
            where = f"{key}.{name}"
            if name not in obj:
                return False, ("missing_field", where, f"{where} is required")
            v = obj[name]
            if f["type"] == "bool":
                if not isinstance(v, bool):
                    return False, ("wrong_type", where, f"{where} must be a boolean")
                continue
            # bool is a subclass of int in Python, so True would sneak past a plain
            # isinstance check. cJSON_IsNumber does not accept a JSON boolean either.
            if isinstance(v, bool) or not isinstance(v, (int, float)):
                return False, ("wrong_type", where, f"{where} must be a number")
            if f["type"] == "fixed":
                scaled = lround(v * f["scale"])
                if not (f["min"] <= scaled <= f["max"]):
                    lo, hi = f["min"] / f["scale"], f["max"] / f["scale"]
                    return False, ("out_of_range", where, f"{where} must be {lo:g}..{hi:g}")
                continue
            if v != int(v):
                return False, ("wrong_type", where, f"{where} must be an integer")
            v = int(v)
            if f["type"] == "enum":
                if v not in f["values"]:
                    return False, ("not_allowed", where, f"{where} must be one of {f['values']}")
            elif not (f["min"] <= v <= f["max"]):
                return False, ("out_of_range", where, f"{where} must be {f['min']}..{f['max']}")
    return True, None
'''


def emit_python(schema):
    cfg = schema["config"]
    body = {
        d["key"]: {
            "nvs_key": d["nvs_key"],
            "defaults": {f["name"]: f["default"] for f in d["fields"]},
            "fields": [{**f, "scale": f.get("scale", 1)} for f in d["fields"]],
        }
        for d in cfg["domains"]
    }
    return "\n".join([
        f"# {BANNER}",
        "import math",
        "",
        *py_common(schema),
        f"NETWORK = {schema['network']!r}",
        f"RT = {schema['rt']!r}",
        f"TELEMETRY_GROUPS = {schema['telemetry']['groups']!r}",
        f"CALIBRATION = {schema['calibration']!r}",
        f"CONFIG_PATH = {cfg['path']!r}",
        f"DOMAINS = {pprint.pformat(body, indent=4, sort_dicts=False, width=96)}",
        VALIDATE_SRC.rstrip("\n"),
        "",
    ])
```

- [ ] **Шаг 4: Переписать `tools/gen_dongle.py`**

```python
#!/usr/bin/env python3
"""Emitters for contract/dongle-api.json.

Separate from gen_contract.py so the car's file does not grow a second device's shapes.
The two schemas share gen_common's plumbing and nothing else — neither references the
other, and the dongle's rules (lengths, the character class, escaping) live in
firmware/dongle/main/net_cfg.{c,h} where they are host-tested rather than here.
"""
from gen_common import (c_group_defines, c_envelope_defines, c_error_defines,
                        c_endpoint_defines, swift_groups, swift_document, swift_state_enum,
                        swift_struct, swift_error_envelope)

BANNER = "generated from contract/dongle-api.json by tools/gen_contract.py - do not edit"


def emit_dongle_c(schema):
    """A pure C header: preprocessor text only. net_cfg.h includes this and compiles on
    the host with plain cc under -Wall -Wextra -Werror."""
    n, b = schema["network"], schema["bounds"]
    lines = [
        f"/* {BANNER} */",
        "",
        "#ifndef DONGLE_CONTRACT_INC",
        "#define DONGLE_CONTRACT_INC",
        "",
        f"#define DONGLE_PROTO {schema['proto']}",
        f'#define DONGLE_DEVICE "{schema["device"]}"',
        f'#define DONGLE_HOST "{n["host"]}"',
        f"#define DONGLE_PORT {n['port']}",
        f"#define DONGLE_RELAY_HTTP_PORT {schema['relay']['http_port']}",
        f"#define DONGLE_RELAY_RT_PORT {schema['relay']['rt_port']}",
        "",
        f"#define DONGLE_SSID_MIN {b['ssid_min']}",
        f"#define DONGLE_SSID_MAX {b['ssid_max']}",
        f"#define DONGLE_PASS_MIN {b['pass_min']}",
        f"#define DONGLE_PASS_MAX {b['pass_max']}",
        "",
    ]
    lines += c_endpoint_defines(schema, "DONGLE_")
    lines += c_envelope_defines(schema, "DONGLE_")
    lines += c_group_defines(schema, "DONGLE_")
    for k, v in schema["wifi_request"].items():
        lines.append(f'#define DONGLE_WIFI_REQ_{k.upper()} "{v}"')
    lines.append("")
    lines += c_error_defines(schema, "DONGLE_")
    lines += ["#endif /* DONGLE_CONTRACT_INC */", ""]
    return "\n".join(lines)


def emit_dongle_swift(schema):
    n, b, e = schema["network"], schema["bounds"], schema["endpoints"]
    lines = [
        f"// {BANNER}",
        "",
        "public enum DongleContract {",
        f"    public static let proto = {schema['proto']}",
        f'    public static let device = "{schema["device"]}"',
        f'    public static let host = "{n["host"]}"',
        f"    public static let port: UInt16 = {n['port']}",
        f"    public static let relayHttpPort: UInt16 = {schema['relay']['http_port']}",
        f"    public static let relayRtPort: UInt16 = {schema['relay']['rt_port']}",
        "",
    ]
    for k, v in e.items():
        lines.append(f'    public static let {k}Path = "{v}"')
    lines += [
        "",
        f"    public static let ssidMin = {b['ssid_min']}",
        f"    public static let ssidMax = {b['ssid_max']}",
        f"    public static let passMin = {b['pass_min']}",
        f"    public static let passMax = {b['pass_max']}",
        "",
    ]
    for k, v in schema["wifi_request"].items():
        lines.append(f'    public static let {k}Field = "{v}"')
    lines += ["}", ""]
    lines += swift_groups(schema)
    lines += swift_document(schema["status"]["swift"], schema, schema["status"]["groups"],
                            schema["status"]["doc"])
    wifi_fields = {f["name"]: f for f in schema["groups"]["wifi"]["fields"]}
    reply = schema["wifi_reply"]
    lines += swift_struct(reply["swift"],
                          [{"name": schema["envelope"]["proto"], "type": "int",
                            "doc": "the protocol version the device speaks"}] +
                          [wifi_fields[name] for name in reply["fields"]],
                          reply["doc"])
    lines += swift_state_enum("DongleErrorCode", schema["errors"],
                              "The code inside an error envelope.")
    lines += swift_error_envelope("DongleAPIError", "DongleErrorCode", schema)
    return "\n".join(lines)
```

- [ ] **Шаг 5: Прогнать три класса, затем весь файл тестов генератора**

Команда: `python3 tools/test_gen_contract.py 2>&1 | tail -6`
Ожидается: все классы зелёные, КРОМЕ `TestDriftCheck.test_check_script_passes_on_a_clean_tree` и тестов вроде `TestDeterminism`, которые сверяют генератор с закоммиченными артефактами (они позеленеют после перегенерации в Задаче 6). `TestArtifactListing` по-прежнему перечисляет те же пять артефактов и проходит без изменений.

Ещё скомпилировать сгенерированный Swift адаптера вместе с машинкиным — доказать, что имена не сталкиваются:
```bash
python3 -c "import sys; sys.path.insert(0,'tools'); import gen_contract as g, gen_dongle as d, json; open('/tmp/CarAPI.swift','w').write(g.emit_swift(g.load_schema())); open('/tmp/DongleAPI.swift','w').write(d.emit_dongle_swift(json.load(open('contract/dongle-api.json'))))" && swiftc -parse-as-library -emit-library -o /tmp/libAPI.dylib /tmp/CarAPI.swift /tmp/DongleAPI.swift && echo compiled
```
Ожидается: `compiled`.

- [ ] **Шаг 6: Коммит**

```bash
git add tools/gen_contract.py tools/gen_dongle.py tools/gen_common.py tools/test_gen_contract.py
git commit -F- <<'MSG'
gen: python validator for nested /config, grouped doc table, dongle emitters for v2

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 6: Перегенерировать все артефакты и закоммитить

**Файлы:**
- Изменить (сгенерированные): `firmware/car/core/main/cfg_table.inc`, `app/AJMiddleCar/Generated/CarAPI.swift`, `tools/mock_car/generated.py`, `firmware/dongle/main/dongle_contract.inc`, `app/AJMiddleCar/Generated/DongleAPI.swift`, размеченная область `docs/protocol.md`

- [ ] **Шаг 1: Перегенерировать**

Команда: `python3 tools/gen_contract.py && bash tools/check_contract.sh`
Ожидается: `contract: no drift`.

- [ ] **Шаг 2: Прогнать весь файл тестов генератора**

Команда: `python3 tools/test_gen_contract.py 2>&1 | tail -4`
Ожидается: `OK`.

- [ ] **Шаг 3: Убедиться, что «красное окно» ровно такое, как описано**

Команда: `make -C firmware/car/core/test run 2>&1 | tail -3; make -C firmware/dongle/test run 2>&1 | tail -3`
Ожидается: оба НЕ компилируются (`CTL_NONE`, `RT_KEY_HELLO`, `DONGLE_STATE_IDLE`, `.path` … не объявлены). Это ожидаемое состояние до фаз B и C.

- [ ] **Шаг 4: Закоммитить артефакты**

```bash
git add firmware/car/core/main/cfg_table.inc app/AJMiddleCar/Generated/CarAPI.swift tools/mock_car/generated.py firmware/dongle/main/dongle_contract.inc app/AJMiddleCar/Generated/DongleAPI.swift docs/protocol.md
git commit -F- <<'MSG'
gen: regenerate every artifact for wire format v2

The firmware, app and mock host tests are red from this commit until their
phases land — the generated symbols they compile against have changed names.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```
## Фаза B — прошивка машинки

Каждая задача здесь гоняет `make -C firmware/car/core/test run` (хост, обычный `cc`). Фаза начинается с несобирающейся целью; Задача 13 заканчивается зелёным набором и зелёным `idf.py build`.

### Задача 7: Типизированные датаграммы в `control_proto`

**Файлы:**
- Изменить: `firmware/car/core/main/control_proto.h`, `firmware/car/core/main/control_proto.c`
- Тест: `firmware/car/core/test/test_control_proto.c` (whole file)

**Интерфейсы:**
- Даёт: `typedef enum { CT_NONE = 0, CT_HELLO, CT_DRIVE, CT_BYE } control_type_t;` и `control_frame_t { control_type_t type; bool has_proto; uint32_t proto; bool has_seq; uint32_t seq; bool has_axes; float throttle, turn; char sid[CONTROL_SID_MAX]; }`. `control_parse_frame(msg, len, max_len, out)` возвращает 0 только для датаграммы, чей `type` — `hello` (с `session`), `drive` (с `seq`, `throttle`, `turn`) или `bye` (с `seq`); `proto` разбирается, если есть, и здесь не судится. `control_seq_newer` без изменений.

- [ ] **Шаг 1: Заменить `firmware/car/core/test/test_control_proto.c`**

```c
#include "control_proto.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
/* The cap and the key words are the schema's, not this test's — a frame that is legal
   here and rejected on the car (or the reverse) is exactly the class of bug the
   generator exists to remove. The datagrams below are written out as wire bytes on
   purpose: a test that built them from RT_KEY_* would agree with a typo. */
#include "cfg_table.inc"

static int approx(float a, float b) { return fabsf(a - b) < 1e-4f; }

static control_frame_t parse(const char *msg) {
    control_frame_t f;
    int r = control_parse_frame(msg, strlen(msg), RT_MAX_COMMAND, &f);
    if (r != 0) {
        printf("FAIL parse('%s') -> %d, want 0\n", msg, r);
        assert(0);
    }
    return f;
}

static void drive(const char *msg, float et, float ey) {
    control_frame_t f = parse(msg);
    if (f.type != CT_DRIVE || !f.has_axes || !approx(f.throttle, et) || !approx(f.turn, ey)) {
        printf("FAIL drive('%s') -> type=%d has_axes=%d throttle=%.4f turn=%.4f (want %.4f %.4f)\n",
               msg, f.type, f.has_axes, f.throttle, f.turn, et, ey);
        assert(0);
    }
}

static void bad(const char *msg) {
    control_frame_t f;
    size_t len = msg ? strlen(msg) : 0;
    int r = control_parse_frame(msg, len, RT_MAX_COMMAND, &f);
    if (r != -1) {
        printf("FAIL bad('%s') -> r=%d, want -1\n", msg ? msg : "(null)", r);
        assert(0);
    }
}

static void seq_newer(uint32_t seq, uint32_t last, bool want) {
    if (control_seq_newer(seq, last) != want) {
        printf("FAIL seq_newer(%u,%u) != %d\n", seq, last, want);
        assert(0);
    }
}

int main(void) {
    /* --- drive, as the app sends it ------------------------------------------ */
    drive("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0.5,\"turn\":0}", 0.5f, 0.0f);
    drive("{\"proto\":2,\"type\":\"drive\",\"seq\":2,\"throttle\":0,\"turn\":1}", 0.0f, 1.0f);
    drive("{\"proto\":2,\"type\":\"drive\",\"seq\":3,\"throttle\":-1,\"turn\":-0.5}", -1.0f, -0.5f);
    drive("{\"turn\":-1.0,\"throttle\":1.0,\"seq\":4,\"type\":\"drive\",\"proto\":2}", 1.0f, -1.0f);   /* key order */
    drive("{ \"proto\" : 2 , \"type\" : \"drive\" , \"seq\" : 5 , \"throttle\" : 0.25 , \"turn\" : 0.75 }",
          0.25f, 0.75f);                                                                       /* whitespace */

    control_frame_t f = parse("{\"proto\":2,\"type\":\"drive\",\"seq\":1234,\"throttle\":0.50,\"turn\":-0.25}");
    assert(f.type == CT_DRIVE && f.has_seq && f.seq == 1234 && f.has_proto && f.proto == 2);
    assert(f.sid[0] == '\0');

    /* proto is carried, not judged: the classifier owns that rule. A drive without it
       parses; a drive with a foreign one parses too. */
    f = parse("{\"type\":\"drive\",\"seq\":6,\"throttle\":0,\"turn\":0}");
    assert(f.type == CT_DRIVE && !f.has_proto);
    f = parse("{\"proto\":7,\"type\":\"drive\",\"seq\":6,\"throttle\":0,\"turn\":0}");
    assert(f.has_proto && f.proto == 7);

    /* The longest legal drive fits the command cap: ten-digit seq, three-decimal axes
       with signs. 96 is the schema's cap; this is what it was sized for. */
    const char *widest = "{\"proto\":2,\"type\":\"drive\",\"seq\":4294967295,\"throttle\":-1.000,\"turn\":-1.000}";
    assert(strlen(widest) <= RT_MAX_COMMAND);
    drive(widest, -1.0f, -1.0f);

    /* --- hello ---------------------------------------------------------------- */
    f = parse("{\"proto\":2,\"type\":\"hello\",\"session\":\"7f3a91c2\"}");
    assert(f.type == CT_HELLO && strcmp(f.sid, "7f3a91c2") == 0 && f.has_proto && f.proto == 2);
    assert(!f.has_seq && !f.has_axes);
    f = parse("{\"type\":\"hello\",\"session\":\"a\"}");                     /* 1 char, no proto */
    assert(f.type == CT_HELLO && !f.has_proto && strcmp(f.sid, "a") == 0);
    f = parse("{\"proto\":2,\"type\":\"hello\",\"session\":\"abcdefghijklmno\"}");  /* 15 chars */
    assert(strcmp(f.sid, "abcdefghijklmno") == 0);
    bad("{\"proto\":2,\"type\":\"hello\",\"session\":\"abcdefghijklmnop\"}");       /* 16: refused, not cut */
    bad("{\"proto\":2,\"type\":\"hello\",\"session\":\"\"}");
    bad("{\"proto\":2,\"type\":\"hello\",\"session\":\"7f3a-91c2\"}");              /* not alphanumeric */
    bad("{\"proto\":2,\"type\":\"hello\",\"session\":12345678}");                   /* not a string */
    bad("{\"proto\":2,\"type\":\"hello\"}");                                        /* no session */
    bad("{\"proto\":2,\"type\":\"hello\",\"hello\":\"7f3a91c2\"}");                 /* the v1 key */

    /* --- bye ------------------------------------------------------------------ */
    f = parse("{\"proto\":2,\"type\":\"bye\",\"seq\":1235}");
    assert(f.type == CT_BYE && f.has_seq && f.seq == 1235 && !f.has_axes);
    /* Axes on a goodbye are tolerated (the v1 stop wrote zeros) but not required. */
    f = parse("{\"proto\":2,\"type\":\"bye\",\"seq\":1236,\"throttle\":0,\"turn\":0}");
    assert(f.type == CT_BYE && f.has_axes);
    /* Every app->car datagram except a hello carries seq — a goodbye included. One
       without it would bypass replay protection, so it is dropped rather than half-
       honoured. */
    bad("{\"proto\":2,\"type\":\"bye\"}");

    /* --- type is the discriminator, and it is required ------------------------ */
    bad("{\"proto\":2,\"seq\":1,\"throttle\":0,\"turn\":0}");                       /* no type */
    bad("{\"proto\":2,\"type\":\"drove\",\"seq\":1,\"throttle\":0,\"turn\":0}");    /* unknown */
    bad("{\"proto\":2,\"type\":\"telemetry\",\"seq\":1}");                          /* car->app only */
    bad("{\"proto\":2,\"type\":\"hello_ack\",\"session\":\"7f3a91c2\"}");           /* car->app only */
    bad("{\"proto\":2,\"type\":drive,\"seq\":1,\"throttle\":0,\"turn\":0}");        /* not a string */
    bad("{\"proto\":2,\"type\":\"drive\",\"type\":\"bye\",\"seq\":1}");             /* twice */

    /* --- a drive needs both axes and a seq ------------------------------------ */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0.5}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"turn\":0.5}");
    bad("{\"proto\":2,\"type\":\"drive\",\"throttle\":0.5,\"turn\":0}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":\"x\",\"turn\":0}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":nan,\"turn\":0}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":1e400,\"turn\":0}");  /* not finite */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":-1,\"throttle\":0,\"turn\":0}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1.5,\"throttle\":0,\"turn\":0}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":4294967296,\"throttle\":0,\"turn\":0}");
    bad("{\"proto\":1.5,\"type\":\"drive\",\"seq\":1,\"throttle\":0,\"turn\":0}");    /* proto not an int */
    /* The v1 spellings are no longer keys the car knows. */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"t\":0.5,\"y\":0}");

    /* --- duplicated keys are two instructions in one datagram ------------------ */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"seq\":2,\"throttle\":0,\"turn\":0}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0,\"throttle\":1,\"turn\":0}");

    /* --- not JSON, not an object, or over the cap ----------------------------- */
    bad("");
    bad(NULL);
    bad("drive");
    bad("[{\"type\":\"drive\"}]");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0,\"turn\":0");     /* unterminated */
    /* Only a real key position counts: the name inside a string value must not match. */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"note\":\"throttle\":0,\"turn\":0}");
    /* The audit's shared pinned frames: the mock pins these same bytes with these same
       outcomes, because byte-identical datagrams once drove the car and the mock apart. */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":7,\"throttle\":.5,\"turn\":0}");      /* bare mantissa */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":8,\"throttle\":+1,\"turn\":0}");      /* leading plus */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":01,\"throttle\":0,\"turn\":0}");      /* leading zero */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":12,\"throttle\":0.5x,\"turn\":0}");   /* trailing junk */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\"0.5,\"turn\":0}");      /* missing colon */
    /* Nothing is read past `len`, so a buffer that is not NUL-terminated is safe. */
    {
        const char first[] = "{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0.5,\"turn\":0}";
        const char raw[]   = "{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0.5,\"turn\":0}"
                             "{\"proto\":2,\"type\":\"drive\",\"seq\":2,\"throttle\":1,\"turn\":1}";
        control_frame_t part;
        assert(control_parse_frame(raw, sizeof(first) - 1, RT_MAX_COMMAND, &part) == 0);
        assert(part.has_axes && approx(part.throttle, 0.5f) && approx(part.turn, 0.0f) && part.seq == 1);
    }
    {
        char big[RT_MAX_COMMAND + 64];
        int n = snprintf(big, sizeof(big),
                         "{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0,\"turn\":0,\"pad\":\"");
        while (n < RT_MAX_COMMAND + 1) big[n++] = 'x';
        big[n++] = '"'; big[n++] = '}'; big[n] = '\0';
        bad(big);
    }
    /* Keys the car does not look for are ignored — but nested objects cannot smuggle one
       in: "seq" inside a sub-object is not the datagram's seq. */
    drive("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0.1,\"turn\":0.2,\"extra\":{\"seq\":9}}",
          0.1f, 0.2f);
    f = parse("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0.1,\"turn\":0.2,\"extra\":{\"seq\":9}}");
    assert(f.seq == 1);

    /* --- the sequence gate ---------------------------------------------------- */
    seq_newer(2, 1, true);
    seq_newer(1, 1, false);
    seq_newer(1, 2, false);
    seq_newer(0, 0xFFFFFFFFu, true);           /* wrap */
    seq_newer(0xFFFFFFFFu, 0, false);
    seq_newer(0x80000000u, 0, false);          /* half a ring away is "older" */
    seq_newer(0x7FFFFFFFu, 0, true);

    printf("test_control_proto: all passed\n");
    return 0;
}
```

- [ ] **Шаг 2: Собрать только этот тест и увидеть красное**

Команда: `make -C firmware/car/core/test test_control_proto 2>&1 | tail -5`
Ожидается: ошибка компиляции — `CT_DRIVE` не объявлен, нет члена `has_axes`.

- [ ] **Шаг 3: Переписать `control_proto.h`**

```c
#ifndef CONTROL_PROTO_H
#define CONTROL_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Room for the session id, NUL included. The wire uses eight hex characters; this is
// wider so a longer id is a rejected frame rather than a truncated one — two sessions
// whose ids differ only past the cut would otherwise look like the same session.
#define CONTROL_SID_MAX 16

// What a datagram says it is — its `type` word. The three the app sends; the two the car
// sends (hello_ack, telemetry) are refused here, since a car does not take its own words
// back as instructions.
typedef enum { CT_NONE = 0, CT_HELLO, CT_DRIVE, CT_BYE } control_type_t;

// One decoded real-time datagram. `type` is what the datagram is; the flags say which
// optional keys were there. A caller that needs one and finds it absent must drop the
// frame rather than read the zero left behind.
typedef struct {
    control_type_t type;
    bool     has_proto;
    uint32_t proto;
    bool     has_seq;
    uint32_t seq;
    bool     has_axes;              // both axes were present and finite
    float    throttle, turn;
    char     sid[CONTROL_SID_MAX];  // CT_HELLO: NUL-terminated, alphanumeric, non-empty
} control_frame_t;

// Parse one datagram of `len` bytes into `out`. Zero-alloc and bounded: nothing is read
// past `len`, so the buffer need not be NUL-terminated, and a datagram longer than
// `max_len` is rejected untouched (the caller passes RT_MAX_COMMAND, the largest
// datagram the car accepts — the cap belongs to the transport, so it arrives as an
// argument rather than being compiled in here).
//
// Returns 0 when the datagram is a known type carrying what that type needs — a hello
// with a session, a drive with seq and both axes, a bye with seq — and every key present
// parsed cleanly. Returns -1: oversized, unparseable, no type or a type the app does not
// send, a missing required key, one axis without the other, a non-finite axis, a session
// id that is empty, over-long or not alphanumeric, or any key that appears twice. `*out`
// is undefined on -1.
//
// `proto` is parsed when present and never judged: whether the car speaks it is policy,
// and the classifier in rt_link.h owns it (a foreign hello is still answered).
// Range is deliberately not checked: car_drive clamps, and a parser that also enforced
// policy would have two reasons to change.
int control_parse_frame(const char *msg, size_t len, size_t max_len, control_frame_t *out);

// Pure: is `seq` newer than `last`? Signed difference of unsigned counters, so the
// uint32 wraps correctly — the alternative, `seq > last`, drops every frame for the
// rest of the session the first time the counter passes 2^32.
static inline bool control_seq_newer(uint32_t seq, uint32_t last) {
    return (int32_t)(seq - last) > 0;
}

#endif // CONTROL_PROTO_H
```

- [ ] **Шаг 4: Переписать хвост `control_proto.c`**

Всё до `parse_sid` включительно оставить как есть. После `parse_sid` добавить:

```c
/* The `type` word: a JSON string whose content is exactly one of the app->car words.
   Compared as bytes, not as a token, so "drive " or "Drive" are not drive. */
static int parse_type(const char *p, size_t n, control_type_t *out) {
    if (n < 2 || p[0] != '"') return -1;
    const char *end = memchr(p + 1, '"', n - 1);
    if (!end) return -1;
    size_t k = (size_t)(end - (p + 1));
    if (!token_ends(p, n, k + 2)) return -1;
    if (k == strlen(RT_TYPE_HELLO) && memcmp(p + 1, RT_TYPE_HELLO, k) == 0) { *out = CT_HELLO; return 0; }
    if (k == strlen(RT_TYPE_DRIVE) && memcmp(p + 1, RT_TYPE_DRIVE, k) == 0) { *out = CT_DRIVE; return 0; }
    if (k == strlen(RT_TYPE_BYE)   && memcmp(p + 1, RT_TYPE_BYE, k) == 0)   { *out = CT_BYE;   return 0; }
    return -1;
}
```

Затем заменить всё тело `control_parse_frame` на:

```c
int control_parse_frame(const char *msg, size_t len, size_t max_len, control_frame_t *out) {
    if (msg == NULL || out == NULL || len == 0 || len > max_len) return -1;
    /* A datagram is one object. Anything else — an array, a bare word — is refused
       before a key is looked for, since value_of would find nothing at depth 1 and the
       requirement checks below would then be the only thing standing. */
    size_t i = 0;
    while (i < len && is_ws(msg[i])) i++;
    if (i >= len || msg[i] != '{') return -1;
    size_t j = len;
    while (j > i && is_ws(msg[j - 1])) j--;
    if (j == i || msg[j - 1] != '}') return -1;

    control_frame_t f = {0};
    const char *v = NULL;
    size_t left = 0;

    int r = value_of(msg, len, RT_KEY_TYPE, &v, &left);
    if (r != 0) return -1;                          /* absent or duplicated: no type, no frame */
    if (parse_type(v, left, &f.type) != 0) return -1;

    r = value_of(msg, len, RT_KEY_PROTO, &v, &left);
    if (r < 0) return -1;
    if (r == 0) {
        if (parse_u32(v, left, &f.proto) != 0) return -1;
        f.has_proto = true;
    }
    r = value_of(msg, len, RT_KEY_SEQ, &v, &left);
    if (r < 0) return -1;
    if (r == 0) {
        if (parse_u32(v, left, &f.seq) != 0) return -1;
        f.has_seq = true;
    }
    r = value_of(msg, len, RT_KEY_SESSION, &v, &left);
    if (r < 0) return -1;
    bool has_session = false;
    if (r == 0) {
        if (parse_sid(v, left, f.sid, sizeof(f.sid)) != 0) return -1;
        has_session = true;
    }

    const char *vt = NULL, *vy = NULL;
    size_t left_t = 0, left_y = 0;
    r = value_of(msg, len, RT_KEY_THROTTLE, &vt, &left_t);
    if (r < 0) return -1;
    int ry = value_of(msg, len, RT_KEY_TURN, &vy, &left_y);
    if (ry < 0) return -1;
    if (vt != NULL || vy != NULL) {
        /* One axis without the other is a truncated or corrupt frame, not a command to
           hold the missing axis at zero. */
        if (vt == NULL || vy == NULL) return -1;
        if (parse_num(vt, left_t, &f.throttle) != 0) return -1;
        if (parse_num(vy, left_y, &f.turn) != 0) return -1;
        f.has_axes = true;
    }

    /* What each type needs. Every app->car datagram except a hello carries seq — a
       goodbye included — because one without it would bypass replay protection, so the
       whole frame is dropped rather than half-honoured. The rule lives here as well as in
       rt_link so that the two halves cannot disagree about which datagrams the car acts
       on: they did once, and a goodbye the parser accepted and the transport dropped
       looked like a working feature. */
    switch (f.type) {
        case CT_HELLO: if (!has_session) return -1; break;
        case CT_DRIVE: if (!f.has_seq || !f.has_axes) return -1; break;
        case CT_BYE:   if (!f.has_seq) return -1; break;
        default:       return -1;
    }
    *out = f;
    return 0;
}
```

Проверить существующие хелперы: `token_ends(p, n, k)` возвращает true, когда за токеном длины `k` по адресу `p` идёт пробел, `,`, `}` или конец `n` — на это уже опираются разборщики чисел; `parse_num` отвергает `nan`, `inf` и неконечные результаты (уже делает для `t`/`y` v1 — оставить). Датаграмму не-объект (`[…]`) теперь отсекает проверка ведущей скобки выше, что бы ни делал `value_of`.

- [ ] **Шаг 5: Собрать и прогнать тест**

Команда: `make -C firmware/car/core/test test_control_proto && ./firmware/car/core/test/test_control_proto`
Ожидается: `test_control_proto: all passed`. (Остальные цели в Makefile ещё не собираются — так и должно быть до Задачи 13.)

- [ ] **Шаг 6: Коммит**

```bash
git add firmware/car/core/main/control_proto.h firmware/car/core/main/control_proto.c firmware/car/core/test/test_control_proto.c
git commit -F- <<'MSG'
car(control_proto): datagrams are typed — hello, drive, bye — and say so

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 8: Классификатор сессии переключается по типу и судит `proto` на каждой датаграмме

**Файлы:**
- Изменить: `firmware/car/core/main/rt_link.h` (`rt_session_classify`), `firmware/car/core/main/rt_link.c` (`on_command`, the `case RT_REPLY` log line)
- Тест: `firmware/car/core/test/test_rt_session.c` (whole file)

**Интерфейсы:**
- Берёт: `control_frame_t.type/has_axes/throttle/turn` из Задачи 7.
- Даёт: `rt_session_classify` возвращает `RT_REPLY` для любого `CT_HELLO` с отсутствующим или чужим `proto`; `RT_DROP` для любой не-hello датаграммы с отсутствующим или чужим `proto`; иначе — правила v1, по типу.

- [ ] **Шаг 1: Заменить `firmware/car/core/test/test_rt_session.c`**

```c
/* The session lifecycle: which datagram is acted on, and what a hello, a command, a
 * goodbye and a watchdog trip do to the channel's state.
 *
 * The datagrams are written out as wire bytes on purpose: a test that built frames from
 * the same symbols the parser uses would agree with a typo. The key names are generated
 * (RT_KEY_*) and the parser is the one under test, so these strings are the golden copy.
 */
#define RT_LINK_HOST_TEST
#include "../main/rt_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* proto is 2 because the wire says 2; if the contract ever bumps it, this file is one of
   the places that has to be read. */
#define HELLO_A   "{\"proto\":2,\"type\":\"hello\",\"session\":\"7f3a91c2\"}"
#define HELLO_A2  HELLO_A                        /* the app's repeat, verbatim */
#define HELLO_B   "{\"proto\":2,\"type\":\"hello\",\"session\":\"0b17ac55\"}"
#define SID_A     "7f3a91c2"
#define DRIVE(seq, t, y) \
    "{\"proto\":2,\"type\":\"drive\",\"seq\":" #seq ",\"throttle\":" #t ",\"turn\":" #y "}"
#define BYE(seq) "{\"proto\":2,\"type\":\"bye\",\"seq\":" #seq "}"

static control_frame_t frame(const char *msg) {
    control_frame_t f;
    if (control_parse_frame(msg, strlen(msg), RT_MAX_COMMAND, &f) != 0) {
        printf("FAIL the parser refused '%s'\n", msg);
        assert(0);
    }
    return f;
}

/* What the car does with `msg`, arriving from the session's address or from a stranger.
   `dead` is the dead-sid ring in play, or NULL when a test has no rule 3 to apply. */
static rt_action_t act(const rt_session_t *s, const rt_dead_sids_t *dead,
                       bool from_owner, const char *msg) {
    control_frame_t f = frame(msg);
    return rt_session_classify(s, dead, from_owner, &f);
}

static void refused(const char *msg) {
    control_frame_t f;
    if (control_parse_frame(msg, strlen(msg), RT_MAX_COMMAND, &f) == 0) {
        printf("FAIL the parser accepted '%s'\n", msg);
        assert(0);
    }
}

int main(void) {
    rt_session_t s = {0};

    /* --- before anyone has said hello ---------------------------------------- */
    assert(!rt_session_lost(&s, 0));
    assert(!rt_session_lost(&s, 1000000));
    assert(act(&s, NULL, false, DRIVE(1, 1, 0)) == RT_DROP);

    /* --- adoption ------------------------------------------------------------ */
    assert(act(&s, NULL, false, HELLO_A) == RT_ADOPT);
    rt_session_adopt(&s, SID_A, 500);
    assert(s.last_feed_ms == 500);
    assert(s.have_owner && strcmp(s.sid, SID_A) == 0);
    assert(!s.have_seq);
    assert(!s.armed);
    assert(!rt_session_lost(&s, RT_WATCHDOG_MS + 1));
    assert(!rt_session_lost(&s, RT_WATCHDOG_MS * 100));

    assert(act(&s, NULL, true, HELLO_A2) == RT_REPLY);
    assert(act(&s, NULL, true, HELLO_B) == RT_ADOPT);
    assert(act(&s, NULL, false, HELLO_A2) == RT_ADOPT);

    /* A protocol we cannot speak is answered by name and never adopted — the reply is
       how a flashed-but-not-updated pair finds out, instead of searching forever. */
    assert(act(&s, NULL, false, "{\"proto\":3,\"type\":\"hello\",\"session\":\"deadbeef\"}") == RT_REPLY);
    assert(act(&s, NULL, true,  "{\"proto\":1,\"type\":\"hello\",\"session\":\"deadbeef\"}") == RT_REPLY);
    assert(act(&s, NULL, false, "{\"type\":\"hello\",\"session\":\"deadbeef\"}") == RT_REPLY);

    /* --- commands ------------------------------------------------------------ */
    assert(act(&s, NULL, true, DRIVE(10, 0.5, 0)) == RT_COMMAND);
    assert(act(&s, NULL, false, DRIVE(10, 0.5, 0)) == RT_DROP);        /* not our driver */
    /* proto on every datagram, not only the hello: a drive in a dialect we do not speak
       is dropped, not driven on. Absent counts as foreign — v2 requires it. */
    assert(act(&s, NULL, true, "{\"proto\":1,\"type\":\"drive\",\"seq\":10,\"throttle\":0.5,\"turn\":0}") == RT_DROP);
    assert(act(&s, NULL, true, "{\"type\":\"drive\",\"seq\":10,\"throttle\":0.5,\"turn\":0}") == RT_DROP);
    rt_session_command(&s, 10, 1000);
    assert(s.armed && s.have_seq && s.last_seq == 10 && s.last_feed_ms == 1000);
    assert(!rt_session_lost(&s, 1000 + RT_WATCHDOG_MS));
    assert(rt_session_lost(&s, 1000 + RT_WATCHDOG_MS + 1));

    assert(act(&s, NULL, true, DRIVE(10, 1, 0)) == RT_DROP);
    assert(act(&s, NULL, true, DRIVE(9, 1, 0)) == RT_DROP);
    assert(act(&s, NULL, true, DRIVE(11, 1, 0)) == RT_COMMAND);

    rt_session_command(&s, 0xFFFFFFFFu, 2000);
    assert(act(&s, NULL, true, DRIVE(0, 0, 0)) == RT_COMMAND);
    assert(act(&s, NULL, true, DRIVE(4294967295, 0, 0)) == RT_DROP);

    /* --- the datagrams the car will not act on -------------------------------- */
    refused("{\"proto\":2,\"type\":\"drive\",\"throttle\":0,\"turn\":0}");
    refused("{\"proto\":2,\"type\":\"bye\"}");
    control_frame_t bare = { .type = CT_BYE, .has_proto = true, .proto = RT_PROTO };  /* has_seq false, by hand */
    assert(rt_session_classify(&s, NULL, true, &bare) == RT_DROP);
    control_frame_t none = { .type = CT_NONE, .has_proto = true, .proto = RT_PROTO, .has_seq = true, .seq = 12 };
    assert(rt_session_classify(&s, NULL, true, &none) == RT_DROP);

    /* --- goodbye -------------------------------------------------------------- */
    rt_session_command(&s, 100, 3000);
    assert(act(&s, NULL, true, BYE(101)) == RT_BYE);
    assert(act(&s, NULL, true, "{\"proto\":1,\"type\":\"bye\",\"seq\":102}") == RT_DROP);  /* foreign proto */
    rt_session_bye(&s);
    assert(!s.armed && !s.have_owner && !s.have_seq);
    assert(!rt_session_lost(&s, 3000 + RT_WATCHDOG_MS + 1));
    assert(!rt_session_lost(&s, 3000 + RT_WATCHDOG_MS * 1000));
    assert(act(&s, NULL, true, DRIVE(102, 1, 0)) == RT_DROP);
    assert(act(&s, NULL, true, HELLO_A) == RT_ADOPT);

    /* --- the watchdog trip ---------------------------------------------------- */
    rt_session_adopt(&s, SID_A, 9000);
    rt_session_command(&s, 500, 10000);
    assert(rt_session_lost(&s, 10000 + RT_WATCHDOG_MS + 1));
    rt_session_trip(&s);
    assert(s.have_owner && strcmp(s.sid, SID_A) == 0);
    assert(!s.armed && !rt_session_lost(&s, 10000 + RT_WATCHDOG_MS * 100));
    assert(s.have_seq && s.last_seq == 500);
    assert(act(&s, NULL, true, DRIVE(3, 0.2, 0)) == RT_DROP);
    assert(act(&s, NULL, true, DRIVE(501, 0.2, 0)) == RT_COMMAND);

    /* --- adoption clears the gate too ----------------------------------------- */
    rt_session_command(&s, 900, 20000);
    rt_session_adopt(&s, "0b17ac55", 21000);
    assert(act(&s, NULL, true, DRIVE(1, 0, 0)) == RT_COMMAND);

    /* --- dead sids: a stale hello cannot evict a live driver (rule 3) -------- */
    rt_dead_sids_t dead = {0};
    rt_dead_note(&dead, "deadbee1");
    assert(rt_dead_known(&dead, "deadbee1"));
    assert(!rt_dead_known(&dead, "7f3a91c2"));
    rt_dead_note(&dead, "deadbee2");
    rt_dead_note(&dead, "deadbee3");
    rt_dead_note(&dead, "deadbee4");
    rt_dead_note(&dead, "deadbee5");
    assert(!rt_dead_known(&dead, "deadbee1"));
    assert(rt_dead_known(&dead, "deadbee5"));

    rt_session_t live = {0};
    rt_session_adopt(&live, "0b17ac55", 1000);
    rt_dead_note(&dead, "deadsid1");
    assert(act(&live, &dead, false, "{\"proto\":2,\"type\":\"hello\",\"session\":\"deadsid1\"}") == RT_REPLY);
    assert(act(&live, &dead, false, HELLO_A) == RT_ADOPT);
    rt_session_t empty = {0};
    assert(act(&empty, &dead, false, "{\"proto\":2,\"type\":\"hello\",\"session\":\"deadsid1\"}") == RT_ADOPT);
    rt_dead_note(&dead, "0b17ac55");
    assert(act(&live, &dead, true, "{\"proto\":2,\"type\":\"hello\",\"session\":\"0b17ac55\"}") == RT_REPLY);

    /* --- mortality (rule 4) --------------------------------------------------- */
    rt_session_t m = {0};
    assert(!rt_session_idle(&m, 999999));
    rt_session_adopt(&m, "7f3a91c2", 2000);
    assert(!rt_session_idle(&m, 2000 + RT_SESSION_IDLE_MS));
    assert(rt_session_idle(&m, 2000 + RT_SESSION_IDLE_MS + 1));
    rt_session_command(&m, 7, 5000);
    assert(!rt_session_idle(&m, 5000 + 100));
    assert(!rt_session_idle(&m, 5000 + RT_SESSION_IDLE_MS + 1));
    rt_session_trip(&m);
    assert(!rt_session_idle(&m, 5000 + RT_SESSION_IDLE_MS));
    assert(rt_session_idle(&m, 5000 + RT_SESSION_IDLE_MS + 1));
    rt_session_bye(&m);
    assert(!rt_session_idle(&m, 5000 + 2 * RT_SESSION_IDLE_MS));

    printf("test_rt_session: all passed\n");
    return 0;
}
```

- [ ] **Шаг 2: Собрать и увидеть красное**

Команда: `make -C firmware/car/core/test test_rt_session 2>&1 | tail -5`
Ожидается: ошибка компиляции в `rt_link.h` (нет членов `has_hello`, `bye`, `has_ty`).

- [ ] **Шаг 3: Переписать `rt_session_classify` в `rt_link.h`**

```c
static inline rt_action_t rt_session_classify(const rt_session_t *s,
                                              const rt_dead_sids_t *dead,
                                              bool from_owner,
                                              const control_frame_t *f) {
    if (f->type == CT_HELLO) {
        /* A hello from a protocol we do not speak is answered by name and not adopted:
           a session neither side can parse is worse than no session, and the reply is
           how the mismatch becomes visible at all. */
        if (!f->has_proto || f->proto != RT_PROTO) return RT_REPLY;
        /* A repeat of the live session's own hello is answered but changes nothing.
           The app repeats the handshake until it is answered, so a retransmission must
           not reset the sequence gate or disarm the watchdog of a session that is
           already driving. */
        if (s->have_owner && from_owner && strcmp(s->sid, f->sid) == 0) return RT_REPLY;
        /* A dead session's replayed hello must not evict a live driver. With no live
           session there is nobody to protect, so the sid may return — refusing it
           would wedge a client whose session idled out mid-handshake. */
        if (s->have_owner && dead != NULL && rt_dead_known(dead, f->sid)) return RT_REPLY;
        return RT_ADOPT;   /* a different sid, or a different address: last hello wins */
    }
    /* proto rides on every datagram, and every datagram is judged by it: a drive in a
       dialect this car does not speak is not driven on. The hello above is the one
       exception, and only so the mismatch can be answered. */
    if (!f->has_proto || f->proto != RT_PROTO) return RT_DROP;
    if (!s->have_owner || !from_owner) return RT_DROP;   /* not our driver */
    /* Every app->car datagram except a hello carries seq — a goodbye included. The
       parser refuses these too; the rule is restated here because the ordering test
       below is meaningless without it, and this module does not get to assume its
       input was filtered. */
    if (!f->has_seq) return RT_DROP;
    /* Replay protection is what leaving TCP buys: a reordered or duplicated command
       costs one dropped datagram instead of blocking the queue behind a retransmission. */
    if (s->have_seq && !control_seq_newer(f->seq, s->last_seq)) return RT_DROP;
    if (f->type == CT_BYE)                  return RT_BYE;
    if (f->type == CT_DRIVE && f->has_axes) return RT_COMMAND;
    return RT_DROP;
}
```

- [ ] **Шаг 4: Обновить `rt_link.c`**

В `on_command` заменить `f->t, f->y` на `f->throttle, f->turn` (в вызовах `car_drive` и `recovery_note_command`). В ветке `case RT_REPLY:` предупреждение `"hello with proto %u, this car speaks %d"` оставить — туда по-прежнему попадают только hello. `send_hello_reply` переписывается в Задаче 10.

- [ ] **Шаг 5: Собрать и прогнать**

Команда: `make -C firmware/car/core/test test_rt_session test_control_proto && ./firmware/car/core/test/test_rt_session`
Ожидается: `test_rt_session: all passed`.

- [ ] **Шаг 6: Коммит**

```bash
git add firmware/car/core/main/rt_link.h firmware/car/core/main/rt_link.c firmware/car/core/test/test_rt_session.c
git commit -F- <<'MSG'
car(rt_link): classify by type, and judge proto on every datagram

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```
### Задача 9: Слова владельца, три общие группы телеметрии и принтер группы `device`

**Файлы:**
- Изменить: `firmware/car/core/main/link.h` (`link_src_name`, the `_Static_assert`), `firmware/car/core/main/telemetry.h` (whole file), `firmware/car/core/main/telemetry.c` (`telemetry_gather`, `telemetry_json`)
- Создать: `firmware/car/core/main/device_json.h`, `firmware/car/core/test/test_device_json.c`
- Изменить тесты: `firmware/car/core/test/test_link.c` (`ctl_vocabulary`), `firmware/car/core/test/test_telemetry.c` (целиком), `firmware/car/core/test/test_contract_wire.c` (целиком), `firmware/car/core/test/Makefile` (добавить `test_device_json`)

**Интерфейсы:**
- Даёт: `telemetry_t { uint32_t seq; int rssi; int rx_hz; uint32_t timeouts; long uptime_s; uint32_t free_heap; bool calibrated; bool bus_ok; const char *owner; }`; `int telemetry_groups(char *buf, size_t n, const telemetry_t *t)` — члены `link`, `motors`, `system` без обрамляющих скобок; `int telemetry_datagram(char *buf, size_t n, const telemetry_t *t)` — вся датаграмма `{"proto":2,"type":"telemetry","seq":N,<groups>}`; `int telemetry_json(char *buf, size_t n)` (IDF: сбор, затем `telemetry_datagram`). `link_src_name()` возвращает слова `MOTORS_OWNER_*`. `device_json.h`: `int fw_build_number(const char *fw)` (−1, если нет `+цифры`), `int device_group_json(char *buf, size_t n, const char *fw, bool rolled_back)` — `"device":{"id":"ajmiddlecar","fw":"…","build":N,"rolled_back":…}` без хвостовой запятой.

- [ ] **Шаг 1: Написать тесты**

`firmware/car/core/test/test_link.c` — заменить `ctl_vocabulary` на:

```c
/* Telemetry's "owner" is a closed vocabulary the app switches on. The names come from
   the schema through link.h; this is the check that every source has one and that no
   two share it — a duplicate would report the wrong owner, and a missing one would
   send "?" to a phone that has no case for it. */
static void owner_vocabulary(void) {
    const link_src_t all[] = { LINK_SRC_NONE, LINK_SRC_RECOVER, LINK_SRC_CONSOLE,
                               LINK_SRC_RT, LINK_SRC_CALIB, LINK_SRC_OTA, LINK_SRC_SAFE };
    const int n = (int)(sizeof(all) / sizeof(all[0]));
    assert(n == MOTORS_OWNER_COUNT);
    for (int i = 0; i < n; i++) {
        assert(strcmp(link_src_name(all[i]), "?") != 0);
        for (int j = i + 1; j < n; j++) {
            assert(strcmp(link_src_name(all[i]), link_src_name(all[j])) != 0);
        }
    }
    assert(strcmp(link_src_name(LINK_SRC_NONE), MOTORS_OWNER_IDLE) == 0);
    assert(strcmp(link_src_name(LINK_SRC_RECOVER), MOTORS_OWNER_RECOVERING) == 0);
    assert(strcmp(link_src_name(LINK_SRC_RT), MOTORS_OWNER_REMOTE) == 0);
    assert(strcmp(link_src_name(LINK_SRC_CALIB), MOTORS_OWNER_CALIBRATION) == 0);
    assert(strcmp(link_src_name(LINK_SRC_OTA), MOTORS_OWNER_UPDATE) == 0);
    assert(strcmp(link_src_name(LINK_SRC_SAFE), MOTORS_OWNER_SAFE_STOP) == 0);
    assert(strcmp(link_src_name((link_src_t)99), "?") == 0);
}
```
и вызов в `main` поменять на `owner_vocabulary();`.

`firmware/car/core/test/test_telemetry.c` — заменить файл:

```c
#define TELEMETRY_HOST_TEST
#include "../main/telemetry.h"
#include "contract.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    char buf[RT_MAX_DATAGRAM];
    telemetry_t t = { .seq = 88, .rssi = -55, .rx_hz = 10, .timeouts = 2, .uptime_s = 123,
                      .free_heap = 198000, .calibrated = true, .owner = MOTORS_OWNER_REMOTE,
                      .bus_ok = true };
    int n = telemetry_groups(buf, sizeof(buf), &t);
    assert(n > 0);
    assert(strcmp(buf,
        "\"link\":{\"rx_hz\":10,\"rssi_dbm\":-55,\"timeouts\":2},"
        "\"motors\":{\"bus\":\"ok\",\"calibrated\":true,\"owner\":\"remote\"},"
        "\"system\":{\"uptime_s\":123,\"free_heap\":198000}") == 0);

    n = telemetry_datagram(buf, sizeof(buf), &t);
    assert(n > 0 && n == (int)strlen(buf));
    assert(strcmp(buf,
        "{\"proto\":2,\"type\":\"telemetry\",\"seq\":88,"
        "\"link\":{\"rx_hz\":10,\"rssi_dbm\":-55,\"timeouts\":2},"
        "\"motors\":{\"bus\":\"ok\",\"calibrated\":true,\"owner\":\"remote\"},"
        "\"system\":{\"uptime_s\":123,\"free_heap\":198000}}") == 0);

    /* The push is a datagram, so the whole frame has to be one. Worst case: every
       counter wide, a negative RSSI and the longest owner name. */
    telemetry_t wide = { .seq = 4294967295u, .rssi = -100, .rx_hz = 999,
                         .timeouts = 4294967295u, .uptime_s = 999999999,
                         .free_heap = 4294967295u, .calibrated = true,
                         .owner = MOTORS_OWNER_CALIBRATION, .bus_ok = false };
    int w = telemetry_datagram(buf, sizeof(buf), &wide);
    assert(w > 0);
    assert(w <= RT_MAX_DATAGRAM);
    assert(w > RT_MAX_COMMAND);   /* ...and would not fit the command cap */
    assert(strstr(buf, "\"bus\":\"down\""));
    printf("test_telemetry: widest push frame is %d bytes of %d\n", w, RT_MAX_DATAGRAM);

    /* A NULL owner is reported as idle rather than crashing snprintf. */
    t.owner = NULL;
    n = telemetry_groups(buf, sizeof(buf), &t);
    assert(n > 0 && strstr(buf, "\"owner\":\"idle\""));
    t.owner = MOTORS_OWNER_REMOTE;

    /* 0 dBm is "not measured", and the wire says so with null, not with a number that
       would read as a very strong signal. */
    t.calibrated = false; t.rssi = 0;
    n = telemetry_groups(buf, sizeof(buf), &t);
    assert(n > 0 && strstr(buf, "\"calibrated\":false") && strstr(buf, "\"rssi_dbm\":null"));

    assert(telemetry_groups(buf, 8, &t) == -1);
    assert(telemetry_datagram(buf, 40, &t) == -1);

    printf("test_telemetry: all passed\n");
    return 0;
}
```

`firmware/car/core/test/test_device_json.c` — новый:

```c
#include "../main/device_json.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    assert(fw_build_number("v1.0+784") == 784);
    assert(fw_build_number("v1.0+784-dirty") == 784);
    assert(fw_build_number("v1.0") == -1);
    assert(fw_build_number("v1.0+") == -1);
    assert(fw_build_number("v1.0+x") == -1);
    assert(fw_build_number("") == -1);

    char buf[160];
    int n = device_group_json(buf, sizeof(buf), "v1.0+784", false);
    assert(n > 0 && n == (int)strlen(buf));
    assert(strcmp(buf, "\"device\":{\"id\":\"ajmiddlecar\",\"fw\":\"v1.0+784\",\"build\":784,"
                       "\"rolled_back\":false}") == 0);
    n = device_group_json(buf, sizeof(buf), "v1.0", true);
    assert(n > 0 && strstr(buf, "\"build\":-1") && strstr(buf, "\"rolled_back\":true"));
    assert(device_group_json(buf, 20, "v1.0+784", false) == -1);
    printf("test_device_json: all passed\n");
    return 0;
}
```

`firmware/car/core/test/test_contract_wire.c` — заменить `main` (`slurp`, `str_after`, `same` оставить):

```c
/* Does `frame` contain `"name":` inside the object that begins right after `"group":{`? */
static int in_group(const char *frame, const char *group, const char *name) {
    char open[80], key[80];
    snprintf(open, sizeof(open), "\"%s\":{", group);
    snprintf(key, sizeof(key), "\"%s\":", name);
    const char *g = strstr(frame, open);
    if (!g) return 0;
    const char *end = strchr(g, '}');
    const char *k = strstr(g, key);
    return k != NULL && end != NULL && k < end;
}

int main(void) {
    char *json = slurp(CONTRACT_JSON);
    char v[64];

    /* --- identity: one car, one set of names -------------------------------- */
    assert(str_after(json, "device", v, sizeof(v)));
    same("device", v, CAR_DEVICE_ID);
    assert(str_after(json, "ssid", v, sizeof(v)));
    same("network.ssid", v, CAR_AP_SSID);
    assert(str_after(json, "password", v, sizeof(v)));
    same("network.password", v, CAR_AP_PASS);

    /* --- telemetry: every field of every group the schema lists, in its group ---- */
    telemetry_t t = { .seq = 88, .rssi = -55, .rx_hz = 10, .timeouts = 2, .uptime_s = 123,
                      .free_heap = 198000, .calibrated = true, .owner = MOTORS_OWNER_REMOTE,
                      .bus_ok = true };
    char frame[RT_MAX_DATAGRAM];
    assert(telemetry_datagram(frame, sizeof(frame), &t) > 0);

    /* Walk "groups": for each group named in "telemetry".groups, every "name" inside
       that group's "fields" must sit inside that group's object in the frame. The
       schema's shape is fixed and its strings carry no escapes, so a substring walk is
       enough. */
    const char *groups = strstr(json, "\"groups\"");
    assert(groups);
    const char *telemetry = strstr(json, "\"telemetry\"");
    assert(telemetry);
    const char *tg = strstr(telemetry, "\"groups\"");
    assert(tg);
    const char *tg_end = strchr(tg, ']');
    assert(tg_end);
    int n_fields = 0, n_groups = 0;
    const char *scan = tg;
    for (;;) {
        /* the next quoted word inside telemetry.groups */
        const char *q = strchr(scan, '"');
        if (!q || q > tg_end) break;
        q = strchr(q + 1, '"');           /* skip the "groups" key itself on the first pass */
        if (!q || q > tg_end) break;
        const char *open = strchr(q + 1, '"');
        if (!open || open > tg_end) break;
        const char *close = strchr(open + 1, '"');
        if (!close || close > tg_end) break;
        char gname[32];
        size_t len = (size_t)(close - open - 1);
        assert(len < sizeof(gname));
        memcpy(gname, open + 1, len);
        gname[len] = '\0';
        scan = close;
        n_groups++;
        /* that group's definition */
        char pat[48];
        snprintf(pat, sizeof(pat), "\"%s\": {", gname);
        const char *def = strstr(groups, pat);
        if (!def) { snprintf(pat, sizeof(pat), "\"%s\":{", gname); def = strstr(groups, pat); }
        assert(def);
        const char *def_end = strstr(def, "]");   /* the fields array's end */
        assert(def_end);
        const char *fs = def;
        for (;;) {
            const char *next = str_after(fs, "name", v, sizeof(v));
            if (!next || next > def_end) break;
            fs = next;
            if (!in_group(frame, gname, v)) {
                printf("FAIL telemetry field \"%s.%s\" is in the schema and not in the frame:\n"
                       "  %s\n", gname, v, frame);
                assert(0);
            }
            n_fields++;
        }
    }
    assert(n_groups == 3);
    assert(n_fields == 8);
    assert(strstr(frame, "\"proto\":2,\"type\":\"telemetry\",\"seq\":88,"));

    free(json);
    printf("test_contract_wire: OK (%d telemetry fields in %d groups, device \"%s\")\n",
           n_fields, n_groups, CAR_DEVICE_ID);
    return 0;
}
```

Обход выше рассчитан на форматирование схемы с отступами (`"link": {` с пробелом — так пишет `json.dump(indent=2)` и так написан файл Задачи 1); запасной шаблон покрывает компактный файл.

`firmware/car/core/test/Makefile` — добавить `test_device_json` в `all`, `run` и `clean` с правилом:

```make
test_device_json: test_device_json.c ../main/device_json.h ../main/identity.h ../main/cfg_table.inc
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
```

- [ ] **Шаг 2: Собрать четыре теста и увидеть красное**

Команда: `make -C firmware/car/core/test test_link test_telemetry test_device_json test_contract_wire 2>&1 | grep -c error`
Ожидается: ненулевое число ошибок (`MOTORS_OWNER_IDLE`, `telemetry_groups`, `device_json.h` отсутствуют).

- [ ] **Шаг 3: `link.h`**

Заменить `link_src_name` и `_Static_assert`:

```c
/* Pure: the word telemetry reports in motors.owner, and logs use. The spellings are the
 * schema's, so the app and the mock read the same words this returns. */
static inline const char *link_src_name(link_src_t s) {
    switch (s) {
        case LINK_SRC_NONE:    return MOTORS_OWNER_IDLE;
        case LINK_SRC_RECOVER: return MOTORS_OWNER_RECOVERING;
        case LINK_SRC_CONSOLE: return MOTORS_OWNER_CONSOLE;
        case LINK_SRC_RT:      return MOTORS_OWNER_REMOTE;
        case LINK_SRC_CALIB:   return MOTORS_OWNER_CALIBRATION;
        case LINK_SRC_OTA:     return MOTORS_OWNER_UPDATE;
        case LINK_SRC_SAFE:    return MOTORS_OWNER_SAFE_STOP;
        default:               return "?";
    }
}

/* One enumerator per word in the schema's motors.owner values (LINK_SRC_NONE included): a
 * value added to the contract must break this build rather than surface as "?" on the wire. */
_Static_assert(LINK_SRC_SAFE + 2 == MOTORS_OWNER_COUNT, "link_src_t and motors.owner disagree");
```
Комментарий у include на строке 6 поменять на `/* RT_WATCHDOG_MS, RT_COMMAND_HZ, the MOTORS_OWNER_* vocabulary */`.

- [ ] **Шаг 4: `device_json.h` (новый)**

```c
#ifndef DEVICE_JSON_H
#define DEVICE_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "contract.h"
#include "identity.h"

/* The `device` group — who is answering — printed once, for the hello reply and for
 * /status. One printer, so the two cannot spell the identity differently and present
 * on the phone as "wrong car". Pure: host-tested in test_device_json.c. */

/* The number after '+' in a version like "v1.0+784", or -1 when there is none. The app
 * compares this integer against the release it knows; the tag parse used to be its job. */
static inline int fw_build_number(const char *fw) {
    const char *plus = strchr(fw, '+');
    if (!plus || plus[1] < '0' || plus[1] > '9') return -1;
    int n = 0;
    for (const char *p = plus + 1; *p >= '0' && *p <= '9'; p++) {
        if (n > 214748363) return -1;   /* would not fit an int */
        n = n * 10 + (*p - '0');
    }
    return n;
}

/* "device":{...} — no braces around, no trailing comma. Returns the length, or -1 when
 * it does not fit: a truncated identity parses as a different car, so a caller must
 * refuse to send rather than send what fits. */
static inline int device_group_json(char *buf, size_t n, const char *fw, bool rolled_back) {
    int r = snprintf(buf, n,
        "\"" KEY_GROUP_DEVICE "\":{\"" KEY_DEVICE_ID "\":\"" CAR_DEVICE_ID "\","
        "\"" KEY_DEVICE_FW "\":\"%s\",\"" KEY_DEVICE_BUILD "\":%d,"
        "\"" KEY_DEVICE_ROLLED_BACK "\":%s}",
        fw, fw_build_number(fw), rolled_back ? "true" : "false");
    if (r < 0 || r >= (int)n) return -1;
    return r;
}

#endif /* DEVICE_JSON_H */
```

- [ ] **Шаг 5: `telemetry.h`**

Заменить структуру и `telemetry_fields` (enum `telem_consumer_t` и IDF-секцию оставить):

```c
// Live telemetry snapshot: the three groups every push and every /status carries.
typedef struct {
    uint32_t seq;         // push counter, so the app can drop a reordered datagram
    int      rssi;        // dBm, 0 = not measured (printed as null)
    int      rx_hz;       // drive datagrams/sec
    uint32_t timeouts;    // watchdog trips since boot
    long     uptime_s;    // seconds
    uint32_t free_heap;   // bytes
    bool     calibrated;  // valid calibration present
    bool     bus_ok;      // false once a PCA9685 write failed and has not since succeeded
    const char *owner;    // which source owns the actuator: one of the MOTORS_OWNER_* words
} telemetry_t;

// Pure: the "link", "motors" and "system" members (NO surrounding braces, no trailing
// comma). Shared by the real-time push and /status. Every key is a generated macro, so a
// rename in the schema cannot survive here; test_contract_wire checks the nesting.
// Returns the length, or -1 on truncation.
static inline int telemetry_groups(char *buf, size_t n, const telemetry_t *t) {
    char rssi[12];
    if (t->rssi != 0) snprintf(rssi, sizeof(rssi), "%d", t->rssi);
    else              snprintf(rssi, sizeof(rssi), "null");
    int r = snprintf(buf, n,
        "\"" KEY_GROUP_LINK "\":{\"" KEY_LINK_RX_HZ "\":%d,\"" KEY_LINK_RSSI_DBM "\":%s,"
            "\"" KEY_LINK_TIMEOUTS "\":%u},"
        "\"" KEY_GROUP_MOTORS "\":{\"" KEY_MOTORS_BUS "\":\"%s\",\"" KEY_MOTORS_CALIBRATED "\":%s,"
            "\"" KEY_MOTORS_OWNER "\":\"%s\"},"
        "\"" KEY_GROUP_SYSTEM "\":{\"" KEY_SYSTEM_UPTIME_S "\":%ld,\"" KEY_SYSTEM_FREE_HEAP "\":%u}",
        t->rx_hz, rssi, (unsigned)t->timeouts,
        t->bus_ok ? MOTORS_BUS_OK : MOTORS_BUS_DOWN, t->calibrated ? "true" : "false",
        t->owner ? t->owner : MOTORS_OWNER_IDLE,
        t->uptime_s, (unsigned)t->free_heap);
    if (r < 0 || r >= (int)n) return -1;
    return r;
}

// Pure: the whole push datagram, {"proto":2,"type":"telemetry","seq":N,<groups>}.
static inline int telemetry_datagram(char *buf, size_t n, const telemetry_t *t) {
    int r = snprintf(buf, n, "{\"" KEY_PROTO "\":%d,\"" RT_KEY_TYPE "\":\"" RT_TYPE_TELEMETRY "\","
                             "\"" RT_KEY_SEQ "\":%u,", RT_PROTO, (unsigned)t->seq);
    if (r < 0 || r >= (int)n) return -1;
    int g = telemetry_groups(buf + r, n - (size_t)r, t);
    if (g < 0) return -1;
    r += g;
    if ((size_t)r + 2 > n) return -1;   /* the brace and the NUL */
    buf[r++] = '}';
    buf[r] = '\0';
    return r;
}
```
Обновить комментарий к IDF-прототипу: `int telemetry_json(char *buf, size_t n);  // gather + telemetry_datagram for the rt_link push`.

- [ ] **Шаг 6: `telemetry.c`**

В `telemetry_gather`: `out->rx_hz = fps_now(who); out->timeouts = rt_link_wdt_trips(); out->free_heap = …; out->owner = link_src_name(link_owner());` (переименовать четыре члена; `s_rssi`, `calibrated`, `bus_ok`, `seq` без изменений). Заменить `telemetry_json`:

```c
int telemetry_json(char *buf, size_t n) {
    telemetry_t t;
    telemetry_gather(&t, TELEM_PUSH);
    return telemetry_datagram(buf, n, &t);
}
```

- [ ] **Шаг 7: Собрать и прогнать четыре теста**

Команда: `make -C firmware/car/core/test test_link test_telemetry test_device_json test_contract_wire && (cd firmware/car/core/test && ./test_link && ./test_telemetry && ./test_device_json && ./test_contract_wire)`
Ожидается: четыре строки `passed`/`OK`; `test_telemetry` печатает ширину самого широкого кадра (≈ 215 байт, меньше 320).

- [ ] **Шаг 8: Коммит**

```bash
git add firmware/car/core/main/link.h firmware/car/core/main/telemetry.h firmware/car/core/main/telemetry.c firmware/car/core/main/device_json.h firmware/car/core/test/test_link.c firmware/car/core/test/test_telemetry.c firmware/car/core/test/test_device_json.c firmware/car/core/test/test_contract_wire.c firmware/car/core/test/Makefile
git commit -F- <<'MSG'
car(telemetry): the three shared groups, the owner words, one device printer

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 10: Конверт ошибок, `/status` в группах и `hello_ack`

**Файлы:**
- Изменить: `firmware/car/core/main/api_util.h`, `api_util.c`, `status_api.h`, `status_api.c`, `rt_link.c` (`send_hello_reply`)
- Тест: none host-side beyond what Task 9 pinned (these are IDF files); compiled in Task 13.

**Интерфейсы:**
- Даёт: `esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *code, const char *field, const char *msg)` — `{"proto":2,"error":{"code":…,"message":…[,"field":…]}}`; `api_reply_ok(req)` — `{"proto":2,"ok":true}`; `esp_err_t api_reply_json(httpd_req_t *req, const char *members)` — шлёт `{"proto":2,<members>}`; `bool status_api_rolled_back(void)` (лениво, с кэшем).

- [ ] **Шаг 1: `api_util.h` / `api_util.c`**

Заголовок: прототип ошибки — `esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *code, const char *field, const char *msg);` с комментарием `/* {"proto":2,"error":{"code":"<code>","message":"<msg>","field":"<field>"}} — code is one of the ERR_* words; field is omitted when "" (the whole body is at fault). */`, и добавить:

```c
/* {"proto":2,<members>} — the caller supplies the members without the outer braces and
 * without a leading comma; the envelope's proto is prepended here so no endpoint can
 * forget it. */
esp_err_t api_reply_json(httpd_req_t *req, const char *members);
```

Реализация:

```c
#include "api_util.h"
#include <stdio.h>
#include "contract.h"

esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *code,
                          const char *field, const char *msg) {
    char buf[224];
    int n;
    if (field && field[0]) {
        n = snprintf(buf, sizeof(buf),
                     "{\"" KEY_PROTO "\":%d,\"" KEY_ERROR "\":{\"" KEY_ERROR_CODE "\":\"%s\","
                     "\"" KEY_ERROR_MESSAGE "\":\"%s\",\"" KEY_ERROR_FIELD "\":\"%s\"}}",
                     RT_PROTO, code, msg, field);
    } else {
        n = snprintf(buf, sizeof(buf),
                     "{\"" KEY_PROTO "\":%d,\"" KEY_ERROR "\":{\"" KEY_ERROR_CODE "\":\"%s\","
                     "\"" KEY_ERROR_MESSAGE "\":\"%s\"}}",
                     RT_PROTO, code, msg);
    }
    if (n < 0 || n >= (int)sizeof(buf)) return ESP_FAIL;
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

esp_err_t api_reply_ok(httpd_req_t *req) {
    return api_reply_json(req, "\"" KEY_OK "\":true");
}

esp_err_t api_reply_json(httpd_req_t *req, const char *members) {
    char buf[640];
    int n = snprintf(buf, sizeof(buf), "{\"" KEY_PROTO "\":%d,%s}", RT_PROTO, members);
    if (n < 0 || n >= (int)sizeof(buf)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}
```
`api_read_body` без изменений. (`buf[640]` вмещает `/status` в худшем случае ≈ 420, `/config` ≈ 230, `/calibration` ≈ 260.)

- [ ] **Шаг 2: `status_api.h` / `status_api.c`**

В заголовок добавить:

```c
/* Did the bootloader revert the previous OTA? Read once, lazily, cached: rt_link_start
 * runs before status_api_start, and the hello reply carries this flag too. */
bool status_api_rolled_back(void);
```

В `status_api.c`: сделать `read_rollback_state` ленивой —

```c
static bool s_rollback = false;
static bool s_rollback_read = false;

bool status_api_rolled_back(void) {
    if (!s_rollback_read) {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_ota_img_states_t st;
        s_rollback = other != NULL &&
                     esp_ota_get_state_partition(other, &st) == ESP_OK &&
                     st == ESP_OTA_IMG_ABORTED;
        s_rollback_read = true;
        if (s_rollback) ESP_LOGW(TAG, "the previous OTA was rolled back by the bootloader");
    }
    return s_rollback;
}
```
(`read_rollback_state` удалить; из `status_api_start` вызывать `status_api_rolled_back()` там, где вызывалась она). `s_radio_fw`/`s_radio_ok` оставить, но инициализировать `s_radio_fw[0] = '\0'` для «недоступно» — `read_radio_version` копирует версию только если `radio_flash_version()` вернула непустую строку и не литерал "unavailable" (проверить, что она возвращает; если сегодня "unavailable" — превращать в пустую строку). Добавить:

```c
static const char *radio_state_word(void) {
    if (s_radio_fw[0] == '\0') return RADIO_STATE_UNAVAILABLE;
    return s_radio_ok ? RADIO_STATE_OK : RADIO_STATE_MISMATCH;
}
```

Заменить `status_get` на эту версию — шесть групп в порядке схемы (link/motors/system через их общий принтер, чтобы слова оставались одними и теми же):

```c
static esp_err_t status_get(httpd_req_t *req) {
    telemetry_t t;
    telemetry_gather(&t, TELEM_STATUS);
    char device[160];
    if (device_group_json(device, sizeof(device), esp_app_get_description()->version,
                          status_api_rolled_back()) < 0) {
        ESP_LOGE(TAG, "/status could not render the device group");
        return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "identity too long");
    }
    char groups[256];
    if (telemetry_groups(groups, sizeof(groups), &t) < 0) {
        ESP_LOGE(TAG, "/status could not render its telemetry groups");
        return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "telemetry unavailable");
    }
    /* telemetry_groups prints link, motors, system. The schema's status order is device,
       link, motors, radio, storage, system — so the system member is split off the tail
       and radio/storage go in before it. Splitting on the last group's opening key keeps
       the three words spelled by the one printer telemetry uses. */
    char *sys = strstr(groups, "\"" KEY_GROUP_SYSTEM "\":{");
    if (!sys || sys == groups || sys[-1] != ',') {
        ESP_LOGE(TAG, "/status could not find the system group");
        return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "status malformed");
    }
    sys[-1] = '\0';                       /* groups is now link,motors; sys is system */
    char radio_fw[32];
    if (s_radio_fw[0]) snprintf(radio_fw, sizeof(radio_fw), "\"%s\"", s_radio_fw);
    else               snprintf(radio_fw, sizeof(radio_fw), "null");
    char members[560];
    int n = snprintf(members, sizeof(members),
                     "%s,%s,"
                     "\"" KEY_GROUP_RADIO "\":{\"" KEY_RADIO_FW "\":%s,"
                         "\"" KEY_RADIO_EXPECTED "\":\"" RADIO_EXPECTED_FW "\","
                         "\"" KEY_RADIO_STATE "\":\"%s\"},"
                     "\"" KEY_GROUP_STORAGE "\":{\"" KEY_STORAGE_RESET_AT_BOOT "\":%s},"
                     "%s",
                     device, groups, radio_fw, radio_state_word(),
                     s_nvs_wiped ? "true" : "false", sys);
    if (n < 0 || n >= (int)sizeof(members)) {
        ESP_LOGE(TAG, "/status does not fit its buffer");
        return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "status too long");
    }
    return api_reply_json(req, members);
}
```
Подключить `"device_json.h"`, убрать использование `identity.h`/`RT_KEY_*`. Зарегистрировать обработчик на `PATH_STATUS`.

- [ ] **Шаг 3: `rt_link.c` — ответ на hello**

```c
static void send_hello_reply(int sock, const char *sid, const struct sockaddr_in *to) {
    char device[160];
    if (device_group_json(device, sizeof(device), esp_app_get_description()->version,
                          status_api_rolled_back()) < 0) {
        ESP_LOGE(TAG, "hello reply: the device group does not fit");
        return;
    }
    char buf[RT_MAX_DATAGRAM];
    int n = snprintf(buf, sizeof(buf),
                     "{\"" KEY_PROTO "\":%d,\"" RT_KEY_TYPE "\":\"" RT_TYPE_HELLO_ACK "\","
                     "\"" RT_KEY_SESSION "\":\"%s\",%s}",
                     RT_PROTO, sid, device);
    if (n < 0 || n >= (int)sizeof(buf)) {
        /* A truncated identity is worse than none: it would parse as a different car. */
        ESP_LOGE(TAG, "hello reply does not fit a datagram");
        return;
    }
    if (sendto(sock, buf, (size_t)n, 0, (const struct sockaddr *)to, sizeof(*to)) < 0) {
        ESP_LOGW(TAG, "hello reply failed: errno %d", errno);
    }
}
```
Добавить `#include "device_json.h"` и `#include "status_api.h"`.

- [ ] **Шаг 4: Коммит (компиляцию докажет Задача 13)**

```bash
git add firmware/car/core/main/api_util.h firmware/car/core/main/api_util.c firmware/car/core/main/status_api.h firmware/car/core/main/status_api.c firmware/car/core/main/rt_link.c
git commit -F- <<'MSG'
car(http): the v2 envelope on every reply, /status in groups, hello_ack with the device group

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 11: Один `/config`

**Файлы:**
- Изменить: `firmware/car/core/main/cfg_contract.h`, `firmware/car/core/main/cfg_api.c` (whole file)
- Создать: `firmware/car/core/main/cfg_value.h`, `firmware/car/core/test/test_cfg_value.c`
- Изменить тесты: `firmware/car/core/test/test_cfg_table.c`, `firmware/car/core/test/Makefile`

**Интерфейсы:**
- Даёт: `cfg_type_t { CFG_INT, CFG_BOOL, CFG_ENUM, CFG_FIXED }`; `cfg_field_t` получает `int32_t scale;` последним членом; `cfg_domain_t.key` вместо `.path`. `cfg_value.h`: `bool cfg_value_from_double(const cfg_field_t *f, double v, int32_t *out)` (правило типа: bool сюда не попадает; `CFG_FIXED` округляет от нуля; int/enum требуют целого), `int cfg_value_print(const cfg_field_t *f, int32_t v, char *buf, size_t n)` (bool → `true`/`false`, fixed → дробь с числом знаков по `scale`, иначе целое), `const char *cfg_value_check(const cfg_field_t *f, int32_t v)` (NULL, если в диапазоне, иначе слово `ERR_*`).

- [ ] **Шаг 1: Тесты**

`firmware/car/core/test/test_cfg_value.c` — новый:

```c
#include "../main/cfg_contract.h"
#include "../main/cfg_table.inc"
#include "../main/cfg_value.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const cfg_field_t *find(const char *key, const char *name) {
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        if (strcmp(CFG_DOMAINS[i].key, key) != 0) continue;
        for (int f = 0; f < CFG_DOMAINS[i].n_fields; f++)
            if (strcmp(CFG_DOMAINS[i].fields[f].name, name) == 0) return &CFG_DOMAINS[i].fields[f];
    }
    return NULL;
}

int main(void) {
    const cfg_field_t *gear = find("wheel", "gear_ratio");
    const cfg_field_t *rise = find("ramp", "rise_ms");
    const cfg_field_t *quad = find("wheel", "quadrature");
    const cfg_field_t *en   = find("recovery", "enabled");
    assert(gear && rise && quad && en);
    int32_t v;
    /* fixed: decimal on the wire, integer inside, half away from zero */
    assert(cfg_value_from_double(gear, 9.0, &v) && v == 900);
    /* 9.125 x 100 is exactly 912.5: lround says 913 (half away from zero). 9.005 would
       not do here — it is 9.00499… in binary on both sides, so both agree on 900. */
    assert(cfg_value_from_double(gear, 9.125, &v) && v == 913);
    assert(cfg_value_from_double(gear, 9.124, &v) && v == 912);
    assert(cfg_value_from_double(gear, 300.004, &v) && v == 30000);
    assert(cfg_value_check(gear, 30000) == NULL);
    assert(cfg_value_from_double(gear, 300.0051, &v) && strcmp(cfg_value_check(gear, v), ERR_OUT_OF_RANGE) == 0);
    /* int: a fraction is a type error, not a truncation */
    assert(cfg_value_from_double(rise, 300, &v) && v == 300);
    assert(!cfg_value_from_double(rise, 25.7, &v));
    assert(cfg_value_check(rise, 2000) == NULL);
    assert(strcmp(cfg_value_check(rise, 2001), ERR_OUT_OF_RANGE) == 0);
    assert(strcmp(cfg_value_check(rise, -1), ERR_OUT_OF_RANGE) == 0);
    /* enum: not in the list is not_allowed, not out_of_range */
    assert(cfg_value_from_double(quad, 3, &v) && strcmp(cfg_value_check(quad, v), ERR_NOT_ALLOWED) == 0);
    assert(cfg_value_check(quad, 4) == NULL);
    /* bool: printed as a word */
    char buf[32];
    assert(cfg_value_print(en, 1, buf, sizeof(buf)) > 0 && strcmp(buf, "true") == 0);
    assert(cfg_value_print(en, 0, buf, sizeof(buf)) > 0 && strcmp(buf, "false") == 0);
    assert(cfg_value_print(gear, 900, buf, sizeof(buf)) > 0 && strcmp(buf, "9.00") == 0);
    assert(cfg_value_print(gear, 901, buf, sizeof(buf)) > 0 && strcmp(buf, "9.01") == 0);
    assert(cfg_value_print(gear, 30000, buf, sizeof(buf)) > 0 && strcmp(buf, "300.00") == 0);
    assert(cfg_value_print(rise, 300, buf, sizeof(buf)) > 0 && strcmp(buf, "300") == 0);
    assert(cfg_value_print(find("trim", "balance_pct"), -30, buf, sizeof(buf)) > 0 && strcmp(buf, "-30") == 0);
    assert(cfg_value_print(gear, 900, buf, 3) == -1);
    printf("test_cfg_value: all passed\n");
    return 0;
}
```

`firmware/car/core/test/test_cfg_table.c` — заменить тело `main` после хелпера (`find` оставить, но сравнивать первый параметр с `CFG_DOMAINS[i].key`):

```c
int main(void) {
    assert(CFG_DOMAIN_COUNT == 5);
    assert(CFG_MAX_FIELDS == 4);
    assert(strcmp(CFG_CONFIG_PATH, "/config") == 0);
    assert(RT_PORT == 4210);
    assert(RT_WATCHDOG_MS == 300);
    assert(RT_MAX_COMMAND == 96);
    assert(RT_MAX_DATAGRAM == 320);
    assert(RT_MAX_COMMAND < RT_MAX_DATAGRAM);
    assert(RT_PROTO == 2);
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) assert(CFG_DOMAINS[i].n_fields <= CFG_MAX_FIELDS);

    const cfg_field_t *d = find("wheel", "diameter_mm");
    assert(d && d->type == CFG_INT && d->min == 20 && d->max == 150 && d->def == 65 && d->scale == 1);
    const cfg_field_t *g = find("wheel", "gear_ratio");
    assert(g && g->type == CFG_FIXED && g->min == 100 && g->max == 30000 && g->def == 900 && g->scale == 100);
    const cfg_field_t *q = find("wheel", "quadrature");
    assert(q && q->type == CFG_ENUM && q->n_allowed == 3);
    assert(q->allowed[0] == 1 && q->allowed[1] == 2 && q->allowed[2] == 4);
    const cfg_field_t *e = find("recovery", "enabled");
    assert(e && e->type == CFG_BOOL && e->def == 1);
    const cfg_field_t *w = find("recovery", "window_ms");
    assert(w && w->min == 1000 && w->max == 10000 && w->def == 5000);
    const cfg_field_t *t = find("trim", "balance_pct");
    assert(t && t->min == -30 && t->max == 30 && t->def == 0);
    assert(find("wheel", "nonexistent") == NULL);
    assert(find("dims", "track_mm") == NULL);        /* the JSON key is chassis; dims is the NVS key */
    assert(find("chassis", "track_mm") != NULL);

    /* Every domain must name a distinct NVS key and a distinct JSON key. */
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        for (int j = i + 1; j < CFG_DOMAIN_COUNT; j++) {
            assert(strcmp(CFG_DOMAINS[i].nvs_key, CFG_DOMAINS[j].nvs_key) != 0);
            assert(strcmp(CFG_DOMAINS[i].key, CFG_DOMAINS[j].key) != 0);
        }
    }
    printf("test_cfg_table: OK\n");
    return 0;
}
```

Makefile: добавить `test_cfg_value` в `all`, `run`, `clean` с правилом:

```make
test_cfg_value: test_cfg_value.c ../main/cfg_value.h ../main/cfg_table.inc ../main/cfg_contract.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
```

- [ ] **Шаг 2: Собрать и увидеть красное**

Команда: `make -C firmware/car/core/test test_cfg_table test_cfg_value 2>&1 | grep -c error`
Ожидается: ненулевое число ошибок (`CFG_FIXED`, `.key`, `scale`, `cfg_value.h`).

- [ ] **Шаг 3: `cfg_contract.h`**

```c
typedef enum { CFG_INT, CFG_BOOL, CFG_ENUM, CFG_FIXED } cfg_type_t;

typedef struct {
    const char    *name;
    cfg_type_t     type;
    int32_t        min;         /* CFG_BOOL: 0..1; CFG_ENUM: the value bounds; CFG_FIXED: x scale */
    int32_t        max;
    int32_t        def;
    const int32_t *allowed;     /* NULL unless CFG_ENUM */
    uint8_t        n_allowed;
    int32_t        scale;       /* CFG_FIXED: wire decimal x scale = the integer held; 1 otherwise */
} cfg_field_t;

typedef struct {
    const char        *key;     /* the member name inside /config */
    const char        *nvs_key;
    const cfg_field_t *fields;
    uint8_t            n_fields;
} cfg_domain_t;
```

- [ ] **Шаг 4: `cfg_value.h` (новый, чистый)**

```c
#ifndef CFG_VALUE_H
#define CFG_VALUE_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "cfg_contract.h"
#include "contract.h"

/* A JSON number -> the integer the descriptor holds. false when the number is not the
 * field's kind: an int or enum given a fraction (cJSON's valueint would TRUNCATE 25.7 to
 * 25 under a 200, and the generated validator answers wrong_type for the same bytes).
 * CFG_FIXED takes any finite number and rounds half away from zero, as lround does and
 * as the mock's lround does. Booleans never reach here — cJSON_IsBool is checked first. */
static inline bool cfg_value_from_double(const cfg_field_t *f, double v, int32_t *out) {
    if (!isfinite(v)) return false;
    if (f->type == CFG_FIXED) {
        double s = v * (double)f->scale;
        if (s > 2147483647.0 || s < -2147483648.0) return false;
        *out = (int32_t)lround(s);
        return true;
    }
    if (v != (double)(int32_t)v) return false;
    *out = (int32_t)v;
    return true;
}

/* NULL when `v` is acceptable for `f`, else the ERR_* word that names why. */
static inline const char *cfg_value_check(const cfg_field_t *f, int32_t v) {
    if (f->type == CFG_ENUM) {
        for (uint8_t k = 0; k < f->n_allowed; k++) if (f->allowed[k] == v) return NULL;
        return ERR_NOT_ALLOWED;
    }
    if (v < f->min || v > f->max) return ERR_OUT_OF_RANGE;
    return NULL;
}

/* The wire spelling of a held value: true/false, a decimal with scale's digits, or an
 * integer. Returns the length, or -1 when it does not fit. */
static inline int cfg_value_print(const cfg_field_t *f, int32_t v, char *buf, size_t n) {
    int r;
    if (f->type == CFG_BOOL) {
        r = snprintf(buf, n, "%s", v ? "true" : "false");
    } else if (f->type == CFG_FIXED) {
        int digits = 0;
        for (int32_t s = f->scale; s > 1; s /= 10) digits++;
        int32_t whole = v / f->scale, frac = v % f->scale;
        if (frac < 0) frac = -frac;
        if (v < 0 && whole == 0) r = snprintf(buf, n, "-0.%0*ld", digits, (long)frac);
        else                     r = snprintf(buf, n, "%ld.%0*ld", (long)whole, digits, (long)frac);
    } else {
        r = snprintf(buf, n, "%ld", (long)v);
    }
    if (r < 0 || r >= (int)n) return -1;
    return r;
}

#endif /* CFG_VALUE_H */
```

- [ ] **Шаг 5: Переписать `cfg_api.c`**

Оставить include'ы, `TAG`, `_Static_assert`, typedef'ы биндингов, десять функций `*_get_v`/`*_set_v` и `BINDINGS[]` — но ключом `BINDINGS[]` сделать ключ домена: `{ "ramp", … }, { "trim", … }, { "recovery", recover_get_v, recover_set_v, recovery_save }, { "wheel", … }, { "chassis", dims_get_v, dims_set_v, dims_save }`. Всё от `domain_for` до конца заменить на:

```c
static const cfg_binding_t *binding_for(const char *key) {
    for (size_t i = 0; i < sizeof(BINDINGS) / sizeof(BINDINGS[0]); i++) {
        if (strcmp(BINDINGS[i].key, key) == 0) return &BINDINGS[i];
    }
    return NULL;
}

/* One domain as "<key>":{...}, appended at buf+*at. */
static bool render_domain(const cfg_domain_t *d, const int32_t *vals, char *buf, size_t n, size_t *at) {
    int w = snprintf(buf + *at, n - *at, "\"%s\":{", d->key);
    if (w < 0 || (size_t)w >= n - *at) return false;
    *at += (size_t)w;
    for (int i = 0; i < d->n_fields; i++) {
        char v[32];
        if (cfg_value_print(&d->fields[i], vals[i], v, sizeof(v)) < 0) return false;
        w = snprintf(buf + *at, n - *at, "%s\"%s\":%s", i ? "," : "", d->fields[i].name, v);
        if (w < 0 || (size_t)w >= n - *at) return false;
        *at += (size_t)w;
    }
    if (*at + 2 > n) return false;
    buf[(*at)++] = '}';
    buf[*at] = '\0';
    return true;
}

/* Every domain, as the members of /config's body. The reply to a POST is this too: the
 * client sees what the car now holds, not a receipt. */
static esp_err_t reply_config(httpd_req_t *req) {
    char members[400];
    size_t at = 0;
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        const cfg_domain_t *d = &CFG_DOMAINS[i];
        const cfg_binding_t *b = binding_for(d->key);
        if (!b) return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "domain unbound");
        int32_t vals[CFG_MAX_FIELDS];
        b->get(vals);
        if (i && at + 1 < sizeof(members)) members[at++] = ',';
        if (!render_domain(d, vals, members, sizeof(members), &at)) {
            return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "response too long");
        }
    }
    return api_reply_json(req, members);
}

static esp_err_t cfg_get(httpd_req_t *req) { return reply_config(req); }

/* A dotted path for the error's field member. */
static const char *dotted(char *buf, size_t n, const char *key, const char *name) {
    snprintf(buf, n, "%s.%s", key, name);
    return buf;
}

/* Parse one present domain into vals[]. Returns NULL, or an error already sent. */
static esp_err_t parse_domain(httpd_req_t *req, const cfg_domain_t *d, const cJSON *obj,
                              int32_t *vals, bool *sent) {
    char where[48];
    *sent = false;
    if (!cJSON_IsObject(obj)) {
        *sent = true;
        return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, d->key, "expected an object");
    }
    /* Unknown keys are refused: this is a two-party API where a typo is a bug. */
    for (const cJSON *it = obj->child; it; it = it->next) {
        bool known = false;
        for (int i = 0; i < d->n_fields; i++) if (strcmp(d->fields[i].name, it->string) == 0) known = true;
        if (!known) {
            *sent = true;
            return api_reply_error(req, "400 Bad Request", ERR_UNKNOWN_FIELD,
                                   dotted(where, sizeof(where), d->key, it->string), "no such field");
        }
    }
    for (int i = 0; i < d->n_fields; i++) {
        const cfg_field_t *f = &d->fields[i];
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, f->name);
        dotted(where, sizeof(where), d->key, f->name);
        if (!it) {
            *sent = true;
            return api_reply_error(req, "400 Bad Request", ERR_MISSING_FIELD, where, "required");
        }
        if (f->type == CFG_BOOL) {
            if (!cJSON_IsBool(it)) {
                *sent = true;
                return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, where, "expected a boolean");
            }
            vals[i] = cJSON_IsTrue(it) ? 1 : 0;
            continue;
        }
        if (!cJSON_IsNumber(it) || !cfg_value_from_double(f, it->valuedouble, &vals[i])) {
            *sent = true;
            return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, where,
                                   f->type == CFG_FIXED ? "expected a number" : "expected an integer");
        }
        const char *bad = cfg_value_check(f, vals[i]);
        if (bad) {
            *sent = true;
            return api_reply_error(req, "400 Bad Request", bad, where,
                                   bad == ERR_NOT_ALLOWED ? "not an allowed value" : "out of range");
        }
    }
    return ESP_OK;
}

static esp_err_t cfg_post(httpd_req_t *req) {
    char body[512];
    if (api_read_body(req, body, sizeof(body)) < 0) {
        return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "body missing or too long");
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "malformed JSON");
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "expected a JSON object");
    }
    /* Pass one: validate everything. Nothing is applied until every present domain is
       good, so a body that is half right changes nothing. */
    bool present[CFG_DOMAIN_COUNT] = {0};
    int32_t vals[CFG_DOMAIN_COUNT][CFG_MAX_FIELDS];
    int n_present = 0;
    for (const cJSON *it = j->child; it; it = it->next) {
        int which = -1;
        for (int i = 0; i < CFG_DOMAIN_COUNT; i++) if (strcmp(CFG_DOMAINS[i].key, it->string) == 0) which = i;
        if (which < 0) {
            esp_err_t e = api_reply_error(req, "400 Bad Request", ERR_UNKNOWN_FIELD, it->string,
                                          "not a configuration domain");
            cJSON_Delete(j);
            return e;
        }
        bool sent;
        esp_err_t e = parse_domain(req, &CFG_DOMAINS[which], it, vals[which], &sent);
        if (sent) { cJSON_Delete(j); return e; }
        present[which] = true;
        n_present++;
    }
    cJSON_Delete(j);
    if (n_present == 0) {
        return api_reply_error(req, "400 Bad Request", ERR_MISSING_FIELD, "", "no configuration domain in the body");
    }
    /* Pass two: apply, then persist, domain by domain. A persist that fails rolls its own
       domain back before answering — the running car and the client must not hold two
       truths — and answers 500; domains applied before it stay applied, which the reply
       (had it been sent) would have shown. */
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        if (!present[i]) continue;
        const cfg_binding_t *b = binding_for(CFG_DOMAINS[i].key);
        if (!b) return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, CFG_DOMAINS[i].key, "domain unbound");
        int32_t prev[CFG_MAX_FIELDS];
        b->get(prev);
        if (!b->set(vals[i])) {
            return api_reply_error(req, "500 Internal Server Error", ERR_WRITE_FAILED, CFG_DOMAINS[i].key, "could not apply");
        }
        if (b->save() != ESP_OK) {
            if (!b->set(prev)) {
                ESP_LOGE(TAG, "%s: could not persist, and could not roll back either — the running "
                              "value now differs from both NVS and the client", CFG_DOMAINS[i].key);
            }
            return api_reply_error(req, "500 Internal Server Error", ERR_WRITE_FAILED, CFG_DOMAINS[i].key, "could not persist");
        }
    }
    return reply_config(req);
}

esp_err_t cfg_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    httpd_uri_t g = { .uri = CFG_CONFIG_PATH, .method = HTTP_GET,  .handler = cfg_get };
    httpd_uri_t p = { .uri = CFG_CONFIG_PATH, .method = HTTP_POST, .handler = cfg_post };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &g), TAG, "reg GET " CFG_CONFIG_PATH);
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &p), TAG, "reg POST " CFG_CONFIG_PATH);
    ESP_LOGI(TAG, "config endpoint registered (%d domains)", CFG_DOMAIN_COUNT);
    return ESP_OK;
}
```
Член структуры биндинга `path` переименовать в `key`, добавить `#include "cfg_value.h"`.

- [ ] **Шаг 6: Собрать и прогнать два хост-теста**

Команда: `make -C firmware/car/core/test test_cfg_table test_cfg_value && (cd firmware/car/core/test && ./test_cfg_table && ./test_cfg_value)`
Ожидается: `test_cfg_table: OK`, `test_cfg_value: all passed`.

- [ ] **Шаг 7: Коммит**

```bash
git add firmware/car/core/main/cfg_contract.h firmware/car/core/main/cfg_value.h firmware/car/core/main/cfg_api.c firmware/car/core/test/test_cfg_value.c firmware/car/core/test/test_cfg_table.c firmware/car/core/test/Makefile
git commit -F- <<'MSG'
car(config): one /config — every domain, validated whole before any of it is applied

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 12: `/calibration` по углам

**Файлы:**
- Изменить: `firmware/car/core/main/calib_api.c` (whole file)
- Создать: `firmware/car/core/main/calib_wire.h`, `firmware/car/core/test/test_calib_wire.c`
- Изменить: `firmware/car/core/main/car.h`/`car.c` (add `void car_get_calibration(motors_config_t *out)` — copy the published table), `firmware/car/core/test/Makefile`

**Интерфейсы:**
- Даёт: `calib_wire.h`: `int calib_corner_index(const char *word)` (0..3 или −1, порядок `CORNER_*` = `POS_FL..POS_RR`), `const char *calib_corner_word(int pos)`, `int calib_direction_forward(const char *word)` (1 вперёд, 0 назад, −1 неизвестно), `int calib_table_json(char *buf, size_t n, bool calibrated, const motors_config_t *cfg)` — `"calibrated":…,"wheels":[…]`.

- [ ] **Шаг 1: Тест**

`firmware/car/core/test/test_calib_wire.c`:

```c
#include "../main/calib_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    assert(calib_corner_index("front_left") == POS_FL);
    assert(calib_corner_index("rear_right") == POS_RR);
    assert(calib_corner_index("Front_Left") == -1);
    assert(calib_corner_index("") == -1);
    assert(strcmp(calib_corner_word(POS_RL), "rear_left") == 0);
    assert(calib_corner_word(7) == NULL);
    assert(calib_direction_forward("forward") == 1);
    assert(calib_direction_forward("reverse") == 0);
    assert(calib_direction_forward("fwd") == -1);

    motors_config_t cfg = { .deadzone = 0.05f };
    cfg.wheels[POS_FL] = (wheel_calib_t){ .channel_pair = 0, .sign = 1 };
    cfg.wheels[POS_FR] = (wheel_calib_t){ .channel_pair = 1, .sign = 1 };
    cfg.wheels[POS_RL] = (wheel_calib_t){ .channel_pair = 2, .sign = -1 };
    cfg.wheels[POS_RR] = (wheel_calib_t){ .channel_pair = 3, .sign = 1 };
    char buf[320];
    int n = calib_table_json(buf, sizeof(buf), true, &cfg);
    assert(n > 0 && n == (int)strlen(buf));
    assert(strcmp(buf,
        "\"calibrated\":true,\"wheels\":["
        "{\"corner\":\"front_left\",\"pair\":0,\"inverted\":false},"
        "{\"corner\":\"front_right\",\"pair\":1,\"inverted\":false},"
        "{\"corner\":\"rear_left\",\"pair\":2,\"inverted\":true},"
        "{\"corner\":\"rear_right\",\"pair\":3,\"inverted\":false}]") == 0);
    n = calib_table_json(buf, sizeof(buf), false, &cfg);
    assert(n > 0 && strcmp(buf, "\"calibrated\":false,\"wheels\":[]") == 0);
    assert(calib_table_json(buf, 40, true, &cfg) == -1);
    printf("test_calib_wire: all passed\n");
    return 0;
}
```

Makefile: добавить `test_calib_wire` в `all`/`run`/`clean` с правилом `test_calib_wire: test_calib_wire.c ../main/calib_wire.h ../main/cfg_table.inc` → `$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)`. Сверить имена членов `wheel_calib_t` в `motors.h` (`channel_pair`, `sign`) — тест использует их так же, как `calibration.h`.

- [ ] **Шаг 2: Собрать и увидеть красное**

Команда: `make -C firmware/car/core/test test_calib_wire 2>&1 | tail -2`
Ожидается: `calib_wire.h: No such file`.

- [ ] **Шаг 3: `calib_wire.h` (новый, чистый)**

```c
#ifndef CALIB_WIRE_H
#define CALIB_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "contract.h"
#include "motors.h"

/* The wire's words for the calibration table, mapped to motors.h's positions. The table
 * is stored by position (calibration.c) and spoken by corner name; this is the edge. */

static inline const char *calib_corner_word(int pos) {
    switch (pos) {
        case POS_FL: return CORNER_FRONT_LEFT;
        case POS_FR: return CORNER_FRONT_RIGHT;
        case POS_RL: return CORNER_REAR_LEFT;
        case POS_RR: return CORNER_REAR_RIGHT;
        default:     return NULL;
    }
}

static inline int calib_corner_index(const char *word) {
    for (int p = 0; p < POS_COUNT; p++) {
        if (strcmp(calib_corner_word(p), word) == 0) return p;
    }
    return -1;
}

/* 1 forward, 0 reverse, -1 for a word the wire does not use. */
static inline int calib_direction_forward(const char *word) {
    if (strcmp(word, DIRECTION_FORWARD) == 0) return 1;
    if (strcmp(word, DIRECTION_REVERSE) == 0) return 0;
    return -1;
}

/* "calibrated":…,"wheels":[…] — the GET /calibration members, and the POST's reply. An
 * uncalibrated car answers an empty table rather than the defaults it is not using. */
static inline int calib_table_json(char *buf, size_t n, bool calibrated, const motors_config_t *cfg) {
    int r = snprintf(buf, n, "\"" KEY_CALIB_CALIBRATED "\":%s,\"" KEY_CALIB_WHEELS "\":[",
                     calibrated ? "true" : "false");
    if (r < 0 || r >= (int)n) return -1;
    size_t at = (size_t)r;
    if (calibrated) {
        for (int p = 0; p < POS_COUNT; p++) {
            int w = snprintf(buf + at, n - at,
                             "%s{\"" KEY_CALIB_CORNER "\":\"%s\",\"" KEY_CALIB_PAIR "\":%u,"
                             "\"" KEY_CALIB_INVERTED "\":%s}",
                             p ? "," : "", calib_corner_word(p),
                             (unsigned)cfg->wheels[p].channel_pair,
                             cfg->wheels[p].sign < 0 ? "true" : "false");
            if (w < 0 || (size_t)w >= n - at) return -1;
            at += (size_t)w;
        }
    }
    if (at + 2 > n) return -1;
    buf[at++] = ']';
    buf[at] = '\0';
    return (int)at;
}

#endif /* CALIB_WIRE_H */
```

- [ ] **Шаг 4: `car.h` / `car.c`**

Добавить `void car_get_calibration(motors_config_t *out);` — «копия опубликованной таблицы; чтение — обмен указателя, так что это одна загрузка и один memcpy» — рядом с `car_set_calibration`, копируя из того же опубликованного указателя, который читает `car_drive`.

- [ ] **Шаг 5: Переписать `calib_api.c`**

```c
#include "calib_api.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "http_server.h"
#include "calibration.h"
#include "calib_wire.h"
#include "car.h"
#include "link.h"
#include "motors.h"
#include "api_util.h"
#include "contract.h"

static const char *TAG = "calib_api";

static esp_err_t reply_table(httpd_req_t *req) {
    motors_config_t cfg;
    car_get_calibration(&cfg);
    char members[320];
    if (calib_table_json(members, sizeof(members), calibration_is_valid(), &cfg) < 0) {
        return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "table too long");
    }
    return api_reply_json(req, members);
}

// GET /calibration -> {"proto":2,"calibrated":…,"wheels":[…]}
static esp_err_t calib_get(httpd_req_t *req) { return reply_table(req); }

// POST /calibration/spin  {"pair":0..3,"direction":"forward"|"reverse"}. Pulses ~0.6 s.
static esp_err_t calib_spin(httpd_req_t *req) {
    char b[96];
    if (api_read_body(req, b, sizeof(b)) < 0) {
        return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "body missing or too long");
    }
    cJSON *j = cJSON_Parse(b);
    if (!j) return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "malformed JSON");
    /* Unknown keys are refused: this is a two-party API where a typo is a bug. */
    for (const cJSON *it = j->child; it; it = it->next) {
        if (strcmp(it->string, KEY_CALIB_PAIR) != 0 && strcmp(it->string, KEY_CALIB_DIRECTION) != 0) {
            esp_err_t e = api_reply_error(req, "400 Bad Request", ERR_UNKNOWN_FIELD, it->string, "no such field");
            cJSON_Delete(j);
            return e;
        }
    }
    cJSON *jp = cJSON_GetObjectItemCaseSensitive(j, KEY_CALIB_PAIR);
    cJSON *jd = cJSON_GetObjectItemCaseSensitive(j, KEY_CALIB_DIRECTION);
    if (!jp) { cJSON_Delete(j); return api_reply_error(req, "400 Bad Request", ERR_MISSING_FIELD, KEY_CALIB_PAIR, "required"); }
    if (!jd) { cJSON_Delete(j); return api_reply_error(req, "400 Bad Request", ERR_MISSING_FIELD, KEY_CALIB_DIRECTION, "required"); }
    if (!cJSON_IsNumber(jp) || jp->valuedouble != (double)jp->valueint) {
        cJSON_Delete(j);
        return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, KEY_CALIB_PAIR, "expected an integer");
    }
    if (!cJSON_IsString(jd)) {
        cJSON_Delete(j);
        return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, KEY_CALIB_DIRECTION, "expected a word");
    }
    int pair = jp->valueint;
    int fwd = calib_direction_forward(jd->valuestring);
    cJSON_Delete(j);
    if (pair < 0 || pair >= CALIB_PAIRS) {
        return api_reply_error(req, "400 Bad Request", ERR_OUT_OF_RANGE, KEY_CALIB_PAIR, "pair 0..3");
    }
    if (fwd < 0) {
        return api_reply_error(req, "400 Bad Request", ERR_NOT_ALLOWED, KEY_CALIB_DIRECTION, "forward or reverse");
    }
    ESP_LOGI(TAG, "spin pair %d %s", pair, fwd ? "fwd" : "rev");
    if (!car_spin_pair((uint8_t)pair, fwd != 0)) {
        return api_reply_error(req, "409 Conflict", ERR_BUSY, "", "actuator busy");
    }
    vTaskDelay(pdMS_TO_TICKS(LINK_HOLD_CALIB_MS));
    link_release_must(LINK_SRC_CALIB);
    return api_reply_ok(req);
}

// POST /calibration  {"wheels":[{"corner","pair","inverted"} x4]} — any order, each corner once.
static esp_err_t calib_save(httpd_req_t *req) {
    char b[320];
    if (api_read_body(req, b, sizeof(b)) < 0) {
        return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "body missing or too long");
    }
    cJSON *j = cJSON_Parse(b);
    if (!j) return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "malformed JSON");
    for (const cJSON *it = j->child; it; it = it->next) {
        if (strcmp(it->string, KEY_CALIB_WHEELS) != 0) {
            esp_err_t e = api_reply_error(req, "400 Bad Request", ERR_UNKNOWN_FIELD, it->string, "no such field");
            cJSON_Delete(j);
            return e;
        }
    }
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, KEY_CALIB_WHEELS);
    if (!arr) { cJSON_Delete(j); return api_reply_error(req, "400 Bad Request", ERR_MISSING_FIELD, KEY_CALIB_WHEELS, "required"); }
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != POS_COUNT) {
        cJSON_Delete(j);
        return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, KEY_CALIB_WHEELS, "expected four wheels");
    }
    motors_config_t cfg = { .deadzone = 0.05f };
    unsigned seen = 0;
    char where[40];
    for (int i = 0; i < POS_COUNT; i++) {
        cJSON *w = cJSON_GetArrayItem(arr, i);
        cJSON *jc = cJSON_GetObjectItemCaseSensitive(w, KEY_CALIB_CORNER);
        cJSON *jp = cJSON_GetObjectItemCaseSensitive(w, KEY_CALIB_PAIR);
        cJSON *ji = cJSON_GetObjectItemCaseSensitive(w, KEY_CALIB_INVERTED);
        snprintf(where, sizeof(where), "%s[%d]", KEY_CALIB_WHEELS, i);
        for (const cJSON *it = cJSON_IsObject(w) ? w->child : NULL; it; it = it->next) {
            if (strcmp(it->string, KEY_CALIB_CORNER) != 0 && strcmp(it->string, KEY_CALIB_PAIR) != 0 &&
                strcmp(it->string, KEY_CALIB_INVERTED) != 0) {
                esp_err_t e = api_reply_error(req, "400 Bad Request", ERR_UNKNOWN_FIELD, where, "no such field");
                cJSON_Delete(j);
                return e;
            }
        }
        if (!cJSON_IsString(jc) || !cJSON_IsNumber(jp) || !cJSON_IsBool(ji) ||
            jp->valuedouble != (double)jp->valueint) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, where, "wheel needs {corner,pair,inverted}");
        }
        int pos = calib_corner_index(jc->valuestring);
        if (pos < 0) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", ERR_NOT_ALLOWED, where, "unknown corner");
        }
        if (seen & (1u << pos)) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", ERR_NOT_ALLOWED, where, "corner repeated");
        }
        seen |= 1u << pos;
        /* Range-checked BEFORE narrowing: (uint8_t)256 is 0, inside what calibration_valid accepts. */
        if (jp->valueint < 0 || jp->valueint >= CALIB_PAIRS) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", ERR_OUT_OF_RANGE, where, "pair 0..3");
        }
        cfg.wheels[pos].channel_pair = (uint8_t)jp->valueint;
        cfg.wheels[pos].sign = cJSON_IsTrue(ji) ? -1 : 1;
    }
    cJSON_Delete(j);
    esp_err_t e = calibration_save(&cfg);   /* validates: pairs 0..3 each once */
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "save rejected: %s", esp_err_to_name(e));
        return api_reply_error(req, "400 Bad Request", ERR_NOT_ALLOWED, KEY_CALIB_WHEELS, "pairs must be 0..3, each once");
    }
    car_set_calibration(&cfg);
    calibration_set_valid(true);
    ESP_LOGI(TAG, "calibration saved and applied");
    return reply_table(req);
}

esp_err_t calib_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    httpd_uri_t get  = { .uri = PATH_CALIBRATION, .method = HTTP_GET,  .handler = calib_get };
    httpd_uri_t spin = { .uri = PATH_SPIN,        .method = HTTP_POST, .handler = calib_spin };
    httpd_uri_t save = { .uri = PATH_CALIBRATION, .method = HTTP_POST, .handler = calib_save };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get),  TAG, "reg GET " PATH_CALIBRATION);
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &spin), TAG, "reg POST " PATH_SPIN);
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &save), TAG, "reg POST " PATH_CALIBRATION);
    ESP_LOGI(TAG, "calibration endpoints registered");
    return ESP_OK;
}
```

- [ ] **Шаг 6: Собрать и прогнать**

Команда: `make -C firmware/car/core/test test_calib_wire && ./firmware/car/core/test/test_calib_wire`
Ожидается: `test_calib_wire: all passed`.

- [ ] **Шаг 7: Коммит**

```bash
git add firmware/car/core/main/calib_wire.h firmware/car/core/main/calib_api.c firmware/car/core/main/car.h firmware/car/core/main/car.c firmware/car/core/test/test_calib_wire.c firmware/car/core/test/Makefile
git commit -F- <<'MSG'
car(calibration): corners by name, inverted instead of sign, the table on GET

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 13: Коды `/ota`, маршруты, весь хост-набор машинки зелёный, `idf.py build`

**Файлы:**
- Изменить: `firmware/car/core/main/ota_api.c` (every `api_reply_error` call), `firmware/car/core/main/http_server.c` (`max_uri_handlers`, the `/` route), `firmware/car/core/main/main.c` (log text only)

- [ ] **Шаг 1: `ota_api.c`**

Каждый `api_reply_error(req, status, "", msg)` получает код третьим аргументом: `"actuator busy"` → `ERR_BUSY` со статусом `409 Conflict` (запрос в порядке, занят актуатор — тот же код, что у `/calibration/spin`); `"image too small"` → `ERR_TOO_SMALL`; оба `"image too large"` → `ERR_NOT_FIRMWARE` с сообщением `"image larger than the slot"`; `"no ota partition"`, `"ota begin failed"`, `"ota write failed"`, `"set boot failed"` → `ERR_WRITE_FAILED`; `"recv error"` → `ERR_INTERNAL` с сообщением `"upload stalled"`; `"image invalid"` → `ERR_NOT_FIRMWARE`. Зарегистрировать на `PATH_OTA`. Добавить `#include "contract.h"`.

- [ ] **Шаг 2: `http_server.c`**

Маршрутов теперь 8: `/`, `GET+POST /config`, `GET+POST /calibration`, `POST /calibration/spin`, `GET /status`, `POST /ota`; поставить `config.max_uri_handlers = 12;` и обновить комментарий. Корень регистрировать на `PATH_ROOT`. Тело 404 для неизвестного пути — своё у IDF, не трогать.

- [ ] **Шаг 3: `main.c`**

Строка лога `"config endpoints are down, all five domains"` → `"the /config endpoint is down"`; `"calibration endpoint"` остаётся.

- [ ] **Шаг 4: Весь хост-набор машинки**

Команда: `make -C firmware/car/core/test clean && make -C firmware/car/core/test run 2>&1 | tail -20`
Ожидается: каждый тест печатает свою зелёную строку; `test_rt_glue`, `test_recovery`, `test_watchdog`, `test_mixer`, `test_motors`, `test_ramp`, `test_trim`, `test_wheel`, `test_calibration`, `test_radio_ota` не тронуты и зелёные.

- [ ] **Шаг 5: `idf.py build`**

Команда:
```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car && source tools/env-p4.sh && cd firmware/car/core && idf.py reconfigure > /dev/null && idf.py build 2>&1 | tail -5
```
Ожидается: `Project build complete`. Ошибки компиляции в IDF-файлах (`status_api.c`, `rt_link.c`, `cfg_api.c`, `calib_api.c`, `ota_api.c`, `api_util.c`, `car.c`) чинить здесь — хост-тесты их не видят.

- [ ] **Шаг 6: Коммит**

```bash
git add firmware/car/core/main/ota_api.c firmware/car/core/main/http_server.c firmware/car/core/main/main.c
git commit -F- <<'MSG'
car: v2 error codes on /ota, the route table, and a green build

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```
## Фаза C — прошивка адаптера

Каждая задача гоняет `make -C firmware/dongle/test run` (хост, обычный `cc`). Фаза начинается с несобирающимся тестовым набором (переименованные макросы `DONGLE_STATE_*`, исчезнувшие `DONGLE_KEY_*`/`DONGLE_NETKEY_*`); Задача 18 заканчивается зелёным набором и зелёным `idf.py build`.

### Задача 14: `net_cfg` — коды ошибок и тело ответа `/wifi`; переименование `DONGLE_STATE_*`

**Файлы:**
- Изменить: `firmware/dongle/main/net_cfg.h`, `net_cfg.c`, `wifi_state.c`, `wifi_sta.c`, `screens.c`, `screens.h`
- Тесты: `firmware/dongle/test/test_net_cfg.c`, `test_wifi_state.c`, `test_screens.c`

**Интерфейсы:**
- Даёт: `const char *net_cfg_err_code(net_cfg_err_t e)` — `DONGLE_ERR_BAD_LENGTH` для `*_LEN`, `DONGLE_ERR_BAD_CHARS` для `*_BYTE`, `""` для `NET_CFG_OK`; `int net_cfg_render_wifi_reply(const net_cfg_t *cfg, const char *state, char *buf, size_t n)` — всё тело `{"proto":1,"ssid":"…","state":"…"}` (SSID экранируется, пароля нет никогда). `net_cfg_render_public` удаляется. Во всём коде адаптера `DONGLE_STATE_<X>` → `DONGLE_WIFI_STATE_<X>`.

- [ ] **Шаг 1: Механическое переименование состояний**

```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car/firmware/dongle
sed -i '' 's/DONGLE_STATE_/DONGLE_WIFI_STATE_/g' main/wifi_state.c main/wifi_sta.c main/screens.c main/screens.h test/test_wifi_state.c test/test_screens.c
grep -rn "DONGLE_STATE_" main test | grep -v dongle_contract.inc   # ожидается: пусто
```

- [ ] **Шаг 2: Тесты в `test_net_cfg.c`**

Заменить пять тестов рендера (`test_public_render_never_leaks_the_password`, `test_public_render_when_unconfigured`, `test_public_render_boundary_is_exact`, `test_validated_values_always_fit_the_public_worst_case`, `test_render_escapes_a_quote_in_the_ssid`, `test_render_escapes_a_backslash_in_the_ssid`, `test_render_still_escapes_a_legacy_control_byte`) на их двойники для `net_cfg_render_wifi_reply` и добавить тест кодов. В `main()` вызвать новые имена вместо старых.

```c
static void test_errors_name_their_code(void) {
    net_cfg_t c;
    assert(strcmp(net_cfg_err_code(net_cfg_validate("", "drive1234", &c)), DONGLE_ERR_BAD_LENGTH) == 0);
    assert(strcmp(net_cfg_err_code(net_cfg_validate("AJMiddleCar", "short", &c)), DONGLE_ERR_BAD_LENGTH) == 0);
    assert(strcmp(net_cfg_err_code(net_cfg_validate("AJ\tCar", "drive1234", &c)), DONGLE_ERR_BAD_CHARS) == 0);
    assert(strcmp(net_cfg_err_code(net_cfg_validate("AJMiddleCar", "dr\x7fve1234", &c)), DONGLE_ERR_BAD_CHARS) == 0);
    assert(strcmp(net_cfg_err_code(NET_CFG_OK), "") == 0);
}

static void test_wifi_reply_never_leaks_the_password(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);
    char buf[160];
    int n = net_cfg_render_wifi_reply(&c, DONGLE_WIFI_STATE_SEARCHING, buf, sizeof(buf));
    assert(n > 0 && n == (int)strlen(buf));
    assert(strcmp(buf, "{\"proto\":1,\"ssid\":\"AJMiddleCar\",\"state\":\"searching\"}") == 0);
    assert(strstr(buf, "drive1234") == NULL);
}

static void test_wifi_reply_boundary_is_exact(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);
    char scratch[160];
    int len = net_cfg_render_wifi_reply(&c, DONGLE_WIFI_STATE_CONNECTED, scratch, sizeof(scratch));
    assert(len > 0);
    assert(net_cfg_render_wifi_reply(&c, DONGLE_WIFI_STATE_CONNECTED, scratch, (size_t)len) == -1);
    assert(net_cfg_render_wifi_reply(&c, DONGLE_WIFI_STATE_CONNECTED, scratch, (size_t)len + 1) == len);
}

static void test_wifi_reply_escapes_a_quote_and_a_backslash(void) {
    net_cfg_t c;
    assert(net_cfg_validate("Say \"hi\"\\", "drive1234", &c) == NET_CFG_OK);
    char buf[200];
    assert(net_cfg_render_wifi_reply(&c, DONGLE_WIFI_STATE_IDLE, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "\"ssid\":\"Say \\\"hi\\\"\\\\\"") != NULL);
}

static void test_validated_values_always_fit_the_reply_worst_case(void) {
    /* Whatever net_cfg_validate accepts must render: 32 bytes of '"' double to 64. */
    char ssid[NET_SSID_MAX + 1];
    memset(ssid, '"', NET_SSID_MAX);
    ssid[NET_SSID_MAX] = '\0';
    net_cfg_t c;
    assert(net_cfg_validate(ssid, "", &c) == NET_CFG_OK);
    char buf[128];   /* 11 + 64 + 2 + 22 (state key + longest word "searching") + NUL, with margin */
    assert(net_cfg_render_wifi_reply(&c, DONGLE_WIFI_STATE_SEARCHING, buf, sizeof(buf)) > 0);
}
```

Тесты `test_escape_*` и `test_equal_*` остаются как есть.

- [ ] **Шаг 3: Запустить и увидеть красное**

Команда: `make -C firmware/dongle/test test_net_cfg 2>&1 | tail -3`
Ожидается: ошибка компиляции — `net_cfg_err_code`/`net_cfg_render_wifi_reply` не объявлены.

- [ ] **Шаг 4: `net_cfg.h` / `net_cfg.c`**

В заголовке: убрать объявление `net_cfg_render_public` (и абзац про него), добавить:

```c
/* The contract's word for a rejection — what the app switches on. "" for NET_CFG_OK. */
const char *net_cfg_err_code(net_cfg_err_t e);

/* The POST /wifi reply: {"proto":1,"ssid":"…","state":"…"}, the network as now held and
 * what the radio is doing about it. NEVER contains the password: the app holds that
 * value itself and has no use for reading it back. The SSID is JSON-escaped (a real
 * network can be named with a quote); `state` is one of DONGLE_WIFI_STATE_* and is
 * written as is. Returns the length written, or -1 if buf is too small. */
int net_cfg_render_wifi_reply(const net_cfg_t *cfg, const char *state, char *buf, size_t n);
```

В `net_cfg.c` рядом с `net_cfg_err_field`:

```c
const char *net_cfg_err_code(net_cfg_err_t e)
{
    switch (e) {
    case NET_CFG_SSID_LEN:
    case NET_CFG_PASS_LEN:  return DONGLE_ERR_BAD_LENGTH;
    case NET_CFG_SSID_BYTE:
    case NET_CFG_PASS_BYTE: return DONGLE_ERR_BAD_CHARS;
    case NET_CFG_OK:        break;
    }
    return "";
}
```

и вместо `net_cfg_render_public`:

```c
int net_cfg_render_wifi_reply(const net_cfg_t *cfg, const char *state, char *buf, size_t n)
{
    size_t pos = 0;
    char head[40];
    snprintf(head, sizeof(head), "{\"" DONGLE_KEY_PROTO "\":%d,\"" DONGLE_KEY_WIFI_SSID "\":\"", DONGLE_PROTO);
    if (!append_str(buf, n, &pos, head)) return -1;
    if (!append_escaped(buf, n, &pos, cfg->ssid)) return -1;
    if (!append_str(buf, n, &pos, "\",\"" DONGLE_KEY_WIFI_STATE "\":\"")) return -1;
    if (!append_str(buf, n, &pos, state)) return -1;
    if (!append_str(buf, n, &pos, "\"}")) return -1;
    buf[pos] = '\0';
    return (int)pos;
}
```
(`append_str`/`append_escaped` уже есть в файле; проверить, что они оставляют байт под NUL — граничный тест это и пинит.)

- [ ] **Шаг 5: Собрать и прогнать три теста**

Команда: `make -C firmware/dongle/test test_net_cfg test_wifi_state test_screens && (cd firmware/dongle/test && ./test_net_cfg && ./test_wifi_state && ./test_screens)`
Ожидается: все три зелёные.

- [ ] **Шаг 6: Коммит**

```bash
git add firmware/dongle/main/net_cfg.h firmware/dongle/main/net_cfg.c firmware/dongle/main/wifi_state.c firmware/dongle/main/wifi_sta.c firmware/dongle/main/screens.c firmware/dongle/main/screens.h firmware/dongle/test/test_net_cfg.c firmware/dongle/test/test_wifi_state.c firmware/dongle/test/test_screens.c
git commit -F- <<'MSG'
dongle(net_cfg): error codes, the /wifi reply body, wifi-state macros by their group

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 15: Конверт ошибок адаптера и коды `/ota`

**Файлы:**
- Изменить: `firmware/dongle/main/api_util.h`, `api_util.c`, `ota_api.c`

**Интерфейсы:**
- Даёт: `esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *code, const char *field, const char *msg)` — `{"proto":1,"error":{"code","message"[,"field"]}}`; `api_reply_ok` — `{"proto":1,"ok":true}`; `esp_err_t api_reply_json(httpd_req_t *req, const char *members)` — `{"proto":1,<members>}`.

- [ ] **Шаг 1: `api_util.h` / `api_util.c`**

Тот же код, что у машинки в Задаче 10, с двумя отличиями: `#include "dongle_contract.inc"` вместо `contract.h`, и макросы с префиксом `DONGLE_` (`DONGLE_KEY_PROTO`, `DONGLE_KEY_ERROR`, `DONGLE_KEY_ERROR_CODE`, `DONGLE_KEY_ERROR_MESSAGE`, `DONGLE_KEY_ERROR_FIELD`, `DONGLE_KEY_OK`, `DONGLE_PROTO`). Буфер `api_reply_json` — 640 байт (статус адаптера в худшем случае ≈ 520). В шапке заголовка оставить абзац о том, что это намеренный двойник файла машинки.

- [ ] **Шаг 2: `ota_api.c`**

Каждому `api_reply_error(req, status, "", msg)` добавить код третьим аргументом: `"image too small"` → `DONGLE_ERR_TOO_SMALL`; оба `"image too large"` → `DONGLE_ERR_NOT_FIRMWARE` (сообщение `"image larger than the slot"`); `"no ota partition"`, `"ota begin failed"`, `"ota write failed"`, `"set boot failed"` → `DONGLE_ERR_WRITE_FAILED`; `"image still pending verify"` (409) → `DONGLE_ERR_BUSY`; `"recv error"` → `DONGLE_ERR_INTERNAL` (сообщение `"upload stalled"`); `"image invalid"` → `DONGLE_ERR_NOT_FIRMWARE`. Маршрут остаётся `DONGLE_PATH_OTA`.

- [ ] **Шаг 3: Коммит (компиляцию докажет Задача 18)**

```bash
git add firmware/dongle/main/api_util.h firmware/dongle/main/api_util.c firmware/dongle/main/ota_api.c
git commit -F- <<'MSG'
dongle(http): the v2 envelope on every reply, codes on /ota

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 16: `/status` адаптера в группах — чистый рендер `status_json`

**Файлы:**
- Создать: `firmware/dongle/main/status_json.h`, `firmware/dongle/main/status_json.c`, `firmware/dongle/test/test_status_json.c`
- Изменить: `firmware/dongle/main/status_api.c` (`status_get`), `firmware/dongle/main/CMakeLists.txt` (добавить `status_json.c` в `SRCS`), `firmware/dongle/test/Makefile`

**Интерфейсы:**
- Даёт: `status_view_t` (ниже) и `int status_json_render(const status_view_t *v, char *buf, size_t n)` — всё тело `/status` с `proto` первым; `-1`, если не влезло.

- [ ] **Шаг 1: Тест `test_status_json.c`**

```c
#include "../main/status_json.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static status_view_t sample(void) {
    status_view_t v = {
        .fw = "v1.0+789", .idf = "v6.0.2", .rolled_back = false,
        .usb_state = DONGLE_USB_STATE_UP,
        .ssid = "AJMiddleCar", .configured = true, .wifi_state = DONGLE_WIFI_STATE_CONNECTED,
        .connected = true, .rssi_dbm = -53, .channel = 1,
        .attempts_used = 0, .attempts_max = 5,
        .to_car_x10 = 100, .to_phone_x10 = 50, .udp_sessions = 1, .tcp_connections = 2,
        .last_errno = 118, .last_error_message = "No route to host", .last_error_count = 3,
        .last_error_age_s = 41,
        .uptime_s = 412, .free_heap = 8551152,
    };
    return v;
}

int main(void) {
    char buf[640];
    status_view_t v = sample();
    int n = status_json_render(&v, buf, sizeof(buf));
    assert(n > 0 && n == (int)strlen(buf));
    assert(strcmp(buf,
        "{\"proto\":1,"
        "\"device\":{\"id\":\"ajdongle\",\"fw\":\"v1.0+789\",\"build\":789,\"rolled_back\":false,\"idf\":\"v6.0.2\"},"
        "\"usb\":{\"state\":\"up\"},"
        "\"wifi\":{\"ssid\":\"AJMiddleCar\",\"configured\":true,\"state\":\"connected\","
                  "\"rssi_dbm\":-53,\"channel\":1,\"attempts\":{\"used\":0,\"max\":5}},"
        "\"relay\":{\"to_car_hz\":10.0,\"to_phone_hz\":5.0,\"udp_sessions\":1,\"tcp_connections\":2,"
                   "\"last_error\":{\"errno\":118,\"message\":\"No route to host\",\"count\":3,\"age_s\":41}},"
        "\"system\":{\"uptime_s\":412,\"free_heap\":8551152}}") == 0);

    /* Not connected: the readings that need a link are null, not 0. Never failed: no
       error object at all. Nothing sent: an empty ssid and configured false. */
    v.connected = false; v.wifi_state = DONGLE_WIFI_STATE_IDLE; v.ssid = ""; v.configured = false;
    v.last_errno = 0; v.to_car_x10 = 7; v.to_phone_x10 = 0;
    n = status_json_render(&v, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"ssid\":\"\",\"configured\":false,\"state\":\"idle\",\"rssi_dbm\":null,\"channel\":null"));
    assert(strstr(buf, "\"last_error\":null"));
    assert(strstr(buf, "\"to_car_hz\":0.7,\"to_phone_hz\":0.0"));

    /* A quote in the SSID is escaped, so the document stays one document. */
    v.ssid = "Say \"hi\"";
    n = status_json_render(&v, buf, sizeof(buf));
    assert(n > 0 && strstr(buf, "\"ssid\":\"Say \\\"hi\\\"\""));

    /* rolled_back and a version without a build number */
    v.rolled_back = true; v.fw = "v1.0";
    n = status_json_render(&v, buf, sizeof(buf));
    assert(n > 0 && strstr(buf, "\"build\":-1,\"rolled_back\":true"));

    /* Worst case fits: 32 quote bytes in the SSID, every counter wide. */
    char wide_ssid[33]; memset(wide_ssid, '"', 32); wide_ssid[32] = '\0';
    v = sample(); v.ssid = wide_ssid; v.rssi_dbm = -128; v.channel = 14;
    v.to_car_x10 = 65535; v.to_phone_x10 = 65535; v.udp_sessions = 4; v.tcp_connections = 4;
    v.last_errno = 2147483647; v.last_error_message = "Software caused connection abort";
    v.last_error_count = 4294967295u; v.last_error_age_s = 4294967295u;
    v.uptime_s = 2147483647L; v.free_heap = 4294967295u;
    n = status_json_render(&v, buf, sizeof(buf));
    assert(n > 0 && n < 620);
    printf("test_status_json: widest body is %d bytes\n", n);

    assert(status_json_render(&v, buf, 64) == -1);
    printf("test_status_json: all passed\n");
    return 0;
}
```

Makefile: `test_status_json: test_status_json.c ../main/status_json.c ../main/status_json.h ../main/net_cfg.c ../main/dongle_contract.inc` → `$(CC) $(CFLAGS) -o $@ test_status_json.c ../main/status_json.c ../main/net_cfg.c`; добавить в `all`, `run`, `clean`.

- [ ] **Шаг 2: Собрать и увидеть красное**

Команда: `make -C firmware/dongle/test test_status_json 2>&1 | tail -2`
Ожидается: `status_json.h: No such file`.

- [ ] **Шаг 3: `status_json.h`**

```c
#ifndef STATUS_JSON_H
#define STATUS_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "dongle_contract.inc"

/* Everything /status says, as plain values, so the body can be rendered and tested on the
 * host with no ESP-IDF in sight. status_api.c fills this from the live modules in the
 * read order its own comments prescribe; this file only spells the JSON.
 *
 * Pure: no ESP-IDF, no cJSON. Compiled with plain cc in the test Makefile. */
typedef struct {
    const char *fw;                 /* esp_app_desc_t.version */
    const char *idf;
    bool        rolled_back;
    const char *usb_state;          /* DONGLE_USB_STATE_* */
    const char *ssid;               /* raw; escaped here */
    bool        configured;
    const char *wifi_state;         /* DONGLE_WIFI_STATE_* */
    bool        connected;          /* rssi_dbm and channel are readings only when true */
    int         rssi_dbm;
    unsigned    channel;
    unsigned    attempts_used;
    unsigned    attempts_max;
    unsigned    to_car_x10;         /* relay_stats keeps packets/s x10; the wire says 10.0 */
    unsigned    to_phone_x10;
    unsigned    udp_sessions;
    unsigned    tcp_connections;
    int         last_errno;         /* 0: never failed since boot -> last_error is null */
    const char *last_error_message; /* strerror(last_errno); ASCII, no quotes */
    unsigned    last_error_count;
    unsigned    last_error_age_s;
    long        uptime_s;
    unsigned    free_heap;
} status_view_t;

/* The whole GET /status body, proto first, groups in the contract's order. Returns the
 * length, or -1 when it does not fit — a truncated document parses as something else or
 * nothing, and the caller answers 500 rather than sending what fits. */
int status_json_render(const status_view_t *v, char *buf, size_t n);

#endif /* STATUS_JSON_H */
```

- [ ] **Шаг 4: `status_json.c`**

```c
#include "status_json.h"

#include <stdio.h>
#include <string.h>

#include "net_cfg.h"   /* net_cfg_escape: the one escaper, so /status cannot drift from /wifi */

/* The number after '+' in "v1.0+789", or -1. A deliberate twin of the car's
 * device_json.h helper: the two firmwares do not reference each other. */
static int build_number(const char *fw)
{
    const char *plus = strchr(fw, '+');
    if (plus == NULL || plus[1] < '0' || plus[1] > '9') {
        return -1;
    }
    int n = 0;
    for (const char *p = plus + 1; *p >= '0' && *p <= '9'; p++) {
        if (n > 214748363) {
            return -1;
        }
        n = n * 10 + (*p - '0');
    }
    return n;
}

int status_json_render(const status_view_t *v, char *buf, size_t n)
{
    char ssid_esc[72]; /* 32 SSID bytes, every one a quote, doubles to 64, +NUL */
    if (net_cfg_escape(v->ssid, ssid_esc, sizeof(ssid_esc)) < 0) {
        return -1;
    }
    char rssi[12], channel[12], last_error[128];
    if (v->connected) {
        snprintf(rssi, sizeof(rssi), "%d", v->rssi_dbm);
        snprintf(channel, sizeof(channel), "%u", v->channel);
    } else {
        snprintf(rssi, sizeof(rssi), "null");
        snprintf(channel, sizeof(channel), "null");
    }
    if (v->last_errno != 0) {
        int w = snprintf(last_error, sizeof(last_error),
                         "{\"" DONGLE_KEY_RELAY_LAST_ERROR_ERRNO "\":%d,"
                         "\"" DONGLE_KEY_RELAY_LAST_ERROR_MESSAGE "\":\"%s\","
                         "\"" DONGLE_KEY_RELAY_LAST_ERROR_COUNT "\":%u,"
                         "\"" DONGLE_KEY_RELAY_LAST_ERROR_AGE_S "\":%u}",
                         v->last_errno, v->last_error_message ? v->last_error_message : "",
                         v->last_error_count, v->last_error_age_s);
        if (w < 0 || (size_t)w >= sizeof(last_error)) {
            return -1;
        }
    } else {
        snprintf(last_error, sizeof(last_error), "null");
    }
    int r = snprintf(buf, n,
        "{\"" DONGLE_KEY_PROTO "\":%d,"
        "\"" DONGLE_KEY_GROUP_DEVICE "\":{"
            "\"" DONGLE_KEY_DEVICE_ID "\":\"" DONGLE_DEVICE "\","
            "\"" DONGLE_KEY_DEVICE_FW "\":\"%s\","
            "\"" DONGLE_KEY_DEVICE_BUILD "\":%d,"
            "\"" DONGLE_KEY_DEVICE_ROLLED_BACK "\":%s,"
            "\"" DONGLE_KEY_DEVICE_IDF "\":\"%s\"},"
        "\"" DONGLE_KEY_GROUP_USB "\":{\"" DONGLE_KEY_USB_STATE "\":\"%s\"},"
        "\"" DONGLE_KEY_GROUP_WIFI "\":{"
            "\"" DONGLE_KEY_WIFI_SSID "\":\"%s\","
            "\"" DONGLE_KEY_WIFI_CONFIGURED "\":%s,"
            "\"" DONGLE_KEY_WIFI_STATE "\":\"%s\","
            "\"" DONGLE_KEY_WIFI_RSSI_DBM "\":%s,"
            "\"" DONGLE_KEY_WIFI_CHANNEL "\":%s,"
            "\"" DONGLE_KEY_WIFI_ATTEMPTS "\":{"
                "\"" DONGLE_KEY_WIFI_ATTEMPTS_USED "\":%u,"
                "\"" DONGLE_KEY_WIFI_ATTEMPTS_MAX "\":%u}},"
        "\"" DONGLE_KEY_GROUP_RELAY "\":{"
            "\"" DONGLE_KEY_RELAY_TO_CAR_HZ "\":%u.%u,"
            "\"" DONGLE_KEY_RELAY_TO_PHONE_HZ "\":%u.%u,"
            "\"" DONGLE_KEY_RELAY_UDP_SESSIONS "\":%u,"
            "\"" DONGLE_KEY_RELAY_TCP_CONNECTIONS "\":%u,"
            "\"" DONGLE_KEY_RELAY_LAST_ERROR "\":%s},"
        "\"" DONGLE_KEY_GROUP_SYSTEM "\":{"
            "\"" DONGLE_KEY_SYSTEM_UPTIME_S "\":%ld,"
            "\"" DONGLE_KEY_SYSTEM_FREE_HEAP "\":%u}}",
        DONGLE_PROTO,
        v->fw, build_number(v->fw), v->rolled_back ? "true" : "false", v->idf,
        v->usb_state,
        ssid_esc, v->configured ? "true" : "false", v->wifi_state, rssi, channel,
        v->attempts_used, v->attempts_max,
        v->to_car_x10 / 10u, v->to_car_x10 % 10u, v->to_phone_x10 / 10u, v->to_phone_x10 % 10u,
        v->udp_sessions, v->tcp_connections, last_error,
        v->uptime_s, v->free_heap);
    if (r < 0 || (size_t)r >= n) {
        return -1;
    }
    return r;
}
```

- [ ] **Шаг 5: `status_api.c` — `status_get` через `status_view_t`**

Оставить порядок чтения и комментарии к нему (attempts → state → ap_info; usb; relay), убрать собственный `snprintf`-шаблон и `ssid_esc`, собрать `status_view_t` и вызвать рендер:

```c
static esp_err_t status_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    net_cfg_t cfg;
    bool configured = net_api_current(&cfg);

    /* Read order: attempts BEFORE state, state BEFORE the radio's figures — the reasons
       are the ones the previous version of this function gave at length, and they have
       not changed: a count can only be older than the state framing it, and a 0 RSSI
       after a "connected" is a link that dropped in between, not an impossible pairing. */
    unsigned attempts = (unsigned)wifi_sta_attempts();
    const char *wifi_state = wifi_sta_state_name();
    int8_t ap_rssi;
    uint8_t ap_channel;
    bool connected = wifi_sta_ap_info(&ap_rssi, &ap_channel) && wifi_sta_connected();

    relay_stats_t *relay = relay_stats_shared();
    unsigned errno_age = 0;
    if (relay->last_errno != 0) {
        errno_age = (unsigned)(((uint32_t)(esp_timer_get_time() / 1000) - relay->last_fail_ms) / 1000u);
    }

    status_view_t v = {
        .fw = app->version, .idf = app->idf_ver, .rolled_back = s_rollback,
        .usb_state = usb_net_host_attached() ? DONGLE_USB_STATE_UP : DONGLE_USB_STATE_DOWN,
        .ssid = configured ? cfg.ssid : "", .configured = configured,
        .wifi_state = wifi_state, .connected = connected,
        .rssi_dbm = (int)ap_rssi, .channel = (unsigned)ap_channel,
        .attempts_used = attempts, .attempts_max = (unsigned)WIFI_JOIN_ATTEMPTS,
        .to_car_x10 = (unsigned)relay->to_car_x10, .to_phone_x10 = (unsigned)relay->to_phone_x10,
        .udp_sessions = (unsigned)relay->udp_used, .tcp_connections = (unsigned)relay->tcp_used,
        .last_errno = relay->last_errno, .last_error_message = strerror(relay->last_errno),
        .last_error_count = (unsigned)relay->errno_count, .last_error_age_s = errno_age,
        .uptime_s = (long)(esp_timer_get_time() / 1000000),
        .free_heap = (unsigned)esp_get_free_heap_size(),
    };
    char body[640];
    int n = status_json_render(&v, body, sizeof(body));
    if (n < 0) {
        ESP_LOGE(TAG, "/status does not fit its buffer");
        return api_reply_error(req, "500 Internal Server Error", DONGLE_ERR_INTERNAL, "", "status too long");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}
```
Добавить `#include <string.h>` (strerror), `#include "status_json.h"`; убрать `#include "net_cfg.h"`, если больше не нужен. Проверить сигнатуру `wifi_sta_ap_info` — по заголовку она возвращает `bool` (успех чтения); если `false`, считать `connected = false`. Обновить комментарий к `cfg.max_uri_handlers` (`GET /status`, `POST /wifi`, `POST /ota` — три).

- [ ] **Шаг 6: `CMakeLists.txt`**

Добавить `status_json.c` в список `SRCS` компонента `main`.

- [ ] **Шаг 7: Собрать и прогнать**

Команда: `make -C firmware/dongle/test test_status_json && ./firmware/dongle/test/test_status_json`
Ожидается: `test_status_json: all passed` и строка с шириной тела (< 560).

- [ ] **Шаг 8: Коммит**

```bash
git add firmware/dongle/main/status_json.h firmware/dongle/main/status_json.c firmware/dongle/main/status_api.c firmware/dongle/main/CMakeLists.txt firmware/dongle/test/test_status_json.c firmware/dongle/test/Makefile
git commit -F- <<'MSG'
dongle(status): five groups, null for what is not measured, last_error as an object

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 17: `POST /wifi` отвечает состоянием; `GET /net` исчезает

**Файлы:**
- Изменить: `firmware/dongle/main/net_api.c`, `firmware/dongle/main/net_api.h` (комментарии), `firmware/dongle/main/main.c` (комментарий, если упоминает `/net`)

**Интерфейсы:**
- Даёт: единственный маршрут `POST DONGLE_PATH_WIFI`; тело запроса `{"ssid","password"}` (ключи `DONGLE_WIFI_REQ_*`), любой другой ключ — `unknown_field`; ответ — `net_cfg_render_wifi_reply(&s_cfg, wifi_sta_state_name(), …)`.

- [ ] **Шаг 1: `net_api.c`**

Удалить `net_get` и его регистрацию. В `net_post`:

```c
static esp_err_t reply_state(httpd_req_t *req)
{
    char body[160];
    int n = net_cfg_render_wifi_reply(&s_cfg, wifi_sta_state_name(), body, sizeof(body));
    if (n < 0) {
        ESP_LOGE(TAG, "POST /wifi reply does not fit its buffer");
        return api_reply_error(req, "500 Internal Server Error", DONGLE_ERR_INTERNAL, "", "reply too long");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}
```

- `api_read_body` неудача → `api_reply_error(req, "400 Bad Request", DONGLE_ERR_BAD_JSON, "", "body missing or too long")`;
- ` ` → `DONGLE_ERR_BAD_CHARS`, поле `""`;
- `cJSON_Parse == NULL` → `DONGLE_ERR_BAD_JSON`;
- не объект → `DONGLE_ERR_BAD_JSON`, `"expected a JSON object"`;
- неизвестный ключ (перебор `root->child`, сравнение с `DONGLE_WIFI_REQ_SSID`/`DONGLE_WIFI_REQ_PASSWORD`) → `DONGLE_ERR_UNKNOWN_FIELD`, поле — имя ключа;
- отсутствует `ssid`/`password` → `DONGLE_ERR_MISSING_FIELD` с именем поля; не строка → `DONGLE_ERR_WRONG_TYPE` с именем поля;
- `net_cfg_validate` ≠ OK → `api_reply_error(req, "400 Bad Request", net_cfg_err_code(verr), net_cfg_err_field(verr), net_cfg_err_msg(verr))`;
- без изменений и радио работает (`connected || trying`) → `reply_state(req)` (200, без вызова радио);
- без изменений после `failed`/`idle` → `wifi_sta_join`; ошибка → `DONGLE_ERR_RADIO_REFUSED` (500), иначе `reply_state`;
- новая сеть → запомнить, `wifi_sta_join`; ошибка → `DONGLE_ERR_RADIO_REFUSED`; иначе `reply_state`.

Регистрация: только `POST DONGLE_PATH_WIFI`. Большой комментарий о семантике «неизменённый POST не перезапускает работающее радио» сохранить — он остаётся верным.

- [ ] **Шаг 2: Коммит**

```bash
git add firmware/dongle/main/net_api.c firmware/dongle/main/net_api.h firmware/dongle/main/main.c
git commit -F- <<'MSG'
dongle(wifi): POST /wifi answers with the network and the radio's state; GET /net is gone

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 18: Весь хост-набор адаптера и `idf.py build`

- [ ] **Шаг 1: Хост-тесты**

Команда: `make -C firmware/dongle/test clean && make -C firmware/dongle/test run 2>&1 | tail -12`
Ожидается: семь зелёных строк (`test_net_cfg`, `test_wifi_state`, `test_udp_sess`, `test_tcp_pending`, `test_relay_stats`, `test_screens`, `test_status_json`).

- [ ] **Шаг 2: Сборка**

Команда:
```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car && source tools/env-p4.sh && cd firmware/dongle && idf.py reconfigure > /dev/null && idf.py build 2>&1 | tail -5
```
Ожидается: `Project build complete`. Ошибки компиляции в IDF-файлах (`status_api.c`, `net_api.c`, `ota_api.c`, `api_util.c`) чинить здесь.

- [ ] **Шаг 3: Коммит (если что-то правилось)**

```bash
git add -A firmware/dongle
git commit -F- <<'MSG'
dongle: a green build on wire format v2

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```
## Фаза D — приложение

Каждая задача гоняет свои хост-тесты той же строкой `swiftc`, что и `tools/test-all.sh` (см. «Порядок сборки»). Фаза начинается с несобирающимися хост-тестами (переименованные генерируемые символы); Задача 24 заканчивается зелёными `app/tests/*` и двумя зелёными `xcodebuild`.

### Задача 19: Типизированные датаграммы в `RTFrame` и `SessionPolicy`

**Файлы:**
- Изменить: `app/AJMiddleCar/RTFrame.swift` (целиком), `app/AJMiddleCar/SessionPolicy.swift` (`HandshakeOutcome`)
- Тесты: `app/tests/rtframe/main.swift` (целиком), `app/tests/sessionpolicy/main.swift` (целиком)

**Интерфейсы:**
- Даёт: `RTFrame.hello(sid:)`, `RTFrame.command(seq:throttle:turn:)`, `RTFrame.bye(seq:)`, `RTFrame.parse(_:) -> Inbound?` с `Inbound.helloReply(sid: String, device: DeviceInfo)`, `.protoMismatch(sid:theirs:)`, `.telemetry(Telemetry)`; `RTFrame.nextSeq`, `seqNewer`, `sessionID` без изменений. Ручная структура `Telemetry` удаляется — её место занимает сгенерированная. `SessionPolicy.HandshakeOutcome.identity(DeviceInfo)`.

- [ ] **Шаг 1: Тесты**

`app/tests/rtframe/main.swift`:

```swift
// Host test for the real-time wire. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// The three datagrams the app sends, exactly as the car parses them.
check(RTFrame.hello(sid: "7f3a91c2") == #"{"proto":2,"type":"hello","session":"7f3a91c2"}"#, "hello")
check(RTFrame.command(seq: 1234, throttle: 0.5, turn: -0.25)
        == #"{"proto":2,"type":"drive","seq":1234,"throttle":0.50,"turn":-0.25}"#, "drive")
check(RTFrame.command(seq: 0, throttle: 2, turn: -2)
        == #"{"proto":2,"type":"drive","seq":0,"throttle":1.00,"turn":-1.00}"#, "drive clamps")
check(RTFrame.command(seq: 1, throttle: .nan, turn: .infinity)
        == #"{"proto":2,"type":"drive","seq":1,"throttle":0.00,"turn":0.00}"#, "non-finite is a stop")
check(RTFrame.bye(seq: 1235) == #"{"proto":2,"type":"bye","seq":1235}"#, "bye")

// The widest drive the app can emit stays under the car's command cap.
check(RTFrame.command(seq: .max, throttle: -1, turn: -1).utf8.count <= CarContract.maxCommand,
      "the widest drive fits max_command")

// Every app->car datagram except hello carries seq — a goodbye included.
for seq in [UInt32(0), 1, 1235, .max] {
    check(RTFrame.bye(seq: seq).contains("\"\(CarContract.seqField)\":\(seq)}"), "bye carries its seq")
    check(RTFrame.command(seq: seq, throttle: 0, turn: 0).contains("\"\(CarContract.seqField)\":\(seq),"),
          "drive carries its seq")
}
check(!RTFrame.hello(sid: "7f3a91c2").contains(CarContract.seqField), "hello carries no seq")
var byeSeq = UInt32(7)
var seen: [UInt32] = []
for _ in 0..<3 { byeSeq = RTFrame.nextSeq(byeSeq); seen.append(byeSeq) }
check(seen == [8, 9, 10], "repeated goodbyes advance the sequence")

// Decimal point, two decimals, whatever the phone's locale.
check(RTFrame.command(seq: 1, throttle: 0.5, turn: 0.25).contains(#""throttle":0.50"#), "decimal point")
check(RTFrame.command(seq: 1, throttle: 0.5, turn: 0.25).contains(#""turn":0.25"#), "two decimals")

// -- inbound ----------------------------------------------------------------------
let ack = #"{"proto":2,"type":"hello_ack","session":"7f3a91c2","device":{"id":"ajmiddlecar","fw":"v1.0+784","build":784,"rolled_back":false}}"#
if case .helloReply(let sid, let device)? = RTFrame.parse(ack) {
    check(sid == "7f3a91c2", "ack sid")
    check(device == DeviceInfo(id: "ajmiddlecar", fw: "v1.0+784", build: 784, rolled_back: false), "ack device")
} else { check(false, "hello_ack parses") }

check(RTFrame.parse(#"{"proto":3,"type":"hello_ack","session":"7f3a91c2","device":{"id":"ajmiddlecar","fw":"v9","build":9,"rolled_back":false}}"#)
        == .protoMismatch(sid: "7f3a91c2", theirs: 3), "a foreign proto is reported by name")
check(RTFrame.parse(#"{"type":"hello_ack","session":"7f3a91c2"}"#) == .protoMismatch(sid: "7f3a91c2", theirs: 0),
      "no proto reads as proto 0")
check(RTFrame.parse(#"{"proto":2,"type":"hello_ack","session":"7f3a91c2"}"#) == nil,
      "an ack without its device group is not an identity")
// The v1 reply, as an old car would send it: not an ack at all — it has no type.
check(RTFrame.parse(#"{"proto":1,"hello":"7f3a91c2","device":"ajmiddlecar","fw":"v1.0+784"}"#) == nil,
      "a v1 reply is ignored")

let tele = #"{"proto":2,"type":"telemetry","seq":88,"link":{"rx_hz":10,"rssi_dbm":-58,"timeouts":0},"motors":{"bus":"ok","calibrated":true,"owner":"remote"},"system":{"uptime_s":812,"free_heap":200000}}"#
if case .telemetry(let t)? = RTFrame.parse(tele) {
    check(t.seq == 88 && t.link.rx_hz == 10 && t.link.rssi_dbm == -58 && t.link.timeouts == 0, "telemetry link")
    check(t.motors.bus == .ok && t.motors.calibrated && t.motors.owner == .remote, "telemetry motors")
    check(t.system.uptime_s == 812 && t.system.free_heap == 200000, "telemetry system")
} else { check(false, "telemetry parses") }
if case .telemetry(let t)? = RTFrame.parse(#"{"proto":2,"type":"telemetry","seq":1,"link":{"rx_hz":0,"rssi_dbm":null,"timeouts":3},"motors":{"bus":"down","calibrated":false,"owner":"hover"},"system":{"uptime_s":1,"free_heap":1}}"#) {
    check(t.link.rssi_dbm == nil, "null rssi is nil")
    check(t.motors.bus == .down, "bus down")
    check(t.motors.owner == .unknown("hover"), "an owner word this build does not know is kept, not dropped")
} else { check(false, "telemetry with nulls parses") }
check(RTFrame.parse(#"{"proto":1,"type":"telemetry","seq":1,"link":{"rx_hz":0,"rssi_dbm":null,"timeouts":0},"motors":{"bus":"ok","calibrated":true,"owner":"idle"},"system":{"uptime_s":1,"free_heap":1}}"#) == nil,
      "telemetry in a foreign proto is dropped")
check(RTFrame.parse(#"{"proto":2,"type":"telemetry","seq":1}"#) == nil, "telemetry without its groups is dropped")
check(RTFrame.parse(#"{"proto":2,"type":"drive","seq":1,"throttle":0,"turn":0}"#) == nil, "our own datagram types are not inbound")
check(RTFrame.parse("not json") == nil, "junk")

// The car's telemetry counter wraps like ours.
check(RTFrame.seqNewer(1, than: 0) && !RTFrame.seqNewer(0, than: 1) && RTFrame.seqNewer(0, than: Int(UInt32.max)),
      "telemetry seq ordering wraps")
check(RTFrame.sessionID(0xdeadbeef) == "deadbeef" && RTFrame.sessionID(1) == "00000001", "session id is 8 hex chars")

if failures == 0 { print("test_rtframe: OK") } else { exit(1) }
```

`app/tests/sessionpolicy/main.swift` — заменить блок рукопожатия (до `// -- backoff`):

```swift
// -- handshake filtering: a reply for another sid is a leftover from a previous socket. ----
let sid = "7f3a91c2"
let me = DeviceInfo(id: "ajmiddlecar", fw: "v1.0+517", build: 517, rolled_back: false)
func ack(_ sid: String, proto: Int = 2) -> String {
    #"{"proto":\#(proto),"type":"hello_ack","session":"\#(sid)","device":{"id":"ajmiddlecar","fw":"v1.0+517","build":517,"rolled_back":false}}"#
}
check(SessionPolicy.handshakeOutcome(RTFrame.parse(ack(sid)), sid: sid) == .identity(me),
      "our sid's reply is the identity")
check(SessionPolicy.handshakeOutcome(RTFrame.parse(ack("deadbeef")), sid: sid) == .ignore,
      "another sid's reply is ignored — ownership is not resumable")
check(SessionPolicy.handshakeOutcome(RTFrame.parse(ack(sid, proto: 3)), sid: sid) == .protoMismatch(theirs: 3),
      "a proto mismatch for our sid is reported, not ignored")
check(SessionPolicy.handshakeOutcome(RTFrame.parse(ack("deadbeef", proto: 3)), sid: sid) == .ignore,
      "a proto mismatch for another sid is a leftover too")
let telemetry = Telemetry(proto: 2, seq: 1,
                          link: LinkInfo(rx_hz: 0, rssi_dbm: nil, timeouts: 0),
                          motors: MotorsInfo(bus: .ok, calibrated: true, owner: .idle),
                          system: SystemInfo(uptime_s: 5, free_heap: 1))
check(SessionPolicy.handshakeOutcome(.telemetry(telemetry), sid: sid) == .ignore,
      "telemetry during the handshake is not an answer")
check(SessionPolicy.handshakeOutcome(nil, sid: sid) == .ignore, "garbage is ignored")
```

- [ ] **Шаг 2: Собрать тест и увидеть красное**

Команда (см. «Порядок сборки»): `name=rtframe` — ожидается ошибка компиляции (`RTFrame.command(seq:t:y:)`, `Telemetry()`).

- [ ] **Шаг 3: `RTFrame.swift`**

```swift
import Foundation

/// The real-time wire, as text. Pure — no Network, no I/O — so it is host-tested with `swiftc`.
///
/// Every constant and every key comes from `CarContract` / `RTType`, generated from
/// `contract/car-api.json`, and the shapes the car sends decode into the generated `DeviceInfo`
/// and `Telemetry`. Four hand-written copies of one protocol is what the generator exists to
/// prevent, so a string literal on the wire path in this file is a bug.
enum RTFrame {

    // MARK: - app → car

    /// Opens a session. Repeated until the car answers; the reply carries the car's identity.
    static func hello(sid: String) -> String {
        "{\"\(CarContract.protoField)\":\(CarContract.proto),"
        + "\"\(CarContract.typeField)\":\"\(RTType.hello)\","
        + "\"\(CarContract.sessionField)\":\"\(sid)\"}"
    }

    /// One 10 Hz drive. `String(format:)` with no locale formats in the C locale, so a phone
    /// set to a comma-decimal language cannot emit `0,50` and desync the car's parser.
    static func command(seq: UInt32, throttle: Double, turn: Double) -> String {
        String(format: "{\"%@\":%d,\"%@\":\"%@\",\"%@\":%u,\"%@\":%.2f,\"%@\":%.2f}",
               CarContract.protoField, CarContract.proto,
               CarContract.typeField, RTType.drive,
               CarContract.seqField, seq,
               CarContract.throttleField, clamp(throttle),
               CarContract.turnField, clamp(turn))
    }

    /// A deliberate stop. Distinct from silence: it suppresses the car's retreat and drops
    /// ownership, so the car stops where it stands instead of retracing its path back to us.
    static func bye(seq: UInt32) -> String {
        String(format: "{\"%@\":%d,\"%@\":\"%@\",\"%@\":%u}",
               CarContract.protoField, CarContract.proto,
               CarContract.typeField, RTType.bye,
               CarContract.seqField, seq)
    }

    /// `seq` is a `uint32` the car compares as `(int32_t)(seq - last) > 0`, so wrapping past
    /// `UInt32.max` is correct on both sides and needs no reset.
    static func nextSeq(_ seq: UInt32) -> UInt32 { seq &+ 1 }

    /// The same comparison, for the counter the car puts in *its* telemetry.
    static func seqNewer(_ seq: Int, than last: Int) -> Bool {
        let delta = UInt32(truncatingIfNeeded: seq) &- UInt32(truncatingIfNeeded: last)
        return Int32(bitPattern: delta) > 0
    }

    /// A per-session id: 8 hex characters, as the contract's `session` field specifies.
    static func sessionID(_ value: UInt32 = .random(in: .min ... .max)) -> String {
        String(format: "%08x", value)
    }

    /// Non-finite input is a stop, not a direction: max(-1, .nan) is -1, so an unguarded NaN
    /// would stream as sustained full reverse — and formatted raw it would not even be JSON.
    private static func clamp(_ v: Double) -> Double {
        v.isFinite ? Swift.min(1, Swift.max(-1, v)) : 0
    }

    // MARK: - car → app

    enum Inbound: Equatable {
        /// The car adopted us. Its identity arrives here, the same `device` group `/status` carries.
        case helloReply(sid: String, device: DeviceInfo)
        /// A car answered our hello speaking a protocol this app does not. Reported rather than
        /// dropped, so the app can say so instead of searching forever.
        case protoMismatch(sid: String, theirs: Int)
        case telemetry(Telemetry)
    }

    private struct HelloAck: Decodable { let device: DeviceInfo }

    /// Classify one datagram by its `type`. Returns nil for anything that is not JSON, has no
    /// type, is a type the car does not send, or does not carry what its type needs — a stray
    /// datagram must not disturb a live session.
    static func parse(_ text: String) -> Inbound? {
        guard let data = text.data(using: .utf8),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let type = j[CarContract.typeField] as? String else { return nil }
        let theirs = j[CarContract.protoField] as? Int ?? 0
        switch type {
        case RTType.helloAck:
            guard let sid = j[CarContract.sessionField] as? String else { return nil }
            // Parsing an unknown protocol as if it were ours is how a version mismatch turns into
            // an unexplainable bug; swallowing it is how it turns into an endless radar.
            guard theirs == CarContract.proto else { return .protoMismatch(sid: sid, theirs: theirs) }
            guard let ack = try? JSONDecoder().decode(HelloAck.self, from: data) else { return nil }
            return .helloReply(sid: sid, device: ack.device)
        case RTType.telemetry:
            guard theirs == CarContract.proto,
                  let t = try? JSONDecoder().decode(Telemetry.self, from: data) else { return nil }
            return .telemetry(t)
        default:
            return nil
        }
    }
}
```

`SessionPolicy.swift`: `case identity(DeviceInfo)` и в `handshakeOutcome`: `case .helloReply(let replySid, let device) where replySid == sid: return .identity(device)`.

- [ ] **Шаг 4: Прогнать оба теста**

`name=rtframe` и `name=sessionpolicy` — ожидается `test_rtframe: OK`, `test_sessionpolicy: OK`.

- [ ] **Шаг 5: Коммит**

```bash
git add app/AJMiddleCar/RTFrame.swift app/AJMiddleCar/SessionPolicy.swift app/tests/rtframe/main.swift app/tests/sessionpolicy/main.swift
git commit -F- <<'MSG'
app(rt): typed datagrams; the hello reply carries the device group; generated Telemetry

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 20: Личность машинки через `DeviceInfo`, и мостик `LegacyIdentity`

**Файлы:**
- Создать: `app/AJMiddleCar/LegacyIdentity.swift`, `app/tests/legacyidentity/main.swift`, `app/tests/legacyidentity/sources`
- Изменить: `app/AJMiddleCar/CarTransport.swift` (`Event`, `Identity`, `session()`, `awaitHello`, `sendLoop`), `app/AJMiddleCar/CarLink.swift` (`handle`, `fetchRadio`, `probedFw`, зонд), `app/AJMiddleCar/AppFlow.swift` (`carProbed`), `app/AJMiddleCar/AJMiddleCarApp.swift` (`onChange(of: link.probedFw)`)
- Тест: `app/tests/carlink/main.swift` — без изменений по сути (`SessionState` не меняется); только проверить, что собирается.

**Интерфейсы:**
- Даёт: `struct LegacyIdentity: Equatable { let device: String; let fw: String; static func parse(_ data: Data) -> LegacyIdentity? }`; `CarTransport.Event.sessionOpened(DeviceInfo)`; `CarLink.probedFw: String?` (`@Published`); `AppFlow.carProbed(fw: String?)`.

- [ ] **Шаг 1: Тест `app/tests/legacyidentity/main.swift`** (файл `sources` содержит одну строку `LegacyIdentity.swift`)

```swift
// Host test for the one piece of v1 the app still understands: who answered /status.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

let v1car = #"{"device":"ajmiddlecar","fw":"v1.0+784","proto":1,"seq":1,"rx_fps":0,"rssi":0,"wdt_trips":0,"uptime_s":3,"heap":1,"calibrated":true,"bus_ok":true,"ctl":"none","rollback":false,"nvs_wiped":false,"radio":{"fw":"3.0.6","expected":"3.0.6","ok":true}}"#
check(LegacyIdentity.parse(Data(v1car.utf8)) == LegacyIdentity(device: "ajmiddlecar", fw: "v1.0+784"), "a v1 car")
let v1dongle = #"{"device":"ajdongle","fw":"v1.0+789","idf":"v6.0.2","usb":"up","rollback":false,"net":{"ssid":"","state":"idle","rssi":0}}"#
check(LegacyIdentity.parse(Data(v1dongle.utf8)) == LegacyIdentity(device: "ajdongle", fw: "v1.0+789"), "a v1 dongle")
let v2 = #"{"proto":2,"device":{"id":"ajmiddlecar","fw":"v1.0+800","build":800,"rolled_back":false},"link":{}}"#
check(LegacyIdentity.parse(Data(v2.utf8)) == LegacyIdentity(device: "ajmiddlecar", fw: "v1.0+800"), "a v2 document too")
check(LegacyIdentity.parse(Data(#"{"device":"x"}"#.utf8)) == nil, "no fw is no identity")
check(LegacyIdentity.parse(Data(#"{"device":{"id":"x"}}"#.utf8)) == nil, "no fw is no identity (v2)")
check(LegacyIdentity.parse(Data(#"[1,2]"#.utf8)) == nil, "not an object")
check(LegacyIdentity.parse(Data("junk".utf8)) == nil, "not json")

if failures == 0 { print("test_legacyidentity: OK") } else { exit(1) }
```

- [ ] **Шаг 2: Собрать — красное (`LegacyIdentity` не существует)**

- [ ] **Шаг 3: `LegacyIdentity.swift`**

```swift
import Foundation

/// Who answered `/status`, read the way BOTH formats spell it. The one bridge across the
/// v1→v2 flag day: a v1 car does not answer a v2 hello at all, and a v1 dongle's `/status`
/// does not decode as a v2 document, so without this the app could never learn that a board
/// is behind — and could never push the update that fixes it.
///
/// The v1 keys are literals on purpose: they are no longer in the contract, and the day no
/// board in the field speaks v1 this file is deleted, not maintained.
struct LegacyIdentity: Equatable {
    let device: String
    let fw: String

    static func parse(_ data: Data) -> LegacyIdentity? {
        guard let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        // v2: "device":{"id":…,"fw":…}
        if let d = j["device"] as? [String: Any], let id = d["id"] as? String, let fw = d["fw"] as? String {
            return LegacyIdentity(device: id, fw: fw)
        }
        // v1: "device":…,"fw":… at the top level
        if let id = j["device"] as? String, let fw = j["fw"] as? String {
            return LegacyIdentity(device: id, fw: fw)
        }
        return nil
    }
}
```

- [ ] **Шаг 4: `CarTransport.swift`**

- `enum Event`: `case sessionOpened(DeviceInfo)` (вместо `device:fw:`), `protoMismatch`, `sessionClosed` без изменений.
- Удалить приватную `struct Identity`; `Handshake.identity(DeviceInfo)`.
- В `session()`: `emit(.sessionOpened(identity))`; `guard identity.id == CarContract.device else { … }`.
- В `awaitHello`: `case .identity(let device): return .identity(device)`.
- В `sendLoop`: `RTFrame.command(seq: seq, throttle: c.t, turn: c.y)` (`c.t`/`c.y` — внутренние имена `ControlIntent`, их не трогать).

- [ ] **Шаг 5: `CarLink.swift`**

`handle(_:)`:

```swift
case .sessionOpened(let info):
    probe?.cancel()
    probedFw = nil
    self.device = info.id
    lastTelemetrySeq = nil
    if info.id == CarContract.device {
        self.fw = info.fw
        session = .adopted(device: info.id, fw: info.fw)
        // The bootloader's verdict on the last update rides with the handshake now; /status
        // is fetched only for the radio.
        rollback = info.rolled_back
        fetchRadio()
        config?.prefetchDriveGeometry()
    } else {
        self.fw = nil
        session = .foreign(device: info.id)
    }
```
В `.sessionClosed` в конце добавить `scheduleProbe()`. В `fetchRadio()` заменить разбор словаря на:

```swift
if let data, let s = try? JSONDecoder().decode(CarStatus.self, from: data) {
    self.radio = .known(fw: s.radio.fw ?? "", ok: s.radio.state == .ok)
    return
}
```
(`RadioStatus.known(fw:ok:)` остаётся; `FirmwareView` показывает «—», если `fw` пустой — найти место вывода и добавить `fw.isEmpty ? "—" : fw`.) Удалить чтение `j["rollback"]`.

Зонд — новые члены класса:

```swift
/// The v1 bridge (spec: "The flag day, and the two bridges across it"). A v1 car drops a v2
/// hello unanswered, so its version cannot reach the update gate through the handshake. When
/// hellos go unanswered, `/status` is read once through the relay and its identity — v1 or v2
/// spelling — is published here for `AppFlow.carProbed`, which may force an update but never
/// declares the car ready. Cleared the moment a real session opens.
@Published private(set) var probedFw: String?
private var probe: Task<Void, Never>?
private var lastProbeAt: ContinuousClock.Instant?
private static let probeAfter: Duration = .seconds(2)
private static let probeSpacing: Duration = .seconds(5)

private func scheduleProbe() {
    probe?.cancel()
    probe = Task { [weak self, transport] in
        try? await Task.sleep(for: Self.probeAfter)
        guard !Task.isCancelled, let self, case .none = self.session else { return }
        if let last = self.lastProbeAt, ContinuousClock.now - last < Self.probeSpacing { return }
        self.lastProbeAt = ContinuousClock.now
        guard let data = try? await transport.get(CarContract.statusPath, timeout: 2),
              let id = LegacyIdentity.parse(data), id.device == CarContract.device,
              !Task.isCancelled else { return }
        self.probedFw = id.fw
    }
}
```
Вызвать `scheduleProbe()` также в конце `start()` и `probe?.cancel()` в `shutDown`.

- [ ] **Шаг 6: `AppFlow.swift` и `AJMiddleCarApp.swift`**

Рядом с `carIdentified`:

```swift
/// The v1 bridge's half of the gate: an identity read from `/status` because the hello went
/// unanswered. It may FORCE an update — that is its whole purpose — but never declares the
/// car ready, since no session exists to drive over.
func carProbed(fw: String?) {
    guard let fw, phase == .awaitingCar || phase == .ready else { return }
    if !GateRule.mayDrive(deviceBuild: UpdateClient.buildNumber(fw),
                          latestBuild: UpdateClient.buildNumber(latestTag)) {
        setPhase(.updateRequired)
    }
}
```
В `AJMiddleCarApp` после `.onChange(of: link.fw)`: `.onChange(of: link.probedFw) { _, fw in flow.carProbed(fw: fw) }`.

- [ ] **Шаг 7: Прогнать `legacyidentity` и `carlink`**

Ожидается: `test_legacyidentity: OK`, `test_carlink: OK`.

- [ ] **Шаг 8: Коммит**

```bash
git add app/AJMiddleCar/LegacyIdentity.swift app/tests/legacyidentity app/AJMiddleCar/CarTransport.swift app/AJMiddleCar/CarLink.swift app/AJMiddleCar/AppFlow.swift app/AJMiddleCar/AJMiddleCarApp.swift
git commit -F- <<'MSG'
app(link): the car's identity is the device group; a /status probe bridges a v1 car to its update

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 21: Адаптер — сгенерированный `DongleStatus`, `POST /wifi`, мостик для v1

**Файлы:**
- Удалить: `app/AJMiddleCar/DongleStatus.swift`
- Изменить: `app/AJMiddleCar/DongleClient.swift`, `app/AJMiddleCar/DongleLink.swift`, `app/AJMiddleCar/AppFlow.swift` (`readStatus`, `askDongleToJoin`), `app/AJMiddleCar/ConnectView.swift` (только комментарии/поля, если читает `status.device`)
- Тесты: `app/tests/donglestatus/main.swift` (целиком; `sources` — пустой файл или удалить строку `DongleStatus.swift`), `app/tests/donglelink/main.swift` (фикстуры и новые случаи; `sources`: `DongleLink.swift`, `LegacyIdentity.swift`, `UpdateRules.swift`, `CarError.swift`)

**Интерфейсы:**
- Даёт: `DongleReply.legacy(LegacyIdentity)`; `DongleReply.decode(_ data: Data) -> DongleReply`; `DongleClient.statusData() async throws -> Data`, `DongleClient.join(ssid:password:) async throws -> DongleWifiReply`, `retryJoin` — то же; `DongleLink.next` понимает `.legacy`.

- [ ] **Шаг 1: Тесты**

`app/tests/donglestatus/main.swift`:

```swift
// Host test for the generated /status decoder. Run with swiftc; no XCTest.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

let full = #"{"proto":1,"device":{"id":"ajdongle","fw":"v1.0+789","build":789,"rolled_back":false,"idf":"v6.0.2"},"usb":{"state":"up"},"wifi":{"ssid":"AJMiddleCar","configured":true,"state":"connected","rssi_dbm":-53,"channel":1,"attempts":{"used":0,"max":5}},"relay":{"to_car_hz":10.0,"to_phone_hz":5.0,"udp_sessions":1,"tcp_connections":2,"last_error":{"errno":118,"message":"No route to host","count":3,"age_s":41}},"system":{"uptime_s":412,"free_heap":8551152}}"#
let s = try! JSONDecoder().decode(DongleStatus.self, from: Data(full.utf8))
check(s.proto == 1 && s.device.id == "ajdongle" && s.device.build == 789 && s.device.idf == "v6.0.2", "device")
check(s.usb.state == .up, "usb")
check(s.wifi.ssid == "AJMiddleCar" && s.wifi.configured && s.wifi.state == .connected, "wifi")
check(s.wifi.rssi_dbm == -53 && s.wifi.channel == 1 && s.wifi.attempts == DongleWifiAttempts(used: 0, max: 5), "wifi readings")
check(s.relay.to_car_hz == 10.0 && s.relay.udp_sessions == 1, "relay")
check(s.relay.last_error == DongleRelayError(errno: 118, message: "No route to host", count: 3, age_s: 41), "last error")
check(s.system.uptime_s == 412 && s.system.free_heap == 8551152, "system")

let idle = #"{"proto":1,"device":{"id":"ajdongle","fw":"v1.0+789","build":789,"rolled_back":true,"idf":"v6.0.2"},"usb":{"state":"up"},"wifi":{"ssid":"","configured":false,"state":"idle","rssi_dbm":null,"channel":null,"attempts":{"used":0,"max":5}},"relay":{"to_car_hz":0.0,"to_phone_hz":0.0,"udp_sessions":0,"tcp_connections":0,"last_error":null},"system":{"uptime_s":3,"free_heap":1}}"#
let i = try! JSONDecoder().decode(DongleStatus.self, from: Data(idle.utf8))
check(i.wifi.rssi_dbm == nil && i.wifi.channel == nil && i.relay.last_error == nil, "nulls decode as nil")
check(i.device.rolled_back && !i.wifi.configured && i.wifi.state == .idle, "idle after boot")

// A state word this build does not know is kept, not a decode failure.
let odd = full.replacingOccurrences(of: #""state":"connected""#, with: #""state":"dreaming""#)
check((try? JSONDecoder().decode(DongleStatus.self, from: Data(odd.utf8)))?.wifi.state == .unknown("dreaming"),
      "unknown wifi state")
// A document missing a group is not a status.
let short = #"{"proto":1,"device":{"id":"ajdongle","fw":"v1.0+789","build":789,"rolled_back":false,"idf":"v6.0.2"}}"#
check((try? JSONDecoder().decode(DongleStatus.self, from: Data(short.utf8))) == nil, "a short document throws")

// The POST /wifi reply and the error envelope.
let reply = try! JSONDecoder().decode(DongleWifiReply.self, from: Data(#"{"proto":1,"ssid":"AJMiddleCar","state":"searching"}"#.utf8))
check(reply == DongleWifiReply(proto: 1, ssid: "AJMiddleCar", state: .searching), "wifi reply")
let err = try! JSONDecoder().decode(DongleAPIError.self, from: Data(#"{"proto":1,"error":{"code":"bad_length","message":"ssid must be 1..32 bytes","field":"ssid"}}"#.utf8))
check(err.error.code == .bad_length && err.error.field == "ssid", "error envelope")
let errNoField = try! JSONDecoder().decode(DongleAPIError.self, from: Data(#"{"proto":1,"error":{"code":"bad_json","message":"x"}}"#.utf8))
check(errNoField.error.field == nil && errNoField.error.code == .bad_json, "error envelope without a field")

if failures == 0 { print("test_donglestatus: OK") } else { exit(1) }
```

`app/tests/donglelink/main.swift` — фикстура `reply(...)`:

```swift
func reply(fw: String, rollback: Bool, ssid: String, state: String, rssi: Int = -50,
           device: String = DongleContract.device) -> DongleReply {
    let json = #"""
    {"proto":1,
     "device":{"id":"\#(device)","fw":"\#(fw)","build":0,"rolled_back":\#(rollback),"idf":"v6.0.2"},
     "usb":{"state":"up"},
     "wifi":{"ssid":"\#(ssid)","configured":\#(!ssid.isEmpty),"state":"\#(state)","rssi_dbm":\#(rssi),"channel":1,
             "attempts":{"used":0,"max":5}},
     "relay":{"to_car_hz":0.0,"to_phone_hz":0.0,"udp_sessions":0,"tcp_connections":0,"last_error":null},
     "system":{"uptime_s":1,"free_heap":1}}
    """#
    return DongleReply.decode(Data(json.utf8))
}
```
Все `DongleNetState.x` → `"x"` (строки состояний как литералы фикстуры), `DongleUsbState.up` → `"up"`. Добавить в конец, перед итогом:

```swift
// -- the v1 bridge: an old dongle is recognised by its legacy identity and updated ---------
let v1 = #"{"device":"ajdongle","fw":"\#(behind)","idf":"v6.0.2","usb":"up","rollback":false,"net":{"ssid":"","state":"idle","rssi":0}}"#
check(DongleLink.next(reply: DongleReply.decode(Data(v1.utf8)), latestTag: latest, expectedSSID: carSSID) == .updating,
      "a v1 dongle behind the release is updated, not declared faulty")
let v1other = v1.replacingOccurrences(of: "ajdongle", with: "someones-adapter")
check(DongleLink.next(reply: DongleReply.decode(Data(v1other.utf8)), latestTag: latest, expectedSSID: carSSID)
        == .wrongDongle(device: "someones-adapter"), "a foreign v1 adapter is named, not flashed")
let v1current = v1.replacingOccurrences(of: behind, with: current)
check(DongleLink.next(reply: DongleReply.decode(Data(v1current.utf8)), latestTag: latest, expectedSSID: carSSID) == .faulty,
      "a v1 dongle that is not behind cannot be driven from — the release it matches is v1")
check(DongleReply.decode(Data("junk".utf8)).isFaulty, "junk decodes as faulty")
```

- [ ] **Шаг 2: Собрать оба — красное**

- [ ] **Шаг 3: Удалить `DongleStatus.swift`; `DongleClient.swift`**

Удалить `net()`. Заменить `status()` на:

```swift
/// The raw `/status` body: `DongleReply.decode` reads it as v2 or, failing that, as a v1
/// identity — the one piece of the old format this app still understands.
func statusData() async throws -> Data {
    try await get(DongleContract.statusPath)
}
```
`join`/`retryJoin` возвращают `DongleWifiReply`; `postCredentials`:

```swift
@discardableResult
private func postCredentials(ssid: String, password: String) async throws -> DongleWifiReply {
    let body: [String: Any] = [DongleContract.ssidField: ssid, DongleContract.passwordField: password]
    let data = try await post(DongleContract.wifiPath, body: try JSONSerialization.data(withJSONObject: body))
    return try JSONDecoder().decode(DongleWifiReply.self, from: data)
}
```
В шапке класса заменить `GET/POST /net` на `POST /wifi`.

- [ ] **Шаг 4: `DongleLink.swift`**

```swift
public enum DongleReply {
    case status(DongleStatus)
    /// A v1 dongle: its `/status` does not decode as a v2 document, but says who it is. The
    /// only thing to do with it is update it — see `next`.
    case legacy(LegacyIdentity)
    case silent
    case faulty
    case denied

    /// Read a `/status` body as v2, else as a v1 identity, else as a fault. Pure.
    public static func decode(_ data: Data) -> DongleReply {
        if let s = try? JSONDecoder().decode(DongleStatus.self, from: data) { return .status(s) }
        if let id = LegacyIdentity.parse(data) { return .legacy(id) }
        return .faulty
    }

    public static func of(_ error: Error) -> DongleReply { … без изменений … }
}
```
В `next`:

```swift
let status: DongleStatus
switch reply {
case .status(let s): status = s
case .legacy(let id):
    guard id.device == DongleContract.device else { return .wrongDongle(device: id.device) }
    // The bridge's one job. A v1 dongle that is NOT behind the release means the release
    // itself is v1, which this build cannot drive through: faulty, not readyForCar.
    return UpdateRules.mustUpdate(carFw: id.fw, latestTag: latestTag) ? .updating : .faulty
case .silent: return .plugIn
case .faulty: return .faulty
case .denied: return .accessDenied
}
guard status.device.id == DongleContract.device else { return .wrongDongle(device: status.device.id) }
if status.device.rolled_back { … UpdateRules.mustUpdate(carFw: status.device.fw, …) … }
else if UpdateRules.mustUpdate(carFw: status.device.fw, latestTag: latestTag) { return .updating }
guard status.wifi.ssid == expectedSSID else { return .sendCredentials }
switch status.wifi.state { … те же ветки … }
```
Комментарии, упоминающие `GET /net`, переписать: `configured`/`ssid` теперь читаются из `/status`.

- [ ] **Шаг 5: `AppFlow.swift`**

`readStatus`: `let data = try await dongle.statusData(); lastStatusFailure = nil; return DongleReply.decode(data)`. В `askDongleToJoin` — `let reply = try await dongle.join(...)` / `retryJoin(...)`; залогировать `reply.state` (`print("dongle \(DongleContract.wifiPath): \(reply.state)")`). Упоминания `POST /net` в комментариях → `POST /wifi`.

- [ ] **Шаг 6: Прогнать `donglestatus` и `donglelink`**

Ожидается: `test_donglestatus: OK`, `test_donglelink: OK`.

- [ ] **Шаг 7: Коммит**

```bash
git add -A app/AJMiddleCar/DongleStatus.swift app/AJMiddleCar/DongleClient.swift app/AJMiddleCar/DongleLink.swift app/AJMiddleCar/AppFlow.swift app/AJMiddleCar/ConnectView.swift app/tests/donglestatus app/tests/donglelink
git commit -F- <<'MSG'
app(dongle): the generated status decoder, POST /wifi, and the v1 bridge

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 22: Конфигурация одним объектом

**Файлы:**
- Изменить: `app/AJMiddleCar/ConfigState.swift` (протокол), `app/AJMiddleCar/ConfigStore.swift`, `app/AJMiddleCar/RampView.swift`, `TrimView.swift`, `RecoverView.swift`, `WheelParamsView.swift`, `CarDimensionsView.swift`, `ControlIntent.swift`, `Tricks*.swift`/`TrickSim*.swift` (где читают `wheel`/`dims`), `GalleryView.swift` (сиды доменов)
- Тесты: `app/tests/configstate/main.swift`, `app/tests/carapi/main.swift`

**Интерфейсы:**
- Даёт: `protocol ConfigDomain: Codable, Equatable, Sendable { static var key: String { get }; static var default: Self { get }; static func pick(from: CarConfig) -> Self?; static func wrap(_ v: Self) -> CarConfig }`; `ConfigStore.ramp/trim/recovery/wheel/chassis`.

- [ ] **Шаг 1: Тесты**

`app/tests/carapi/main.swift`:

```swift
// Host test for the generated contract. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

check(Wheel.default == Wheel(diameter_mm: 65, encoder_ppr: 11, gear_ratio: 9.0, quadrature: 4), "Wheel.default")
check(Recovery.default == Recovery(enabled: true, window_ms: 5000), "Recovery.default")
check(Chassis.default == Chassis(track_mm: 130, wheelbase_mm: 210), "Chassis.default")
check(Ramp.default == Ramp(rise_ms: 300), "Ramp.default")
check(Trim.default == Trim(balance_pct: 0), "Trim.default")

check(Wheel.diameter_mmRange == 20...150, "diameter range")
check(Wheel.gear_ratioRange == 1.0...300.0, "gear ratio range")
check(Trim.balance_pctRange == -30...30, "trim range")
check(Recovery.window_msRange == 1000...10000, "window range")
check(Wheel.quadratureAllowed == [1, 2, 4], "quadrature allowed")

check(Wheel.key == "wheel" && Chassis.key == "chassis" && Ramp.key == "ramp"
      && Trim.key == "trim" && Recovery.key == "recovery", "domain keys")
check(CarContract.configPath == "/config", "one config path")

// /config round-trips: a subset encodes without the domains it does not carry.
let cfg = #"{"proto":2,"ramp":{"rise_ms":300},"trim":{"balance_pct":0},"recovery":{"enabled":true,"window_ms":5000},"wheel":{"diameter_mm":70,"encoder_ppr":12,"gear_ratio":9.6,"quadrature":2},"chassis":{"track_mm":130,"wheelbase_mm":210}}"#
let decoded = try! JSONDecoder().decode(CarConfig.self, from: Data(cfg.utf8))
check(decoded.wheel == Wheel(diameter_mm: 70, encoder_ppr: 12, gear_ratio: 9.6, quadrature: 2), "decode wheel")
check(Wheel.pick(from: decoded) == decoded.wheel, "pick")
let subset = String(decoding: try! JSONEncoder().encode(Ramp.wrap(Ramp(rise_ms: 400))), as: UTF8.self)
check(subset.contains(#""ramp":{"rise_ms":400}"#) && !subset.contains("wheel") && !subset.contains("proto"),
      "a wrapped domain encodes alone")
let back = try! JSONDecoder().decode(CarConfig.self, from: try! JSONEncoder().encode(decoded))
check(back == decoded, "round trip")

check(CarContract.proto == 2 && CarContract.device == "ajmiddlecar" && CarContract.rtPort == 4210, "contract")
check(CarContract.seqField == "seq" && CarContract.throttleField == "throttle" && CarContract.turnField == "turn", "rt keys")
check(RTType.helloAck == "hello_ack" && RTType.drive == "drive", "rt types")
check(CarContract.maxCommand < CarContract.maxDatagram, "caps")
check(MotorsOwner.all.count == 7 && MotorsOwner.all.contains(.remote), "owner vocabulary")
check(MotorsOwner(rawValue: "safe_stop") == .safe_stop && MotorsOwner(rawValue: "x") == .unknown("x"), "owner words")
check(CalibCorner.all == [.front_left, .front_right, .rear_left, .rear_right], "corners")
check(CarErrorCode(rawValue: "out_of_range") == .out_of_range, "error codes")

// The error envelope decodes with and without a field.
let e = try! JSONDecoder().decode(CarAPIError.self, from: Data(#"{"proto":2,"error":{"code":"out_of_range","message":"m","field":"ramp.rise_ms"}}"#.utf8))
check(e.error.code == .out_of_range && e.error.field == "ramp.rise_ms", "envelope")

if failures == 0 { print("test_carapi: OK") } else { exit(1) }
```

`app/tests/configstate/main.swift`: заменить `Wheel(diameter_mm: 70, ppr: 12, gear_x100: 960, quad: 2)` на `Wheel(diameter_mm: 70, encoder_ppr: 12, gear_ratio: 9.6, quadrature: 2)` (и `edited` аналогично), а последнюю проверку — на:

```swift
check(Ramp.key == "ramp" && Trim.key == "trim" && Recovery.key == "recovery"
      && Wheel.key == "wheel" && Chassis.key == "chassis", "the five domains")
check(Wheel.pick(from: Wheel.wrap(carsOwn)) == carsOwn, "wrap then pick is the identity")
```

- [ ] **Шаг 2: Собрать — красное**

- [ ] **Шаг 3: `ConfigState.swift` и `ConfigStore.swift`**

```swift
/// A configuration domain: one member of `/config`, generated from `contract/car-api.json`.
protocol ConfigDomain: Codable, Equatable, Sendable {
    static var key: String { get }
    static var `default`: Self { get }
    /// This domain out of a whole `/config` document, nil when the car did not send it.
    static func pick(from: CarConfig) -> Self?
    /// A `/config` body carrying only this domain — what a POST sends.
    static func wrap(_ v: Self) -> CarConfig
}

extension Ramp: ConfigDomain {}
extension Trim: ConfigDomain {}
extension Recovery: ConfigDomain {}
extension Wheel: ConfigDomain {}
extension Chassis: ConfigDomain {}
```

В `ConfigDomainStore`:

```swift
private func read() async -> Result<T, CarError> {
    do {
        let cfg = try JSONDecoder().decode(CarConfig.self, from: try await transport.get(CarContract.configPath))
        guard let v = T.pick(from: cfg) else { return .failure(.malformed("\(T.key) missing from /config")) }
        return .success(v)
    } catch let e as CarError { return .failure(e) } catch { return .failure(.malformed(String(describing: error))) }
}

private func write(_ v: T) async -> Result<T, CarError> {
    do {
        // The car answers a POST with the whole configuration as now held, so what is kept
        // is what the car has, not what was sent.
        let data = try await transport.post(CarContract.configPath, body: try JSONEncoder().encode(T.wrap(v)))
        let cfg = try JSONDecoder().decode(CarConfig.self, from: data)
        return .success(T.pick(from: cfg) ?? v)
    } catch let e as CarError { return .failure(e) } catch { return .failure(.malformed(String(describing: error))) }
}
```
`ConfigStore`: `let recovery = ConfigDomainStore<Recovery>()`, `let chassis = ConfigDomainStore<Chassis>()` (вместо `recover`/`dims`); `prefetchDriveGeometry` — `chassis`.

- [ ] **Шаг 4: Переименования в экранах и логике**

```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car
grep -rn "\.ramp_ms\|\.trim_pct\|\.ppr\b\|\.gear_x100\|\.quad\b\|Recover(\|Dims(\|\.recover\b\|\.dims\b\|<Recover>\|<Dims>\|Recover\.default\|Dims\.default" app/AJMiddleCar --include=*.swift | grep -v Generated
```
Каждое вхождение привести к v2: `ramp_ms`→`rise_ms`, `trim_pct`→`balance_pct`, `ppr`→`encoder_ppr`, `gear_x100`→`gear_ratio` (`Double`; там, где логике нужен целый `gearX100`, — `Int((w.gear_ratio * 100).rounded())`, а из `gearX100` в `Wheel` — `Double(gearX100) / 100`), `quad`→`quadrature`, `Recover`→`Recovery`, `Dims`→`Chassis`, `store.recover`→`store.recovery`, `store.dims`→`store.chassis`. `MotorPresets` остаётся на `gearX100` внутри — меняется только граница с `Wheel`. В `WheelParamsView` поле ввода передаточного числа уже показывает десятичную строку (`gearString`) — теперь она строится из `gear_ratio` напрямую.

- [ ] **Шаг 5: Прогнать `configstate`, `carapi`**

Ожидается: оба `OK`.

- [ ] **Шаг 6: Коммит**

```bash
git add app/AJMiddleCar app/tests/configstate app/tests/carapi
git commit -F- <<'MSG'
app(config): one GET and one POST /config; the domains under their v2 names

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 23: Калибровка по углам и код ошибки в `CarError`

**Файлы:**
- Изменить: `app/AJMiddleCar/CalibClient.swift` (целиком), `app/AJMiddleCar/ControlModel.swift` (`calibSaveBody` → `calibWheels`), `app/AJMiddleCar/CalibrationView.swift` (`spin`, `assignDir`, сохранение), `app/AJMiddleCar/CarError.swift` (`apiCode`)
- Тест: `app/tests/intent/main.swift` (замена проверки `calibSaveBody`)

- [ ] **Шаг 1: Тест**

В `app/tests/intent/main.swift` найти проверку `ControlModel.calibSaveBody` и заменить на:

```swift
let wheels = ControlModel.calibWheels([.fl: (pair: 0, inverted: false), .fr: (pair: 1, inverted: false),
                                       .rl: (pair: 2, inverted: true), .rr: (pair: 3, inverted: false)])
check(wheels == [CalibWheel(corner: .front_left, pair: 0, inverted: false),
                 CalibWheel(corner: .front_right, pair: 1, inverted: false),
                 CalibWheel(corner: .rear_left, pair: 2, inverted: true),
                 CalibWheel(corner: .rear_right, pair: 3, inverted: false)], "wheels by corner, FL FR RL RR")
check(ControlModel.calibWheels([:]).count == 4, "missing corners default to pair 0, not inverted")
```
(если файл `sources` теста не включает `CarAPI.swift` — он всегда на строке компиляции; `ControlModel.swift` уже включён.)

- [ ] **Шаг 2: Собрать — красное**

- [ ] **Шаг 3: `ControlModel.swift`**

```swift
/// The /calibration body's wheels, by corner name. Missing corners default to (0, not
/// inverted) — the wizard only calls this when all four are set.
static func calibWheels(_ a: [Corner: (pair: Int, inverted: Bool)]) -> [CalibWheel] {
    Corner.allCases.map { c in
        let v = a[c] ?? (pair: 0, inverted: false)
        return CalibWheel(corner: c.wire, pair: v.pair, inverted: v.inverted)
    }
}
```
и у `Corner`: `var wire: CalibCorner { switch self { case .fl: return .front_left; case .fr: return .front_right; case .rl: return .rear_left; case .rr: return .rear_right } }`.

- [ ] **Шаг 4: `CalibClient.swift`**

```swift
import Foundation

/// The car's calibration endpoints. Not a config domain — the wizard's protocol is three calls,
/// not one record — so it stays hand-written while the five domains are generic.
@MainActor
final class CalibClient {
    private let transport: CarTransport

    init(transport: CarTransport = .shared) { self.transport = transport }

    func fetch() async throws -> Calibration {
        try JSONDecoder().decode(Calibration.self, from: try await transport.get(CarContract.calibrationPath))
    }

    /// Spin one motor pair, and **say so when it did not happen**: a POST that never reached
    /// the car must not look like a wheel that turned.
    func spin(pair: Int, direction: CalibDirection) async throws {
        let body = try JSONEncoder().encode(SpinBody(pair: pair, direction: direction))
        _ = try await transport.post(CarContract.spinPath, body: body)
    }

    /// Save the table; the car answers with the table as now held.
    func save(_ wheels: [CalibWheel]) async throws -> Calibration {
        let body = try JSONEncoder().encode(SaveBody(wheels: wheels))
        return try JSONDecoder().decode(Calibration.self, from: try await transport.post(CarContract.calibrationPath, body: body))
    }

    private struct SpinBody: Encodable { let pair: Int; let direction: CalibDirection }
    private struct SaveBody: Encodable { let wheels: [CalibWheel] }
}
```

`CalibrationView.swift`: `assign: [Corner: (pair: Int, inverted: Bool)]`; `client.spin(pair: step, direction: .forward)`; `assignDir(1)` → `assign[c] = (pair: step, inverted: false)`, `assignDir(-1)` → `inverted: true` (переименовать в `assign(inverted:)`); сохранение — `_ = try await client.save(ControlModel.calibWheels(assign))`. Если где-то вызывался `fetchCalibrated()` — заменить на `try await client.fetch().calibrated`.

- [ ] **Шаг 5: `CarError.swift`**

```swift
/// The contract's error code inside an HTTP error body, when the device sent the envelope.
/// For logs and for a view that wants to name the reason; nil for a v1 body or a plain
/// HTTP error from something that is not our firmware.
var apiCode: String? {
    guard case .http(_, let body) = self else { return nil }
    struct Envelope: Decodable { struct E: Decodable { let code: String }; let error: E }
    return (try? JSONDecoder().decode(Envelope.self, from: body))?.error.code
}
```
и в `logDescription`: `case .http(let status, _): return "http \(status)" + (apiCode.map { " \($0)" } ?? "")`.

- [ ] **Шаг 6: Прогнать `intent`, `configstate`, `donglelink`** (последние два включают `CarError.swift`)

Ожидается: все `OK`.

- [ ] **Шаг 7: Коммит**

```bash
git add app/AJMiddleCar/CalibClient.swift app/AJMiddleCar/ControlModel.swift app/AJMiddleCar/CalibrationView.swift app/AJMiddleCar/CarError.swift app/tests/intent/main.swift
git commit -F- <<'MSG'
app(calibration): corners by name; the error code out of the envelope

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 24: Экраны на новой телеметрии и две сборки `xcodebuild`

**Файлы:**
- Изменить: `app/AJMiddleCar/DriveView.swift`, `SettingsView.swift`, `GalleryView.swift`, `L.swift`, `FirmwareView.swift` (радио «—»), всё, что найдёт grep ниже

- [ ] **Шаг 1: Найти каждое чтение старых полей телеметрии**

```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car
grep -rn "\.rxFps\|\.wdtTrips\|\.busOk\|\.uptimeS\|\.heap\b\|\.ctl\b\|telemetry?\.calibrated\|telemetry?\.rssi\|\.rssi\b\|CtlOwner\.\|TelemetryKey\.\|Telemetry()" app/AJMiddleCar --include=*.swift | grep -v Generated
```
Соответствие: `rxFps`→`link.rx_hz`, `rssi`→`link.rssi_dbm` (`Int?`), `wdtTrips`→`link.timeouts`, `busOk`→`motors.bus == .ok`, `calibrated`→`motors.calibrated`, `ctl`→`motors.owner` (`MotorsOwner`; сравнения со строками `CtlOwner.rt` → `.remote` и т. д.), `uptimeS`→`system.uptime_s`, `heap`→`system.free_heap`. Там, где поля были опциональными (`Int?`) и остались обязательными, убрать развёртывание. В `GalleryView.mockLink` собирать `Telemetry(proto: 2, seq: 1, link: LinkInfo(...), motors: MotorsInfo(...), system: SystemInfo(...))`.

- [ ] **Шаг 2: Все хост-тесты приложения**

```bash
for dir in app/tests/*/; do name="$(basename "$dir")"; extra=(); if [ -f "${dir}sources" ]; then while read -r s; do [ -n "$s" ] && extra+=("app/AJMiddleCar/$s"); done < "${dir}sources"; fi; swiftc -o "/tmp/hosttest_$name" app/AJMiddleCar/Generated/CarAPI.swift app/AJMiddleCar/Generated/DongleAPI.swift ${extra[@]+"${extra[@]}"} "${dir}main.swift" && "/tmp/hosttest_$name" || { echo "FAILED: $name"; break; }; done
```
Ожидается: каждая директория печатает `OK`.

- [ ] **Шаг 3: Сборка под симулятор и под устройство**

```bash
cd app && xcodegen generate > /dev/null && cd ..
xcodebuild build -project app/AJMiddleCar.xcodeproj -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle 2>&1 | tail -3
xcodebuild build -project app/AJMiddleCar.xcodeproj -scheme AJMiddleCar -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO -derivedDataPath /tmp/ddata-middle-ios 2>&1 | tail -3
```
Ожидается: `** BUILD SUCCEEDED **` дважды. Ветка под устройство в `CarHost.swift` компилируется только второй командой — не пропускать.

- [ ] **Шаг 4: Коммит**

```bash
git add app
git commit -F- <<'MSG'
app: every screen reads the grouped telemetry; both builds green on v2

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```
## Фаза E — мок, конформанс, документация, полный прогон

### Задача 25: Мок — типизированные датаграммы и сгруппированная телеметрия

**Файлы:**
- Изменить: `tools/mock_car/state.py` (`parse_frame`, `CarState.config`, `apply_config`, `field_of` → удалить, `save_calibration`, `calibration_table`, `telemetry`), `tools/mock_car/rt_link.py` (`_handle`, `_adopt`, `push_telemetry`)
- Тесты: `tools/mock_car/test_state.py`, `tools/mock_car/test_rtlink.py`

**Интерфейсы:**
- Даёт: `parse_frame(data) -> dict | None` с ключами из `RT["keys"]` и обязательным `type`; `CarState.config` — словарь по ключу домена во **внутренних** целых (`gear_ratio` хранится как 900); `CarState.apply_config(body) -> (True, None) | (False, (code, field, message))` — всё тело сразу, атомарно; `CarState.config_wire() -> dict` — все домены в проводном виде; `CarState.save_calibration(wheels) -> (True, None) | (False, (code, field, message))` для списка `{"corner","pair","inverted"}`; `CarState.calibration_table() -> list`; `CarState.telemetry(rx_hz, bump=True) -> dict` — `{"proto","type","seq","link":{…},"motors":{…},"system":{…}}`; `CarState.status_groups(rx_hz) -> dict` — те же три группы без `proto/type/seq`.

- [ ] **Шаг 1: Тесты** — переписать по соответствию v1→v2 (ключи, слова, формы), и добавить новые случаи. Соответствие для `test_state.py::TestWireShapes` и `test_rtlink.py`:
  - каждая датаграмма от приложения получает `"proto":2,"type":"…"`; `hello` → `{"proto":2,"type":"hello","session":"7f3a91c2"}`; команда → `{"proto":2,"type":"drive","seq":N,"throttle":T,"turn":Y}`; goodbye → `{"proto":2,"type":"bye","seq":N}`;
  - `parse_frame` возвращает словарь с `"type"`; проверки `frame["bye"]` → `frame["type"] == "bye"`; `"t"/"y"` → `"throttle"/"turn"`; `"hello"` → `"session"`;
  - «нечего исполнять» → «нет типа / неизвестный тип / типу не хватает ключей»; таблица общих пиновых фреймов — та же, что в `test_control_proto.c` Задачи 7 (голая мантисса, ведущий плюс, ведущий ноль, хвостовой мусор, дубликат ключа, дробный proto, вложенный `seq`);
  - ответ на hello: `{"proto":2,"type":"hello_ack","session":sid,"device":{"id":…,"fw":…,"build":N,"rolled_back":false}}`; чужой proto по-прежнему отвечен, но не принят; **новое**: `drive`/`bye` с чужим или отсутствующим `proto` отбрасываются;
  - телеметрия: `{"proto":2,"type":"telemetry","seq":…,"link":{"rx_hz","rssi_dbm","timeouts"},"motors":{"bus","calibrated","owner"},"system":{"uptime_s","free_heap"}}`; `rssi_dbm` — `None`, когда `car.rssi == 0`; `owner` — слова v2 (`CTL_*` в `state.py` переименовать в `OWNER_IDLE … OWNER_SAFE_STOP` со значениями из `GROUPS["motors"]`);
  - `TestConfig`: `apply_config({"ramp": {"rise_ms": 400}})`; отказ возвращает `(False, ("out_of_range", "ramp.rise_ms", …))`; `config["wheel"]["gear_ratio"] == 900` после `apply_config({"wheel": {…,"gear_ratio": 9.0,…}})` и `913` после `9.125`; `config_wire()["wheel"]["gear_ratio"] == 9.0`; половина тела с ошибкой ничего не меняет (два домена, второй плохой — первый не применён);
  - калибровка: `save_calibration([{"corner":"front_left","pair":0,"inverted":False},…])`; повтор угла → `("not_allowed", "wheels[2]", …)`; неизвестный угол → `not_allowed`; `calibration_table()` возвращает список в порядке FL, FR, RL, RR с `inverted`.

- [ ] **Шаг 2: Прогнать — красное**

Команда: `python3 tools/mock_car/test_state.py 2>&1 | tail -3; python3 tools/mock_car/test_rtlink.py 2>&1 | tail -3`

- [ ] **Шаг 3: `state.py`**

`parse_frame`:

```python
def parse_frame(data, max_command=None):
    """One inbound datagram -> a dict of the fields it carried, or None to drop it.

    The mock's `control_parse_frame` (firmware/car/core/main/control_proto.c). Same answer for
    the same bytes is the whole point, so the rules are the car's, not JSON's:

      * over the *command* cap -> dropped.
      * `type` is required and must be one the app sends: hello, drive, bye.
      * every key that is present must parse, or the whole datagram is dropped.
      * hello needs session; drive needs seq and both axes; bye needs seq.
      * proto is carried when present and judged by the link, not here.
    Range is deliberately not checked here either — the arbiter clamps.
    """
    cap = RT["max_command"] if max_command is None else max_command
    if not data or len(data) > cap:
        return None
    try:
        frame = json.loads(data, object_pairs_hook=_no_duplicates)
    except (ValueError, UnicodeDecodeError):
        return None
    if not isinstance(frame, dict):
        return None
    K, T = RT["keys"], RT["types"]
    kind = frame.get(K["type"])
    if kind not in (T["hello"], T["drive"], T["bye"]):
        return None
    out = {K["type"]: kind}
    for key in (K["proto"], K["seq"]):
        if key in frame:
            if not valid_seq(frame[key]):
                return None
            out[key] = frame[key]
    if K["session"] in frame:
        if not valid_sid(frame[K["session"]]):
            return None
        out[K["session"]] = frame[K["session"]]
    has_t, has_y = K["throttle"] in frame, K["turn"] in frame
    if has_t or has_y:
        if not (has_t and has_y):
            return None
        t, y = number(frame[K["throttle"]]), number(frame[K["turn"]])
        if t is None or y is None:
            return None
        out[K["throttle"]], out[K["turn"]] = t, y
    if kind == T["hello"] and K["session"] not in out:
        return None
    if kind == T["drive"] and (K["seq"] not in out or K["throttle"] not in out):
        return None
    if kind == T["bye"] and K["seq"] not in out:
        return None
    return out
```

`CarState`: `self.config = {key: dict(d["defaults"]) for key, d in DOMAINS.items()}` (внутренние целые; `defaults` в схеме уже целые). Заменить `apply_config`/`field_of`:

```python
def apply_config(self, body):
    """Validate the WHOLE body, then apply every present domain. Returns (True, None) or
    (False, (code, field, message)) — the car's two-pass rule: a body that is half
    right changes nothing."""
    ok, err = validate_config(body)
    if not ok:
        return False, err
    for key in body:
        self.config[key] = from_wire(key, body[key])
    return True, None

def config_wire(self):
    """Every domain as GET /config answers it: fixed fields as decimals."""
    return {key: to_wire(key, values) for key, values in self.config.items()}
```
Все обращения `self.config["/recover"]` → `self.config["recovery"]`; `self.config["/ramp"]["ramp_ms"]` → `self.config["ramp"]["rise_ms"]`; `trim_pct` → `balance_pct`.

Калибровка:

```python
def save_calibration(self, wheels):
    """Mirrors calib_api.c: four wheels, each corner once, pairs 0..3 each once."""
    corners, keys = CALIBRATION["corners"], CALIBRATION["keys"]
    if not isinstance(wheels, list) or len(wheels) != 4:
        return False, ("wrong_type", keys["wheels"], "expected four wheels")
    table, seen = {}, set()
    for i, w in enumerate(wheels):
        where = f"{keys['wheels']}[{i}]"
        if not isinstance(w, dict):
            return False, ("wrong_type", where, "wheel needs {corner,pair,inverted}")
        for key in w:
            if key not in (keys["corner"], keys["pair"], keys["inverted"]):
                return False, ("unknown_field", where, "no such field")
        corner, pair, inverted = w.get(keys["corner"]), w.get(keys["pair"]), w.get(keys["inverted"])
        if not isinstance(corner, str) or not isinstance(inverted, bool) \
                or isinstance(pair, bool) or not isinstance(pair, (int, float)) or float(pair) != int(pair):
            return False, ("wrong_type", where, "wheel needs {corner,pair,inverted}")
        if corner not in corners:
            return False, ("not_allowed", where, "unknown corner")
        if corner in seen:
            return False, ("not_allowed", where, "corner repeated")
        seen.add(corner)
        if not 0 <= int(pair) < CALIBRATION["pairs"]:
            return False, ("out_of_range", where, "pair 0..3")
        table[corner] = (int(pair), inverted)
    if {p for p, _ in table.values()} != set(range(CALIBRATION["pairs"])):
        return False, ("not_allowed", keys["wheels"], "pairs must be 0..3, each once")
    self._calibration = table
    self._calibrated = True
    return True, None

def calibration_table(self):
    """The wheels array GET /calibration answers: FL, FR, RL, RR; empty when not calibrated."""
    if not self._calibrated:
        return []
    keys = CALIBRATION["keys"]
    return [{keys["corner"]: c, keys["pair"]: self._calibration[c][0], keys["inverted"]: self._calibration[c][1]}
            for c in CALIBRATION["corners"]]
```
(`self._calibration = {}` в `__init__`; там, где мок читал старую таблицу пар/знаков для управления, — читать `self._calibration[corner]` → `(pair, inverted)`; если ничего не читал, оставить.)

Телеметрия:

```python
def status_groups(self, rx_hz):
    """The link/motors/system groups, built by walking the schema so a field added to the
    contract and not to the map below raises here rather than going missing on the wire."""
    values = {
        "link": {"rx_hz": int(rx_hz), "rssi_dbm": self.rssi if self.rssi != 0 else None,
                 "timeouts": self._wdt_trips},
        "motors": {"bus": "ok" if self._bus_ok else "down", "calibrated": self._calibrated,
                   "owner": self._owner},
        "system": {"uptime_s": int(self._now - self._started), "free_heap": self.heap},
    }
    return {g: {f["name"]: values[g][f["name"]] for f in GROUPS[g]["fields"]} for g in TELEMETRY_GROUPS}

def telemetry(self, rx_hz, bump=True):
    if bump:
        self._tele_seq += 1
    K, T = RT["keys"], RT["types"]
    return {ENVELOPE["proto"]: PROTO, K["type"]: T["telemetry"], K["seq"]: self._tele_seq,
            **self.status_groups(rx_hz)}
```
`self._owner` хранит слова v2: заменить константы `CTL_NONE … CTL_SAFE` на `OWNER_IDLE = "idle"`, `OWNER_RECOVERING = "recovering"`, `OWNER_CONSOLE`, `OWNER_REMOTE`, `OWNER_CALIBRATION`, `OWNER_UPDATE`, `OWNER_SAFE_STOP`, взятые из `GROUPS["motors"]["fields"][2]["values"]` по индексу, с той же ролью «позиция — ранг», что была у `CTL_VALUES`. Импорты из `generated`: `PROTO, DEVICE, RT, ENVELOPE, GROUPS, TELEMETRY_GROUPS, CALIBRATION, DOMAINS, validate_config, to_wire, from_wire`.

- [ ] **Шаг 4: `rt_link.py`**

В `_handle`: после `parse_frame` — `K, T = RT["keys"], RT["types"]`; `if frame[K["type"]] == T["hello"]: self._adopt(frame, addr, now); return`; затем **до** проверки владельца: `if frame.get(K["proto"]) != PROTO: self._drop(now, f"proto {frame.get(K['proto'])!r}, this car speaks {PROTO}"); return`; ветка goodbye — `if frame[K["type"]] == T["bye"]:`; команда — `self.car.note_command(frame[K["throttle"]], frame[K["turn"]], now)`.

В `_adopt`: `sid = frame[K["session"]]`; ответ —

```python
reply = {ENVELOPE["proto"]: PROTO, K["type"]: T["hello_ack"], K["session"]: sid,
         "device": {"id": self.car.device, "fw": self.car.fw,
                    "build": build_number(self.car.fw), "rolled_back": self.car.rollback}}
```
где `build_number(fw)` — новая функция в `state.py` (число после `+`, иначе `-1`), и ключи `"device"/"id"/"fw"/"build"/"rolled_back"` берутся из `GROUPS["device"]` (`GROUPS["device"]["fields"][i]["name"]`), а не как литералы. `push_telemetry` без изменений (телеметрия строится в `CarState`).

- [ ] **Шаг 5: Прогнать оба теста**

Команда: `python3 tools/mock_car/test_state.py && python3 tools/mock_car/test_rtlink.py`
Ожидается: `OK` дважды.

- [ ] **Шаг 6: Коммит**

```bash
git add tools/mock_car/state.py tools/mock_car/rt_link.py tools/mock_car/test_state.py tools/mock_car/test_rtlink.py
git commit -F- <<'MSG'
mock(rt): typed datagrams, the device group in hello_ack, grouped telemetry

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 26: Мок — REST v2; конформанс-инструменты

**Файлы:**
- Изменить: `tools/mock_car/mock_car.py` (все обработчики и `build_app`), `tools/conformance.py`, `tools/conformance_rt.py`

**Интерфейсы:**
- Даёт (мок): `GET /` — текст; `GET /status` — `{"proto":2,"device":{…},"link":{…},"motors":{…},"radio":{"fw":"mock","expected":"mock","state":"ok"},"storage":{"reset_at_boot":…},"system":{…}}`; `GET /config` и `POST /config` — по спеке; `GET /calibration`, `POST /calibration`, `POST /calibration/spin`; `POST /ota`; ошибки — `{"proto":2,"error":{"code","message"[,"field"]}}`.

- [ ] **Шаг 1: `mock_car.py`**

```python
from generated import (DEVICE, DOMAINS, PROTO, RT, ENVELOPE, ENDPOINTS, GROUPS, CALIBRATION,
                       CONFIG_PATH)
from state import CarState, build_number, parse_image_version

def reply(members, status=200):
    """Every JSON the car emits starts with proto."""
    return web.json_response({ENVELOPE["proto"]: PROTO, **members}, status=status)

def json_error(status, code, message, field=""):
    """The car's rejection envelope. `field` is omitted when the body as a whole is at fault."""
    err = {ENVELOPE["code"]: code, ENVELOPE["message"]: message}
    if field:
        err[ENVELOPE["field"]] = field
    return web.json_response({ENVELOPE["proto"]: PROTO, ENVELOPE["error"]: err}, status=status)

async def cfg_get(request):
    return reply(request.app["car"].config_wire())

async def cfg_post(request):
    car = request.app["car"]
    try:
        body = await request.json()
    except ValueError:
        return json_error(400, "bad_json", "malformed JSON")
    ok, err = car.apply_config(body)
    if not ok:
        code, field, message = err
        return json_error(400, code, message, field)
    print(f"{CONFIG_PATH}: {car.config_wire()}")
    return reply(car.config_wire())

async def status(request):
    car, link = request.app["car"], request.app["link"]
    now = asyncio.get_running_loop().time()
    dev = [f["name"] for f in GROUPS["device"]["fields"]]
    return reply({
        "device": dict(zip(dev, [car.device, car.fw, build_number(car.fw), car.rollback])),
        **car.status_groups(link.rx_fps(now, "status")),
        "radio": {"fw": "mock", "expected": "mock", "state": "ok"},
        "storage": {"reset_at_boot": car.nvs_wiped},
    })
```
Порядок групп в `status` должен быть порядком схемы (`STATUS_GROUPS`): собрать словарь так, чтобы `device, link, motors, radio, storage, system` шли в этом порядке (вставить `radio` и `storage` перед `system`: взять `groups = car.status_groups(...)`, затем `{"device":…, "link": groups["link"], "motors": groups["motors"], "radio": …, "storage": …, "system": groups["system"]}`).

```python
async def calib_get(request):
    car = request.app["car"]
    k = CALIBRATION["keys"]
    return reply({k["calibrated"]: car.calibrated, k["wheels"]: car.calibration_table()})

async def calib_spin(request):
    car = request.app["car"]
    k = CALIBRATION["keys"]
    try:
        body = await request.json()
    except ValueError:
        return json_error(400, "bad_json", "malformed JSON")
    if not isinstance(body, dict):
        return json_error(400, "bad_json", "expected a JSON object")
    for key in body:
        if key not in (k["pair"], k["direction"]):
            return json_error(400, "unknown_field", "no such field", key)
    for key in (k["pair"], k["direction"]):
        if key not in body:
            return json_error(400, "missing_field", "required", key)
    pair, direction = body[k["pair"]], body[k["direction"]]
    if isinstance(pair, bool) or not isinstance(pair, (int, float)) or float(pair) != int(pair):
        return json_error(400, "wrong_type", "expected an integer", k["pair"])
    if not isinstance(direction, str):
        return json_error(400, "wrong_type", "expected a word", k["direction"])
    if not 0 <= int(pair) < CALIBRATION["pairs"]:
        return json_error(400, "out_of_range", "pair 0..3", k["pair"])
    if direction not in CALIBRATION["directions"]:
        return json_error(400, "not_allowed", "forward or reverse", k["direction"])
    forward = direction == CALIBRATION["directions"][0]
    now = asyncio.get_running_loop().time()
    if not car.begin_spin(now, int(pair), 1 if forward else 0):
        return json_error(409, "busy", "actuator busy")
    print(f"calib: spin pair={int(pair)} {direction}")
    await asyncio.sleep(CarState.CALIB_HOLD_MS / 1000.0)
    car.end_spin()
    return reply({ENVELOPE["ok"]: True})

async def calib_save(request):
    car = request.app["car"]
    k = CALIBRATION["keys"]
    try:
        body = await request.json()
    except ValueError:
        return json_error(400, "bad_json", "malformed JSON")
    if not isinstance(body, dict) or k["wheels"] not in body:
        return json_error(400, "missing_field", "required", k["wheels"])
    for key in body:
        if key != k["wheels"]:
            return json_error(400, "unknown_field", "no such field", key)
    ok, err = car.save_calibration(body[k["wheels"]])
    if not ok:
        code, field, message = err
        return json_error(400, code, message, field)
    print(f"calib: saved {car.calibration_table()}")
    return reply({k["calibrated"]: car.calibrated, k["wheels"]: car.calibration_table()})
```
`ota`: `json_error(409, "busy", "actuator busy")` вместо 500 (как у прошивки после Задачи 13); `"too_small"`; `0xE9` → `json_error(400, "not_firmware", "not an ESP image")`; успех — `reply({ENVELOPE["ok"]: True})`. `root` без изменений. `build_app`: маршруты `ENDPOINTS["root"]`, `ENDPOINTS["status"]`, `ENDPOINTS["calibration"]` (GET и POST), `ENDPOINTS["spin"]`, `ENDPOINTS["ota"]`, `CONFIG_PATH` (GET и POST). Проверить, что `car.calibrated` — свойство над `_calibrated`.

- [ ] **Шаг 2: `tools/conformance.py`**

Матрица REST против v2. Каждая проверка — то, что делает и прошивка, и мок:
- `GET /status`: 200, JSON, `proto == PROTO`, набор ключей верхнего уровня = `["proto"] + STATUS_GROUPS`, у каждой группы — ровно поля из `GROUPS[g]` и типы по схеме (`int`→int не bool, `bool`→bool, `str`→str, `state`→одно из `values`, `nullable`→ или None); `device.id == DEVICE`; `device.build == build_number(device.fw)`.
- `GET /`: как раньше (строка начинается с `device.id`).
- `GET /nosuchthing`: 404.
- `GET /config`: 200, `proto`, ключи = `list(DOMAINS)`; каждый домен — поля схемы, `fixed` — число.
- `POST /config` по одному домену со значениями `GET`'а — 200 и тело равно `GET /config`; границы диапазонов (`min`, `max`) принимаются, `min-1`/`max+1` — 400 с `error.code == "out_of_range"` и `error.field == "<key>.<name>"`; `fixed`: `max/scale + 0.0051` → `out_of_range`, `9.125` → 200 и в ответе `9.13`; неизвестный домен → `unknown_field` с `field` = его имя; неизвестное поле → `unknown_field` с `"<key>.<name>"`; пропущенное → `missing_field`; булево вместо числа → `wrong_type`; дробь вместо целого → `wrong_type`; значение enum вне списка → `not_allowed`; пустое тело `{}` → `missing_field`; не-JSON → `bad_json`; половина тела с ошибкой → после 400 `GET /config` не изменился.
- `GET /calibration`: 200, `calibrated` bool, `wheels` список; если `calibrated`, ровно 4 записи с `corner` из `CALIBRATION["corners"]` в этом порядке.
- `POST /calibration/spin`: `{"pair":9,"direction":"forward"}` → `out_of_range`/`pair`; `{"pair":0,"direction":"forward","x":1}` → `unknown_field`/`x`; `{"pair":0,"direction":"up"}` → `not_allowed`/`direction`; `{}` → `missing_field`/`pair`; не-JSON → `bad_json`; `{"pair":0,"direction":"forward"}` → 200 `{"proto","ok":true}` (по времени ≥ `CALIB_HOLD_MS`, как сейчас).
- `POST /calibration`: три колеса → `wrong_type`/`wheels`; повтор угла → `not_allowed`/`wheels[i]`; повтор пары → `not_allowed`/`wheels`; `pair` строкой → `wrong_type`; без `wheels` → `missing_field`/`wheels`; хорошая таблица → 200 и тело с `calibrated: true` и той же таблицей; после неё `GET /calibration` совпадает.
- `POST /ota`: 32 байта → 400 `too_small`; 4 КБ не-образ → 400 `not_firmware`.
Хелперы `expect_rejected(where, path, body, code, field=None, status=400)` — проверять `error.code` и, если задан, `error.field`; `field` отсутствует в теле ⇔ ожидаемый `None`.

- [ ] **Шаг 3: `tools/conformance_rt.py`**

Все кадры через `RT["keys"]`/`RT["types"]`: `hello` → `{"proto":PROTO,"type":"hello","session":sid}`, ответ — `type == "hello_ack"`, `session == sid`, `device` — объект с полями `GROUPS["device"]` и `device.id == DEVICE`; чужой proto в hello → ответ с `proto == PROTO`, сессия не открыта (следующий `drive` не даёт `rx_hz`); `drive`/`bye` — с `proto` и `type`; **новое**: `drive` без `proto` или с `proto: 1` не двигает `link.rx_hz` (телеметрия остаётся с `rx_hz == 0`); телеметрия — `type == "telemetry"`, три группы, `seq` без пропусков; правило-6 фреймы (`rule6_seq_frames`, `padded_frame`) — те же байты с `"type":"drive"` и новыми именами осей; кап 96 байт — `padded_frame` дополняет `drive` до 96/97.

- [ ] **Шаг 4: Прогнать конформанс против мока**

```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car
tools/mock_car/.venv/bin/python tools/mock_car/mock_car.py --host 127.0.0.1 --port 8137 --rt-port 4237 > /tmp/mock.log 2>&1 & MOCK=$!
sleep 1; python3 tools/conformance.py http://127.0.0.1:8137 && python3 tools/conformance_rt.py 127.0.0.1:4237; kill $MOCK
```
Ожидается: обе утилиты печатают свой итог без `FAIL`. (Если `.venv` нет — создать по инструкции из `tools/test-all.sh`.)

- [ ] **Шаг 5: Коммит**

```bash
git add tools/mock_car/mock_car.py tools/conformance.py tools/conformance_rt.py
git commit -F- <<'MSG'
mock+conformance: the v2 REST surface, checked field by field against the contract

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

### Задача 27: Документация, статус спеки, полный прогон

**Файлы:**
- Изменить: `docs/protocol.md` (рукописная часть), `README.md` (примеры JSON, если есть), `firmware/dongle/README.md` (примеры `/status`, `/net` → `/wifi`), `CLAUDE.md` (абзац «The contract», список маршрутов, `mix` не трогать), `docs/superpowers/specs/2026-09-13-wire-format-v2-design.md` (строка статуса)

- [ ] **Шаг 1: `docs/protocol.md`**

Переписать рукописные разделы по спеке: версия 2; раздел UDP — пять датаграмм с примерами из спеки и таблицей полей; правило `proto` на каждой датаграмме и `type`; `hello_ack` с группой `device`; телеметрия в трёх группах; `GET /status` в шести группах (пример из спеки); раздел «Configuration — REST» — `GET/POST /config` и семантика «домены целиком, всё тело проверяется до применения, ответ — полная конфигурация» — сгенерированная таблица остаётся между маркерами; `/calibration` (три запроса, углы по имени); `POST /ota` без изменений; конверт ошибок и список кодов. Ссылку на v1 не оставлять — предыдущий формат описан в истории git.

- [ ] **Шаг 2: `firmware/dongle/README.md`, `README.md`, `CLAUDE.md`**

В README адаптера: примеры `/status` (v2, из спеки), `POST /wifi` и его ответ, конверт ошибок; `GET /net` убрать. В `README.md` — если приводятся примеры телеметрии или статуса, заменить на v2. В `CLAUDE.md` — в абзаце «The contract» упомянуть группы/состояния/коды ошибок и что генератор пишет и структуры статуса; в «Firmware architecture» — `control_proto` «typed: hello/drive/bye», `cfg_api` «one /config», `calib_api` «corners by name»; `identity.h`/`board.h` без изменений.

- [ ] **Шаг 3: Статус спеки**

`**Status:** design, approved 2026-09-13; implemented <дата коммита>` — в первой строке спеки.

- [ ] **Шаг 4: Полный прогон**

```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car && CONFORMANCE=required tools/test-all.sh 2>&1 | tail -15
```
Ожидается: `== all green ==`.

- [ ] **Шаг 5: Коммит**

```bash
git add docs/protocol.md README.md firmware/dongle/README.md CLAUDE.md docs/superpowers/specs/2026-09-13-wire-format-v2-design.md
git commit -F- <<'MSG'
docs: wire format v2 — protocol.md, the READMEs and CLAUDE.md describe what ships

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M4G72wQes6BrZoQkMfbDFi
MSG
```

## После плана (не автоматизируется)

Стендовое доказательство спеки — один круг обновления без кабеля: срезать релиз с обоими v2-образами (`tools/release.sh`), поставить v2-приложение из Xcode, подключить адаптер и машинку на v1.0+784/+789; приложение должно обновить адаптер (мостик `LegacyIdentity` через `/status`), затем машинку (зонд `/status` после молчания hello), затем поехать. После — один проезд, один проход мастера калибровки, по одному `POST /config` с каждого экрана настроек и `GET /status` обоих устройств, прочитанный глазами против спеки. Только после этого мостик можно удалять.
