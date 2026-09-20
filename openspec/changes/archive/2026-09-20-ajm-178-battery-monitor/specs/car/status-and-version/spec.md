# Spec Delta

## RENAMED Requirements

- FROM: `### Requirement: `/status` — шесть групп под конвертом`
- TO: `### Requirement: `/status` — семь групп под конвертом`

## MODIFIED Requirements

### Requirement: `/status` — семь групп под конвертом
`GET /status` SHALL отвечать `200` и `application/json` объектом из `proto` и групп `contract.status.groups` в их порядке, каждая — ровно с полями своей группы из `contract.groups`, значениями типов и слов контракта; `null` SHALL стоять только в поле, помеченном как nullable. Группы, общие с телеметрией (`contract.telemetry.groups`), SHALL быть написаны в `/status` так же, как в кадре `telemetry`: одни имена, одни слова, один смысл — с единственной оговоркой про `link.rx_hz` ниже. `system.uptime_s` — секунды с загрузки, `system.free_heap` — свободная куча в байтах; смысл `video` — `car/video-stream`; смысл `battery` — `car/battery-monitor`. Документ, который не помещается целиком, SHALL NOT отправляться усечённым — вместо него `500` с кодом `internal`.

#### Scenario: Форма ответа
- **WHEN** приходит `GET /status`
- **THEN** ключи верхнего уровня — `proto` и семь групп контракта; в каждой группе — ровно её поля; каждое слово состояния — из списка своего поля

#### Scenario: Один смысл в двух местах
- **WHEN** одно и то же состояние машинки читают через `/status` и через кадр `telemetry`
- **THEN** `motors`, `system`, `video` и `battery` в обоих совпадают, а `link` расходится только в `rx_hz`
