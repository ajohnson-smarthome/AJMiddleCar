# Proposal

## Why

Водитель не видит, сколько заряда осталось в паке и сколько машинка ест прямо сейчас: единственный сигнал — BMS, который в ноль отрубает всю машинку разом. Модуль INA260 стоит на плюсе пака, отвечает на шине по `0x41` (`docs/bringup.md`, 2026-09-20), пак опознан — 3S3P LG HG2, 9000 мА·ч. Этап 1 (`AJM-177`, `docs/research/2026-09-20-ina260-compat-and-wiring.md`) сказал, как подключить; этот change — что машинка с этим измеряет и что пульт показывает. Решения согласованы 2026-09-20: только показывать, параметры пака в коде, остаток — гибрид, без NVS.

## What Changes

- **Машинка измеряет пак.** Драйвер INA260 на `i2c_bus` (адрес и скорость — в `board.h`), задача монитора раз в 200 мс читает напряжение, ток и мощность; чип, не ответивший при загрузке или три чтения подряд, — `absent`, и машинка едет как раньше.
- **Остаток — гибрид, чистая арифметика.** Стартовый процент — по напряжению покоя в первые секунды после загрузки (таблица Li-ion на ячейку); дальше кулоны `Σ I·Δt / ёмкость`; в покое (30 с при токе < 0,5 А) — подтяжка к таблице. Ничего не хранится в NVS. Параметры пака — константы в коде: 3S, 3P, 9000 мА·ч, порог `low` 20 % с гистерезисом.
- **На проводе — группа `battery`** в `contract/car-api.json`: `voltage_mv`, `current_ma` (разряд `+`), `power_mw`, `soc_pct` — nullable; `state`: `ok` / `low` / `absent`. Группа входит в телеметрию (5 Гц) и в `/status` — их становится семь. Генератор перегоняет прошивку, Swift, мок и `docs/protocol.md`.
- **Пульт показывает.** В верхнем ряду HUD рядом с «На связи» — иконка батареи с заливкой, процент, вольты и ватты; при `low` — цвет предупреждения и плашка «Батарея разряжена» под рядом; при `absent` — пустая иконка с прочерком, плашки нет. Новых экранов нет; раскладка (`AJM-172`) не двигается.
- **Мок ведёт себя как машинка:** группа `battery` в телеметрии и `/status`, ток от газа, процент убывает по кулонам; флаги `--battery-soc N`, `--battery absent`, `--battery-drain-x N`; conformance проверяет форму группы и `null` при `absent`.

Арбитр, моторы, `/config` — не трогаются: машинка только показывает.

## Capabilities

### New Capabilities

- `car/battery-monitor`: монитор пака на INA260 — измерение, `absent`, правило остатка (старт по покою, кулоны, подтяжка), состояние `low`, параметры пака в коде.

### Modified Capabilities

- `car/status-and-version`: «`/status` — шесть групп под конвертом» → семь, с `battery`; один смысл в `/status` и телеметрии.
- `car/rt-link`: «Телеметрия — владельцу, без запроса» — кадр несёт и `battery`.
- `app/drive-hud`: «Приборы показывают связь и намерение пульта» — бар батареи рядом со связью; «Предупреждения — единственное над кадром, и только пока есть» — плюс «Батарея разряжена».
- `shared/mock-and-conformance`: «Личность, возраст платы и откат — по флагам» — флаги батареи и модель пака (ток от газа, кулоны, `low`).

`shared/contract` не меняется: группа — данные схемы, правила уже общие.

## Impact

- `contract/car-api.json` (+`groups.battery`, `telemetry.groups`, `status.groups`) → `tools/gen_contract.py` → `main/cfg_table.inc`/макросы, `app/AJMiddleCar/Generated/CarAPI.swift`, `tools/mock_car/generated.py`, `docs/protocol.md`; `tools/test_gen_contract.py`, `firmware/car/core/test` (`test_contract_wire` — вложенность группы).
- Прошивка: новые `ina260.{c,h}`, `power_monitor.h`, `battery_soc.{c,h}` (чистый, хост-тест), `battery_pack.h`, `battery.{c,h}` (задача + снимок); `board.h` (+`BOARD_INA260_ADDR`, `BOARD_INA260_HZ`); `telemetry.h/.c` (группа в печати и в `telemetry_gather`); `main.c` (инициализация после `i2c_bus_init`).
- Апп: `BatteryBadge.swift` (новый), `DriveView.swift` (верхний ряд, плашка), `L.swift` + `Localizable.strings`, `GalleryView.swift` (кадры «Drive battery low», «Drive battery absent» после «Drive video off»).
- Мок: `state.py` (группа, модель), `mock_car.py` (флаги), `README.md`; `tools/conformance.py`, `tools/conformance_rt.py`, `test_state.py`.
- Документация: `CLAUDE.md` (модуль в списке архитектуры, шина), `docs/bringup.md` — стендовые пункты.
