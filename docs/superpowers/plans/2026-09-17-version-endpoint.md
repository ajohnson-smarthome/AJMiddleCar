# `/version` — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** у обеих плат появляется `GET /version` — пять полей, формат заморожен контрактом;
`/status` теряет `device`; приложение проверяет и обновляет обе платы одним правилом по
`/version` до того, как читает что-либо протоколо-зависимое; машинка получает свой шаг проверки
(S30/S31/S32); `FirmwareFlow` работает по `/version` для обеих плат.

**Architecture:** контракт → две прошивки → мок и конформанс → приложение (чистый слой, затем
гейт, затем экран прошивки) → документы. Каждая сторона проверяется своими тестами; полный
`tools/test-all.sh` становится зелёным в задаче 7, потому что генераторы задачи 1 меняют
типы, на которые опираются и прошивка адаптера, и приложение, и они догоняют по очереди.

**Tech Stack:** JSON-контракт + `tools/gen_contract.py` / `tools/gen_dongle.py` /
`tools/gen_common.py`; C11 (ESP-IDF 6.0.2, host-тесты plain `cc`); Python 3 (мок aiohttp,
конформанс, unittest); Swift/SwiftUI (host-тесты `swiftc`, сборка `xcodebuild`).

**Spec:** `docs/superpowers/specs/2026-09-17-version-endpoint-design.md`

## Global Constraints

- Проза (план, документация, пояснительная часть коммитов) — по-русски; код, комментарии в
  коде, сообщения коммитов — по-английски; строки локализации — по-русски.
- Генерированные файлы (`cfg_table.inc`, `dongle_contract.inc`, `CarAPI.swift`,
  `DongleAPI.swift`, `generated.py`, таблица в `docs/protocol.md`) **никогда не правятся
  руками** — только схема и `python3 tools/gen_contract.py && python3 tools/gen_dongle.py`;
  `tools/check_contract.sh` должен говорить `no drift`.
- `app/` и `firmware/*` друг на друга не ссылаются; прошивки машинки и адаптера друг на друга
  не ссылаются (`device_json.h` и `status_json.c` — намеренные близнецы).
- Формат `/version` — ровно пять полей в этом порядке: `device`, `fw`, `build`, `proto`,
  `rolled_back`; `build` = `-1`, когда в `fw` нет `+<число>`.
- `hello_ack`, `GET /` машинки, `proto` наверху `/status` — не меняются. `groups.device` у
  машинки остаётся (объект `hello_ack`); у адаптера удаляется.
- ESP-IDF окружение: `source tools/env-p4.sh` перед `idf.py`; сборка машинки — `cd
  firmware/car/core && idf.py build`, адаптера — `cd firmware/dongle && idf.py build` (в обеих
  папках перед первой сборкой в новой сессии — `idf.py reconfigure`). Прошивать платы в этом
  плане не нужно; стенд — после задач, контроллером.
- Хост-тесты прошивок: `make -C firmware/car/core/test run`, `make -C firmware/dongle/test run`.
- Мок: `tools/mock_car/.venv` уже есть; `cd tools/mock_car && .venv/bin/python -u mock_car.py`
  слушает `127.0.0.1:8080` (REST) и UDP `4210`/`4211`; `MOCK_DEVICE=esp32-car` — чужая
  машинка. Конформанс против мока: `python3 tools/conformance.py --write-calibration http://127.0.0.1:8080`
  (так его зовёт `tools/test-all.sh`).
- Один Swift host-тест запускается как в `tools/test-all.sh`:

  ```bash
  d=app/tests/<name>; extra=(); while read -r s; do [ -n "$s" ] && extra+=("app/AJMiddleCar/$s"); done < $d/sources
  swiftc -o /tmp/hosttest_<name> app/AJMiddleCar/Generated/CarAPI.swift app/AJMiddleCar/Generated/DongleAPI.swift "${extra[@]}" $d/main.swift && /tmp/hosttest_<name>
  ```

- `xcodebuild`:

  ```bash
  cd app && xcodegen generate >/dev/null && xcodebuild build -quiet -scheme AJMiddleCar \
    -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle 2>&1 | tail -5
  ```

  Ожидается пустой вывод. Прогон в симуляторе против мока:

  ```bash
  xcrun simctl boot "iPhone 17" 2>/dev/null; open -a Simulator
  xcrun simctl install booted /tmp/ddata-middle/Build/Products/Debug-iphonesimulator/AJMiddleCar.app
  xcrun simctl terminate booted com.adamjohnson.ajmiddlecar 2>/dev/null
  xcrun simctl launch booted com.adamjohnson.ajmiddlecar -viaMock
  sleep 8 && xcrun simctl io booted screenshot /tmp/ladder.png && sips -r -90 /tmp/ladder.png >/dev/null
  ```

- Коммитить только файлы, перечисленные в задаче (`git add <файлы>`), никогда `git add -A`;
  генерированные файлы коммитятся вместе со схемой в задаче 1. В дереве есть посторонний
  неотслеживаемый `docs/research/…` — не трогать. `CLAUDE.md` правится только в задаче 8.
- Сообщения коммитов заканчиваются строками:

  ```
  Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ
  ```

---

### Задача 1: Контракт и генераторы

**Files:**
- Modify: `contract/car-api.json` (`endpoints`, новый раздел `version`, `status.groups`, `groups.device.doc`)
- Modify: `contract/dongle-api.json` (`endpoints`, новый раздел `version`, `status.groups`, удалить `groups.device`, `groups.system` + `idf`)
- Modify: `tools/gen_common.py` (новая `c_version_defines`, `py_common` + `VERSION_FIELDS`)
- Modify: `tools/gen_contract.py` (`emit_c` зовёт `c_version_defines`)
- Modify: `tools/gen_dongle.py` (`emit_dongle_c` зовёт `c_version_defines(schema, "DONGLE_")`)
- Modify: `tools/test_gen_contract.py`
- Regenerate: `firmware/car/core/main/cfg_table.inc`, `firmware/dongle/main/dongle_contract.inc`, `app/AJMiddleCar/Generated/CarAPI.swift`, `app/AJMiddleCar/Generated/DongleAPI.swift`, `tools/mock_car/generated.py`, `docs/protocol.md` (таблица)

**Interfaces:**
- Produces: макросы `PATH_VERSION` / `DONGLE_PATH_VERSION`; `KEY_VERSION_DEVICE`, `KEY_VERSION_FW`, `KEY_VERSION_BUILD`, `KEY_VERSION_PROTO`, `KEY_VERSION_ROLLED_BACK` (и с префиксом `DONGLE_`); Swift `CarContract.versionPath`, `DongleContract.versionPath`; Python `VERSION_FIELDS` (список словарей полей) в `generated.py`; `CarStatus` без `device`; `DongleStatus` без `device`, `DongleSystem` с `idf: String`; `DeviceInfo` остаётся.
- После этой задачи прошивка адаптера и приложение **не собираются** (ждут задач 3 и 5–7) — это ожидаемо; проверка задачи — тесты генератора и `check_contract.sh`.

- [ ] **Шаг 1: Падающие тесты генератора**

В `tools/test_gen_contract.py` добавить класс (в конец файла, перед `if __name__ == "__main__"` — или туда, где живут остальные классы):

```python
class TestVersionSection(unittest.TestCase):
    """GET /version: the one document whose shape never changes — identical in both contracts."""

    def setUp(self):
        self.car = load()
        self.dongle = json.loads((ROOT / "contract" / "dongle-api.json").read_text())

    def test_both_contracts_declare_the_endpoint(self):
        self.assertEqual(self.car["endpoints"]["version"], "/version")
        self.assertEqual(self.dongle["endpoints"]["version"], "/version")

    def test_the_section_is_byte_identical_in_both_contracts(self):
        self.assertEqual(json.dumps(self.car["version"], sort_keys=True),
                         json.dumps(self.dongle["version"], sort_keys=True))

    def test_the_five_fields_in_order(self):
        fields = [(f["name"], f["type"]) for f in self.car["version"]["fields"]]
        self.assertEqual(fields, [("device", "str"), ("fw", "str"), ("build", "int"),
                                  ("proto", "int"), ("rolled_back", "bool")])

    def test_device_left_status_but_the_car_keeps_it_for_the_hello_reply(self):
        self.assertNotIn("device", self.car["status"]["groups"])
        self.assertNotIn("device", self.dongle["status"]["groups"])
        self.assertIn("device", self.car["groups"])          # DeviceInfo — the hello_ack object
        self.assertNotIn("device", self.dongle["groups"])
        self.assertIn("idf", [f["name"] for f in self.dongle["groups"]["system"]["fields"]])

    def test_c_version_defines_for_both_prefixes(self):
        import gen_common
        car = "\n".join(gen_common.c_version_defines(self.car, ""))
        dongle = "\n".join(gen_common.c_version_defines(self.dongle, "DONGLE_"))
        for k in ("DEVICE", "FW", "BUILD", "PROTO", "ROLLED_BACK"):
            self.assertIn(f'#define KEY_VERSION_{k} "{k.lower()}"', car)
            self.assertIn(f'#define DONGLE_KEY_VERSION_{k} "{k.lower()}"', dongle)

    def test_emitters_carry_the_path_and_the_python_table(self):
        import gen_contract, gen_dongle
        self.assertIn('#define PATH_VERSION "/version"', gen_contract.emit_c(self.car))
        self.assertIn('#define DONGLE_PATH_VERSION "/version"', gen_dongle.emit_dongle_c(self.dongle))
        self.assertIn('public static let versionPath = "/version"', gen_contract.emit_swift(self.car))
        self.assertIn('public static let versionPath = "/version"', gen_dongle.emit_dongle_swift(self.dongle))
        py = gen_contract.emit_python(self.car)
        self.assertIn("VERSION_FIELDS = [", py)
        self.assertIn("'rolled_back'", py)
```

Если функция эмиттера адаптера называется не `emit_dongle_c` — посмотреть имя в
`tools/gen_dongle.py` (`grep -n "^def emit" tools/gen_dongle.py`) и подставить.

- [ ] **Шаг 2: Убедиться, что тесты падают**

Run: `python3 tools/test_gen_contract.py TestVersionSection -v`
Expected: 6 тестов, все FAIL/ERROR (`KeyError: 'version'`, `AttributeError: c_version_defines`).

- [ ] **Шаг 3: Схемы**

`contract/car-api.json`:
- в `endpoints` добавить `"version": "/version"` после `"status"`;
- новый раздел верхнего уровня `version` (положить после `endpoints`):

