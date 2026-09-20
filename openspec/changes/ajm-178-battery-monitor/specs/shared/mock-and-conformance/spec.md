# Spec Delta

## MODIFIED Requirements

### Requirement: Личность, возраст платы и откат — по флагам
Личность мока SHALL задаваться флагом `--device` или переменной `MOCK_DEVICE` (флаг старше) и отражаться везде, где машинка называет себя: `device` в `/version`, `device.id` в `hello_ack`, первое слово строки `GET /`; по умолчанию — `device` контракта. Флаг `--no-version` или `MOCK_NO_VERSION` SHALL заставлять мок отвечать `404` на `/version` — как на несуществующий путь — до первой принятой прошивки, после которой `/version` появляется. Флаг `--rollback` SHALL превращать каждую успешную прошивку в «не пережила первую загрузку»: мок возвращается на прежнюю версию с `rolled_back` = `true` в `/version` и в `device` ответа на `hello`. **Деградации** SHALL задаваться флагами, по умолчанию все выключены: `--bus down` — `motors.bus: down`, колёса не двигаются, `spin` → `409 busy`; `--camera off` — `video.state: off`, `view` игнорируются; `--radio mismatch` / `--radio unavailable` — слово в `radio.state` и `fw: null` для `unavailable`; `--reset-at-boot` — `storage.reset_at_boot: true` и домены на умолчаниях контракта; `--write-fail <domain|calibration>` — один `500 write_failed` на первую запись названного, домен откатывается; `--battery absent` — `battery.state: absent` и `null` во всех числах группы. **Пак** SHALL моделироваться по `car/battery-monitor`: `--battery-soc N` — стартовый процент (по умолчанию 80); ток — 300 мА в покое плюс до 8 А пропорционально `|throttle|` удерживаемой команды; напряжение — от процента и тока (12,6 В при 100 % в покое, 11,0 В при 0 %, минус 50 мВ на ампер); мощность — их произведение; процент убывает по кулонам при ёмкости 9000 мА·ч, `--battery-drain-x N` ускоряет разряд в N раз для показа; `state` — `low` по тому же порогу и гистерезису, что у прошивки (20 % / 23 %). Каждый флаг отражается во всех местах, где машинка это показывает.

#### Scenario: Чужая машинка
- **WHEN** мок запущен с `MOCK_DEVICE=esp32-car`
- **THEN** `/version.device` и `device.id` в `hello_ack` — `esp32-car`, и приложение показывает экран «другая машинка»

#### Scenario: День флага
- **WHEN** мок запущен с `--no-version` и приложение спрашивает `/version`
- **THEN** ответ `404`; после первой принятой прошивки тот же запрос отвечает `200` с пятью полями

#### Scenario: Репетиция отката
- **WHEN** мок запущен с `--rollback` и принял прошивку с новой версией
- **THEN** после «перезагрузки» он называет прежнюю версию и `rolled_back` = `true`

#### Scenario: Шина down
- **WHEN** мок запущен с `--bus down` и пульт открывает сессию
- **THEN** телеметрия и `/status` несут `motors.bus: down`, `drive` принимаются, но крошек и движения нет, `POST /calibration/spin` → `409 busy`

#### Scenario: Хранилище стёрто
- **WHEN** мок запущен с `--reset-at-boot`
- **THEN** `/status.storage.reset_at_boot` — `true`, `GET /config` — умолчания контракта, `motors.calibrated` — `false`

#### Scenario: Пак тает под газом
- **WHEN** мок запущен с `--battery-soc 25 --battery-drain-x 600` и пульт держит `throttle` 1,0
- **THEN** `battery.current_ma` около 8300, `soc_pct` убывает и в пределах минуты становится 20 — `state: low`; при отпущенном газе ток около 300 и процент почти стоит

#### Scenario: Без монитора
- **WHEN** мок запущен с `--battery absent`
- **THEN** телеметрия и `/status` несут `battery.state: absent` и `null` в `voltage_mv`, `current_ma`, `power_mw`, `soc_pct`; всё остальное — как без флага
