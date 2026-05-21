# Шпаргалка сессии — восстановление контекста после сжатия

> Обновлять перед каждым сжатием. Дата последнего обновления: 2026-05-21 (сессия 3).

---

## Статус проекта (на текущий момент)

### KiCad схемы — ВСЕ ГОТОВЫ ✓
| Плата | Статус | Файл схемы |
|---|---|---|
| USB/Power board | ✓ схема + ERC ✓ | `pcb/gamma_spectrometer/usb_power_board.kicad_sch` |
| HV board | ✓ схема + ERC ✓ | `pcb/gamma_spectrometer/hv_board.kicad_sch` |
| Divider board | ✓ схема + ERC ✓ | `pcb/gamma_spectrometer/divider_board.kicad_sch` |
| MCU board | ✓ схема + ERC ✓ (0 ошибок) | `pcb/gamma_spectrometer/mcu_board.kicad_sch` |

**Все 5 плат в одном KiCad проекте:** `pcb/gamma_spectrometer/gamma_spectrometer.kicad_pro`

**Схема нумерации компонентов (перенумеровано 2026-05-21):**
| Плата | Диапазон | Пример |
|---|---|---|
| USB/Power board | 1xx | R101, C101, U101 |
| HV board | 2xx | R201, C201, U201 |
| MCU board | 3xx | R301, C301, U302 |
| Divider board | 4xx | R401, C401, U401 |

**Разводка PCB:** не начата ни для одной платы.
**Разъёмы** (U.FL, JST PH, TP, header) в схемах **НЕ нарисованы** — добавляются при разводке.

---

## Текущий статус (2026-05-21)

**Перенумерация компонентов ВЫПОЛНЕНА** (USB=1xx, HV=2xx, MCU=3xx, DIV=4xx).
**report.txt удалён** — единственный источник нетлиста: `gamma_spectrometer.net`.
**Вся документация обновлена** под новую нумерацию.

**Аудит документации ВЫПОЛНЕН (сессия 2, 2026-05-21)** — найдены и исправлены:
- Смещение меток C104-C111 в usb_power_board_schematic.md (критично: C105 = AMS1117 ВЫХОД, не вход!)
- TPS60400 CFLY=C111, VIN=C110 (было C109/C108 — неверно во всех доках)
- adc_mcu_board.md: U9→U202 (TL431 на HV board), U301→U302 (STM32 ref)
- divider_board.md: Q103-Q105→Q401-Q403, C1-C10→C401-C410, C8-C10→C408-C410, R1-R11→R401-R414
- cheatsheet_mistakes.md: C13,C14→C112,C113
- usb_power_board.md: U1-U4→U101-U104, REF3025→TL431DBZ, old cap refs добавлены
- usb_power_board_schematic.md: секции 2-10 обновлены с KiCad-рефами
- Все файлы кросс-проверены через нетлист gamma_spectrometer.net

**Аудит документации ВЫПОЛНЕН (сессия 3, второй проход, затем третий проход)** — найдены и исправлены:
- hv_board_schematic.md: U9→U202 (TL431 на HV board) в разделах 7 и 11; C33→C217 в разделе 11
- usb_power_board.md: архитектурная диаграмма (U1→U101, U2→U102, U3→U104, L1→L101, L2→L102, L3→L103, C19→C115, REF3025→TL431DBZ)
- usb_power_board.md: диаграмма компоновки (D5/D1-D4/U4/U1/U2/U3 → D101/D102/U103/U101/U102/U104)
- usb_power_board.md: BOM (D1 зел.+D5 синий → D102 зел.+D101 синий)