```json
"version": {
  "doc": "GET /version: who this board is and what it runs. The one document whose shape never changes: no field is ever added, renamed or removed; anything else belongs in /status, which lives under proto. A board that answers 404 predates this endpoint and is updated.",
  "fields": [
    {"name": "device",      "type": "str",  "doc": "the device name; ajmiddlecar for the car, ajdongle for the adapter"},
    {"name": "fw",          "type": "str",  "doc": "firmware version as the build prints it: v<semver>+<build>[-<n>-g<sha>[-dirty]]"},
    {"name": "build",       "type": "int",  "doc": "the number after + in fw, parsed by the firmware; -1 when fw carries none"},
    {"name": "proto",       "type": "int",  "doc": "the protocol number of everything else this board serves — this contract's proto"},
    {"name": "rolled_back", "type": "bool", "doc": "the bootloader reverted the last update; sticky until the next successful OTA"}
  ]
}
```

- `status.groups`: убрать `"device"` (остаётся `["link","motors","radio","storage","system","video"]`); `status.doc` не трогать;
- `groups.device.doc` → `"Who is answering — the object in the hello reply. Version and identity for the launch gate come from /version."`.

`contract/dongle-api.json`:
- в `endpoints` добавить `"version": "/version"` после `"status"`;
- тот же раздел `version` — **скопировать байт в байт** из car-api.json;
- `status.groups`: убрать `"device"` (остаётся `["usb","wifi","relay","system"]`);
- удалить `groups.device` целиком;
- в `groups.system.fields` добавить в конец `{"name": "idf", "type": "str", "doc": "the ESP-IDF version this image was built with"}`.

- [ ] **Шаг 4: Генераторы**

`tools/gen_common.py` — после `c_endpoint_defines` добавить:

```python
def c_version_defines(schema, prefix):
    """The keys of GET /version — frozen by contract, so these five names never change."""
    return [f'#define {prefix}KEY_VERSION_{upper(f["name"])} "{f["name"]}"'
            for f in schema["version"]["fields"]] + [""]
```

и в `py_common` после строки `STATUS_GROUPS` добавить:

```python
        f"VERSION_FIELDS = {pprint.pformat(schema['version']['fields'], indent=4, sort_dicts=False, width=96)}",
```

(проверить, что `pprint` импортирован в `gen_common.py`; если нет — `import pprint` наверху).

`tools/gen_contract.py`, `emit_c`: после `out += c_endpoint_defines(schema, "")` добавить
`out += c_version_defines(schema, "")` (и импортировать `c_version_defines` там же, где
импортируются остальные `c_*_defines`).

`tools/gen_dongle.py`: после `lines += c_endpoint_defines(schema, "DONGLE_")` добавить
`lines += c_version_defines(schema, "DONGLE_")` (импорт аналогично).

Swift `versionPath` появляется сам: оба Swift-эмиттера перебирают `endpoints`.

- [ ] **Шаг 5: Перегенерировать и прогнать тесты**

```bash
python3 tools/gen_contract.py && python3 tools/gen_dongle.py
python3 tools/test_gen_contract.py -v 2>&1 | tail -5
tools/check_contract.sh
git status --short
```

Ожидается: все тесты OK (включая старые — если какой-то старый тест ссылался на
`STATUS_GROUPS` с `device` или на `DongleDevice`, поправить его ожидание, это часть задачи);
`no drift`; в `git status` — две схемы, три `tools/*.py`, тест и шесть генерированных файлов.
Проверить глазами: `grep -n "VERSION" firmware/car/core/main/cfg_table.inc
firmware/dongle/main/dongle_contract.inc` — по шесть строк на плату; `grep -n "versionPath"
app/AJMiddleCar/Generated/*.swift` — по одной; `grep -n "struct DongleDevice\|var device" app/AJMiddleCar/Generated/DongleAPI.swift` — пусто; `grep -n "idf" app/AJMiddleCar/Generated/DongleAPI.swift` — в `DongleSystem`.

- [ ] **Шаг 6: Коммит**

```bash
git add contract/car-api.json contract/dongle-api.json tools/gen_common.py tools/gen_contract.py tools/gen_dongle.py \
        tools/test_gen_contract.py firmware/car/core/main/cfg_table.inc firmware/dongle/main/dongle_contract.inc \
        app/AJMiddleCar/Generated/CarAPI.swift app/AJMiddleCar/Generated/DongleAPI.swift tools/mock_car/generated.py docs/protocol.md
git commit -m "contract: GET /version — five fields, frozen; device leaves /status

Both contracts gain the same version section (device, fw, build, proto,
rolled_back) and the /version endpoint; the generators emit its key
macros for both firmwares and a VERSION_FIELDS table for the mock and
conformance. The device group leaves /status on both boards — the car
keeps groups.device as the hello_ack object, the adapter drops it and
moves idf into system. The firmwares and the app follow in their own
commits.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ"
```

---

### Задача 2: Прошивка машинки — `GET /version`, `/status` без `device`

**Files:**
- Modify: `firmware/car/core/main/device_json.h` (новый `version_json`)
- Modify: `firmware/car/core/test/test_device_json.c`
- Modify: `firmware/car/core/main/status_api.c` (`version_get`, регистрация, `status_get` без `device`)

**Interfaces:**
- Consumes: `PATH_VERSION`, `KEY_VERSION_*`, `RT_PROTO` из `cfg_table.inc` (задача 1); `fw_build_number`, `CAR_DEVICE_ID`, `status_api_rolled_back()`.
- Produces: `static inline int version_json(char *buf, size_t n, const char *fw, bool rolled_back)` — полный документ `{…}`, возвращает длину или `-1`; `GET /version` на порту 80.
- `device_group_json` и `send_hello_reply` в `rt_link.c` — **не меняются**.

- [ ] **Шаг 1: Падающий тест**

В `firmware/car/core/test/test_device_json.c` перед `printf("test_device_json: all passed\n");` добавить:

```c
    /* GET /version: the one document whose shape never changes. Flat, five keys, in order. */
    char ver[160];
    n = version_json(ver, sizeof(ver), "v1.0+879", false);
    assert(n > 0 && n == (int)strlen(ver));
    assert(strcmp(ver, "{\"device\":\"ajmiddlecar\",\"fw\":\"v1.0+879\",\"build\":879,"
                       "\"proto\":2,\"rolled_back\":false}") == 0);
    n = version_json(ver, sizeof(ver), "v1.0", true);
    assert(n > 0 && strstr(ver, "\"build\":-1,\"proto\":2,\"rolled_back\":true}"));
    assert(version_json(ver, 30, "v1.0+879", false) == -1);
```

- [ ] **Шаг 2: Убедиться, что тест падает**

Run: `make -C firmware/car/core/test run 2>&1 | tail -3`
Expected: ошибка компиляции `implicit declaration of function 'version_json'`.

- [ ] **Шаг 3: Принтер**

В `firmware/car/core/main/device_json.h` после `device_group_json` добавить:

```c
/* GET /version — the whole document, braces included: who this board is and what it runs.
 * The one shape that never changes (contract `version`): five keys, this order, nothing
 * else — anything more belongs in /status. Returns the length, or -1 when it does not fit. */
static inline int version_json(char *buf, size_t n, const char *fw, bool rolled_back) {
    int r = snprintf(buf, n,
        "{\"" KEY_VERSION_DEVICE "\":\"" CAR_DEVICE_ID "\","
        "\"" KEY_VERSION_FW "\":\"%s\","
        "\"" KEY_VERSION_BUILD "\":%d,"
        "\"" KEY_VERSION_PROTO "\":%d,"
        "\"" KEY_VERSION_ROLLED_BACK "\":%s}",
        fw, fw_build_number(fw), RT_PROTO, rolled_back ? "true" : "false");
    if (r < 0 || r >= (int)n) return -1;
    return r;
}
```

Обновить комментарий над `device_group_json`: вместо «for the hello reply and for /status» —
«for the hello reply; /status no longer carries it, /version has its own printer below».

- [ ] **Шаг 4: Тест проходит**

Run: `make -C firmware/car/core/test run 2>&1 | grep device_json`
Expected: `test_device_json: all passed`.

- [ ] **Шаг 5: Хендлер и `/status` без `device`**

В `firmware/car/core/main/status_api.c`:

(а) перед `status_get` добавить:

```c
/* GET /version — identity and version, the document the app reads before anything else
 * and the only one whose shape is frozen. Sent raw, not through api_reply_json: that
 * envelope prepends proto, and /version spells its own. */
static esp_err_t version_get(httpd_req_t *req) {
    char body[160];
    int n = version_json(body, sizeof(body), esp_app_get_description()->version,
                         status_api_rolled_back());
    if (n < 0) {
        ESP_LOGE(TAG, "/version could not render");
        return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "version too long");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}
```

(б) в `status_get` удалить блок `char device[160]; if (device_group_json(...) < 0) { … }` и
в `snprintf(members, …)` убрать первый `"%s,"` из формата и аргумент `device`, так что
формат начинается с `"%s,"` для `groups` (`link,motors`), далее `radio`, `storage`, `%s` (`sys`).
Комментарий «The schema's status order is device, link, …» → «…is link, motors, radio,
storage, system, video».

(в) в `status_api_start` рядом с регистрацией `/status` добавить:

```c
    httpd_uri_t v = { .uri = PATH_VERSION, .method = HTTP_GET, .handler = version_get };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &v), TAG, "cannot register GET /version");
```

(точную форму `ESP_RETURN_ON_ERROR`/сообщения взять с соседней регистрации `/status`).

- [ ] **Шаг 6: Сборка**

```bash
source tools/env-p4.sh && cd firmware/car/core && idf.py reconfigure >/dev/null && idf.py build 2>&1 | tail -3
```

Ожидается: `Project build complete`. Затем `make -C firmware/car/core/test run 2>&1 | tail -2` — all passed.

- [ ] **Шаг 7: Коммит**

```bash
git add firmware/car/core/main/device_json.h firmware/car/core/test/test_device_json.c firmware/car/core/main/status_api.c
git commit -m "feat(car): GET /version; /status without the device group

version_json prints the frozen five-field document — device, fw, build,
proto, rolled_back — raw, outside the proto envelope. /status starts at
link now; the hello reply keeps its device object and its printer.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ"
```

---

### Задача 3: Прошивка адаптера — `GET /version`, `/status` без `device`, `idf` в `system`

**Files:**
- Modify: `firmware/dongle/main/status_json.h` (`status_view_t` без `fw`/`rolled_back`; новая `version_json_render`)
- Modify: `firmware/dongle/main/status_json.c`
- Modify: `firmware/dongle/test/test_status_json.c`
- Modify: `firmware/dongle/main/status_api.c` (`version_get`, регистрация, инициализатор view без `fw`/`rolled_back`)

