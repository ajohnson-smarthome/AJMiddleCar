# Выключатель видео — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** кнопка на экране езды, которая выключает видео на машинке: поле `video.enabled` в
конфиге, машинка не стримит при `false` и закрывает поток за ≤100 мс, экран без видео — как до
видео.

**Architecture:** одно поле в контракте, розданное генератором на четыре стороны. На машинке
флаг — ещё одна причина, по которой подписки нет, в чистой арифметике `video_sub.h`; задача
управления видео ничего нового не узнаёт. В приложении — чистое правило «(конфиг, накрыт ли
экран) → (раскладка, слать ли `view`)», кнопка через существующий `ConfigStore.video`, и два
режима `body` одного `DriveView`. Мок чтит флаг, конформанс это проверяет.

**Tech Stack:** JSON-контракт + `tools/gen_contract.py`; C11 (ESP-IDF 6.0.2, host-тесты
plain `cc`); Python 3 (мок, конформанс, unittest); Swift/SwiftUI (host-тесты `swiftc`).

**Spec:** `docs/superpowers/specs/2026-09-15-video-switch-design.md`

## Global Constraints

- Проза (план, документация, коммиты — пояснительная часть) — по-русски; код, комментарии в
  коде, сообщения коммитов — по-английски.
- Генерированные файлы (`cfg_table.inc`, `CarAPI.swift`, `generated.py`, таблица в
  `docs/protocol.md`) **никогда не правятся руками** — только схема и `python3
  tools/gen_contract.py`; `tools/check_contract.sh` должен говорить `no drift`.
- `app/` и `firmware/car/core/` друг на друга не ссылаются; шов — контракт и мок.
- Коммитить только файлы, перечисленные в задаче (`git add <файлы>`), никогда `git add -A`.
  `CLAUDE.md` не трогать. (HUD-работа пользователя, которая лежала в `DriveView.swift` и
  соседях незакоммиченной, закоммичена 5ea8b79 перед задачей 6.)
- Слова `video.state` не меняются: `off`, `idle`, `streaming`.
- Кнопка — форма шестерёнки: 40×32, `p.panel`, обводка `p.line`, скругление 10.
- Переключение экрана — только по подтверждённому ответу машинки (`store.value`), не
  оптимистично.
- Host-тесты: `tools/test-all.sh` зелёный в конце каждой задачи, где он затронут.

---

### Задача 1: Контракт — поле `video.enabled`

**Files:**
- Modify: `contract/car-api.json` (домен `video`, `config.domains`)
- Modify: `tools/test_gen_contract.py`
- Regenerate: `firmware/car/core/main/cfg_table.inc`, `app/AJMiddleCar/Generated/CarAPI.swift`,
  `tools/mock_car/generated.py`, `docs/protocol.md` (таблица между маркерами)

**Interfaces:**
- Produces: `CFG_VIDEO_FIELDS[1] = { "enabled", CFG_BOOL, …, default 1 }` в `cfg_table.inc`
  (порядок полей: `bitrate_kbps`, `enabled`); Swift `Video(bitrate_kbps: Int, enabled: Bool)`,
  `Video.default == Video(bitrate_kbps: 2500, enabled: true)`; Python
  `DOMAINS["video"]["defaults"] == {"bitrate_kbps": 2500, "enabled": True}`.

- [ ] **Шаг 1: Тесты генератора — RED**

В `tools/test_gen_contract.py`:

В `test_config_structs` (класс `TestSwiftEmitter`) заменить строку
`self.assertIn("    static let \`default\` = Video(bitrate_kbps: 2500)", self.lines())` на:

```python
        self.assertIn("    public var enabled: Bool", self.lines())
        self.assertIn("    static let `default` = Video(bitrate_kbps: 2500, enabled: true)", self.lines())
```

В `TestDocEmitter.test_table_has_a_row_per_field_grouped_by_domain` добавить после строки с
`recovery`:

```python
        self.assertIn("| `video` | `enabled` | bool | true \\| false | true |", out)
        self.assertEqual(out.count("| `video` |"), 2)
```

В `TestSchema` (класс, где живёт `test_ranges_match_the_firmware_today`) добавить тест:

```python
    def test_video_domain_has_the_switch_and_the_firmware_default_agrees(self):
        """`enabled` is the video switch (spec 2026-09-15); and the car's own boot default for
        the bitrate must be the schema's — the descriptor table carries the schema's number,
        but a car with nothing in NVS starts from VIDEO_CFG_BITRATE_DEFAULT."""
        video = next(d for d in load()["config"]["domains"] if d["key"] == "video")
        names = [f["name"] for f in video["fields"]]
        self.assertEqual(names, ["bitrate_kbps", "enabled"])
        enabled = video["fields"][1]
        self.assertEqual((enabled["type"], enabled["default"]), ("bool", True))
        bitrate = video["fields"][0]
        src = (ROOT / "firmware" / "car" / "core" / "main" / "video_cfg.h").read_text()
        self.assertIn(f"#define VIDEO_CFG_BITRATE_DEFAULT {bitrate['default']}", src)
        self.assertIn("#define VIDEO_CFG_ENABLED_DEFAULT true", src)
```

- [ ] **Шаг 2: Убедиться, что тесты падают**

Run: `python3 -m unittest tools.test_gen_contract 2>&1 | tail -5`
Expected: `FAILED (failures=3)` — `enabled` нет в схеме, `Video.default` без второго аргумента,
`VIDEO_CFG_BITRATE_DEFAULT 1500` в `video_cfg.h`.

- [ ] **Шаг 3: Схема**

В `contract/car-api.json`, домен `video` (`"key": "video"` в `config.domains`), массив
`fields` — после поля `bitrate_kbps` добавить:

```json
          {"name": "enabled", "type": "bool", "default": true,
           "doc": "the car streams video at all; false and it ignores every view, stops a running stream within 100 ms and puts the sensor in standby — the drive screen's video button, remembered on the car"}
```

(Запятая после закрывающей `}` поля `bitrate_kbps`.) Домен `video` — `"doc": "The FPV
encoder. Applied at the next stream start, not live."` — заменить на:
`"The FPV encoder and the video switch. bitrate_kbps applies at the next stream start;
enabled applies at once."`

- [ ] **Шаг 4: Дефолт в прошивке — `video_cfg.h`**

В `firmware/car/core/main/video_cfg.h` заменить
`#define VIDEO_CFG_BITRATE_DEFAULT 1500` на:

```c
#define VIDEO_CFG_BITRATE_DEFAULT 2500
#define VIDEO_CFG_ENABLED_DEFAULT true
```

(Остальное в `video_cfg.h` — в задаче 2; здесь только два define, чтобы тест контракта прошёл.)

- [ ] **Шаг 5: Регенерация и проверка**

Run: `python3 tools/gen_contract.py && python3 -m unittest tools.test_gen_contract 2>&1 | tail -3 && tools/check_contract.sh`
Expected: `OK`, `contract: no drift`.

Run: `grep -n "enabled" firmware/car/core/main/cfg_table.inc app/AJMiddleCar/Generated/CarAPI.swift | grep -i video`
Expected: строка `    { "enabled", CFG_BOOL, 0, 1, 1, NULL, 0, 1 },` в `CFG_VIDEO_FIELDS`
(в точности как у `recovery.enabled`); `public var enabled: Bool` в `struct Video`;
`CFG_DOMAINS` — `{ "video", "video", CFG_VIDEO_FIELDS, 2 }`.

- [ ] **Шаг 6: Коммит**

```bash
git add contract/car-api.json tools/test_gen_contract.py firmware/car/core/main/cfg_table.inc \
    app/AJMiddleCar/Generated/CarAPI.swift tools/mock_car/generated.py docs/protocol.md \
    firmware/car/core/main/video_cfg.h
git commit -m "feat(contract): video.enabled — the video switch, remembered on the car

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Задача 2: Машинка — флаг в `video_cfg` и в `/config`

**Files:**
- Modify: `firmware/car/core/main/video_cfg.h`, `firmware/car/core/main/video_cfg.c`
- Modify: `firmware/car/core/main/cfg_api.c` (`video_get_v`, `video_set_v`)

**Interfaces:**
- Produces: `bool video_cfg_get_enabled(void)`, `bool video_cfg_set_enabled(bool on)`;
  строка домена в NVS — `{"bitrate_kbps":N,"enabled":true|false}`; старая строка без
  `enabled` читается как `true`.
- Consumes: `CFG_VIDEO_FIELDS` из задачи 1 (порядок: `bitrate_kbps`, `enabled`).

`video_cfg.c` — ESP-IDF-клей (cJSON, FreeRTOS-мьютекс, NVS через `cfg_json`), как
`recovery.c`; host-тестов на него нет, и этот план их не заводит: разбор — три строки на cJSON,
пинуется стендом (раздел «Стенд» спеки: «выключить, перезагрузить машинку — не стримит»).
Дефолты и порядок полей пинует тест контракта из задачи 1.

- [ ] **Шаг 1: Заголовок**

`firmware/car/core/main/video_cfg.h` целиком:

```c
#ifndef VIDEO_CFG_H
#define VIDEO_CFG_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// The `video` domain of /config: the encoder's target bitrate, in kbit/s, and the video
// switch. Bounds here are the contract's (test_gen_contract's ranges test reads them from
// this file, and its defaults test reads the two defaults). The bitrate is read at stream
// start — video_link asks video_cfg_get_bitrate() when it opens the encoder, so a change
// lands on the next stream rather than mid-frame. The switch is read on every tick of the
// video control task: off, and a running stream ends within that tick and no view opens one.
#define VIDEO_CFG_BITRATE_MIN     500
#define VIDEO_CFG_BITRATE_MAX     3000
#define VIDEO_CFG_BITRATE_DEFAULT 2500
#define VIDEO_CFG_ENABLED_DEFAULT true

// Load the domain from NVS (defaults above; a stored string without `enabled` — written by
// a firmware before the switch existed — reads as enabled, so an update never turns the
// picture off).
esp_err_t video_cfg_init(void);
// Clamped to the bounds. Returns false only when the lock could not be taken in time.
bool video_cfg_set_bitrate(uint16_t kbps);
uint16_t video_cfg_get_bitrate(void);
bool video_cfg_set_enabled(bool on);
bool video_cfg_get_enabled(void);
// Persist as {"bitrate_kbps":N,"enabled":B} under the NVS key "video".
esp_err_t video_cfg_save(void);

#endif // VIDEO_CFG_H
```

- [ ] **Шаг 2: Реализация**

В `firmware/car/core/main/video_cfg.c`:

После `static uint16_t s_bitrate = VIDEO_CFG_BITRATE_DEFAULT;` добавить:

```c
static bool     s_enabled = VIDEO_CFG_ENABLED_DEFAULT;
```

После функции `video_cfg_get_bitrate` добавить:

```c
bool video_cfg_set_enabled(bool on) {
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    s_enabled = on;
    xSemaphoreGive(s_lock);
    return true;
}

