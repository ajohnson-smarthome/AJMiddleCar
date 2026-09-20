# Глубина и дальность кроме VL53L5CX: что купить и где — листинги Alibaba и AliExpress

**Дата:** 2026-09-20 · Linear: проект «Ресерч VL53L5X V2». Продолжает
`2026-09-20-tof-vl53l5cx.md` (§ 3.1 — «не картинка», таблица альтернатив) и
`2026-06-10-lidar-*.md` (360° лидары, TF-Luna). Здесь — только рынок: цены, MOQ, продавцы на
2026-09-20 (выдача поиска; карточки на обеих площадках за капчей — открывать руками) и
пригодность каждого для P4. Что *делать* с каждым — в тех документах.

## 1. Вывод

Три направления, в каждом один разумный выбор и одна цена:

| Направление | Что брать | Alibaba | AliExpress | На P4 |
|---|---|---|---|---|
| **Шире вперёд** (парктроник 90° вместо 45°) | **VL53L7CX** модуль | €8,4–8,8, MOQ 1 (Kezhan Weite, reorder 43 %) | €10 (`1005012719615446`) … €21 | да: тот же ULD, порт `grrtzm/v53l7cx-library` |
| **Картинка глубины** (100×100, «покрасить близкое прямо в кадр») | **Sipeed MaixSense-A010** | €21–28, MOQ 1 (Gegu Dingxin €21, 66 отзывов; Xuanxin 7 лет €23,5, 10 продаж) | €28 (`1005005215344922`, 50 продаж) | да: UART до 3 Мбит/с, глубину считает сам |
| **Карта комнаты** (360°) | **RPLIDAR C1** | €46,7–54,6 (Lantu), €54,5 (ND Computing, 2 продажи), €60,7 у самого Slamtec | €54 (`1005012565614536`, 62 продажи) | да: UART 460800, 3,3 В; мотор → свой DC-DC от 3S |

**Arducam ToF (B0410, 240×180)** — единственный из обсуждавшихся, кого брать **не надо**: на
Ali €92 (`1005012027658272`), на Alibaba нет вовсе (только промышленные ToF от €280), и на P4 он
не встаёт — единственный MIPI-CSI занят камерой, драйвера нет, глубину из четырёх фазовых
кадров считает хост. То, что он показал бы, показывает A010 в 2,4 раза грубее по оси — § 3.

Итого «всё сразу» — L7CX + A010 + C1 ≈ **€90–110** с Alibaba, и каждое встаёт на свой
интерфейс: I²C, UART, UART. Ничто из этого не отменяет два L5CX спереди и сзади.

## 2. VL53L7CX / VL53L8CX — тот же 8×8, шире и быстрее

| | VL53L5CX (есть) | VL53L7CX | VL53L8CX |
|---|---|---|---|
| Поле | 45°×45° | **90°×65°** | 65°×65° |
| Интерфейс | I²C 1 МГц | I²C 1 МГц | **SPI** + I²C |
| Особенность | — | два L5CX «домиком» = один L7CX, без шва и адресного ритуала | лучше на солнце, SPI не мешает актуатору на I²C |
| Драйвер на P4 | `rjrp44/vl53l5cx` | `grrtzm/v53l7cx-library` (ULD, IDF ≥ 5.5, `i2c_master`) | `rjrp44/vl53l8cx` |

Тот же ULD, та же прошивка ~84 КБ при каждом включении, те же 64 числа. L7CX — если бампер
хочет видеть колёса при повороте; L8CX — если машинка поедет на улицу.

