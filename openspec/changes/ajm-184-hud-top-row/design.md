# Design

## Context

Мотивация — `proposal.md`, требования — `specs/app/drive-hud/spec.md`. Макет, по которому принимались все решения ниже, — канвас **«HUD: верхний ряд»**: https://claude.ai/artifact/9mEhRbjdJrX9QP3xWw9kAE — каталог состояний верхнего ряда в светлой теме (связь × 4, батарея × 5, картинка × 3, бар × 5) и два кадра тёмной, все в масштабе 2× (1 pt = 2 px), цвета — `Theme.swift`, подписи — `Localizable.strings`. Числа в этом документе — оттуда.

Сейчас верхний ряд — один `HStack(height: 32)` в `DriveView`: слева `SignalBars` + «На связи» 12 pt, при картинке `statusItem("video", …)` 10 pt с `opacity(0.8)`, затем `BatteryBadge` (иконка 22 × 11, подпись 10 pt); справа `SchemeToggle` (13 pt + 6/6 pt, r9, без тела) и `ControlBar` (3 × 44 × 32, r10, `segOff`). Под рядом — `scrim(.top)`: градиент `bg` 0,78 → 0 за 64 pt, на середине ряда — ~0,44. `TricksControl` красит глиф в `accent` в покое. Предупреждения — `notices.padding(.top, 52)`.

Ограничения: `tools/test-all.sh` — единственный судья воркеров (хост-тесты Swift + `xcodebuild`); симулятор и снимки галереи — у PM. Провод, прошивка, мок и контракт не трогаются: показания те же, меняется только их вид.

## Goals / Non-Goals

**Goals:**
- Один вид «прибор» на три прибора слева, и одна пара метрик пилюль на две пилюли справа — так следующий прибор в ряду не придумывает себе кегль.
- Лента как единственная почва ряда над картинкой; палитра темы над ней снова законна.
- Вердикт прибора картинки — чистый и хост-тестируемый, как `BatteryGauge`; состояние потерь снимается в галерее без машинки.

**Non-Goals:**
- Тема, палитра, токены — не меняются; новых цветов нет.
- Окно кадра, `DriveLayout` и его точки — не меняются: лента ложится поверх, ничего не сдвигая.
- Приёмник, счётчики (`fps`, `lostLast10s`, `hasPicture`), подписка — без изменений.
- Плашка предупреждений — вид прежний, меняется только отступ сверху.
- Адаптация ряда под яркость кадра, размытие, тени — отвергнуты на канвасе (V1–V3, B1–B4), не возвращаются.

## Decisions

### 1. Лента — вместо `scrim(.top)`, поверх окна, не вместо его верха
`scrim(.top)` заменяется на `band`: `Rectangle().fill(p.bg).frame(height: 56)` с `Rectangle().fill(p.line).frame(height: 1)` по нижней кромке, `.frame(maxHeight: .infinity, alignment: .top).ignoresSafeArea().allowsHitTesting(false)` — те же модификаторы, что у нынешнего скрима, поэтому лента, как и он, идёт от края до края экрана, поверх полей и окна. Рисуется при `windowLive` (тот же предикат, что монтирует `VideoView`): без картинки лента — фон на фоне, а линия была бы коробкой под пустотой. `scrim(.bottom)` остаётся. Высота — константа `HudBand.height = 56` рядом с рядом (12 + 32 + 12), а не поле `DriveLayout`: раскладка окна и точек не меняется, и хост-тест `drivelayout` про ленту ничего не проверяет. Предупреждения — `padding(.top, HudBand.height + 8)` вместо 52: под линией, не касаясь ряда.
*Альтернатива:* ужать окно под ленту (кадр 346 pt высотой, 615 шириной) — меняет всю раскладку и точки в спеке ради 14 % кадра сверху, которые у рыбьего глаза и так небо/потолок; на канвасе B5 выбран именно с лентой поверх.

### 2. Две пилюли — одни метрики
`enum HudPill { static let height: CGFloat = 32; static let radius: CGFloat = 10 }` в `ControlBar.swift`; `ControlBarSegment.size.height` и скругление бара берутся из него, `SchemeToggle` — `.frame(height: HudPill.height)` на сегментах вместо `padding(.vertical, 6)` и `HudPill.radius` вместо 9. Тела не меняются: переключатель прозрачный с обводкой `line` и `panel` у выбранного, бар на `segOff`. `TricksControl.tint` — `isRunning ? p.warn : p.text`; кольцо и глифы прежние.
*Альтернатива:* тело у пилюль (`panel`, материал, стекло) — отвергнуто на канвасе.

