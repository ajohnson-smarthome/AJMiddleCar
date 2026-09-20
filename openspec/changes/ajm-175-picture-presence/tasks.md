# Tasks

Пункт — один воркер, один PR. Проверка: `openspec validate ajm-175-picture-presence --strict`; `CONFORMANCE=required tools/test-all.sh` (`tools/test-all.sh` подхватывает `app/tests/*/` сам — новому тесту нужен файл `sources` с `PicturePresence.swift`, как у `videohold`); `cd app && xcodegen generate && xcodebuild build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle`. Поведение — по дельте `specs/app/drive-hud/spec.md`, решения — `design.md`.

## 1. Плашка уходит с первым кадром

- [ ] 1.1 `PicturePresence` (`app/AJMiddleCar/PicturePresence.swift`: `staleAfter = 2`, `frame(at:) -> Bool` — `true` только на переходе «появилась», `present(at:)`, `reset()`) с хост-тестом `app/tests/picturepresence` по образцу `videohold` (пусто → нет; кадр → есть, переход; второй кадр через 0,5 с → не переход; 2 с тишины → нет; кадр после тишины → переход; `reset` → нет) и файлом `sources` (`PicturePresence.swift`) — `tools/test-all.sh` подхватит каталог сам; `VideoLink`: `state.lastFrameAt` → `state.presence`, ветка `.frame` в `receiveLoop` на переходе поднимает `hasPicture = true` через `Task { @MainActor }` с проверкой `conn === c` (как `socketFailed`), `publishStats` считает `hasPicture` через `presence.present(at:)`, `State.reset()` сбрасывает `presence`; `DriveView.noPicture` не трогать; проверить: хост-тест зелёный в `tools/test-all.sh`, `xcodebuild` собирается, `grep -n 'lastFrameAt' app/AJMiddleCar/VideoLink.swift` пуст; источники: `app/AJMiddleCar/VideoLink.swift` (`State`, `receiveLoop` `.frame`, `publishStats`, `close`, `socketFailed`), `VideoHold.swift` + `app/tests/videohold/main.swift` (образец чистого модуля и теста), `tools/test-all.sh:31-43` (как собираются `app/tests/*/`) (AJM-176)

## 2. Проверка глазами

- [ ] 2.1 (PM) Симулятор против мока после слияния: включить видео сегментом бара, записать экран; ожидание — «Нет картинки · Камера ждёт» сменяется картинкой одним шагом, без «Нет картинки» без причины поверх идущей картинки; выключить и включить быстро — плашка при включении показана до первого кадра нового сокета
