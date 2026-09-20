# Design

## Context

Мотивация — `proposal.md`; электрика и совместимость — `docs/research/2026-09-20-ina260-compat-and-wiring.md`; стенд — `docs/bringup.md` (2026-09-20): модуль на `0x41`, ID `0x5449/0x2270`, чтения по умолчанию (`0x6127`) дают ток/напряжение/мощность в регистрах `0x01/0x02/0x03` с LSB 1,25 мА / 1,25 мВ / 10 мВт.

Что есть в коде: `i2c_bus.c` — один мастер, устройства добавляются со своей `scl_speed_hz` (PCA 400 кГц, SCCB 100); `telemetry_gather()` (`telemetry.c`) собирает `telemetry_t` для пуша 5 Гц (`rt_link`, приоритет 6) и для `/status` (httpd), `telemetry_groups()` в `telemetry.h` печатает группы макросами из контракта, `test_contract_wire` проверяет вложенность; генератор `tools/gen_contract.py` + `gen_common.py` обходят `groups` обобщённо (`nullable` → `Optional` в Swift, `None` в моке); мок строит группы через `status_groups()` по схеме — поле без значения роняет мок, а не теряется на проводе. В аппе `DriveView` читает `telemetry?.link…` из `CarLink.lastTelemetry`, верхний ряд — `HStack` с `SignalBars` и `statusItem`.

## Goals / Non-Goals

**Goals:** группа `battery` на проводе и в трёх реализациях; чистое правило остатка с хост-тестом; драйвер, не задевающий актуатор; индикатор в верхнем ряду без сдвига раскладки; мок, на котором бар тает.

**Non-Goals:** `/config` домен батареи, экран настроек, ALERT-пин, влияние на актуатор, запись в NVS, компенсация температуры и внутреннего сопротивления, INA226/INA228 (интерфейс драйвера их допускает, реализации нет).

## Decisions

### 1. Контракт: пять полей, nullable числа, три слова
`groups.battery`: `voltage_mv` int nullable, `current_ma` int nullable, `power_mw` int nullable, `soc_pct` int nullable, `state` state (`swift: BatteryState`, `ok|low|absent`). Добавляется в `telemetry.groups` и `status.groups` последней. Целые в мили-единицах — как `rssi_dbm`, без `fixed` и дробей на проводе; `power_mw` отдельным полем, а не `V×I` на пульте, потому что чип считает мощность на своих усреднённых выборках, а пульт получил бы произведение двух независимо усреднённых чисел. `null` при `absent` — уже принятая форма (`rssi_dbm`, `radio.fw`).
*Альтернатива:* `soc_pct` только в `/status` — нет: бар живёт на экране вождения, который читает телеметрию.

### 2. Прошивка — четыре файла, один из них чистый
- `power_monitor.h` — интерфейс: `esp_err_t power_monitor_init(void)` (детект: `0xFE == 0x5449`, `0xFF == 0x2270`, запись `CONFIG` = усреднение 16, 1,1 мс, непрерывно), `esp_err_t power_monitor_read(power_sample_t *s)` — `{int32_t mv, ma, mw;}`. Единственная реализация — `ina260.c`: три чтения по 2 байта, LSB по даташиту, знак тока — two's complement. INA226/228 позже — вторая реализация того же заголовка, выбор — `board.h`.
- `battery_pack.h` — константы пака: `BATTERY_CELLS 3`, `BATTERY_PARALLEL 3`, `BATTERY_CAPACITY_MAH 9000`, `BATTERY_LOW_PCT 20`, `BATTERY_LOW_CLEAR_PCT 23`, `BATTERY_REST_MA 500`, `BATTERY_REST_S 30`, `BATTERY_START_S 3`, таблица покоя Li-ion NMC на ячейку (мВ → %): 4200→100, 4100→90, 4000→80, 3920→70, 3850→60, 3780→50, 3720→40, 3660→30, 3600→20, 3500→10, 3400→5, 3200→0 — стартовая, правится по стенду. Не `board.h`: пак — не плата; `board.h` получает только `BOARD_INA260_ADDR 0x41` и `BOARD_INA260_HZ 100000`.
- `battery_soc.{c,h}` — **чистый**, без ESP-IDF: `battery_soc_init(&s)`, `int battery_soc_step(&s, mv, ma, dt_ms)` → `soc_pct` или `-1` пока не определён; `battery_soc_rest_pct(mv_per_cell)` — таблица с линейной интерполяцией; `battery_soc_reset(&s)` на возврат из `absent`. Внутри — стартовое окно (среднее `mv` за `BATTERY_START_S` при `|ma| < BATTERY_REST_MA`), кулоны в мА·мс, счётчик покоя, подтяжка ≤ 1 %/с. Хост-тест `test_battery_soc.c` в `firmware/car/core/test/` — сценарии спеки числами.
- `battery.{c,h}` — задача `battery` (приоритет 3, как видео; стек 3072), период 200 мс: `power_monitor_read` → `battery_soc_step` → снимок `{state, mv, ma, mw, soc}` под `portMUX`; три ошибки подряд → `absent`, удача → `ok` + `battery_soc_reset`. `battery_snapshot(&out)` — для `telemetry_gather`. Гистерезис `low` — здесь же (два порога из `battery_pack.h`).
- `telemetry_t` получает `battery_state`, `battery_mv/ma/mw/soc` (`int32_t`, `INT32_MIN` = `null`); `telemetry_groups()` печатает группу последней; `test_contract_wire` — вложенность и `null`.
- `main.c`: `battery_init()` сразу после `pca9685_init`, до сети — как камера: отказ не ломает загрузку.