bool video_cfg_get_enabled(void) {
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return s_enabled;
    bool v = s_enabled;
    xSemaphoreGive(s_lock);
    return v;
}
```

`video_cfg_save` заменить на:

```c
esp_err_t video_cfg_save(void) {
    char buf[56];
    snprintf(buf, sizeof(buf), "{\"bitrate_kbps\":%u,\"enabled\":%s}",
             video_cfg_get_bitrate(), video_cfg_get_enabled() ? "true" : "false");
    return cfg_json_save("video", buf);
}
```

В `video_cfg_init`: буфер `char buf[40];` → `char buf[56];`, а внутри блока
`if (cfg_json_load(...)) { ... }` после разбора `bitrate_kbps` (перед `cJSON_Delete(j);`)
добавить:

```c
        cJSON *je = cJSON_GetObjectItemCaseSensitive(j, "enabled");
        if (cJSON_IsBool(je)) s_enabled = cJSON_IsTrue(je);   /* absent: the default, on */
```

и лог в конце заменить на:

```c
    ESP_LOGI(TAG, "bitrate_kbps = %u, %s (boot)", s_bitrate, s_enabled ? "enabled" : "disabled");
```

- [ ] **Шаг 3: `/config`**

В `firmware/car/core/main/cfg_api.c` заменить две строки

```c
static void video_get_v(int32_t *o) { o[0] = video_cfg_get_bitrate(); }
static bool video_set_v(const int32_t *v) { return video_cfg_set_bitrate((uint16_t)v[0]); }
```

на:

```c
static void video_get_v(int32_t *o) { o[0] = video_cfg_get_bitrate(); o[1] = video_cfg_get_enabled() ? 1 : 0; }
static bool video_set_v(const int32_t *v) {
    return video_cfg_set_bitrate((uint16_t)v[0]) && video_cfg_set_enabled(v[1] != 0);
}
```

- [ ] **Шаг 4: Сборка**

Run: `cd firmware/car/core && source ../../../tools/env-p4.sh >/dev/null && idf.py build 2>&1 | grep -E "error|Project build complete" | tail -2`
Expected: `Project build complete.`

- [ ] **Шаг 5: Коммит**

```bash
git add firmware/car/core/main/video_cfg.h firmware/car/core/main/video_cfg.c firmware/car/core/main/cfg_api.c
git commit -m "feat(car): video.enabled in the video domain — stored, served, and the boot default the contract says

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Задача 3: Машинка — флаг в подписке и остановка потока

**Files:**
- Modify: `firmware/car/core/main/video_sub.h`
- Modify: `firmware/car/core/test/test_video_sub.c`
- Modify: `firmware/car/core/main/video_link.c` (`ctl_task`)

**Interfaces:**
- Produces: `video_sub_view(s, owner_sid, f, now_ms, enabled, force_idr)` — новый параметр
  `bool enabled` перед `force_idr`; `video_sub_expired` без изменений (вызывающий передаёт
  `owner_alive && enabled`).
- Consumes: `video_cfg_get_enabled()` из задачи 2.

- [ ] **Шаг 1: Тест — RED**

В `firmware/car/core/test/test_video_sub.c` у **каждого** существующего вызова
`video_sub_view(&s, X, &Y, T, &idr)` вставить `true` перед `&idr`:
`video_sub_view(&s, X, &Y, T, true, &idr)` (sed: `s/, &idr)/, true, \&idr)/`). Затем перед
`puts(...)`/`return 0` в конце `main` добавить:

```c
    /* The switch (spec 2026-09-15): off, the owner's view is ignored — nothing opens. */
    video_sub_init(&s);
    assert(video_sub_view(&s, "7f3a91c2", &f, 7000, false, &idr) == VS_IGNORE && !s.subscribed && !idr);
    assert(video_sub_view(&s, "7f3a91c2", &k, 7000, false, &idr) == VS_IGNORE && !idr);
    /* On: opens as before. Then the flag drops mid-stream — the caller folds it into
       `owner_alive`, and the subscription is over on that very call, not at the timeout. */
    assert(video_sub_view(&s, "7f3a91c2", &f, 7100, true, &idr) == VS_START);
    assert(!video_sub_expired(&s, true, 7200));
    assert(video_sub_expired(&s, false, 7200));
    /* Back on: nothing opens by itself — only the next view does. */
    video_sub_end(&s);
    assert(!s.subscribed);
    assert(!video_sub_expired(&s, true, 7300));
    assert(video_sub_view(&s, "7f3a91c2", &f, 7400, true, &idr) == VS_START);
```

- [ ] **Шаг 2: Убедиться, что не собирается**

Run: `make -C firmware/car/core/test test_video_sub 2>&1 | grep -c "error"`
Expected: число > 0 (`too many arguments to function 'video_sub_view'`).

- [ ] **Шаг 3: `video_sub.h`**

Заменить сигнатуру и начало `video_sub_view`:

```c
// `owner_sid` is rt_link's owner ("" when nobody). `enabled` is the video switch
// (`video.enabled` in /config): off, every view is ignored, whoever sends it — the switch is
// one more reason there is no subscription, not a state of its own. *force_idr is set only
// when the view asked for a keyframe and VIDEO_IDR_MIN_MS has passed since the last one
// forced — never on VS_START, since a fresh stream begins with an IDR anyway.
static inline video_sub_verdict_t video_sub_view(video_sub_t *s, const char *owner_sid,
                                                 const control_frame_t *f, uint32_t now_ms,
                                                 bool enabled, bool *force_idr) {
    *force_idr = false;
    if (!enabled) return VS_IGNORE;
    if (f->type != CT_VIEW) return VS_IGNORE;
```

(остальное тело без изменений). Комментарий над `video_sub_expired` заменить на:

```c
// True when a live subscription should end: the phone stopped asking, or the rt session it
// was tied to is gone — or the switch went off; the caller folds that into `owner_alive`,
// which is why a stream ends on the control task's next tick rather than at the timeout.
```

- [ ] **Шаг 4: Тест зелёный**