**Interfaces:**
- Consumes: `DONGLE_PATH_VERSION`, `DONGLE_KEY_VERSION_*`, `DONGLE_KEY_SYSTEM_IDF`, `DONGLE_PROTO`, `DONGLE_DEVICE` (задача 1); `s_rollback` (уже есть в `status_api.c`).
- Produces: `int version_json_render(const char *fw, bool rolled_back, char *buf, size_t n)`; `GET /version` на `:8080`; `/status` = `proto, usb, wifi, relay, system{uptime_s, free_heap, idf}`.

- [ ] **Шаг 1: Падающий тест**

В `firmware/dongle/test/test_status_json.c`:
- в `sample()` удалить `.fw = "v1.0+789",` и `.rolled_back = false,` (поле `.idf` остаётся);
- в эталонной строке удалить строку `"\"device\":{…},"` и заменить хвост
  `"\"system\":{\"uptime_s\":412,\"free_heap\":8551152}}"` на
  `"\"system\":{\"uptime_s\":412,\"free_heap\":8551152,\"idf\":\"v6.0.2\"}}"`;
- перед завершающим `printf` добавить:

```c
    /* GET /version: flat, five keys, frozen. */
    char ver[160];
    n = version_json_render("v1.0+879", false, ver, sizeof(ver));
    assert(n > 0 && n == (int)strlen(ver));
    assert(strcmp(ver, "{\"device\":\"ajdongle\",\"fw\":\"v1.0+879\",\"build\":879,"
                       "\"proto\":1,\"rolled_back\":false}") == 0);
    n = version_json_render("v1.0", true, ver, sizeof(ver));
    assert(n > 0 && strstr(ver, "\"build\":-1,\"proto\":1,\"rolled_back\":true}"));
    assert(version_json_render("v1.0+879", false, ver, 30) == -1);
```

- [ ] **Шаг 2: Убедиться, что тест падает**

Run: `make -C firmware/dongle/test run 2>&1 | tail -3`
Expected: ошибка компиляции (`no member named 'fw'` / `version_json_render` не объявлена).

- [ ] **Шаг 3: Рендерер**

`firmware/dongle/main/status_json.h`: удалить из `status_view_t` поля `const char *fw;` и
`bool rolled_back;` (комментарий у `idf` — «printed under system»); после прототипа
`status_json_render` добавить:

```c
/* GET /version — the whole document, braces included. The one shape that never changes
 * (contract `version`): device, fw, build, proto, rolled_back — nothing else. Returns the
 * length, or -1 when it does not fit. */
int version_json_render(const char *fw, bool rolled_back, char *buf, size_t n);
```

`firmware/dongle/main/status_json.c`:
- в `status_json_render` удалить из формата блок `"\"" DONGLE_KEY_GROUP_DEVICE "\":{ … },"` (пять строк) и аргументы `v->fw, build_number(v->fw), v->rolled_back ? "true" : "false", v->idf,`; в группе `system` формат становится
  `"\"" DONGLE_KEY_GROUP_SYSTEM "\":{\"" DONGLE_KEY_SYSTEM_UPTIME_S "\":%ld,\"" DONGLE_KEY_SYSTEM_FREE_HEAP "\":%u,\"" DONGLE_KEY_SYSTEM_IDF "\":\"%s\"}}"` с аргументами `v->uptime_s, v->free_heap, v->idf`;
- в конец файла добавить:

```c
int version_json_render(const char *fw, bool rolled_back, char *buf, size_t n)
{
    int r = snprintf(buf, n,
        "{\"" DONGLE_KEY_VERSION_DEVICE "\":\"" DONGLE_DEVICE "\","
        "\"" DONGLE_KEY_VERSION_FW "\":\"%s\","
        "\"" DONGLE_KEY_VERSION_BUILD "\":%d,"
        "\"" DONGLE_KEY_VERSION_PROTO "\":%d,"
        "\"" DONGLE_KEY_VERSION_ROLLED_BACK "\":%s}",
        fw, build_number(fw), DONGLE_PROTO, rolled_back ? "true" : "false");
    if (r < 0 || (size_t)r >= n) {
        return -1;
    }
    return r;
}
```

- [ ] **Шаг 4: Тест проходит**

Run: `make -C firmware/dongle/test run 2>&1 | grep status_json`
Expected: `test_status_json: all passed`.

- [ ] **Шаг 5: Хендлер**

В `firmware/dongle/main/status_api.c`:

(а) в инициализаторе `status_view_t v = { … }` удалить `.fw = app->version,` и
`.rolled_back = s_rollback,` (оставить `.idf = app->idf_ver,`);

(б) перед `status_get` добавить:

```c
/* GET /version — identity and version, read by the app before anything else and the only
 * document whose shape is frozen. */
static esp_err_t version_get(httpd_req_t *req)
{
    char body[160];
    int n = version_json_render(esp_app_get_description()->version, s_rollback, body, sizeof(body));
    if (n < 0) {
        ESP_LOGE(TAG, "/version does not fit its buffer");
        return api_reply_error(req, "500 Internal Server Error", DONGLE_ERR_INTERNAL, "", "version too long");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}
```

(в) в `status_api_start`, рядом с регистрацией `/status`, зарегистрировать
`{ .uri = DONGLE_PATH_VERSION, .method = HTTP_GET, .handler = version_get }` тем же способом;
комментарий «Three are registered: GET /status here, …» → «Four are registered: GET /status
and GET /version here, …».

- [ ] **Шаг 6: Сборка**

```bash
source tools/env-p4.sh && cd firmware/dongle && idf.py reconfigure >/dev/null && idf.py build 2>&1 | tail -3
```

Ожидается: `Project build complete`; `make -C firmware/dongle/test run` — all passed.

- [ ] **Шаг 7: Коммит**

```bash
git add firmware/dongle/main/status_json.h firmware/dongle/main/status_json.c firmware/dongle/test/test_status_json.c firmware/dongle/main/status_api.c
git commit -m "feat(dongle): GET /version; /status without the device group, idf under system

version_json_render prints the frozen five-field document, the twin of
the car's; /status starts at usb and carries the IDF version in system.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ"
```

---

### Задача 4: Мок и конформанс

**Files:**
- Modify: `tools/mock_car/mock_car.py` (`version` route; `status` без `device`)
- Modify: `tools/mock_car/test_state.py` (если есть тест на форму `/status` с `device`)
- Modify: `tools/conformance.py` (`version()`, `status()` без `device`, `identity()` по `/version`)

**Interfaces:**
- Consumes: `ENDPOINTS["version"]`, `VERSION_FIELDS`, `STATUS_GROUPS` без `device`, `PROTO`, `DEVICE` из `generated.py` (задача 1); `build_number` из `state.py`.
- Produces: мок отвечает `GET /version` `{"device":…,"fw":…,"build":…,"proto":2,"rolled_back":…}`; конформанс проверяет его.

- [ ] **Шаг 1: Мок**

В `tools/mock_car/mock_car.py`:

(а) в `status(request)` удалить строку `dev = [...]` и элемент `"device": dict(zip(...))` из
`reply({...})`; комментарий «Schema order (STATUS_GROUPS): device, link, …» → «…: link, motors, …»;

(б) после `status` добавить:

```python
async def version(request):
    """GET /version — the frozen five-field document, raw (it spells its own proto)."""
    car = request.app["car"]
    return web.json_response({
        "device": car.device,
        "fw": car.fw,
        "build": build_number(car.fw),
        "proto": PROTO,
        "rolled_back": car.rollback,
    })
```

(в) в `app.add_routes([...])` после `web.get(ENDPOINTS["status"], status),` добавить
`web.get(ENDPOINTS["version"], version),`.

- [ ] **Шаг 2: Конформанс**

В `tools/conformance.py`:

(а) в импорт из `generated` добавить `VERSION_FIELDS`;

(б) после `status(self)` добавить:

```python
    def version(self):
        print("/version")
        status, ctype, parsed, _ = self.call("GET", "/version")
        if not self.expect_json("/version", status, ctype, parsed, 200):
            return
        names = [f["name"] for f in VERSION_FIELDS]
        self.check(list(parsed) == names,
                   f"/version: keys {list(parsed)}, want exactly {names} in this order")
        for f in VERSION_FIELDS:
            v = parsed.get(f["name"])
            self.check(self.status_field_ok(v, f), f"/version.{f['name']} is {v!r}, want {f['type']}")
        self.check(parsed.get("device") == DEVICE, f"/version.device {parsed.get('device')!r}, want {DEVICE!r}")
        self.check(parsed.get("build") == build_number(parsed.get("fw", "")),
                   f"/version.build {parsed.get('build')!r}, want build_number({parsed.get('fw')!r})")
        self.check(parsed.get("proto") == PROTO, f"/version.proto {parsed.get('proto')!r}, want {PROTO}")
```

(в) в `status(self)` удалить три `self.check(...)` про `parsed.get("device")` (блок после цикла по группам);

(г) в `identity(self)` заменить чтение `device` из `/status` на
`_, _, ver, _ = self.call("GET", "/version"); device = (ver or {}).get("device", "")`;

(д) в `run(self)` после `self.status()` добавить `self.version()`.

- [ ] **Шаг 3: Проверка против мока**

```bash
python3 tools/mock_car/test_state.py 2>&1 | tail -2
cd tools/mock_car && (nohup .venv/bin/python -u mock_car.py >/tmp/mock.log 2>&1 &) && cd ../.. && sleep 1
curl -s http://127.0.0.1:8080/version; echo
curl -s http://127.0.0.1:8080/status | python3 -c "import sys,json; print(list(json.load(sys.stdin)))"
```

Ожидается: `test_state` OK (если упал тест на форму `/status` — поправить ожидание, `device`
там больше нет); `/version` — пять полей в порядке; ключи `/status` — `proto, link, motors,
radio, storage, system, video`. Затем `python3 tools/conformance.py --write-calibration http://127.0.0.1:8080` против этого
мока: все проверки, включая `/version`, проходят. Мок остановить: `pkill -f mock_car.py`.

- [ ] **Шаг 4: Коммит**

```bash
git add tools/mock_car/mock_car.py tools/mock_car/test_state.py tools/conformance.py
git commit -m "mock+conformance: GET /version; /status without device

The mock serves the frozen five-field document from its own identity
and rollback flag; conformance checks its keys, order, types and the
build/fw agreement, and reads the identity line's device from /version.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ"
```

(Если `test_state.py` не менялся — не добавлять его в `git add`.)

---

### Задача 5: Приложение, чистый слой — `Identity`, `VersionRule`, `DongleLink` только про сеть

