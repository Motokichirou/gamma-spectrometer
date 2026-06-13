<h1 align="center">Гамма-спектрометр на NaI(Tl)</h1>

<p align="center">
  <img src="https://img.shields.io/badge/MCU-STM32G474-blue" alt="MCU">
  <img src="https://img.shields.io/badge/детектор-NaI(Tl)%20%C3%9863%C3%9763-green" alt="Detector">
  <img src="https://img.shields.io/badge/ФЭУ-Hamamatsu%20R1307-green" alt="PMT">
  <img src="https://img.shields.io/badge/протокол-shproto-orange" alt="Protocol">
  <img src="https://img.shields.io/badge/статус-в%20разработке-yellow" alt="Status">
  <img src="https://img.shields.io/badge/HW-CERN--OHL--S--2.0-lightgrey" alt="HW License">
  <img src="https://img.shields.io/badge/SW-MIT-lightgrey" alt="SW License">
</p>

<!-- README-I18N:START -->
<p align="center"><b>Русский</b> | <a href="./README.en.md">English</a></p>
<!-- README-I18N:END -->

Портативный цилиндрический гамма-спектрометр на кристалле NaI(Tl) Ø63×63 мм и ФЭУ Hamamatsu R1307, с интерфейсом USB-C, совместимым с [BecqMoni](https://github.com/Am6er/BecqMoni) / AtomSpectra (протокол shproto).

> 📚 **Документация:** [обзор аппаратной части](docs/en/hardware.md) · [прошивка](docs/en/firmware.md) · [протокол shproto](docs/en/protocol.md) · [симуляции](docs/en/simulations.md) (англ.)

---

## Ключевые характеристики

| Параметр | Значение |
|---|---|
| Детектор | NaI(Tl) Ø63×63 мм |
| ФЭУ | Hamamatsu R1307 (8 каскадов, Ø76 мм) |
| Диапазон энергий | 50 кэВ – 3.5 МэВ |
| Целевое FWHM @ Cs-137 | 6.0–6.5 % |
| Каналов спектра | 8192 |
| Скорость счёта | до ~30 000 имп/с |
| МК | STM32G474 @ 170 МГц (12-бит АЦП, 4 Мвыб/с) |
| ВВ питание | 0 … −1500 В (MAX1847 + 8-ступенчатый умножитель CW) |
| Интерфейс | USB-C → UART 600 кбит/с (FT231X), протокол shproto |
| Питание | USB 5 В, ~370 мА |
| Форм-фактор | Цилиндрический стек, платы Ø45 мм |

---

## Аппаратная архитектура

### Стек плат (снизу → вверх)

```
 ┌─────────────────────────────────────────┐
 │  USB/Power board  Ø45 mm               │  ← кабель USB-C
 │  LP5907 · AMS1117 · TPS60400 · FT231X  │
 ├─────────────────────────────────────────┤
 │  HV board         Ø45 mm               │
 │  MAX1847 · IRLML0040 · CW ×8 · −1500 V │
 ├─────────────────────────────────────────┤
 │  MCU board        Ø45 mm               │
 │  STM32G474 · CR-RC² shaper · ADC · SPI │
 ├─────────────────────────────────────────┤
 │  Divider board    Ø42–45 mm            │  ← ФЭУ Hamamatsu R1307 + NaI(Tl)
 │  R1307 divider · MMBTA42 buffers       │
 │  AD8000 CFA (×−15, BW = 99 MHz)        │
 └─────────────────────────────────────────┘
```

Межплатные соединения:
- **Сигнал**: коаксиал RG-178 + U.FL (делитель → МК, ±431 мВ для Cs-137)
- **Питание ±5 В_A**: JST PH 3-пин (МК → делитель)
- **−HV**: экранированный провод ≥5 кВ вдоль края стека (ВВ → делитель)
- **Все прочие сигналы**: pin-header 2×N с шагом 2.54 мм

### Цепь сигнала

```
NaI(Tl) + R1307            Плата делителя                 Плата МК
─────────────    ─────────────────────────────   ──────────────────────────────────
 Сцинтилляция →   Делитель (цепочка 7.72 МОм)   →  CR  (OPAMP1, τ=1 мкс)
 τ_спада=230 нс   Активные буферы (Dy6/7/8)        RC  (OPAMP2, τ=510 нс)
 I_анода(Cs-137)  AD8000 ×(−15), BW=99 МГц      →  PGA ×4 (OPAMP3)
  = 0.94 мА       V_вых = −431 мВ (Cs-137)         V_ADC = baseline 2.116 В
                                                   ADC1_IN12, 4 Мвыб/с
                                                   гистограмма 8192 кан. → USB
```

**Полярность АЦП:** импульсы отрицательны относительно baseline 2.116 В → `amplitude = baseline − sample`

### Калибровка энергии (по симуляции, модель OPAMP G474)

| Источник | Энергия | Канал АЦП / 8192 |
|---|---|---|
| Am-241 | 59.5 кэВ | 101 |
| Cs-137 | 662 кэВ | 1138 |
| Co-60 | 1.33 МэВ | 2277 |
| Tl-208 | 2.61 МэВ | 4470 |
| Потолок | 3.5 МэВ | 6021 |

---

## Статус проекта

| Задача | Статус |
|---|---|
| Схема — плата USB/питания | ✅ Готово (ERC 0) |
| Схема — плата ВВ | ✅ Готово (ERC 0) |
| Схема — плата МК | ✅ Готово (ERC 0) |
| Схема — плата делителя | ✅ Готово (ERC 0) |
| LTspice sim1: устойчивость AD8000 | ✅ Готово (BW=99 МГц, peaking<0.5 дБ) |
| LTspice sim2: шейпер CR-RC² | ✅ Готово (PF=0.202, ширина импульса ~1.5 мкс) |
| LTspice sim3: бюджет шумов | ✅ Готово (σ=94.4 мкВ rms, 0.31 кан. rms) |
| BOM с MPN + footprint | ✅ Готово (81 конд. / 17 MPN, 79 R / 28 MPN, все активные) |
| Разводка PCB — все платы | 🔲 Не начато |
| Прошивка STM32 | 🚧 Этапы 1–2 + самотест работают на Nucleo (shproto, реальная гистограмма); этап 3 — на боевой плате |
| ПО на PC (совместимость с BecqMoni) | 🔲 Не начато |

---

## Структура репозитория

```
gamma-spectrometer/
├── docs/
│   ├── adc_mcu_board.md              # Плата МК v1.6 — полная техническая документация
│   ├── divider_board.md              # Плата делителя + AD8000
│   ├── usb_power_board.md            # Плата USB/питания — обзор
│   ├── usb_power_board_schematic.md  # Плата USB/питания — детальная схема
│   ├── hv_board_schematic.md         # Плата ВВ — детальная схема
│   ├── cheatsheet_mistakes.md        # Правила проверки и журнал прошлых ошибок
│   └── refs/                         # Даташиты
├── firmware/                         # Прошивка STM32G474 (этапы 1–2 на Nucleo работают)
├── host/                             # ПО на PC — BecqMoni/shproto (не начато)
├── pcb/
│   └── gamma_spectrometer/           # Проект KiCad (все 4 платы)
└── simulation/
    ├── models/                       # SPICE-модели (opamp_g474, AD8000)
    ├── scripts/                      # Python-скрипты анализа
    ├── sim1_ad8000.cir               # Устойчивость и полоса AD8000
    ├── sim2_shaper.cir               # Формирователь импульсов CR-RC²
    └── sim3_noise.cir                # Полный бюджет шумов
```

---

## Бюджет шумов (sim3)

| Вклад | FWHM |
|---|---|
| Статистика ФЭУ R1307 + NaI(Tl) | 6.3 % |
| Пульсации ВВ (типично) | 0.3–0.5 % |
| Нелинейность АЦП (12-бит, ENOB ~10.5) | 0.1 % |
| Шум электроники (σ = 94.4 мкВ rms) | **0.064 %** |
| **Итого** | **~6.32 %** |

Шум электроники **на два порядка ниже** статистики ФЭУ — разрешение ограничено ФЭУ, а не электроникой.

---

## Протокол: shproto / AtomSpectra

Совместим с [BecqMoni](https://github.com/Am6er/BecqMoni) и [nanopro](https://github.com/Am6er/nanopro).

- UART 600 000 бит/с, 8N1
- Кадр: `0xFE | CMD | PAYLOAD (с байт-стаффингом) | CRC16-MODBUS | 0xA5`
- Cmd `0x01`: спектр (до 8192 × 32-битных каналов)
- Cmd `0x04`: статус (прошедшее время, CPS, число невалидных импульсов)
- Keep-alive: `0xFF` от ПК каждую ~1 с

---

## Лицензия

Файлы аппаратного дизайна (KiCad, LTspice) — [CERN OHL-S v2](https://ohwr.org/cern_ohl_s_v2.txt)  
ПО (прошивка, host) — [MIT](https://opensource.org/licenses/MIT)