Run: `make -C firmware/car/core/test test_video_sub && ./firmware/car/core/test/test_video_sub`
Expected: `test_video_sub: OK`.

- [ ] **Шаг 5: `video_link.c`**

В `ctl_task` заменить

```c
        char owner[CONTROL_SID_MAX];
        rt_link_owner_sid(owner);
        uint32_t t = now_ms();
```

на:

```c
        char owner[CONTROL_SID_MAX];
        rt_link_owner_sid(owner);
        uint32_t t = now_ms();
        /* The switch is read here, once per tick, and nowhere else: a view meets it as one
           more reason to be ignored, and a running stream meets it as one more way for the
           subscription to be over — on this tick, not at the 3 s timeout. */
        bool enabled = video_cfg_get_enabled();
```

вызов `video_sub_view(&sub, owner, &f, t, &idr)` → `video_sub_view(&sub, owner, &f, t, enabled, &idr)`;
и `if (video_sub_expired(&sub, owner[0] != '\0', t)) {` → `if (video_sub_expired(&sub, owner[0] != '\0' && enabled, t)) {`,
а лог в этой ветке `ESP_LOGI(TAG, "viewer gone — stream ends");` →
`ESP_LOGI(TAG, enabled ? "viewer gone — stream ends" : "video switched off — stream ends");`.

- [ ] **Шаг 6: Сборка и все host-тесты**

Run: `cd firmware/car/core && idf.py build 2>&1 | grep -E "error|Project build complete" | tail -2 && make -C test run 2>&1 | tail -2`
Expected: `Project build complete.`, последняя строка — `frame_crop: ok` (все тесты прошли).

- [ ] **Шаг 7: Коммит**

```bash
git add firmware/car/core/main/video_sub.h firmware/car/core/test/test_video_sub.c firmware/car/core/main/video_link.c
git commit -m "feat(car): the video switch ends the subscription — no view opens one, a running stream ends on the next tick

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Задача 4: Мок чтит флаг, конформанс проверяет

**Files:**
- Modify: `tools/mock_car/video.py`
- Modify: `tools/mock_car/test_video.py`
- Modify: `tools/conformance_video.py`

**Interfaces:**
- Consumes: `car.config["video"]["enabled"]` (мок хранит конфиг доменов как `dict`,
  `CarState.apply_config` уже кладёт туда `enabled` из задачи 1).

- [ ] **Шаг 1: Тест мока — RED**

В `tools/mock_car/test_video.py` в `_FakeCar` добавить поле:

```python
class _FakeCar:
    video_state = "idle"
    video_fps = 0
    video_kbps = 0
    config = {"video": {"bitrate_kbps": 2500, "enabled": True}}
```

и новый класс перед `if __name__ == "__main__":`:

```python
def _view(sid="sid"):
    return json.dumps({K["proto"]: PROTO, K["type"]: T["view"], K["session"]: sid}).encode()


class TheSwitch(unittest.TestCase):
    """`video.enabled` false: a view opens nothing, and a stream that is running ends on the
    next tick — the car's rule, so the conformance sweep can tell a car that ignores the
    switch from one that honours it."""

    def test_off_ignores_the_owners_view(self):
        car = _FakeCar()
        car.config = {"video": {"bitrate_kbps": 2500, "enabled": False}}
        v = VideoLink(car, _FakeLink(), SC + SPS + SC + PPS + SC + IDR, loop=_FakeLoop())
        v.transport = _FakeTransport()
        v.datagram_received(_view(), ("127.0.0.1", 40000))
        self.assertIsNone(v.peer)
        self.assertEqual(car.video_state, "idle")

    def test_switching_off_mid_stream_ends_it(self):
        car = _FakeCar()
        car.config = {"video": {"bitrate_kbps": 2500, "enabled": True}}
        v = VideoLink(car, _FakeLink(), SC + SPS + SC + PPS + SC + IDR, loop=_FakeLoop())
        v.transport = _FakeTransport()
        v.datagram_received(_view(), ("127.0.0.1", 40000))
        self.assertIsNotNone(v.peer)
        car.config["video"]["enabled"] = False
        self.assertTrue(v.tick(0.0))          # one tick of run(): stopped
        self.assertIsNone(v.peer)
        self.assertEqual(car.video_state, "idle")
```

и в шапке файла после `from video import VideoLink, access_units   # noqa: E402` добавить:

```python
import json
from generated import PROTO, RT   # noqa: E402
K, T = RT["keys"], RT["types"]
```

(`SC`, `SPS`, `PPS`, `IDR` уже определены в файле — существующий тест ими пользуется.)

- [ ] **Шаг 2: Убедиться, что падает**

Run: `python3 tools/mock_car/test_video.py 2>&1 | tail -3`
Expected: `FAILED` — `AttributeError: 'VideoLink' object has no attribute 'tick'` и/или
`peer` не `None` при выключенном.

- [ ] **Шаг 3: `video.py`**

В `datagram_received` после проверки сессии (`if self.link.session is None or ...: return`)
добавить:

```python
        if not self.car.config["video"]["enabled"]:
            return                          # the switch: a view opens nothing, whoever sends it
```

Вынести из `run()` тело одной итерации в метод `tick(now) -> bool` (True — поток остановлен
или не идёт, False — кадр отправлен), чтобы тест мог позвать его без event loop:

```python
    def tick(self, now):
        """One period of the sender. Returns True when there was nothing to send: no viewer,
        or the stream just ended (session over, viewer gone, switch off)."""
        if self.peer is None:
            return True
        if self.link.session is None:
            self._stop("session over")
            return True
        if not self.car.config["video"]["enabled"]:
            self._stop("video switched off")   # the car's rule: within one tick, not the timeout
            return True
        if now - self.last_view > timeout_s():
            self._stop("viewer gone")
            return True
        if self.want_key:
            self.want_key = False
            idr_idx = self._last_idr_at_or_before(self.pos)
            au, key = self.units[idr_idx][0], True
        else:
            au, key = self.units[self.pos]
            self.pos = (self.pos + 1) % len(self.units)
        for d in chunks(au, self.stream, self.frame, key, int(now * 1000) & 0xFFFFFFFF):
            self._emit(d)
        self.frame = (self.frame + 1) & 0xFFFF
        self._sent_frames += 1
        if now - self._sec_at >= 1.0:
            self.car.video_fps = self._sent_frames
            self.car.video_kbps = int(self._sent_bytes * 8 / 1000 / (now - self._sec_at))
            self._sent_frames = self._sent_bytes = 0
            self._sec_at = now
        return False

    async def run(self):
        period = 1.0 / VIDEO["fps"]
        next_at = self.loop.time()
        while True:
            next_at += period
            await asyncio.sleep(max(0.0, next_at - self.loop.time()))
            self.tick(self.loop.time())
```

и на уровне модуля:

```python
def timeout_s():
    return VIDEO["subscribe_timeout_ms"] / 1000
```

(Комментарий R3 про повтор последнего IDR переносится в `tick` как был.)

- [ ] **Шаг 4: Тесты мока зелёные**

Run: `python3 tools/mock_car/test_video.py 2>&1 | tail -2 && python3 tools/mock_car/test_state.py 2>&1 | tail -1`
Expected: `OK` оба.

- [ ] **Шаг 5: Конформанс — проверка выключателя**

В `tools/conformance_video.py`: добавить в шапку `import urllib.request`, в
`VideoConformance.__init__` параметр `http_port` (`self.http = f"http://{host}:{http_port}"`),
и метод:

```python
    def post_config(self, body):
        req = urllib.request.Request(self.http + "/config", data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status

    def check_switch(self, rt, v, view, seq):
        """`video.enabled` off: two seconds of views, not one datagram. On: a frame, an IDR,
        within two seconds. The stream the run just watched ends within a tick, not at the
        subscribe timeout — a datagram after 0.5 s is a car that ignores the switch. The rt
        session is kept alive at command_hz throughout (the car's watchdog is 300 ms), and
        views go every subscribe_ms as the phone would send them."""
        drive_period = 1.0 / RT["command_hz"]
        view_period = VIDEO["subscribe_ms"] / 1000
        next_drive = next_view = 0.0

        def keepalive():
            nonlocal seq, next_drive, next_view
            now = time.monotonic()
            if now >= next_drive:
                seq += 1
                rt.sendto(enc({K["proto"]: PROTO, K["type"]: T["drive"], K["seq"]: seq,
                               K["throttle"]: 0, K["turn"]: 0}), self.rt_addr)
                next_drive = now + drive_period
                try:
                    while True:
                        rt.recvfrom(RT["max_datagram"])
                except (socket.timeout, BlockingIOError):
                    pass
            if now >= next_view:
                v.sendto(view, self.video_addr)
                next_view = now + view_period

        self.check(self.post_config({"video": {"enabled": False}}) == 200, "POST video.enabled=false")
        t0 = time.monotonic()
        while time.monotonic() - t0 < 0.5:          # the tick the car is allowed to end the stream in
            keepalive()
            try:
                v.recvfrom(HDR + VIDEO["chunk_bytes"] + 64)
            except socket.timeout:
                pass
        heard = 0
        t0 = time.monotonic()
        while time.monotonic() - t0 < 2.0:
            keepalive()
            try:
                v.recvfrom(HDR + VIDEO["chunk_bytes"] + 64)
                heard += 1
            except socket.timeout:
                pass
        self.check(heard == 0, f"switched off, but {heard} datagram(s) still arrived")

        self.check(self.post_config({"video": {"enabled": True}}) == 200, "POST video.enabled=true")
        rx = Receiver()
        got_idr = False
        next_view = 0.0                                # the first view goes at once
        t0 = time.monotonic()
        while time.monotonic() - t0 < 2.0 and not got_idr:
            keepalive()
            try:
                data, _ = v.recvfrom(HDR + VIDEO["chunk_bytes"] + 64)
            except socket.timeout:
                continue
            ev, frame = rx.feed(data)
            if ev == Receiver.FRAME and 5 in nal_types(frame):
                got_idr = True
        self.check(got_idr, "switched back on, but no keyframe within 2 s")
        return seq
```

В `run()` перед `rt.sendto(enc({... T["bye"] ...}))` добавить `seq = self.check_switch(rt, v, view, seq)`
(bye идёт с `seq + 1`, как и было); в `main()` — аргумент
`p.add_argument("--http-port", type=int, default=8080, help="the car's REST port (the mock's --port)")`
и передать `a.http_port` в конструктор. В `tools/test-all.sh` вызов
`python3 tools/conformance_video.py 127.0.0.1 --rt-port "$RT_PORT" --video-port "$VIDEO_PORT" --seconds 6`
получает ещё `--http-port "$PORT"` (`$PORT` — тот, что передаётся моку как `--port`).

Выключатель возвращается в `true` самим тестом, так что конфиг мока после прогона — как до.

- [ ] **Шаг 6: Весь набор**

Run: `tools/test-all.sh 2>&1 | tail -4`
Expected: `video conformance: all checks passed`, `== all green ==`.

- [ ] **Шаг 7: Коммит**