**Аудит документации ВЫПОЛНЕН (сессия 3, третий проход, верификация по нетлисту)** — найдены и исправлены:
- hv_board_schematic.md Section 9: U9→U202 (последняя пропущенная инстанция в таблице бюджета)
- usb_power_board_schematic.md Section 8: D5→D5/D101, R1→R1/R102, R2→R2/R101 (заголовок + 2 строки)
- divider_board.md: C411/C412 = −5V_A bypass (не +5V_A), C413/C414 = +5V_A bypass (не −5V_A) — метки были перепутаны, верифицировано нетлистом
- adc_mcu_board.md Section 3.5: ADC2 sampling 24.5→247.5 циклов (опечатка; требование из hv_board_schematic R_th=110кОм)
- usb_power_board.md бюджет тока: TPS60400 питается от +5V_A (верифицировано нетлистом), строка +5V_A: ~30→~58мА, строка −5V_A уточнена, итог 370→373мА
- Отчёт о находках: docs/audit_pass3_findings.md

**Следующий нерешённый вопрос:** footprints и MPN для компонентов.
- Пользователь не согласился с подходом "таблица → ручной ввод через Edit Symbol Fields"
- Его предпочтительный подход неизвестен — нужно обсудить в начале следующей сессии

## Следующий этап: разводка PCB

Предложение по порядку:
1. **USB/Power board** — простейшая по аналогу, хорошо разогреться
2. **Divider board** — маленькая (Ø42мм), критична по HV-зазорам и C_par ≤ 1пФ на VINM AD8000
3. **HV board** — ВВ-зазоры, заливка силиконом после сборки
4. **MCU board** — аналоговый тракт, 4-слойная разводка

---

## Ключевые решения (не очевидные, важно помнить)

### Общие
- Хедеры между платами: **2×N pin header 2.54мм** — размер считается при разводке
- Питание делителя: ±5V_A через header → MCU board → JST PH → Divider board
- −ВВ кабель: от HV board вдоль края стека к Divider board, в обход MCU board
- KiCad символ NRST на STM32G474METx = **PG10** (из даташита Table 12, стр.58)

### Divider board (делитель ФЭУ)
- ФЭУ: R1307-01, 11 гибких выводов, J_PMT401 = Conn_01x11
- **КРИТИЧНО: НЕ ставить GND на P (анод)**. R415 (Rin 50Ом) — единственный путь тока делителя к GND. GND на P убивает сигнал.
- Анодная цепь: P → R415(Rin 50Ом) → GND; P → R416(Rg 100Ом) → −IN (pin2) AD8000
- Буферы Dy6/7/8: Q401/Q402/Q403 (MMBTA42)
- AD8000: U401, SOIC-8 (AD8000YRDZ). CFA — Rfb(R417=1.5к) идёт FEEDBACK(pin1)↔OUTPUT(pin6)
- Выход AD8000: сетевая метка **AMP_OUT** на OUTPUT (pin6)
- C415 = **Cfb 1пФ NP0** (параллельно Rfb=R417)

### AD8000 пины (SOIC-8, CFA!)
| Пин | Имя | Подключение |
|---|---|---|
| 1 | FEEDBACK | ← Rfb(R417 1.5к) ‖ Cfb(C415 1пФ) ← OUTPUT(pin6) |
| 2 | −IN | ← Rg(R416 100Ом) ← P (анод) |
| 3 | +IN | → GND |
| 4 | −VS | → −5V_A |
| 5 | NC | × (No Connect) |
| 6 | OUTPUT | → AMP_OUT (метка); → Rfb/Cfb → FEEDBACK |
| 7 | +VS | → +5V_A |
| 8 | *POWERDOWN | → +5V_A (усилитель всегда включён) |
| EP | Exposed Pad | → GND |