**Files:**
- Create: `app/AJMiddleCar/Identity.swift` (`DeviceVersion`, `VersionReply`)
- Create: `app/AJMiddleCar/VersionRule.swift` (`RollbackChoice` переезжает сюда, `VersionStep`, `VersionRule`)
- Modify: `app/AJMiddleCar/DongleLink.swift` (только сетевые шаги; `DongleReply` и `RollbackChoice` удаляются)
- Create: `app/tests/identity/main.swift`, `app/tests/identity/sources`
- Create: `app/tests/versionrule/main.swift`, `app/tests/versionrule/sources`
- Modify: `app/tests/donglelink/main.swift`, `app/tests/donglelink/sources`

**Interfaces:**
- Consumes: `CarError` (`.http(status:body:)`, `.truncated`, `.denied`, `.noDongle`, `.refused`, `.timeout`, `.malformed`); `UpdateRules.mustUpdate(carFw:latestTag:)`, `UpdateRules.isUpdateAvailable(running:latest:)`; `DongleStatus` без `device` (задача 1).
- Produces:

```swift
public struct DeviceVersion: Codable, Equatable { let device: String; let fw: String; let build: Int; let proto: Int; let rolled_back: Bool }
public enum VersionReply { case version(DeviceVersion), absent, silent, faulty, denied
    static func decode(_ data: Data) -> VersionReply; static func of(_ error: Error) -> VersionReply }
public enum RollbackChoice: Equatable { case unanswered, recheck(from: String?) }
public enum VersionStep: Equatable { case plugIn, faulty, accessDenied, wrongDevice(name: String), rolledBack, updating, appBehind(proto: Int), ok }
public enum VersionRule { static func step(reply: VersionReply, expectedDevice: String, latestTag: String, appProto: Int, rollback: RollbackChoice) -> VersionStep }
public enum DongleStep: Equatable { case sendCredentials, searchingCar, waiting, retryJoin, readyForCar }
public enum DongleLink { static func next(status: DongleStatus, expectedSSID: String) -> DongleStep }
```

Приложение целиком после этой задачи ещё не собирается (`AppFlow`/`FirmwareFlow` ждут задач 6–7); проверка — три хост-теста.

- [ ] **Шаг 1: Падающий тест `identity`**

`app/tests/identity/sources`:

```
Identity.swift
CarError.swift
```

`app/tests/identity/main.swift`:

```swift
// Host test for DeviceVersion and VersionReply — the frozen /version document and how a read
// of it is classified. Run with swiftc; no XCTest, no simulator.
import Foundation
import Network

var failures = 0
func check(_ ok: Bool, _ what: String) { if !ok { print("FAIL: \(what)"); failures += 1 } }

extension VersionReply {
    var isAbsent: Bool { if case .absent = self { return true }; return false }
    var isSilent: Bool { if case .silent = self { return true }; return false }
    var isFaulty: Bool { if case .faulty = self { return true }; return false }
    var isDenied: Bool { if case .denied = self { return true }; return false }
    var version: DeviceVersion? { if case .version(let v) = self { return v }; return nil }
}

// -- both live documents decode, field for field ----------------------------------------------
let car = #"{"device":"ajmiddlecar","fw":"v1.0+879","build":879,"proto":2,"rolled_back":false}"#
let dongle = #"{"device":"ajdongle","fw":"v1.0+879","build":879,"proto":1,"rolled_back":false}"#
check(VersionReply.decode(Data(car.utf8)).version ==
      DeviceVersion(device: "ajmiddlecar", fw: "v1.0+879", build: 879, proto: 2, rolled_back: false),
      "the car's document decodes")
check(VersionReply.decode(Data(dongle.utf8)).version ==
      DeviceVersion(device: "ajdongle", fw: "v1.0+879", build: 879, proto: 1, rolled_back: false),
      "the adapter's document decodes")
check(VersionReply.decode(Data(#"{"device":"ajdongle","fw":"v1.0","build":-1,"proto":1,"rolled_back":true}"#.utf8)).version?.build == -1,
      "a build of -1 (no +number in fw) is a value, not a decode failure")

// -- anything else that came back is a fault ------------------------------------------------------
check(VersionReply.decode(Data("junk".utf8)).isFaulty, "junk is faulty")
check(VersionReply.decode(Data(#"{"device":"ajdongle","fw":"v1.0+879"}"#.utf8)).isFaulty,
      "a document missing fields is faulty — the shape is frozen, there is no lenient read")
check(VersionReply.decode(Data(#"{"proto":1,"usb":{"state":"up"}}"#.utf8)).isFaulty,
      "a /status body is not a /version body")

// -- the transport's errors, classified --------------------------------------------------------
check(VersionReply.of(CarError.http(status: 404, body: Data())).isAbsent,
      "404 is a board that predates /version — absent, to be updated")
check(VersionReply.of(CarError.http(status: 500, body: Data())).isFaulty, "500 is a fault")
check(VersionReply.of(CarError.truncated(got: 3, want: 9)).isFaulty, "a truncated body is a fault")
check(VersionReply.of(CarError.denied).isDenied, "denied is denied")
check(VersionReply.of(CarError.refused).isSilent, "a refused connection is silence")
check(VersionReply.of(CarError.timeout(3)).isSilent, "a timeout is silence")
check(VersionReply.of(CarError.noDongle(.notAvailable)).isSilent, "no path is silence")
check(VersionReply.of(CarError.malformed("x")).isSilent, "a malformed head is silence")
check(VersionReply.of(CancellationError()).isSilent, "a cancellation is evidence of nothing")

if failures == 0 { print("test_identity: OK") } else { exit(1) }
```

- [ ] **Шаг 2: Убедиться, что тест падает**

Run (команда из Global Constraints с `<name>=identity`).
Expected: ошибка компиляции — нет файла `app/AJMiddleCar/Identity.swift`.

- [ ] **Шаг 3: `Identity.swift`**

```swift
import Foundation

/// GET /version — who a board is and what it runs. The one document in either contract whose
/// shape never changes: five fields, this order, nothing else, so this decoder is hand-written
/// and frozen rather than generated. A board that answers 404 predates the endpoint and is
/// updated; that is `VersionReply.absent`, not a decode.
public struct DeviceVersion: Codable, Equatable {
    /// `ajmiddlecar` or `ajdongle` — the first thing the gate checks, before any other field
    /// of any document is believed.
    public let device: String
    /// The version as the build prints it, `v1.0+879` (dev builds add `-<n>-g<sha>[-dirty]`).
    public let fw: String
    /// The number after `+` in `fw`, parsed by the firmware; -1 when `fw` carries none.
    public let build: Int
    /// The protocol number of everything else this board serves — the car's `car-api.proto`,
    /// the adapter's `dongle-api.proto`. Not ours → the board is newer than this app.
    public let proto: Int
    /// The bootloader reverted the last update; sticky until the next successful OTA.
    public let rolled_back: Bool

    public init(device: String, fw: String, build: Int, proto: Int, rolled_back: Bool) {
        self.device = device; self.fw = fw; self.build = build; self.proto = proto
        self.rolled_back = rolled_back
    }
}

/// What one read of `/version` produced. Five situations with five different things to say;
/// the flow classifies, `VersionRule` decides.
public enum VersionReply {
    /// The document, decoded.
    case version(DeviceVersion)
    /// 404: a board older than the endpoint. Treated as behind — the ordinary forced update
    /// takes it across, and its `POST /ota` has always existed.
    case absent
    /// Nothing answered: no cable, a refused connection, a deadline that expired with no bytes.
    case silent
    /// Something answered and it was not usable: an HTTP error other than 404, a truncated
    /// stream, or a body that is not this document. A device is there and talking.
    case faulty
    /// iOS refused to let the request leave the phone: local-network access is denied.
    case denied

    /// Read a body as the frozen document, else as a fault. Pure.
    public static func decode(_ data: Data) -> VersionReply {
        if let v = try? JSONDecoder().decode(DeviceVersion.self, from: data) { return .version(v) }
        return .faulty
    }

    /// Classify what a client's GET threw. Pure, and here rather than in the flow so the rule
    /// is host-tested — the flow's job is to catch, log and pass it on.
    public static func of(_ error: Error) -> VersionReply {
        if let e = error as? CarError {
            switch e {
            case .http(let status, _): return status == 404 ? .absent : .faulty
            case .truncated: return .faulty
            case .denied: return .denied
            // `.malformed` sits on this side deliberately: `CarError.from` uses it for any
            // `NWError` that is neither an unsatisfied path nor ECONNREFUSED, and the request
            // types use it for a connection that closed without a parseable head. Neither is
            // evidence that a board answered.
            case .noDongle, .refused, .timeout, .malformed: return .silent
            }
        }
        // A `DecodingError` is a complete body this build could not read — something answered.
        // Anything else, a cancellation most of all, is evidence of nothing.
        return error is DecodingError ? .faulty : .silent
    }
}
```

- [ ] **Шаг 4: Тест `identity` проходит**

Run: та же команда. Expected: `test_identity: OK`.

- [ ] **Шаг 5: Падающий тест `versionrule`**

`app/tests/versionrule/sources`:

```
Identity.swift
VersionRule.swift
UpdateRules.swift
CarError.swift
```

`app/tests/versionrule/main.swift`:

```swift
// Host test for VersionRule.step — the one decision the launch gate makes for either board
// from its /version: foreign, rolled back, behind, app behind, or ok. Run with swiftc.
import Foundation
import Network

var failures = 0
func check(_ ok: Bool, _ what: String) { if !ok { print("FAIL: \(what)"); failures += 1 } }

let latest = "v1.0+200"          // what GitHub says is newest
let behind = "v1.0+100"
let current = "v1.0+200"
let ahead = "v1.0+250"           // a board flashed from a newer main by cable
func doc(_ device: String = "ajdongle", fw: String, proto: Int = 1, rolledBack: Bool = false) -> VersionReply {
    .version(DeviceVersion(device: device, fw: fw, build: UpdateRules.buildNumber(fw) ?? -1,
                           proto: proto, rolled_back: rolledBack))
}
func step(_ reply: VersionReply, rollback: RollbackChoice = .unanswered, appProto: Int = 1) -> VersionStep {
    VersionRule.step(reply: reply, expectedDevice: "ajdongle", latestTag: latest,
                     appProto: appProto, rollback: rollback)
}

// -- the three ways a read fails map one to one -----------------------------------------------
check(step(.silent) == .plugIn, "silence: plug it in")
check(step(.faulty) == .faulty, "a bad answer: faulty")
check(step(.denied) == .accessDenied, "denied: access denied")

// -- identity first, before any other field is believed -----------------------------------------
check(step(doc("someones-adapter", fw: behind)) == .wrongDevice(name: "someones-adapter"),
      "a foreign board is named, not flashed — even when it is behind")
check(step(doc("someones-adapter", fw: current, rolledBack: true)) == .wrongDevice(name: "someones-adapter"),
      "a foreign board's rollback flag is not read")

// -- a board that predates /version is behind ----------------------------------------------------
check(step(.absent) == .updating, "404 is a board older than the endpoint: update it")

// -- rollback -----------------------------------------------------------------------------------
check(step(doc(fw: behind, rolledBack: true)) == .rolledBack, "rolled back and unanswered: report it")
check(step(doc(fw: behind, rolledBack: true), rollback: .recheck(from: "v1.0+150")) == .updating,
      "rolled back, rechecked, and the release is newer than what was on offer AND than the board: update")
check(step(doc(fw: behind, rolledBack: true), rollback: .recheck(from: latest)) == .rolledBack,
      "rolled back, rechecked, nothing newer than what was on offer: back to the report")
check(step(doc(fw: current, rolledBack: true), rollback: .recheck(from: "v1.0+150")) == .rolledBack,
      "rolled back, a newer offer, but the board already runs it: nothing to flash")

// -- version ------------------------------------------------------------------------------------
check(step(doc(fw: behind)) == .updating, "behind the release: update")
check(step(doc(fw: "v1.0")) == .updating, "a build without a number is behind")
check(step(doc(fw: current)) == .ok, "current: ok")
check(step(doc(fw: ahead)) == .ok, "ahead of the release, same proto: ok — a dev build from a cable")

// -- protocol, only once the version is not behind -----------------------------------------------
check(step(doc(fw: current, proto: 2)) == .appBehind(proto: 2),
      "current build, another proto: the board is newer than this app")
check(step(doc(fw: ahead, proto: 2)) == .appBehind(proto: 2), "ahead and another proto: app behind")
check(step(doc(fw: behind, proto: 2)) == .updating,
      "behind and another proto: update first — the release carries our proto")
check(step(doc(fw: current, proto: 2), appProto: 2) == .ok, "the app's own proto is a match")

if failures == 0 { print("test_versionrule: OK") } else { exit(1) }
```

- [ ] **Шаг 6: Убедиться, что тест падает**

Run (команда с `<name>=versionrule`). Expected: ошибка компиляции — нет `VersionRule.swift`.

- [ ] **Шаг 7: `VersionRule.swift`**

Перенести `RollbackChoice` из `DongleLink.swift` (вместе с его doc-комментарием; в тексте doc
«The dongle's rollback flag» → «A board's rollback flag», «the adapter's rolled-back screen» →
«a rolled-back screen (S10 for the adapter, S31 for the car)») и добавить:

```swift
import Foundation

/// The decision the launch gate makes for either board from one read of its `/version`.
///
/// Pure by design — no `async`, no networking — so every branch is host-tested. The order is
/// the one `DongleLink` always used: identity, then rollback, then version, then protocol.
/// `latestTag` is not optional: the gate learns the release first (`fetchRelease`) and asks
/// this only with a tag in hand.
public enum VersionStep: Equatable {
    /// Nothing answered at the board's address.
    case plugIn
    /// Something answered and it was not this document.
    case faulty
    /// iOS refused the request: local-network access is denied.
    case accessDenied
    /// A board that is not the expected one. Named, and nothing else of it is read: a foreign
    /// board must be neither flashed nor handed the car's network.
    case wrongDevice(name: String)
    /// The bootloader reverted the last update, and the one answer (`RollbackChoice.recheck`)
    /// found nothing newer to try.
    case rolledBack
    /// Behind the release — or older than `/version` itself (404). The forced update takes it
    /// across; its `POST /ota` has always existed.
    case updating
    /// Not behind, but speaking a protocol this app does not: the board is newer than the app.
    /// Nothing this app can do about it except say so.
    case appBehind(proto: Int)
    /// Ours, current, our protocol. The protocol-dependent documents may be read now.
    case ok
}

public enum VersionRule {
    public static func step(reply: VersionReply, expectedDevice: String, latestTag: String,
                            appProto: Int, rollback: RollbackChoice) -> VersionStep {
        let v: DeviceVersion
        switch reply {
        case .version(let d): v = d
        case .absent: return .updating
        case .silent: return .plugIn
        case .faulty: return .faulty
        case .denied: return .accessDenied
        }
        guard v.device == expectedDevice else { return .wrongDevice(name: v.device) }
        if v.rolled_back {
            switch rollback {
            case .unanswered:
                return .rolledBack
            case .recheck(let from):
                // Newer than what was on offer when they asked, AND newer than what the board
                // runs: the first keeps the image that just rolled back from being re-flashed
                // into the same rollback, the second is the ordinary update question.
                if UpdateRules.isUpdateAvailable(running: from, latest: latestTag),
                   UpdateRules.mustUpdate(carFw: v.fw, latestTag: latestTag) {
                    return .updating
                }
                return .rolledBack
            }
        }
        if UpdateRules.mustUpdate(carFw: v.fw, latestTag: latestTag) { return .updating }
        guard v.proto == appProto else { return .appBehind(proto: v.proto) }
        return .ok
    }
}
```

`UpdateRules.mustUpdate(carFw:latestTag:)` возвращает `true` для `fw` без номера сборки —
проверить в `UpdateRules.swift` (там doc: «running firmware predates versioning (no build
number) or its build is lower»); тест «a build without a number is behind» на это и опирается.

- [ ] **Шаг 8: Тест `versionrule` проходит**

Run: та же команда. Expected: `test_versionrule: OK`.

- [ ] **Шаг 9: `DongleLink` — только про сеть; тест переписать**

`app/AJMiddleCar/DongleLink.swift` — оставить только:

```swift
import Foundation

/// The launch sequence's network half for the adapter, as one pure decision: given the
/// adapter's `/status` — read only once `VersionRule` said `.ok`, so the document is one this
/// build's decoder knows — and the network the car actually expects, what does the app do
/// next. Identity, rollback and version are `VersionRule`'s, decided before this is asked.
public enum DongleStep: Equatable {
    /// Not told the car's network yet — or told some other one (`wifi.ssid` is compared against
    /// the car's own name, not just checked for emptiness): send the credentials.
    case sendCredentials
    /// The radio is scanning and has not seen the car's network: usually a car switched off.
    case searchingCar
    /// `joining`, or a state this build does not know: the radio is working, wait.
    case waiting
    /// `failed` or `idle` with the network in place: the adapter will not get any further on
    /// its own — ask it to try again (bounded by the flow's budget).
    case retryJoin
    /// Joined the car's network: the car may be asked for its version now.
    case readyForCar
}

public enum DongleLink {
    public static func next(status: DongleStatus, expectedSSID: String) -> DongleStep {
        guard status.wifi.ssid == expectedSSID else { return .sendCredentials }
        switch status.wifi.state {
        case .connected: return .readyForCar
        case .failed, .idle: return .retryJoin
        case .searching: return .searchingCar
        case .joining, .unknown: return .waiting
        }
    }
}
```

Комментарии из нынешнего `next` про `idle` («state lock busy… IDLE's one exit is a POST
/wifi») и про сравнение SSID перенести к соответствующим случаям — они объясняют решение и
должны остаться.

`app/tests/donglelink/sources` → `DongleLink.swift` (одна строка).

`app/tests/donglelink/main.swift` — переписать: оставить шапку, `check`, фикстуру `reply(...)`
переделать в `status(ssid:state:)`, возвращающую `DongleStatus` (JSON без `device`:
`{"proto":1,"usb":{…},"wifi":{…},"relay":{…},"system":{"uptime_s":1,"free_heap":1,"idf":"v6.0.2"}}`
— декодировать `JSONDecoder().decode(DongleStatus.self, …)!`), и проверки:

```swift
let carSSID = CarContract.ssid
check(DongleLink.next(status: status(ssid: "", state: "idle"), expectedSSID: carSSID) == .sendCredentials,
      "never configured: send the car's network")
check(DongleLink.next(status: status(ssid: "someOtherNetwork", state: "connected"), expectedSSID: carSSID) == .sendCredentials,
      "configured for another network: re-point it, even though it is connected there")
check(DongleLink.next(status: status(ssid: carSSID, state: "searching"), expectedSSID: carSSID) == .searchingCar, "searching")
check(DongleLink.next(status: status(ssid: carSSID, state: "joining"), expectedSSID: carSSID) == .waiting, "joining: wait")
check(DongleLink.next(status: status(ssid: carSSID, state: "failed"), expectedSSID: carSSID) == .retryJoin, "failed: ask again")
check(DongleLink.next(status: status(ssid: carSSID, state: "idle"), expectedSSID: carSSID) == .retryJoin,
      "idle with the network in place: the join was never recorded — ask again")
check(DongleLink.next(status: status(ssid: carSSID, state: "connected"), expectedSSID: carSSID) == .readyForCar, "connected: the car's turn")
if failures == 0 { print("test_donglelink: OK") } else { exit(1) }
```

Значения `state` — слова из контракта (`DongleWifiState`); `.unknown` в JSON не спеллится —
проверять не нужно.

- [ ] **Шаг 10: Все три теста проходят**

Run: команда для `identity`, `versionrule`, `donglelink`. Expected: три `OK`.
`grep -rn "DongleReply\|RollbackChoice" app/AJMiddleCar/DongleLink.swift` — пусто.

- [ ] **Шаг 11: Коммит**

```bash
git add app/AJMiddleCar/Identity.swift app/AJMiddleCar/VersionRule.swift app/AJMiddleCar/DongleLink.swift \
        app/tests/identity app/tests/versionrule app/tests/donglelink/main.swift app/tests/donglelink/sources
git commit -m "feat(app): DeviceVersion, VersionReply and VersionRule — one gate decision for either board

The frozen /version document gets a hand-written decoder and a pure
rule: foreign, rolled back, behind (404 included), app behind, or ok —
identity first, protocol last. DongleLink keeps only the network half
and reads a /status the version check has already vouched for.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ"
```

---

### Задача 6: Приложение, гейт — `/version` перед `/status`, шаг машинки, экраны S30/S31/S32

**Files:**
- Modify: `app/AJMiddleCar/DongleClient.swift` (`versionData()`)
- Modify: `app/AJMiddleCar/AppFlow.swift` (фазы, `readVersion`, `dongleGate`, `carGate`, `dongleReturned`, `recheckRollback`)
- Modify: `app/AJMiddleCar/AJMiddleCarApp.swift` (`root`, `.updateRequired` без `link.start()`)
- Modify: `app/AJMiddleCar/ConnectView.swift` (ситуации `.rolledBack(device:)`, `.checkingCar`, `.appBehind(device:proto:)`)
- Modify: `app/AJMiddleCar/L.swift`, `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings`
- Modify: `app/AJMiddleCar/GalleryView.swift` (кадры для трёх новых ситуаций, `.rolledBack(device: .dongle)`)