**Листинги.** Почти все продают «VL53L7CX / VL53L8CX» одной карточкой с выбором варианта —
в опциях смотреть, какой именно. Alibaba, модули с крышкой, MOQ 1:
[Kezhan Weite, 4 года](https://www.alibaba.com/product-detail/Original-VL53L7CX-VL53L8CX-Multi-area-8x8TOF_1601613201266.html)
€8,4–8,8, reorder 43 %;
[Youxin, 4 года](https://www.alibaba.com/product-detail/VL53L7CX-VL53L8CX-8x8-Multi-Region-TOF_1601602562231.html)
€10,6–11, 10 продаж, 154 отзыва;
[Runkexin](https://www.alibaba.com/product-detail/VL53L7CX-8x8-Multi-Region-TOF-Time_1601721940492.html)
€10,9;
[Jingmaoyuan, 280 отзывов](https://www.alibaba.com/product-detail/VL53L7CX-VL53L8CX-Multi-Region-8x8-TOF_1601940948368.html)
€11,5;
[Hongyisheng — только L8CX](https://www.alibaba.com/product-detail/VL53L8CX-Low-power-and-High-performance_1601792133512.html)
€15. AliExpress:
[L8CX/L7CX/L4CD с крышкой](https://www.aliexpress.com/item/1005012719615446.html) €9,99;
[VL53L7CX 8×8 400 см, I²C/SPI](https://www.aliexpress.com/item/1005010413391500.html) €21,4;
[L7CX/L8CX](https://www.aliexpress.com/item/1005010796181156.html) €26,2.

**Не брать:** голые кристаллы `VL53L7CXV0GC/1` за €0,09–0,50 «new original» — реальная цена
чипа ~$5, это либо не то, либо не оригинал; и «модуль за €2,64 при MOQ 10» (Tuoxin, 1 год) —
подозрительно дёшево для платы с LDO и крышкой.

## 3. Sipeed MaixSense-A010 — глубинная картинка, которая встаёт на P4

100×100 точек глубины, 8 бит, **70°×60°**, 0,2–2,5 м с шагом 10 мм, iToF 940 нм, глобальный
затвор, до 20 к/с; на борту BL702 (RISC-V 144 МГц) считает глубину сам. Наружу — **UART**
(1,25 мм 4-pin) и USB (виртуальный COM); UART до **3 Мбит/с** (`AT+BAUD`: 9600 … 3000000),
кадры 1…19 к/с (`AT+FPS`), биннинг 100×100 / 50×50 / 25×25 (`AT+BINN`), пакет: заголовок
`00 FF`, длина, 16 байт метаданных, кадр, контрольная сумма, `DD`. Версии: с LCD 240×135 и
Lite без экрана; A075V — 320×240 + RGB, но только USB и €44–92 — не для P4.

**На P4:** UART на любые GPIO (как лидары июня); кадр 100×100 = 10 КБ; при 3 Мбит/с — до
~30 кадров/с по проводу, дальше упирается в сам датчик (20). До телефона: 10 КБ × 5–10 к/с =
0,4–0,8 Мбит/с рядом с видео (2,5) в USB донгла (~4) — влезает; при 50×50 — вчетверо меньше.
Что рисовать — два вида из макета к `2026-09-20-tof-vl53l5cx.md`: кадр глубины у кромки и
«тёплая заливка близкого прямо в картинку».

**Листинги.** Alibaba, все MOQ 1–2, карточка «A010 / A075V» с выбором варианта:
[Gegu Dingxin, 66 отзывов](https://www.alibaba.com/product-detail/GGDX-Chipboard-Sipeed-MaixSense-A010-A075V_1601719013011.html)
€21–74,5;
[Xuanxin, 7 лет, 10 продаж](https://www.alibaba.com/product-detail/Chipboard-Sipeed-Maixsense-A010-a075v-Rgbd_1601651434841.html)
€23,5–78;
[Million Sunshine, 12 лет](https://www.alibaba.com/product-detail/Sipeed-MaixSense-A010-A075V-RGBD-TOF_1601688983379.html)
€22,9–44, MOQ 2;
[Lantu](https://www.alibaba.com/product-detail/MaixSense-RGBD-TOF-3D-Depth-Vision_1601298984046.html)
€27,3–92;
[Aismartlink, 9 лет, reorder 18 %](https://www.alibaba.com/product-detail/Aismartlink-MaixSense-A010V-RGBD-TOF-3D_1601415839748.html)
€28,5–40, MOQ 2. AliExpress:
[Sipeed MaixSense A010/A075V](https://www.aliexpress.com/item/1005005215344922.html)
€27,99, 50 продаж, 5★. Нижняя цена везде — Lite без экрана; нам он и нужен.

## 4. Arducam ToF Camera (B0410) — почему нет

240×180, 70°, 0,15–4 м (Near/Far), 30 к/с на Pi 4, MIPI-CSI-2, глубина считается SDK Arducam на
хосте из четырёх фазовых кадров. На P4: **один MIPI-CSI, и он у камеры** — либо видео, либо
глубина; в `esp_cam_sensor` драйвера нет (есть «Arducam PIVARIETY», но это их RGB-камеры);
фазы в глубину с их калибровкой — не наша работа. Рынок: AliExpress
[XU-1410C Arducam ToF B0410C](https://www.aliexpress.com/item/1005012027658272.html) €92;
Alibaba — не продаётся, поиск отдаёт промышленные ToF от €280.

## 5. 360° лидары — на крышу, если понадобится карта комнаты

Из июньского ресерча; здесь — цены на сегодня. Все — UART, dToF, 12 м, 4–5 тыс. точек/с;
мотор сканера — отдельный 5 В DC-DC от 3S (VBUS под моторами проседает — `bringup.md`).

- **RPLIDAR C1** (Slamtec; лучший по чёрным целям, 6 м по чёрному; UART 460800, 3,3 В):
  Alibaba [Lantu](https://www.alibaba.com/product-detail/RPLIDAR-C1-Fusion-LIDAR-HD-High_1601298835138.html)
  €46,7–54,6 (со скидкой), [ND Computing, 2 продажи](https://www.alibaba.com/product-detail/Slamtec-RPLIDAR-C1-360-Omnidirectional-LiDAR_1601606381837.html)
  €54,5, [сам Shanghai Slamtec](https://www.alibaba.com/product-detail/Rplidar-C1-HD-High-Definition-360_1601208875797.html)
  €60,7, [HelloChip, 512 отзывов](https://www.alibaba.com/product-detail/SLAMTEC-RPLIDAR-C1-lidar-sensor-TOF_1601873448568.html)
  €66–70; AliExpress [C1, 62 продажи](https://www.aliexpress.com/item/1005012565614536.html)
  €54, [кабель USB-UART к нему](https://www.aliexpress.com/item/1005007941355106.html) €8,6.
- **LDROBOT LD19 / D500-кит** (база июня; UART 230400): Alibaba
  [Lonten, 13 лет](https://www.alibaba.com/product-detail/LDROBOT-D500-Lidar-Kit-with-LD06_1601053374178.html)
  €61, MOQ 2, [Kaisheng Century, 45 отзывов](https://www.alibaba.com/product-detail/LDROBOT-LIDAR-LD06-LD19-D200-D500_1601288364156.html)
  €61–136 (LD06/LD19/D200/D500 одной карточкой), [Aismartlink — LD19 kit](https://www.alibaba.com/product-detail/LD19-360-Degree-2D-Lidar-Distance_1601595526553.html)
  €18,5–77 (нижняя цена — не лидар); AliExpress [D500 kit](https://www.aliexpress.com/item/1005007051368551.html)
  €65.
- **YDLIDAR T-mini Plus** (компактнее): на Alibaba не нашёлся ни по одному запросу; AliExpress
  [T-mini Plus kit](https://www.aliexpress.com/item/1005007186772841.html) €97,
  [T-mini Pro Plus](https://www.aliexpress.com/item/1005007399039675.html) €70–78 (78 продаж).
  Дороже C1 при том же классе — C1 остаётся выбором.

## 6. Что проверить у продавца до заказа

- VL53L7CX: что в опции именно **L7CX** (не L8CX и не L4CD), с крышкой, с LDO 1,8 В на борту,
  выведены ли **INT и LPn** — те же вопросы, что к V2-модулям L5CX.
- A010: версия **Lite** (без LCD), в комплекте ли 4-pin кабель 1,25 мм; прошивка с `AT+BAUD`
  до 3000000 (ранние прошивки — до 921600).
- C1: комплект «C1M1» с кабелем; напряжение мотора и пусковой ток по даташиту — под DC-DC.

## Источники

- Sipeed MaixSense-A010: [wiki](https://wiki.sipeed.com/hardware/en/maixsense/maixsense-a010/maixsense-a010.html), [AT-команды](https://wiki.sipeed.com/hardware/en/maixsense/maixsense-a010/at_command_en.html), [Seeed — характеристики](https://www.seeedstudio.com/Sipeed-MaixSense-A010-p-5562.html).
- Arducam ToF B0410: [wiki](https://docs.arducam.com/Raspberry-Pi-Camera/Tof-camera/TOF-Camera/), [Pi Hut](https://thepihut.com/products/time-of-flight-camera-for-raspberry-pi).
- VL53L7CX / L8CX: [ST VL53L8CX](https://www.st.com/en/imaging-and-photonics-solutions/vl53l8cx.html), реестр компонентов `grrtzm/v53l7cx-library`, `rjrp44/vl53l8cx`.
- Листинги — по ссылкам в тексте, поиск Alibaba и AliExpress 2026-09-20.
- Предыстория: `docs/research/2026-06-10-lidar-comparison-tables.md`, `2026-06-10-lidar-integration.md`, `2026-09-20-tof-vl53l5cx.md`.