### 3. Апп — `BatteryBadge`, ничего в `DriveLayout`
`BatteryBadge(battery: Telemetry.Battery?, palette:)` — иконка 22 × 11 pt (скруглённый контур цветом текста/приглушённым, заливка по `soc_pct`, «нос» 2 pt, молния при `current_ma < 0`) и подпись `L.batteryStats(pct:v:w:)` тем же 10-pt шрифтом, что «к/с · потеряно»; цвет по `state`: `ok` — `text`, `low` — `warn`, `absent` — `muted` и «—». Встаёт в левый `HStack` верхнего ряда после статистики видео; `DriveLayout` не меняется. Плашка: `hasWarnings` и `warnings` в `DriveView` получают четвёртое условие `battery.state == .low` → `statusItem("battery.25", L.batteryLow, p.warn)`. Строки: `battery.low` = «Батарея разряжена», `battery.stats` = «%d %% · %@ В · %d Вт», `battery.absent` = «—». Галерея: `mockLink(battery:)` и два кадра сразу после «Drive video off» — «Drive battery low» (18 %, 11,1 В), «Drive battery absent»; индексы позже идущих кадров сдвигаются на два (снимки PM берут по подписи, не по числу).

### 4. Мок — модель в `state.py`, флаги в `mock_car.py`
`values["battery"]` в `status_groups()`; `Battery` в `state.py`: `soc` (float), `absent`, `drain_x`; `step(dt, throttle)` — `ma = 300 + 8000·|throttle|`, `mv = 11000 + 1600·soc/100 − 50·ma/1000`, `mw = mv·ma/1000`, `soc −= ma·dt/3600/9000·drain_x`; `state` по 20/23. Вызывается из такта телеметрии (там уже известна удерживаемая команда). Флаги: `--battery-soc` (80), `--battery absent`, `--battery-drain-x` (1). `README.md` мока — раздел флагов. `test_state.py` — сценарий спеки «Пак тает под газом» и `absent`.

### 5. Conformance
`conformance.py` — `/status` через сгенерированный валидатор групп: семь групп, `battery` с типами; при `--battery absent` (отдельный запуск в `test-all.sh` не нужен — форма `null` проверяется в `test_state.py`). `conformance_rt.py` — `telemetry.groups` из `generated.py` уже включает `battery`; добавить проверку `state` из словаря и `soc_pct` в `0…100` или `null`.

### 6. Документация
`CLAUDE.md`: строки про `ina260`/`battery_soc`/`battery` в списке модулей прошивки, `board.h` — адрес монитора; `docs/protocol.md` — таблица групп из генератора. `docs/bringup.md` — стендовые пункты этой change по мере прохождения.

## Risks / Trade-offs

- [Стартовое окно 3 с ловит не покой — машинка перезагрузилась после езды и пак «отдыхает»] → напряжение восстанавливается за секунды, ошибка старта единицы процентов; подтяжка в покое дотянет за минуту. Принято: точнее — только NVS, а его решили не трогать.
- [Таблица покоя типовая, не по HG2] → стенд: снять напряжение покоя на 100/80/50/20 % и поправить `battery_pack.h`; это `(стенд)`-пункт.
- [Задача монитора и актуатор делят шину] → транзакция ≤ 0,6 мс на 100 кГц, мьютекс драйвера; приоритет 3 ниже актуатора (5) и `rt_link` (6). Проверка на стенде: `link.rx_hz` = 10 и без джиттера с монитором.
- [Ток под ШИМ 1 кГц пилой] → усреднение 16 × 1,1 мс в чипе; если показания всё равно прыгают — 64.
- [Индикатор удлиняет левую часть верхнего ряда] → на 874 pt между «На связи · 22 к/с · потеряно 0 · ▮▮▮ 72 % · 12,3 В · 38 Вт» и переключателем остаётся > 150 pt; на 852 — проверить снимком; при нехватке — прятать «потеряно 0», когда потерь нет.