### HV board
- MAX1847EUP (QSOP-16): KiCad U201
- Снаббер: D201(BAS516) анод→Drain/T201pin4; катод→C208(10нФ)+D202+D203(2×BZT52C18=36В)→GND
- T201 ATB322524: pin1→+5V_HV, pin4→Drain Q201, pin2(dot)→GND, pin3→C209 (CW вход)
- CS цепь: Q201 Source → CS(pin13) И → R205(0.1Ом) → GND; C205(100пФ) параллельно R205
- BAV23S в CW: используется как 1 диод (pin1=A, pin2=K, pin3=NC с крестиком ERC)
- FB: 10×100МОм (R207+R209–R217) от CW_out до FB; R208(2.2МОм) от HV_CONTROL; V_HV = −V_CTRL × 454.5
- HV_ENABLE: активный HIGH; R204(10к) серия + R203(100к) pulldown к GND на SHDN; без P-MOSFET
- TL431(KiCad U202): Vref=2.5В; R218(1.2к) от +5V_HV; REF замкнут на Cathode

### USB/Power board
- U103=FT231XQ (символ QFN-20, footprint→SSOP-16 при разводке)
- U101=LP5907 (KiCad U101), U102=AMS1117 (символ TO-252, footprint→SOT-223 при разводке)
- U104=TPS60400DBV
- C112, C113 на −5V шинах: **полярность обратная** (+ к GND)
- +5V_HV: через L103(100мкГн)+C115(47мкФ)+C116(100нФ) LC-фильтр от +5V_USB
- TPS60400: C111=летающий конд.(CFLY, X7R!), C110=bypass VIN (X7R!), C112=выход (тант., + к GND)

### MCU board — СХЕМА ЗАВЕРШЕНА ✓

**Компоненты:**
- U302 = STM32G474METx (UFQFPN68)
- U301 = TL431DBZ (SOT-23) — VREF+ 2.495В
- U303 = W25Q64JVSSIQ (SPI flash)
- Y301 = кварц ABM8G-16.000MHZ-4Y-T3 (16МГц, пины PF0/PF1)
- FB301 = BLM18EG121SN1 (феррит VDDA)

**Сигнальная цепь:**
```
AMP_OUT → R309(10к=R_cr_in) → node_CR → C315(100п=C17) → PA3(VINM OPAMP1)
                                        ↓
                              C318(100п=C21) → R307(10к=R13) → DAC_TEST(PA4)
PA2(VOUT OPAMP1) ← R305(10к=R_diff) ← PA3
PA2 → C317(100н=C18) → R308(1к=R_int) → PA5(VINM OPAMP2)
PA6(VOUT OPAMP2) ← R306(1к=R_stab)‖C316(510п=C19) ← PA5
PA6 → C314(100н=C20) → PB0(VINP OPAMP3)
PB1(VOUT OPAMP3) → ADC1_IN12
```

**DC-смещение:**
- Делитель A: катод U301 → R312(8.2к) → R313(2к) → GND; узел → PA1+PA7; C321(100н) bypass
- Делитель B: катод U301 → R310(22к) → R311(5.9к) → GND; узел → PB0; C320(100н) bypass
- VREF+: катод U301 → провод (не метка!) → VREF+ STM32

**Прочее:**
- HV_CONTROL: PA8 → R302(10к) → mid → C(100н)→GND; → R304(10к) → HV_CONTROL метка; C(100н)→GND
- NTC: +3.3V_D → R303(10к) → NTC_OUT метка; NTC_IN метка → PB12
- HV_ENABLE метка → PB10; HV_MONITOR метка → PA0
- UART_TX → PA9; UART_RX → PA10; LED → PB6
- SWDIO → PA13; SWCLK → PA14; NRST/PG10 → 4 пада через-hole (SWD)
- No Connect: PA11, PA12, PB7, PB8, PB9, PB11, PB13, PB14, PB15