**Interfaces:**
- Consumes: задача 5 (`VersionReply`, `VersionRule.step`, `DongleLink.next(status:expectedSSID:)`, `RollbackChoice`); `DongleContract.versionPath`, `CarContract.versionPath`, `CarContract.device`, `DongleContract.device`, `CarContract.proto`, `DongleContract.proto`; `CarTransport.shared.get(_:timeout:)`.
- Produces: `AppFlow.Phase` с `.carChecking`, `.carWrong(device:)`, `.carRolledBack`, `.appBehind(device: UpdateRules.Device, proto: Int)`; `func recheckRollback()` (переименованный `recheckDongleRollback`, общий для обеих плат); `ConnectView.Situation.rolledBack(device:)`, `.checkingCar`, `.appBehind(device:proto:)`; строки `car.checkingTitle/Sub`, `rolledBackTitle.<device>`, `rolledBackSub.<device>`, `appBehindTitle`, `appBehindSub.<device>`.
- `FirmwareFlow` в этой задаче не трогается — приложение соберётся, потому что `FirmwareFlow.forCar(link:)` и `DongleState.refresh` продолжают компилироваться (`DongleState.refresh` читает `/status`... **нет**: он читает `s.device.fw`, которого больше нет — поэтому в этой задаче в `DongleState.refresh` временно заменить `case .status(let s) = DongleReply.decode(data)` на разбор через `VersionReply.decode(try? await dongle.versionData())` — см. шаг 7; задача 7 доведёт это до общего `VersionState`).

- [ ] **Шаг 1: Клиент адаптера**

`app/AJMiddleCar/DongleClient.swift` — рядом с `statusData()`:

```swift
    /// The raw `/version` body — the first thing the gate asks any adapter, before `/status`.
    func versionData() async throws -> Data {
        try await get(DongleContract.versionPath)
    }
```

- [ ] **Шаг 2: Фазы и общий читатель**

В `app/AJMiddleCar/AppFlow.swift`:

(а) в `enum Phase` добавить после `case dongleJoinFailed`:

```swift
        /// The car's own check, the adapter's S3 mirrored: `GET /version` through the relay,
        /// polled until the car answers — it is on the adapter's network, so this is a reboot
        /// or a slow AP, not an absent car — and decided by the same `VersionRule`.
        case carChecking
        /// The car's `/version` named another device. Decided here, before any hello.
        case carWrong(device: String)
        /// The car's bootloader reverted its last update — the car's S10.
        case carRolledBack
        /// A board that is not behind the release but speaks a protocol this app does not: the
        /// board is newer than the app. Nothing to do here but say so; the poll goes on so the
        /// screen leaves by itself if the board is reflashed.
        case appBehind(device: UpdateRules.Device, proto: Int)
```

и в `opensLink` добавить `.carChecking, .carWrong, .carRolledBack, .appBehind` на сторону
`false`, а `.updateRequired` перенести на сторону `false` (doc над `opensLink`: «`.updateRequired`
is on the false side now: the forced update runs over HTTP through `/version` and `/ota`, and a
session opened behind it would only shout `wrongProto` at a car whose protocol is the reason
it is being updated»).

(б) `readStatus()` переименовать в `readDongleStatus()` (тело без изменений) и добавить общий
читатель `/version`:

```swift
    /// One `/version` read of either board, classified rather than collapsed into an optional,
    /// and logged once per distinct failure (`lastVersionFailure`) — the same discipline as
    /// `readDongleStatus()`, for the same reason.
    private func readVersion(_ name: String, _ get: () async throws -> Data) async -> VersionReply {
        do {
            let data = try await get()
            let reply = VersionReply.decode(data)
            if case .faulty = reply {
                let what = "\(name): body is not a /version document (\(data.count) bytes)"
                if what != lastVersionFailure { lastVersionFailure = what; print(what) }
            } else {
                lastVersionFailure = nil
            }
            return reply
        } catch {
            let what = "\(name) /version failed: \(Self.describe(error))"
            if what != lastVersionFailure { lastVersionFailure = what; print(what) }
            return VersionReply.of(error)
        }
    }
    private var lastVersionFailure: String?
```

(`private var` внутри `@MainActor final class` — поставить рядом с `lastStatusFailure`.)

- [ ] **Шаг 3: Гейт адаптера — `/version` перед `/status`**

В `dongleGate()` заменить всё от `let reply = await readStatus()` до конца `switch
DongleLink.next(...)` (включая ветки) на:

```swift
            let version = await readVersion("dongle") { try await self.dongle.versionData() }
            // Step 2, once: something is there and is being looked over.
            if case .version = version, !sawDongle {
                sawDongle = true
                setPhase(.dongleChecking)
            }
            // Step 3: the newest release must be established before anything is decided about
            // the adapter — but only once something has answered at all.
            if case .version = version, latestTag == nil {
                if !(await fetchRelease(for: .dongle)) {
                    try? await Task.sleep(for: Self.donglePollInterval)
                    continue
                }
            }
            if case .absent = version, latestTag == nil {
                // A board older than /version is still a board: the release is needed to update it.
                if !(await fetchRelease(for: .dongle)) {
                    try? await Task.sleep(for: Self.donglePollInterval)
                    continue
                }
            }
            // Identity, rollback, version, protocol — the same rule the car's gate uses.
            var proceed = false
            switch VersionRule.step(reply: version, expectedDevice: DongleContract.device,
                                    latestTag: latestTag ?? "", appProto: DongleContract.proto,
                                    rollback: rollbackChoice) {
            case .plugIn:
                // A dongle that is gone is a dongle that will come back knowing nothing: it keeps
                // the car's network in RAM only, so a replug is a fresh device with an empty
                // configuration, and the join budget is per conversation with one dongle.
                dongleJoinAttempts = 0
                dongleJoinGaveUp = false
                setPhase(.dongleAbsent)
            case .faulty:
                setPhase(.dongleFault)
            case .accessDenied:
                setPhase(.dongleDenied)
            case .wrongDevice(let name):
                setPhase(.dongleWrong(device: name))
            case .rolledBack:
                consumeRollbackRecheck()
                setPhase(.dongleRolledBack)
            case .updating:
                consumeRollbackRecheck()
                // Handing over: `FirmwareView` runs the update and this loop stands aside until
                // `dongleUpdateFinished()` puts the phase back to `.dongleChecking`.
                setPhase(.dongleUpdating)
            case .appBehind(let proto):
                setPhase(.appBehind(device: .dongle, proto: proto))
            case .ok:
                proceed = true
            }
            if !proceed {
                try? await Task.sleep(for: Self.donglePollInterval)
                continue
            }
            // The version agrees, so the protocol-dependent document may be read: the network half.
            let status: DongleStatus
            switch await readDongleStatus() {
            case .status(let s): status = s
            case .silent, .faulty, .denied:
                // Answered /version a moment ago and not /status: a reboot in between. Ask again.
                try? await Task.sleep(for: Self.donglePollInterval)
                continue
            }
            switch DongleLink.next(status: status, expectedSSID: CarContract.ssid) {
            case .sendCredentials:
                setPhase(dongleJoinGaveUp ? .dongleJoinFailed : .dongleSendingNet)
                await askDongleToJoin(retry: false)
            case .searchingCar:
                setPhase(.carFinding)
            case .waiting:
                setPhase(.dongleConfiguring)
            case .retryJoin:
                setPhase(dongleJoinGaveUp ? .dongleJoinFailed : .carFinding)
                await askDongleToJoin(retry: true)
            case .readyForCar:
                dongleJoinAttempts = 0
                dongleJoinGaveUp = false
                return
            }
```

Комментарии из старых веток (`.sendCredentials`/`.retryJoin` про бюджет, `.updating` про
экран) сохранить по смыслу. `latestTag ?? ""` не достигается с `nil` — оба `fetchRelease`
выше гарантируют тег для `.version`/`.absent`, а остальные ответы правило решает до чтения
тега; оставить `?? ""` с комментарием «unreachable with nil: see the two fetches above».

`readDongleStatus()` возвращает прежний `DongleReply` — **этот тип теперь живёт в `AppFlow.swift`**
как `private enum DongleStatusReply { case status(DongleStatus), silent, faulty, denied }` с
теми же `decode`/`of` (перенести из старого `DongleLink.swift`, переименовав; `of` может
делегировать `VersionReply.of` и отобразить `.absent` в `.faulty`).

- [ ] **Шаг 4: Гейт машинки**

После `dongleGate()` добавить:

```swift
    /// The car's own check — the adapter's steps 2–3 mirrored. `GET /version` through the
    /// relay until the car answers, then the same `VersionRule`. `.silent` here is a car that
    /// is on the adapter's network (the adapter said `connected`) but not answering HTTP yet — a
    /// reboot, a slow AP — so it is a hold, not "plug it in". Returns once the car is ours,
    /// current and speaking our protocol; every other outcome is a phase this loop keeps
    /// re-deciding from the next read.
    private func carGate() async -> Bool {
        while true {
            if phase == .updateRequired {
                // `FirmwareView` owns the car right now; polling it mid-flash would read the
                // silence as a reboot that never ends.
                try? await Task.sleep(for: Self.donglePollInterval)
                continue
            }
            let version = await readVersion("car") {
                try await CarTransport.shared.get(CarContract.versionPath, timeout: 2)
            }
            if latestTag == nil, !(await fetchRelease(for: .car)) {
                try? await Task.sleep(for: Self.donglePollInterval)
                continue
            }
            switch VersionRule.step(reply: version, expectedDevice: CarContract.device,
                                    latestTag: latestTag ?? "", appProto: CarContract.proto,
                                    rollback: rollbackChoice) {
            case .plugIn, .faulty:
                // Silence through the relay is a rebooting car — unless the relay itself is
                // gone: the adapter unplugged (it forgets the car's network) or dropped off it.
                // The link is not open in this phase, so nobody else would notice; ask the
                // adapter and hand back to its gate when it is not joined any more.
                if CarHost.viaDongle, !(await adapterStillJoined()) { return false }
                setPhase(.carChecking)
            case .accessDenied:
                setPhase(.dongleDenied)
            case .wrongDevice(let name):
                setPhase(.carWrong(device: name))
            case .rolledBack:
                consumeRollbackRecheck()
                setPhase(.carRolledBack)
            case .updating:
                consumeRollbackRecheck()
                setPhase(.updateRequired)
            case .appBehind(let proto):
                setPhase(.appBehind(device: .car, proto: proto))
            case .ok:
                return true
            }
            try? await Task.sleep(for: Self.donglePollInterval)
        }
    }
```

