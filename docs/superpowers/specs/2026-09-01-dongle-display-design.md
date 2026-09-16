# The dongle's display

A 0.96" SSD1306, 128 × 64, monochrome, on the dongle's I²C. What it shows, why those things and
not others, and what the firmware must grow to feed it.

The screens themselves were drawn pixel-exact and agreed one at a time before this document
existed: <https://claude.ai/code/artifact/94434cc8-b754-4214-915c-f20a022806bc>. This spec
carries the decisions and their reasons; the artifact carries the pictures.

## Why the dongle deserves a screen at all

**It is the only component that sees both hops.** The app sees the phone's side; the car sees its
own. When the link fails, neither can say which half broke — and answering that question has
already cost this project whole bench sessions. On 2026-08-31 the dongle was joined to
AJMiddleCar, addressed, RSSI −27 dBm, and *every* relayed datagram failed with `errno 12`. Nothing
in the system could say so. The evidence was a serial log on a laptop.

That is the screen's job: not decoration, and not a second copy of what the app already shows.

## The hard boundary: only its own measurements

`relay_udp.h` and `relay_tcp.h` both state the rule the whole dongle lives under — "it never
parses a control frame or a telemetry push; it only moves them". The screen does not get to
weaken that.

So the display may show: the station's state, SSID, RSSI, channel, its own address and gateway,
USB link state, its own firmware version and rollback flag, uptime, heap, relay slot occupancy,
packet counts, and the last forwarding `errno`.

It may **not** show the car's battery, speed, or anything else that requires reading what passes
through — and, less obviously, it may not label its packet counters in **hertz**. The 10 Hz
control cadence belongs to `contract/car-api.json`; it is the app's and the car's, not the
dongle's. Counting datagrams is the dongle's own observation. Calling them a control frequency
borrows a meaning the dongle is forbidden to know. Packets per second is the honest unit, and it
appears only on the diagnostics pages.

The car's own OTA is invisible here for the same reason: through the relay a firmware image is
indistinguishable from any other REST body. There is no "updating the car" screen and cannot be.

## Hardware

| | |
|---|---|
| Panel | SSD1306 128 × 64, I²C, address `0x3C` or `0x3D` (module-dependent) |
| Bus | I²C, pins declared in `firmware/dongle/main/board.h` — **not yet chosen on hardware** |
| Button | `BOOT` (GPIO0), present on this board, used at runtime to page the reference screens |
| Current | ~20 mA |

**`board.h` is new to the dongle.** `firmware/car/core` has one, and CLAUDE.md describes its role:
"every assumption about the physical board… Bring-up edits this file and nothing else." The
dongle has had no such file — its constants live scattered across module headers. This work
creates the twin and moves the display's pins, bus speed and I²C address into it, so bring-up
touches one file.

Occupied elsewhere on this board: `GPIO19/20` (native USB), `GPIO33–37` (octal PSRAM). I²C is
used nowhere in the dongle today, so the bus is free of internal conflicts.

**Unverified until the panel arrives:** current draw with a phone as the power source. The dongle
is bus-powered by an iPhone, which is particular about peripheral appetite. 20 mA beside Wi-Fi is
nothing, but it must be confirmed on a phone rather than a Mac.

## The library: u8g2, vendored, without its font catalogue

Decision: **take the proven library rather than write a driver.** The SSD1306 init sequence is the
one part that cannot be verified without hardware, and debugging it blind is the expensive
failure mode. u8g2 (olikraus) has carried this panel for over a decade.

It is **not** available through the IDF component manager. The registry holds exactly one
candidate, `nixy4/u8g2` v0.1.4 — published 2026-03, zero downloads, zero stars. This project pins
its dependencies deliberately and explains each pin in `idf_component.yml`; an unadopted
single-author wrapper does not meet that bar for firmware that lives in a pocket.

So u8g2 is vendored as a local component, **minus its font catalogue**:

- `csrc/` is 43 MB, of which `u8g2_fonts.c` alone is 38.8 MB and `u8x8_fonts.c` 1.5 MB — the
  library's entire font collection as C source. The linker discards what the binary does not use,
  but the repository would carry all of it, and it is one enormous translation unit on every
  build.
- Vendored: everything else, ~2.6 MB, **unmodified**, so that upstream updates stay a copy rather
  than a merge. The omission of the two font files is the only deviation and is recorded in the
  component's README.
- The three fonts this design uses are generated instead, by u8g2's **own** converter
  (`tools/font/bdfconv`, 19 C files, builds on the host) from u8g2's own BDFs, restricted to the
  glyphs actually used. Kilobytes, not megabytes.

Original code is therefore limited to two things: the HAL that maps u8g2's byte callback onto the
IDF I²C master driver, and the screen layouts.

## Fonts, and why the copy is upper- and lower-case again

Three, all from u8g2, all `t_cyrillic`:

| Font | Role | Capacity |
|---|---|---|
| `u8g2_font_10x20_t_cyrillic` | the state word | 12 characters |
| `u8g2_font_9x15_t_cyrillic` | held in reserve | 14 characters |
| `u8g2_font_6x12_t_cyrillic` | data rows | 21 characters, 5 rows |

A 5 × 7 matrix cannot hold Cyrillic: «Ш» and «Ж» need three stems and two gaps inside five
columns and collapse into the same solid block. This was established by measurement, not opinion,
after two attempts at deriving a font by rasterising a text face. Hand-drawn bitmap fonts for this
panel have existed for twenty years; the correct move was to take one.

The same measurement retired an earlier constraint: with 12 to 20 rows per glyph, **lower case is
perfectly legible**, so the screen copy is ordinary sentence case rather than the shouting all-caps
the derived font had forced.

## The template

Eight rules. None was designed up front — each arrived after a specific objection while the
screens were drawn one at a time, and each is therefore load-bearing.

1. **A word in 10 × 20, centred** — the state, or the device name. Up to 12 characters. Every
   agreed state name fits; «Машинка не найдена» did not, and shortening it to «Нет сети» produced
   a better name than the long one.
2. **A dithered rule beneath it** — the only divider, and the only graphical element. It has three
   states: a plain line; filled to a level (progress on «Обновление», signal strength on «Связь»);
   and a strip of values over time («Сигнал»). In none of them does it depict an object — only a
   quantity — which is why it does not conflict with rule 4.
3. **Up to two rows of 6 × 12, centred**, 21 characters each.
4. **No objects and no frames.** No phone, no car, no icons.
5. **The state is always the large word**, never the instruction — even on the screens where the
   user must act. Settled on «Не настроен» and binding on «Нет сети» and «Нет хоста».
6. **Only the dongle's own measurements** (see the boundary above).
7. **Instrument register, not conversation.** No first person: not «жду имя сети» but «Сеть не
   задана»; not «больше не пробую» but «Попытки исчерпаны». The app talks to a person — that is
   its role. The hardware reports state.
8. **No warning without a cause.** «Не выдёргивай» was removed from the update screen: the image
   is written to the passive partition and the boot partition is switched only after a successful
   verify (`ota_api.c:102–106`). Interrupting the transfer is safe, so the warning was false.

There are **no exceptions to the template**. A ninth rule ("dense screen, smaller headline") was
drafted for the diagnostics page and then withdrawn — splitting that page into four proved
cheaper than admitting a second headline size.

## The screens

Nine follow the station's own state, exactly as `/status` reports it in `net.state`. Two more are
reference pages reached with `BOOT`.

| # | Screen | When | Rows |
|---|---|---|---|
| 1 | **Старт** | 2 s at power-on | `v1.0+749` |
| 2 | **Не настроен** | `idle` | `Сеть не задана` / `Задаёт приложение` |
| 3 | **Обновление** | own `POST /ota` | rule filled to progress; `62%   1.14/1.83 МБ` |
| 4 | **Поиск сети** | `searching` | `AJMiddleCar` / `Попытка 3 из 5` |
| 5 | **Подключение** | `joining` | `Сеть найдена` / `Получение адреса` |
| 6 | **Связь** | `connected` | rule filled to RSSI; `AJMiddleCar` / `-53 dBm   канал 1` |
| 7 | **Нет сети** | `failed` | `Попытки исчерпаны` / `Проверьте машинку` |
| 8 | **Откат** | rollback flag | `Прежняя версия` / `Нужен новый выпуск` |
| 9 | **Нет хоста** | no USB host | `Питание от COM` / `Стендовый режим` |
| 10 | **Диагностика** | `BOOT`, four pages | see below |
| 11 | **Сигнал** | `BOOT` | RSSI history; `-53 dBm   мин -68` |

Screen 1 is the only one independent of the network: if it appears, power, I²C and the firmware
itself are alive, and the rest can be believed.

Screen 2 is seen once in a device's life. `main.c` loads a stored network and joins immediately,
so from the second boot onward the dongle goes straight to «Поиск сети».

Screen 3 arrives directly after screen 2 on a new dongle: the app updates the adapter *before* it
tells it about any car.

Screens 4 and 5 are deliberately separate. `wifi_sta.c:321` splits them on purpose, and the
answers they call for are opposite: «Поиск сети» usually means the car is switched off, while
«Подключение» means waiting a moment. The attempt counter on screen 4 exists nowhere else today —
`wifi_sm_t.attempts` is internal, and the app sees only a bare "joining".