```bash
git add tools/mock_car/video.py tools/mock_car/test_video.py tools/conformance_video.py tools/test-all.sh
git commit -m "feat(mock): the mock honours video.enabled, and the conformance sweep checks the switch both ways

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Задача 5: Приложение — правило режима экрана (чистое)

**Files:**
- Create: `app/AJMiddleCar/DriveMode.swift`
- Create: `app/tests/drivemode/main.swift`, `app/tests/drivemode/sources`

**Interfaces:**
- Produces: `enum DriveMode { case hud, classic }`, `struct DriveScreenState: Equatable { let
  mode: DriveMode; let watching: Bool }`, `enum DriveModeRule { static func state(config:
  Video?, covered: Bool) -> DriveScreenState }`.

- [ ] **Шаг 1: Тест — RED**

`app/tests/drivemode/sources`:

```
DriveMode.swift
```

`app/tests/drivemode/main.swift`:

```swift
// Host test for DriveModeRule — which drive screen to show and whether to subscribe to
// video, from the car's `video` config and whether a sheet covers the screen
// (docs/superpowers/specs/2026-09-15-video-switch-design.md, §3).
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// The config not read yet: the layout from before video, and no subscription — the car
// would ignore the views if the switch is off, and the HUD would flash if it is on.
check(DriveModeRule.state(config: nil, covered: false) == DriveScreenState(mode: .classic, watching: false),
      "no config yet: classic, not watching")

// Off: classic, not watching, sheet or no sheet.
check(DriveModeRule.state(config: Video(bitrate_kbps: 2500, enabled: false), covered: false)
      == DriveScreenState(mode: .classic, watching: false), "off: classic, not watching")
check(DriveModeRule.state(config: Video(bitrate_kbps: 2500, enabled: false), covered: true)
      == DriveScreenState(mode: .classic, watching: false), "off under a sheet: still classic")

// On: the HUD; a sheet over it stops the views but not the layout underneath.
check(DriveModeRule.state(config: Video(bitrate_kbps: 2500, enabled: true), covered: false)
      == DriveScreenState(mode: .hud, watching: true), "on: hud, watching")
check(DriveModeRule.state(config: Video(bitrate_kbps: 2500, enabled: true), covered: true)
      == DriveScreenState(mode: .hud, watching: false), "on under a sheet: hud, not watching")

if failures > 0 { print("drivemode: \(failures) failure(s)"); exit(1) }
print("drivemode: ok")
```

- [ ] **Шаг 2: Убедиться, что не собирается**

Run: `swiftc -o /tmp/hosttest_drivemode app/AJMiddleCar/Generated/CarAPI.swift app/AJMiddleCar/Generated/DongleAPI.swift app/tests/drivemode/main.swift 2>&1 | grep -c "error"`
Expected: число > 0 (`cannot find 'DriveModeRule' in scope`).

- [ ] **Шаг 3: `DriveMode.swift`**

```swift
/// Which drive screen to show, and whether to ask the car for video. Pure, host-tested
/// (`app/tests/drivemode`): the car's `video` config and whether a sheet covers the screen
/// in, the layout and the subscription gate out — the one place both are decided, so the
/// screen cannot show the HUD while not watching, or watch while showing the old layout.
///
/// `nil` — the config has not been read yet — is the layout from before video, and no
/// views: the car ignores them when the switch is off, and the HUD would flash on and then
/// switch away if it is. The config is prefetched when the car is met, so this is a moment
/// at most (docs/superpowers/specs/2026-09-15-video-switch-design.md, §3).
enum DriveMode: Equatable {
    /// The picture as the screen, instruments on its edges (drive-hud-design).
    case hud
    /// The screen from before video: diagram in the middle, sticks and tricks below.
    case classic
}

struct DriveScreenState: Equatable {
    let mode: DriveMode
    let watching: Bool
}

enum DriveModeRule {
    static func state(config: Video?, covered: Bool) -> DriveScreenState {
        guard let config, config.enabled else { return DriveScreenState(mode: .classic, watching: false) }
        return DriveScreenState(mode: .hud, watching: !covered)
    }
}
```

- [ ] **Шаг 4: Тест зелёный**

Run: `swiftc -o /tmp/hosttest_drivemode app/AJMiddleCar/Generated/CarAPI.swift app/AJMiddleCar/Generated/DongleAPI.swift app/AJMiddleCar/DriveMode.swift app/tests/drivemode/main.swift && /tmp/hosttest_drivemode`
Expected: `drivemode: ok`.

- [ ] **Шаг 5: Коммит**

```bash
git add app/AJMiddleCar/DriveMode.swift app/tests/drivemode/main.swift app/tests/drivemode/sources
git commit -m "feat(app): DriveModeRule — which drive screen, and whether to watch, from the car's video switch

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Задача 6: Приложение — кнопка и два режима экрана езды

**Files:**
- Modify: `app/AJMiddleCar/DriveView.swift` (поверх незакоммиченной работы пользователя —
  см. Global Constraints)
- Modify: `app/AJMiddleCar/L.swift`, `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings`

**Interfaces:**
- Consumes: `DriveModeRule` (задача 5), `ConfigStore.shared.video` (`value`, `isBusy`,
  `save`, `loadIfNeeded`), `Video(bitrate_kbps:enabled:)` (задача 1).

- [ ] **Шаг 1: Строки**

В `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings` после `"video.value"` добавить:

```
"video.on"           = "Видео включено";
"video.off"          = "Видео выключено";
```

В `app/AJMiddleCar/L.swift` рядом с `videoStats`:

```swift
    static var videoOn: String { s("video.on") }
    static var videoOff: String { s("video.off") }
```

- [ ] **Шаг 2: Состояние и кнопка в `DriveView`**

После `@State private var padWasActive = false` добавить:

```swift
    /// The car's `video` domain — the switch lives there, and the screen follows the car's
    /// answer, never the tap: a tap that did not land leaves the picture as it was.
    @ObservedObject private var videoCfg = ConfigStore.shared.video
    /// When the last save failed — the button wears `warn` for 600 ms, and that is all the
    /// drive screen says about it.
    @State private var videoToggleFailedAt: Date = .distantPast
```

После `private var signalColor ...` добавить:

```swift
    private var screen: DriveScreenState {
        DriveModeRule.state(config: videoCfg.value, covered: showSettings || showCalib)
    }

    /// The video switch: same shape as the gear next to it. The tap posts the whole domain
    /// (bitrate as the car has it), disabled while the answer is on its way.
    private var videoButton: some View {
        let on = videoCfg.value?.enabled ?? false
        let failed = Date().timeIntervalSince(videoToggleFailedAt) < 0.6
        return Button {
            guard let cur = videoCfg.value else { return }
            Task {
                if await !videoCfg.save(Video(bitrate_kbps: cur.bitrate_kbps, enabled: !cur.enabled)) {
                    videoToggleFailedAt = Date()
                    // A state change is what redraws the button: set the mark, and clear
                    // it 650 ms later so the stroke goes back to `line` without a tap.
                    try? await Task.sleep(for: .milliseconds(650))
                    videoToggleFailedAt = .distantPast
                }
            }
        } label: {
            Image(systemName: on ? "video" : "video.slash")
                .font(.system(size: 18, weight: .medium))
                .foregroundStyle(p.text)
                .frame(width: 40, height: 32)
                .background(p.panel)
                .clipShape(RoundedRectangle(cornerRadius: 10))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(failed ? p.warn : p.line))
        }
        .disabled(videoCfg.value == nil || videoCfg.isBusy)
        .accessibilityLabel(on ? L.videoOn : L.videoOff)
    }

    private var gearButton: some View {
        Button { showSettings = true } label: {
            Image(systemName: "gearshape")
                .font(.system(size: 18, weight: .medium))
                .foregroundStyle(p.text)
                .frame(width: 40, height: 32)
                .background(p.panel)
                .clipShape(RoundedRectangle(cornerRadius: 10))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(p.line))
        }
        .padding(.leading, 8)
        .disabled(showCalib)   // can't bypass mandatory calibration via Settings
    }
```

- [ ] **Шаг 3: Верхний ряд HUD — кнопка между переключателем и шестерёнкой**

В теле HUD (внутри `GeometryReader`), в верхнем `HStack` заменить

```swift
                    SchemeToggle(scheme: $schemeRaw, palette: p)
                    Button { showSettings = true } label: {
                        Image(systemName: "gearshape")
                            ...
                    }
                    .padding(.leading, 8)
                    .disabled(showCalib)   // can't bypass mandatory calibration via Settings
```

(весь блок `Button { showSettings = true } … .disabled(showCalib)`) на:

```swift
                    SchemeToggle(scheme: $schemeRaw, palette: p)
                    videoButton.padding(.leading, 8)
                    gearButton
```

- [ ] **Шаг 4: Два режима `body`**

Текущее `var body: some View { GeometryReader { geo in ... } .onAppear {...} ... }` разбить:
содержимое `GeometryReader { ... }` (до первого модификатора `.onAppear`) вынести в
`private var hud: some View { GeometryReader { geo in ... } }` без изменений, а `body`
сделать:

```swift
    var body: some View {
        Group {
            if screen.mode == .hud { hud } else { classic }
        }
        .task { await videoCfg.loadIfNeeded() }
        .onAppear { if !preview { video.setWatching(screen.watching) } }
        // (комментарий про bye — как был)
        .onDisappear { if !preview { intent.neutral(); video.setWatching(false) } }
        .onChange(of: screen.watching) { _, watching in
            // One gate for all three reasons not to watch — a sheet over the screen, the
            // switch off on the car, the config not read yet (DriveModeRule).
            if !preview { video.setWatching(watching) }
        }
        // далее — все существующие модификаторы .onReceive / .sheet / .onChange(of: telemetry?.motors.calibrated) без изменений
    }
```

Существующий `.onChange(of: showSettings || showCalib) { _, covered in ... }` **удалить** —
его роль взял `onChange(of: screen.watching)`.

Добавить раскладку до видео (из `DriveView` на 81b96ae, плюс кнопка видео и без
`statusBar` внизу — предупреждения там же, где в HUD не были бы видны: под трюками):

```swift
    /// The screen from before video (81b96ae), for when the car's switch is off: diagram in
    /// the middle, sticks in the corners, tricks and the warnings below. Same components as
    /// the HUD; only the arrangement is its own.
    private var classic: some View {
        ZStack {
            p.bg.ignoresSafeArea()

            VStack {
                HStack {
                    HStack(spacing: 7) {
                        SignalBars(level: linkUp ? signalLevel : 0, color: linkUp ? signalColor : .red)
                        Text(linkUp ? L.driveConnected : L.driveSearching)
                            .font(.system(size: 12)).foregroundStyle(p.muted)
                    }
                    Spacer()
                    SchemeToggle(scheme: $schemeRaw, palette: p)
                    videoButton.padding(.leading, 8)
                    gearButton
                }
                .padding(.horizontal, 18).padding(.top, 8)
                Spacer()
            }

            HStack(spacing: 28) {
                PowerBar(value: sides.left, palette: p)
                DriveDiagram(t: intent.t, y: intent.y, palette: p)
                PowerBar(value: sides.right, palette: p)
            }

            if scheme == .arcade {
                HStack {
                    Spacer()
                    JoystickView(palette: p) { x, y in
                        if arcX == 0 && arcY == 0 && (x != 0 || y != 0) { haptics.tick() }
                        arcX = x; arcY = y; push()
                    }
                    .padding(.trailing, 24)
                }
                .padding(.bottom, 16)
                .frame(maxHeight: .infinity, alignment: .bottom)
            } else {
                HStack {
                    JoystickView(vertical: true, palette: p) { _, y in leftY = y; push() }.padding(.leading, 24)
                    Spacer()
                    JoystickView(vertical: true, palette: p) { _, y in rightY = y; push() }.padding(.trailing, 24)
                }
                .padding(.bottom, 16)
                .frame(maxHeight: .infinity, alignment: .bottom)
            }

            VStack(spacing: 6) {
                Spacer()
                TricksControl(palette: p, running: intent.runningTrick, startedAt: intent.trickStartedAt,
                              onSelect: { intent.startTrick($0) },
                              onStop: { intent.stopTrick() },
                              debugOpen: previewTricksOpen)
                warnings          // amber only, and only while something is wrong — under the FAB, as before video
            }
            .padding(.bottom, 16)
        }
    }
```