`CarTransport` — `actor` с `static let shared`; `get(_:timeout:)` делает HTTP через
`HTTPRequest.perform` по `CarHost` и не требует `start()` (так уже делает `CarLink.fetchRadio`).
Вызов из `@MainActor` — `try await CarTransport.shared.get(...)`, как в примере выше.

`carGate()` возвращает `Bool`: `true` — машинка наша, актуальна, наш протокол; `false` — адаптер,
через который идёт опрос, пропал или потерял сеть машинки (тогда вызывающий возвращается к
гейту адаптера). Помощник для ветки `.plugIn, .faulty`:

```swift
    /// Whether the adapter still reports `connected` to the car's network — the one question
    /// `carGate()` asks when the car goes silent through the relay.
    private func adapterStillJoined() async -> Bool {
        guard case .status(let s) = await readDongleStatus() else { return false }
        return DongleLink.next(status: s, expectedSSID: CarContract.ssid) == .readyForCar
    }
```

Оба гейта вместе — одна функция, и её зовут все три входа:

```swift
    /// The adapter's gate, then the car's, until both agree: the car's gate hands back when the
    /// adapter it talks through has dropped or lost the car's network, and the adapter's gate
    /// is then run again (it forgets the network on every replug).
    private func runGates() async {
        while true {
            if CarHost.viaDongle {
                if !dongleHandedOver {
                    await dongleGate()
                    dongleHandedOver = true
                }
            } else {
                await releaseGate()
            }
            if await carGate() { return }
            dongleHandedOver = false
        }
    }
```

`startupCheck()`: тело после `migrateCacheIfNeeded()` — `await runGates(); setPhase(.awaitingCar)`
(комментарий про два пути сохранить над `runGates()`). `dongleReturned()`: `dongleHandedOver =
false; await runGates(); setPhase(.awaitingCar)`; doc: «Only the dongle half re-runs, and then
the car's check: the adapter came back knowing nothing, and the car may have been reflashed or
restarted meanwhile».

`updateFinished()` (принудительное обновление машинки закончено) — решение заново по свежему
`/version`, как `dongleUpdateFinished()` возвращает в `.dongleChecking`; но `carGate()` мог уже
вернуться (форс, поднятый `carIdentified` посреди сессии), поэтому:

```swift
    /// Forced FirmwareView signals completion: re-decide from a fresh /version. If the launch's
    /// own gate loop is still there (it parks while `.updateRequired`), `.carChecking` wakes it;
    /// otherwise — a forced update raised by `carIdentified` mid-session — run the gates again.
    func updateFinished() {
        guard phase == .updateRequired else { return }
        setPhase(.carChecking)
        guard !gateRunning else { return }
        Task { @MainActor in
            self.gateRunning = true
            defer { self.gateRunning = false }
            await self.runGates()
            self.setPhase(.awaitingCar)
        }
    }
```

`carIdentified(fw:)` — без изменений. Guard в `dongleReturned()` (`phase == .awaitingCar || …`)
дополнить `.carChecking`, `.carWrong`, `.carRolledBack`, `.appBehind` — на случай, если связь
всё же была открыта.

`recheckDongleRollback()` → `recheckRollback()` (общий; тело прежнее; вызов в `root` для обоих
экранов отката).

- [ ] **Шаг 5: Экраны**

`app/AJMiddleCar/ConnectView.swift`, `enum Situation`:
- `case dongleRolledBack` → `case rolledBack(device: UpdateRules.Device)` (doc сохранить, «adapter» → «board»);
- добавить:

```swift
        /// The car's step 2: its `/version` is being asked through the relay. Mirrors
        /// `.checkingDongle`, with the car in the frame.
        case checkingCar
        /// A board that is newer than this app: current against the release, but speaking a
        /// protocol this build does not. No button — nothing the app can do but say so.
        case appBehind(device: UpdateRules.Device, proto: Int)
```

- арт: `.rolledBack(let device)` → как сегодняшний `.dongleRolledBack`, но тело по `device`
  (`device == .car ? CarBody(palette: p) : AdapterBody(palette: p)` — обернуть в `AnyView`, если
  `DeviceScene` требует одного типа); `.checkingCar` → как `.checkingDongle` с `CarBody`;
  `.appBehind(let device, _)` → `DeviceScene(rings: .deco, ringTint: p.warn, chip: (glyph: "exclamationmark.arrow.circlepath", tint: p.warn))` с телом по `device`;
- `title`/`message`: `.rolledBack(let device)` → `L.rolledBackTitle(device)` / `L.rolledBackSub(device)`;
  `.checkingCar` → `L.carCheckingTitle` / `L.carCheckingSub`; `.appBehind(let device, let proto)` →
  `L.appBehindTitle` / `L.appBehindSub(device, proto, device == .car ? CarContract.proto : DongleContract.proto)`;
- кнопки: `.rolledBack` — как у `.dongleRolledBack` (через `onRecheckRollback`); `.checkingCar`,
  `.appBehind` — в список без кнопки.

`Localizable.strings`: удалить `dongle.rolledBackTitle`/`dongle.rolledBackSub`, добавить:

```
"rolledBackTitle.dongle" = "Обновление адаптера откатилось";
"rolledBackTitle.car"    = "Обновление машинки откатилось";
"rolledBackSub.dongle"   = "Адаптер вернулся на прежнюю версию — обновление откатилось. Тот же образ, скорее всего, откатится снова: «Повторить» проверит, вышла ли новая версия.";
"rolledBackSub.car"      = "Машинка вернулась на прежнюю версию — обновление откатилось. Тот же образ, скорее всего, откатится снова: «Повторить» проверит, вышла ли новая версия.";
"car.checkingTitle"      = "Проверяю машинку";
"car.checkingSub"        = "Адаптер в сети машинки. Спрашиваю у неё версию — обновлю до поездки, а не посреди неё.";
"appBehind.title"        = "Приложение устарело";
"appBehindSub.dongle"    = "Адаптер говорит на протоколе %d, а это приложение — на %d. Обнови приложение.";
"appBehindSub.car"       = "Машинка говорит на протоколе %d, а это приложение — на %d. Обнови приложение.";
```

`L.swift`: удалить `dongleRolledBackTitle/Sub`, добавить

```swift
    static func rolledBackTitle(_ d: UpdateRules.Device) -> String { s("rolledBackTitle.\(d.rawValue)") }
    static func rolledBackSub(_ d: UpdateRules.Device) -> String { s("rolledBackSub.\(d.rawValue)") }
    static var carCheckingTitle: String { s("car.checkingTitle") }
    static var carCheckingSub: String { s("car.checkingSub") }
    static var appBehindTitle: String { s("appBehind.title") }
    static func appBehindSub(_ d: UpdateRules.Device, _ theirs: Int, _ ours: Int) -> String { s("appBehindSub.\(d.rawValue)", theirs, ours) }
```

- [ ] **Шаг 6: Корень**

`app/AJMiddleCar/AJMiddleCarApp.swift`, `root`:
- `case .dongleRolledBack:` → `ConnectView(situation: .rolledBack(device: .dongle), onRecheckRollback: { flow.recheckRollback() })`;
- добавить:

```swift
        case .carChecking:
            ConnectView(situation: .checkingCar)
        case .carWrong(let device):
            WrongCarView(palette: p, kind: .foreignDevice(device)) { }   // the poll re-asks by itself
        case .carRolledBack:
            ConnectView(situation: .rolledBack(device: .car), onRecheckRollback: { flow.recheckRollback() })
        case .appBehind(let device, let proto):
            ConnectView(situation: .appBehind(device: device, proto: proto))
```

- `case .updateRequired:` — убрать `.onAppear { link.start() }` (комментарий: «HTTP only — see
  `Phase.opensLink`»).

`WrongCarView` принимает замыкание `onRetry` — если оно обязательное, передать `{}`; в
`WrongCarView` ничего не менять.

- [ ] **Шаг 7: Временная опора для `FirmwareFlow`**

В `app/AJMiddleCar/FirmwareFlow.swift`, `DongleState.refresh`:

```swift
        if let data = try? await dongle.versionData(), case .version(let v) = VersionReply.decode(data) {
            fw = v.fw
            reachable = true
        } else {
            reachable = false
        }
```

(Задача 7 превратит это в общий `VersionState` для обеих плат.)

- [ ] **Шаг 8: Галерея**

`app/AJMiddleCar/GalleryView.swift`: кадр `("Dongle rolled back", ConnectView(situation: .rolledBack(device: .dongle), onRecheckRollback: {}))`;
добавить после него:

```swift
            ("Car checking",             AnyView(ConnectView(situation: .checkingCar))),
            ("Car rolled back",          AnyView(ConnectView(situation: .rolledBack(device: .car), onRecheckRollback: {}))),
            ("App behind (car)",         AnyView(ConnectView(situation: .appBehind(device: .car, proto: 3)))),
            ("App behind (dongle)",      AnyView(ConnectView(situation: .appBehind(device: .dongle, proto: 2)))),
```

- [ ] **Шаг 9: Сборка и прогон**

`xcodebuild` из Global Constraints → пусто. `grep -rn "recheckDongleRollback\|dongleRolledBackTitle\|readStatus()\|\.dongleRolledBack\b" app/AJMiddleCar` — пусто (кроме `Phase.dongleRolledBack`, которая остаётся как фаза).

Прогон против мока (команды из Global Constraints; мок запустить, если не запущен): через 8 с — пульт.
Затем чужая машинка:

```bash
pkill -f mock_car.py; cd tools/mock_car && (MOCK_DEVICE=esp32-car nohup .venv/bin/python -u mock_car.py >/tmp/mock2.log 2>&1 &) && cd ../.. && sleep 1
xcrun simctl terminate booted com.adamjohnson.ajmiddlecar; xcrun simctl launch booted com.adamjohnson.ajmiddlecar -viaMock
sleep 6 && xcrun simctl io booted screenshot /tmp/wrongcar.png && sips -r -90 /tmp/wrongcar.png >/dev/null
pkill -f mock_car.py
```

На скриншоте — «Это другая машинка» с `esp32-car`. Пути к обоим скриншотам — в отчёт.

- [ ] **Шаг 10: Коммит**