### 3. Левая группа — `HudItem` и разделители
Новый `HudItem` (`app/AJMiddleCar/HudItem.swift`): `HStack(spacing: 6) { glyph.frame(height: 14); Text(caption).font(.system(size: 11)).monospacedDigit().foregroundStyle(tint) }`; `tint` — цвет прибора целиком, глиф получает его через `foregroundStyle` от родителя. Разделитель — `HudDivider`: `Rectangle().fill(p.line).frame(width: 1, height: 14)`. Ряд слева — `HStack(spacing: 14) { link; HudDivider; if hasPicture { video; HudDivider }; battery }`: прибор картинки уходит вместе со своим разделителем, батарея сдвигается к связи, как убранный сегмент.
Связь — `HudItem(glyph: SignalBars(…), caption: «На связи» / «Поиск…», tint: p.text)`; полоски свой цвет (красный / `warn` / `accent`) держат сами, как сейчас. Батарея — `BatteryBadge` переезжает на `HudItem`: иконка 22 × 11 в боксе 14, подпись 11 pt; `BatteryGauge` не меняется, тест `batterygauge` — тоже.
Числа 11 / 14 / 6 / 14 — из канваса (T3): 11 pt — между нынешними 12 и 10, чтобы левая группа не перевесила пилюли; 14 pt — высота полосок, к которой подтягиваются остальные глифы.
*Альтернатива:* всё 12 pt (T1) — левая группа тяжелее пилюль; без разделителей (T2) — у левой группы нет ритма правой.

### 4. Прибор картинки — `VideoGauge` (чистый) + `VideoBadge`
`VideoGauge` (`app/AJMiddleCar/VideoGauge.swift`, хост-тест `app/tests/videogauge`): `make(fps:lost:) -> VideoGauge { caption: String /* «N к/с» */, lost: Int? /* nil при 0 */, accessibility: String /* «N к/с, потеряно M» */ }`. `VideoBadge` рисует `HudItem(glyph: Image(systemName: "video") 14 pt regular, caption: g.caption, tint: p.text)` и, при `g.lost != nil`, следом `HStack(spacing: 4) { Image(systemName: "rectangle.dashed") 12 pt; Text("\(lost)") 11 pt monospacedDigit }` в `p.warn` с зазором 10 pt — вторая пара внутри того же прибора, без разделителя. SF `video` (outline) и `rectangle.dashed` — глифы системы, того же семейства, что в баре; свой `Path` для камеры не нужен: при 14 pt контур `video` по штриху совпадает с 1-pt контуром батареи.
Строки: `video.stats` → `"%d к/с"`; новая `video.lost` → `"потеряно %d"` — только для VoiceOver (`accessibilityLabel` прибора), в ряду слово не появляется. `L.videoStats(fps:)` и `L.videoLost(_:)`.
*Альтернатива:* «22 к/с −7» одной строкой (b на канвасе) — отвергнута пользователем в пользу пары с глифом.

### 5. Галерея — состояние потерь без машинки
`VideoLink` получает `#if DEBUG static func preview(fps:lost:hasPicture:) -> VideoLink`, выставляющий три `@Published` без сокета; `CarLink.preview` — параметр `video: VideoLink? = nil`. В `GalleryView` — кадр **«Drive picture lost»** (`fps: 22, lost: 7`) после «Drive battery absent»; кадры «Drive arcade» … «Drive battery absent» получают также `video: .preview(fps: 25, lost: 0, hasPicture: true)`, чтобы прибор картинки в галерее был виден — сегодня в галерее его нет вовсе. Индексы кадров после 55 сдвигаются на один (Settings — 57); PM снимает по подписи из списка, не по памяти.
`DriveView` при `preview` не подписывается (как сейчас), а показания читает из `link.video` — тот же путь, что в бою.

### 6. Документация
`CLAUDE.md`, абзац «The drive screen is one layout, picture or not…»: лента 56 pt под верхним рядом пока окно живо, левая группа — один `HudItem`, прибор картинки «N к/с» + потери при M > 0; ссылка на `openspec/specs/app/drive-hud`. Док-комментарии `DriveView` («The picture's own numbers live next to the link, not in a pill…»), `BatteryBadge`, `TricksControl` — переписать в PR той задачи, что трогает файл.

## Risks / Trade-offs

- [Лента скрывает верхние 56 из 402 pt кадра — 14 %] → машинка и так режет 4:3 до 16:9 по середине; у рыбьего глаза верх — небо или потолок; выбор сделан на канвасе с настоящим кадром под лентой. Если на стенде окажется, что теряется дорога, — вопрос к `video.height`/кропу (`car/video-stream`), не к ленте.
- [11 pt мельче нынешних 12 у «На связи»] → на канвасе в 2× читается; PM сверяет на снимках обеих тем (задача 3.1); если мелко — правится одна константа в `HudItem`, спека чисел не задаёт.
- [SF `video` при 14 pt может не совпасть по штриху с контуром батареи 1 pt] → проверить на снимке; запасной вариант — `weight: .light` или свой `Path` 16 × 12 в `VideoBadge`, вердикт и спека не меняются.
- [`rectangle.dashed` нужен iOS 15+] → цель приложения выше; проверить в `project.yml` при реализации.
- [Две задачи трогают `DriveView.swift`] → последовательны (`blocked-by`), как в ajm-172.
- [Сдвиг индексов галереи после нового кадра] → снимки по подписи; в `tasks.md` индексы новые.