**ERC MCU board:** 2 исправленные ошибки:
1. PWR_FLAG (#FLG0302) на шине GND → удалён
2. Net-(U302-VDDA) без Power output → добавлен PWR_FLAG между FB301 и VDDA

---

## Таблица KiCad-обозначений MCU board (300-серия)

### Резисторы
| Логич. имя | KiCad ref | Номинал | Функция |
|---|---|---|---|
| R1 | R301 | 470Ω | TL431DBZ bias |
| R2 | R303 | 10к | NTC pull-up |
| R3 | R302 | 10к | HV RC stage 1 (PA8→mid) |
| R4 | R304 | 10к | HV RC stage 2 (mid→HV_CONTROL) |
| R5 | R312 | 8.2к | Делитель A верх |
| R6 | R313 | 2к | Делитель A низ |
| R7 | R310 | 22к | Делитель B верх |
| R8 | R311 | 5.9к | Делитель B низ |
| R9 (R_cr_in) | R309 | 10к | AMP_OUT → node_CR |
| R10 (R_diff) | R305 | 10к | OPAMP1 feedback PA2↔PA3 |
| R11 (R_int) | R308 | 1к | RC integrator C317→PA5 |
| R12 (R_stab) | R306 | 1к | OPAMP2 feedback PA5↔PA6 |
| R13 (R_inj) | R307 | 10к | Test pulse inject → node_CR |

### Конденсаторы шейпера и делителей
| Логич. имя | KiCad ref | Номинал | Функция |
|---|---|---|---|
| C17 (C_diff) | C315 | 100пФ NP0 | CR cap node_CR→PA3 |
| C18 (C_couple) | C317 | 100нФ X7R | AC coupling PA2→R308 |
| C19 (C_int) | C316 | 510пФ NP0 | RC integrator PA5↔PA6 |
| C20 (C_buf_in) | C314 | 100нФ X7R | AC coupling PA6→PB0 |
| C21 (C_inj) | C318 | 100пФ NP0 | Test pulse inject cap |
| C_divA bypass | C321 | 100нФ | Bypass делителя A |
| C_divB bypass | C320 | 100нФ | Bypass делителя B |
| C_crys1 | C301 | 18пФ NP0 | Кварц PF0 |
| C_crys2 | C302 | 18пФ NP0 | Кварц PF1 |

### Другие платы
| Плата | Компонент | KiCad ref |
|---|---|---|
| HV board | MAX1847EUP | U201 |
| HV board | TL431DBZ | U202 |
| HV board | IRLML0040 | Q201 |
| HV board | ATB322524 | T201 |
| USB board | FT231XQ | U103 |
| USB board | LP5907MFX-3.3 | U101 |
| USB board | AMS1117CD-3.3 | U102 |
| USB board | TPS60400DBV | U104 |
| Divider board | AD8000YRDZ | U401 |
| Divider board | MMBTA42 (Dy6/7/8) | Q401/Q402/Q403 |

---

## Цепь сигнала (сводно)

```
NaI(Tl)+R1307 → AD8000(×−15) → AMP_OUT → коаксиал U.FL → CR(OPAMP1) → RC(OPAMP2) → PGA×4(OPAMP3) → ADC1_IN12
[Divider board]  ±5V_A          метка       RG-178         [MCU board — STM32G474]
V_anode=31мВ    −431мВ(Cs-137)             τ=1мкс  τ=510нс  DC=2.116В    baseline−sample
```

**Полярность:** импульсы на ADC отрицательные (нечётное число инверсий). В DSP: `amplitude = baseline − sample`.

---

## Правила работы с KiCad

- Схему рисует **пользователь руками**, Claude только советует
- Перед проверкой скриншота: проходить по каждому пину явно
- **Разъёмы в схеме НЕ рисуем** — только метки/символы питания
- VREF+ к катоду TL431 — **провод**, не метка
- Делители A и B питаются от **катода TL431** (2.495В), НЕ от +3.3V_A
- Даташиты искать сначала в `docs/refs/`, потом спрашивать

## Правила работы с нетлистом KiCad

- **gamma_spectrometer.net** = актуальный нетлист, авторитетный источник (report.txt удалён)
- Читать .net ТОЛЬКО через Grep по имени цепи + Read с offset — никогда не читать целиком (4000+ строк)
- Для проверки значения компонента: `Grep "comp.*R305"` → `Read offset=<строка> limit=5`
- **НЕ читать .kicad_sch файлы** — огромные, сжигают токены; вся инфо в docs/*.md