```bash
git add app/AJMiddleCar/DongleClient.swift app/AJMiddleCar/AppFlow.swift app/AJMiddleCar/AJMiddleCarApp.swift \
        app/AJMiddleCar/ConnectView.swift app/AJMiddleCar/L.swift app/AJMiddleCar/Resources/ru.lproj/Localizable.strings \
        app/AJMiddleCar/GalleryView.swift app/AJMiddleCar/FirmwareFlow.swift
git commit -m "feat(app): the gate reads /version first, for both boards

The adapter's gate asks /version before /status and decides with
VersionRule; the car gets the same step (carChecking) through the relay
before any hello — wrong device, rollback, update and 'app behind' are
all decided there. The forced update no longer opens a session: it runs
over HTTP. Three screens join the ladder: checking the car, the car
rolled back, and the app being older than a board.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ"
```

---

### Задача 7: `FirmwareFlow` по `/version` для обеих плат; хвосты; весь набор

**Files:**
- Modify: `app/AJMiddleCar/FirmwareFlow.swift` (`VersionState` вместо `DongleState`; `forCar()` без `link`)
- Modify: `app/AJMiddleCar/SettingsView.swift` (вызов `.forCar(...)`)
- Modify: `app/AJMiddleCar/AJMiddleCarApp.swift` (вызов `.forCar(...)` в `.updateRequired`)
- Modify: `app/AJMiddleCar/CarLink.swift` (удалить `rollback` и его присваивание)
- Modify: `app/AJMiddleCar/GalleryView.swift` (если `fw(...)` строит `forCar(link:)`)

**Interfaces:**
- Consumes: `VersionReply` (задача 5), `DongleClient.versionData()` (задача 6), `CarTransport.shared.get`.
- Produces: `static func forCar() -> FirmwareFlow`, `static func forDongle(client:) -> FirmwareFlow`; оба с `refresh` по `/version` и `isReachable` = «последний `refresh` ответил документом».

- [ ] **Шаг 1: `VersionState`**

В `app/AJMiddleCar/FirmwareFlow.swift` заменить `DongleState` и оба фабричных метода:

```swift
extension FirmwareFlow {
    /// The car, reached through the relay: `/version` for what it runs and whether it answers,
    /// `POST /ota` for the image. No session is involved on either leg — which is what lets a
    /// car speaking an older protocol be updated at all.
    static func forCar() -> FirmwareFlow {
        let state = VersionState { try await CarTransport.shared.get(CarContract.versionPath, timeout: 2) }
        return FirmwareFlow(device: .car,
                            runningFw: { state.fw },
                            isReachable: { state.reachable },
                            refresh: { await state.refresh() },
                            progressPublishedByClient: true,
                            push: { url, client, _ in await client.upload(url) })
    }

    /// The adapter, reached over USB. Same shape; only the client differs.
    static func forDongle(client dongle: DongleClient) -> FirmwareFlow {
        let state = VersionState { try await dongle.versionData() }
        return FirmwareFlow(device: .dongle,
                            runningFw: { state.fw },
                            isReachable: { state.reachable },
                            refresh: { await state.refresh() },
                            push: { url, _, progress in
            guard let data = try? Data(contentsOf: url) else { return .failed(nil) }
            do {
                try await dongle.uploadFirmware(data) { p in
                    Task { @MainActor in progress(p) }
                }
                return .ok
            } catch is CancellationError {
                return .cancelled
            } catch {
                return .failed((error as? CarError).map { String(describing: $0) })
            }
        })
    }
}

/// A board's last `/version` answer, refreshed on demand. A class rather than captured `var`s
/// because the flow's closures all have to see the same value, and because the reboot watch
/// reads it every 500 ms from a task that outlives whichever call started it.
@MainActor
private final class VersionState {
    private let read: () async throws -> Data
    var fw: String?
    var reachable = false
    init(read: @escaping () async throws -> Data) { self.read = read }

    func refresh() async {
        // The frozen document, on both sides of the reboot this watch runs through: a 404 or a
        // stale-format answer would be a board older than this app, which the gate has already
        // sent through an update — so anything but the document is "not reachable yet".
        if let data = try? await read(), case .version(let v) = VersionReply.decode(data) {
            fw = v.fw
            reachable = true
        } else {
            reachable = false
        }
    }
}
```

Doc-комментарий над `check()` («The car answers from `CarLink` and this is free») → «Both
boards answer from `/version`». `progressPublishedByClient` у машинки остаётся `true` (загрузку
ведёт `UpdateClient.upload`).

- [ ] **Шаг 2: Вызовы**

`grep -rn "forCar(link" app/AJMiddleCar` — заменить каждый на `.forCar()` (`SettingsView.swift`,
`AJMiddleCarApp.swift`, возможно `GalleryView.swift`).

- [ ] **Шаг 3: `CarLink.rollback`**

В `app/AJMiddleCar/CarLink.swift` удалить `@Published private(set) var rollback: Bool?` с
doc-комментарием и присваивание `rollback = info.rolled_back` в `.sessionOpened` (с его
комментарием «The bootloader's verdict rides with the handshake now…»); doc над `fetchRadio`,
упоминающий `rollback`, поправить («`rollback` no longer comes from here» → удалить фразу).
`grep -rn "link.rollback\|\.rollback\b" app/AJMiddleCar` → только `car.rollback`-подобных нет; пусто.

- [ ] **Шаг 4: Сборка, весь набор, прогон**

`xcodebuild` → пусто. Затем из корня `tools/test-all.sh 2>&1 | tail -8` → `== all green ==`
(в `== swift host tests ==` — `identity`, `versionrule`, `donglelink` среди прочих).
Прогон против мока → пульт.

- [ ] **Шаг 5: Коммит**

```bash
git add app/AJMiddleCar/FirmwareFlow.swift app/AJMiddleCar/SettingsView.swift app/AJMiddleCar/AJMiddleCarApp.swift \
        app/AJMiddleCar/CarLink.swift app/AJMiddleCar/GalleryView.swift
git commit -m "refactor(app): FirmwareFlow reads /version for either board; CarLink drops the unread rollback flag

One VersionState serves the car (through the relay) and the adapter
(over USB): what it runs and whether it answers both come from the
frozen document, so the reboot watch and the flash gate no longer need
a UDP session. The car's rollback flag from the hello was published and
never read; the gate sees it through /version now.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ"
```

(`GalleryView.swift` добавлять, только если менялся.)

---

### Задача 8: Документы

**Files:**
- Modify: `docs/protocol.md` (проза; таблица уже сгенерирована в задаче 1)
- Modify: `firmware/dongle/README.md` (пример `/status`, добавить `/version`)
- Modify: `CLAUDE.md` (абзац о контракте)
- Modify: `docs/superpowers/specs/2026-09-16-v1-removal-design.md` (§7)
- Modify: `docs/superpowers/specs/2026-09-17-version-endpoint-design.md` (`**Статус:**`)

- [ ] **Шаг 1: `docs/protocol.md`**

Рядом с описанием `GET /status` (найти `grep -n "GET /status" docs/protocol.md`) добавить
раздел:

```markdown
### `GET /version` — the one document that never changes

```json
{"device":"ajmiddlecar","fw":"v1.0+879","build":879,"proto":2,"rolled_back":false}
```

Five fields, this order, and nothing else — ever. `device` is who answers; `fw` is the version
as the build prints it and `build` the number after its `+` (`-1` when there is none); `proto`
is the protocol number of everything else this board serves; `rolled_back` is the bootloader's
verdict on the last update, sticky until the next successful one. The app reads this first, for
both boards, decides "foreign / rolled back / behind / newer than me / fine" before it parses
anything that lives under `proto`, and updates a board that is behind — a board that answers
404 predates the endpoint and counts as behind. The adapter serves the same document at
`192.168.7.1:8080/version` with `proto` from its own contract.
```

В прозе `/status`: убрать `device` из перечня групп и примера; фразу «this reply, not
`/status`, is the app's "is this our car" test» у `hello_ack` дополнить: «— for the session;
the launch gate's test is `/version`».

- [ ] **Шаг 2: README адаптера, CLAUDE.md, спеки**

`firmware/dongle/README.md`: в примере `GET /status` удалить строку `"device":{…}`, в `system`
добавить `"idf":"v6.0.2"`; перед ним — пример `GET /version` (адаптерный документ из спеки);
абзац «Five groups: `device` (…), `usb`…» → «Four groups: `usb`…; identity and version live in
`/version`».

`CLAUDE.md`, абзац «The contract»: «the seven status/telemetry groups (`device`, `link`, …)» →
«the six status groups (`link`, `motors`, `radio`, `storage`, `system`, `video`) and the
frozen `/version` document (`device`, `fw`, `build`, `proto`, `rolled_back`) both boards serve».

`docs/superpowers/specs/2026-09-16-v1-removal-design.md`, §7: добавить в конец абзаца
«*Закреплено контрактом 2026-09-17: личность и версия живут в `/version`, формат которого не
меняется — см. `2026-09-17-version-endpoint-design.md`.*»

`docs/superpowers/specs/2026-09-17-version-endpoint-design.md`: `**Статус:** спека, 2026-09-17.`
→ `**Статус:** реализовано 2026-09-17 (план \`docs/superpowers/plans/2026-09-17-version-endpoint.md\`); стенд — см. \`docs/bringup.md\`.`

- [ ] **Шаг 3: Проверка и коммит**

`tools/check_contract.sh` → `no drift` (проза в `protocol.md` вне маркеров генератора).
`grep -rn "status.device\|/status\.device" docs/protocol.md CLAUDE.md firmware/dongle/README.md` → пусто.

```bash
git add docs/protocol.md firmware/dongle/README.md CLAUDE.md docs/superpowers/specs/2026-09-16-v1-removal-design.md docs/superpowers/specs/2026-09-17-version-endpoint-design.md
git commit -m "docs: /version in the protocol, the README and the project notes

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01X57bwCXmUPyAVv5pKqXtfZ"
```

---

## После задач — контроллер, не субагенты

- **Стенд, день-флаг без кабеля** (спека §7): релиз с обоими образами (`tools/release.sh`);
  приложение с HEAD в симуляторе `-viaDongle` при подключённом адаптере на прежнем релизе:
  S11 (404 → обновление адаптера по USB) → адаптер подключается → S30 → S27 (404 → обновление
  машинки через реле) → S28. Затем `curl` обоих `/version`. Запись в `docs/bringup.md`.
- **Артефакт экранов**: карточки A1/A2 (`/version` первым), новая карточка «Проверяю машинку»
  (S30/S31/S32/S23-по-version), B1 без «hello решает версию», S24 как страховка; кадры галереи
  для четырёх новых ситуаций; переснять и перевыпустить.
- **Память**: `launch-ladder-2026-09.md` — лестница с `/version`, S30–S32.