Screen 7 says attempts have *stopped*, which is the design's own rule
(`WIFI_JOIN_ATTEMPTS`, "a join that keeps failing stops attempting… rather than draining the
phone it is plugged into"). Only a new `POST /net` restarts it.

**Diagnostics, four pages**, headed «Диагностика» in the ordinary 10 × 20 like every other screen,
paged by `BOOT`, with markers showing position. Grouped so that neighbours are worth comparing:

| Page | Rows |
|---|---|
| 1 · network | `Адрес  192.168.4.2` / `Шлюз   192.168.4.1` |
| 2 · radio | `Канал            1` / `Уровень    -53 dBm` |
| 3 · relay | `Слоты  TCP 1 UDP 1` / `Пакеты  10/5 в сек` |
| 4 · state | `errno          нет` / `Аптайм    00:41:12` |

`errno` reads «нет» almost always. It is the line the whole page exists for, and page 4 places it
beside uptime deliberately: together they answer whether a fault predates the last restart, which
otherwise requires a log.

Heap is the one value that did not fit. It matters only when hunting a leak, so it goes to
`GET /status` instead.

**Signal** shows RSSI history because «Связь» already shows the instant value. The dip that breaks
a link is invisible in a single number, and the board has a u.FL connector — the antenna is moved
by hand, and this is the only feedback instrument for that.

## What the firmware must grow

Most of this is independent of the panel and can be built and tested before it arrives.

| Where | What | Note |
|---|---|---|
| `main/board.h` | new; I²C pins, bus speed, panel address, BOOT pin | the file bring-up edits |
| `components/u8g2/` | vendored library minus the font catalogue, plus a README explaining the omission | |
| `main/fonts_cyrillic.c` | three fonts, generated by `bdfconv` | generated, not hand-edited |
| `main/display_hal.c` | u8g2 byte callback onto the IDF I²C master driver | the only glue |
| `main/screens.{c,h}` | **pure**: state → which screen, and the rows it carries | host-tested |
| `main/display.c` | the task: redraw, `BOOT` paging, return-to-state timeout | |
| `main/relay_udp.c`, `relay_tcp.c` | packet counters, slot occupancy, last `errno` with a repeat count | increments where `send`/`recv` already are |
| `main/wifi_sta.c` | expose `attempts` | already computed, visible to nobody |
| `main/usb_net.c` | real NCM link state | also corrects `/status`, where `usb` is hardcoded `"up"` |
| `main/ota_api.c` | bytes received of how many | counter already exists in the receive loop |

### Testing without the panel

`firmware/dongle/test/` already holds four host tests with a Makefile (`net_cfg`, `tcp_pending`,
`udp_sess`, `wifi_state`). `screens.c` is pure by construction and joins them: given a state, it
returns a screen and its rows, and that is assertable without ESP-IDF, I²C or glass.

u8g2 renders into a plain byte buffer before anything reaches the bus. If it compiles on the host
with a null byte callback — **to be established, not assumed** — then the layouts themselves get
host tests that assert real pixels, and only the flush needs hardware.

### Also worth exposing in `GET /status`

Every counter above, plus heap and uptime. They cost nothing extra once measured, and they let the
**app** name the broken hop with no display attached — which is the same problem the screen exists
to solve, for the case where nobody is looking at the dongle.

## Уточнения по итогам стенда (2026-09-16)

- **Заставка — первой строкой `app_main`, а не последней.** `display_start()` стоял в конце
  намеренно (без `ESP_ERROR_CHECK`, чтобы панель не могла откатить прошивку), и панель была
  тёмной всё время инициализации USB, Wi-Fi, релеев и сервера — а после OTA-перезапуска на
  ней висел последний кадр прежнего образа: SSD1306 держит свою RAM через ресет MCU. Теперь
  `display_early()` идёт первым: I²C, один зонд адреса панели (≤ 50 мс; нет ответа — кадр
  пропущен, задача пробует позже своей обычной дорогой), инициализация, кадр заставки. Без
  проверок ошибок, как и раньше. `display_start()` в конце только запускает задачу на уже
  светящейся панели; две секунды заставки считаются от первого кадра.
- **Двенадцатый экран — «Перезапуск».** Плановый перезапуск (сброс с кнопки на 100 %,
  перезапуск после OTA) рисует его перед `esp_restart()` (`display_reboot()`, ждёт отрисовки
  до двух тактов задачи), иначе «Сброс» на полной шкале или «Обновление 100 %» оставались на
  стекле через всю перезагрузку и не отличались от зависания. Одно слово, без строк;
  «Перезапуск» (10 глифов), а не «Перезагрузка» (все 12 — впритык к краям). Аппаратная
  кнопка RST кадра не даёт — там до заставки остаётся старый, но теперь на миллисекунды.
- Все экраны, отрисованные кодом прошивки на хосте, и схема переходов:
  <https://claude.ai/code/artifact/1c537de0-1b17-4e8a-b40a-8c13157d1586>.

## Open until hardware

- I²C pin choice, confirmed against this board's silkscreen (a third-party `ESP32-23 2022-V1.3`).
- Panel address, `0x3C` or `0x3D`.
- Current draw with a phone as the source.
- Redraw rate, splash duration, and how long a `BOOT` page holds before returning to state — all
  judgements better made watching a real panel than argued in advance.