(`DriveDiagram(t:y:palette:)` — та же сигнатура, что в HUD; `warnings` — уже существующее
свойство HUD-версии, с фоном только при `hasWarnings`.)

- [ ] **Шаг 5: Сборка приложения и host-тесты**

Run: `cd app && xcodegen generate >/dev/null && xcodebuild build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle -quiet 2>&1 | grep -E "error|BUILD" | head -5; echo "exit ${PIPESTATUS[0]}"`
Expected: без `error`, `exit 0`.

Run: `tools/test-all.sh 2>&1 | tail -3`
Expected: `== all green ==` (включая `drivemode: ok`).

- [ ] **Шаг 6: Галерея (по желанию, если нужен скриншот classic)**

`GalleryView.swift` — файл пользователя, не трогать. Для глаз: мок + симулятор, `POST
/config {"video":{"enabled":false}}` на `127.0.0.1:8080` — экран езды переключится на
classic; скриншот `xcrun simctl io booted screenshot` + `sips -r -90`.

- [ ] **Шаг 7: Коммит**

HUD-работа пользователя закоммичена раньше (5ea8b79), `DriveView.swift` чист — коммитятся
три файла целиком:

```bash
git add app/AJMiddleCar/L.swift app/AJMiddleCar/Resources/ru.lproj/Localizable.strings app/AJMiddleCar/DriveView.swift
git commit -m "feat(app): the video switch on the drive screen — a button by the gear, the old layout while the car's video is off

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Задача 7: Документация и стенд

**Files:**
- Modify: `docs/protocol.md` (раздел «Configuration — the `video` domain», прозой; таблица —
  генератором уже обновлена в задаче 1)
- Modify: `docs/bringup.md` (стендовый список видео)
- Modify: `docs/superpowers/specs/2026-09-15-video-switch-design.md` (статус)

- [ ] **Шаг 1: `docs/protocol.md`**

В разделе `### Configuration — the \`video\` domain` первый абзац начать с
«Two fields, generated into the domain table below along with the other five — …» и после
абзаца про `fps` добавить:

```markdown
`enabled` is the video switch. It is a setting, remembered on the car, not a state of the
camera: `false` and the car ignores every `view`, whoever sends it, ends a running stream on
the video control task's next tick (≤100 ms, not the 3 s subscribe timeout) and puts the
sensor in standby; `video.state` reads `idle`, and the app knows *why* from this field, not
from telemetry. `true` starts nothing by itself — the next `view` does. The drive screen's
video button is a POST of this field; while it is off the screen is the one from before
video, and no views are sent (`docs/superpowers/specs/2026-09-15-video-switch-design.md`).
```

- [ ] **Шаг 2: `docs/bringup.md`**

В стендовый список видео (`### Video — …`) добавить пункт:

```markdown
- [ ] **The video switch.** The drive screen's button: the picture goes, the screen is the
      old layout, `video.state` is `idle` within a second, `relay.video_kbps` on the adapter
      reads 0 and the car's log says the sensor stopped; back on, the HUD and a first frame
      within a second. Off, then reboot the car, then the drive screen: old layout, no
      stream. A probe from the Mac (`tools/conformance_video.py 192.168.7.1`) while off:
      not one datagram. The bitrate slider under Settings leaves `enabled` as it was.
```

- [ ] **Шаг 3: Статус спеки и одна поправка по итогам реализации**

В `docs/superpowers/specs/2026-09-15-video-switch-design.md` строку `**Статус:**` заменить на:
`**Статус:** реализовано 2026-09-15 (план `docs/superpowers/plans/2026-09-15-video-switch.md`);
стенд — см. `docs/bringup.md`.`

Там же в §1 фразу «`POST /config {"video":{"enabled":false}}` (домен целиком или
подмножество — как у остальных доменов, `cfg_api.c` валидирует всё тело до применения)»
заменить на: «`POST /config {"video":{"bitrate_kbps":2500,"enabled":false}}` — домен
**целиком**: `/config` принимает любое подмножество доменов, но каждый присутствующий домен
должен быть полным (`cfg_api.c` и мок отвечают `missing_field` на `{"video":{"enabled":false}}`
без битрейта); кнопка в приложении и конформанс шлют домен целиком с текущим битрейтом».

- [ ] **Шаг 4: Коммит**

```bash
git add docs/protocol.md docs/bringup.md docs/superpowers/specs/2026-09-15-video-switch-design.md docs/superpowers/plans/2026-09-15-video-switch.md
git commit -m "docs: the video switch — protocol prose, the bench item, the spec's status

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## После плана

Прошивка машинки — через релей на стенд (`POST http://192.168.7.1/ota`), приложение — в
симулятор с `-viaDongle`; релиз (`tools/release.sh` из чистого клона `main`, как 2026-09-15) —
решение пользователя, после стенда.
