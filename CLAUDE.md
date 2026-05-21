# Gamma Spectrometer — проект

**Детектор:** NaI(Tl) Ø63×63 мм + ФЭУ Hamamatsu R1307  
**MCU/ADC:** STM32G474 (12-bit ADC, 4 Msps)  
**Отвечать пользователю на русском языке.**

> **При любой работе с KiCad — читать в начале сессии: `docs/cheatsheet_mistakes.md` + все остальные `docs/*.md` файлы проекта.**

---

## Структура проекта

```
gamma-spectrometer/
├── docs/
│   ├── adc_mcu_board.md          # Плата МК v1.6 (схемотехника)
│   ├── divider_board.md          # Плата делителя + AD8000
│   ├── usb_power_board.md        # USB/питание (общее описание)
│   ├── usb_power_board_schematic.md  # USB/питание (детальная схема)
│   ├── hv_board_schematic.md     # Плата ВВ питания (детальная схема)
│   ├── cheatsheet_mistakes.md    # Шпаргалка: типовые ошибки и правила проверки
│   └── refs/                     # Даташиты
├── firmware/                     # STM32 прошивка (не начата)
├── host/                         # ПО на PC (не начато)
├── pcb/                          # KiCad: все 4 схемы ✓, ERC 0 ошибок ✓ (разводка не начата)
└── simulation/
    ├── models/                   # opamp_g474.sub, opamp_g474_noise.sub, ad8000p.cir
    ├── scripts/                  # analyze_sim1.py, analyze_sim2.py, analyze_sim3.py
    ├── sim1_ad8000.cir           # DONE ✓
    ├── sim2_shaper.cir           # DONE ✓
    └── sim3_noise.cir            # DONE ✓
```

---

## Стек плат (физический порядок снизу вверх)

```
[торец — USB-C кабель]
  1. USB/Power board    Ø45мм  USB-C (JAE DX07S024WJ3R400)
  2. HV board           Ø45мм  MAX1847 flyback + 8-ступ. CW умножитель, 0…−1500В
  3. MCU board          Ø45мм  STM32G474, шейпер CR-RC², ADC
  4. Divider board      Ø42-45мм  Делитель R1307 + AD8000
[торец — ФЭУ R1307 + NaI(Tl)]
```

**Межплатные соединения:**
- Stack: 2×N pin header 2.54мм (размер уточняется при разводке)
- Сигнал (делитель→МК): RG-178 коаксиал + U.FL разъёмы
- Питание делителя (МК→делитель): JST PH 3-пин (+5V_A, AGND, −5V_A)
- −ВВ (ВВ board→делитель): экранированный провод ПВВ-1 (≥5кВ), вдоль края стека

---

## Цепь сигнала

```
NaI(Tl)+R1307 → AD8000(×−15) → коаксиал → CR(OPAMP1) → RC(OPAMP2) → PGA×4(OPAMP3) → ADC
[плата делителя]  ±5V_A           U.FL      [плата МК — G474]                           3.3V
V(anode)=31мВ    amp_out=−431мВ             τ=1мкс       τ=510нс     DC=2.13В
```

**Питание AD8000:** ±5V_A с USB board → через стек → JST PH на плату делителя  
**Полярность:** adc_in идёт ВНИЗ от baseline 2.13В. В DSP: `amplitude = baseline − sample`

---

## Симуляции (все DONE ✓)

### Sim1 — AD8000 (DONE ✓)
`simulation/sim1_ad8000.cir` + `scripts/analyze_sim1.py`
- Rfb=1.5к, Cfb=1п, Cpar=1п → BW=99МГц, peaking<0.5dB ✓
- **Cfb=1пФ обязателен** на PCB делителя

### Sim2 — CR-RC² шейпер (DONE ✓)
`simulation/sim2_shaper.cir` + `scripts/analyze_sim2.py`
- PGA ×4, делитель B Rd=5.9к (V_PB0=0.529В, V_out_DC=2.13В)
- PF=0.202 (G474 GBW=13МГц; идеальный расчёт давал 0.36)

| Источник | Канал/8192 |
|---|---|
| Am-241 (59.5 кэВ) | 101 |
| Cs-137 (662 кэВ) | 1138 |
| Co-60 (1.33 МэВ) | 2277 |
| Tl-208 (2.6 МэВ) | 4470 |
| 3.5 МэВ потолок | 6021 |

### Sim3 — Шум (DONE ✓)
`simulation/sim3_noise.cir` + `scripts/analyze_sim3.py`
- σ_total = 94.4 мкВ rms, σ_baseline = 0.31 канала rms ✓

---

## Платы — статус проектирования

### USB/Power board (СХЕМА В KICAD ✓, ERC ✓)
`docs/usb_power_board_schematic.md` + `pcb/gamma_spectrometer/usb_power_board.kicad_sch`
- LP5907 (U101, +3.3V_A), AMS1117CD-3.3 (U102, +3.3V_D), TPS60400DBV (U104, −5V_A)
- FT231XQ (U103, KiCad символ для FT231XS), PRTR5V0U2X (U_ESD101)
- LC-фильтры 47мкГн на +5V_A и −5V_A (84дБ @ 300кГц)
- **+5V_HV** — power symbol, выход LC-фильтра (L103+C115+C116) от +5V_USB
- **Бюджет USB 2.0**: ~370 мА < 500 мА ✓
- Footprint AMS1117: SOT-223 ✓ (назначен)
- Footprint FT231X: SSOP-16 ✓ (назначен)
- C112, C113 (−5V шина): полярность обратная (+ к GND)

### HV board (СХЕМА В KICAD ✓)
`docs/hv_board_schematic.md`
- **MAX1847EEE+** (U201) flyback контроллер + IRLML0040 (Q201) + ATB322524 T201 (1:10.2) + 8-ступ. CW умножитель
- Питание: +5V_HV (через LC-фильтр с USB board, сырой USB 5В), C206=47мкФ X7R (GRM32ER71A476KE15L) + C204=0.1мкФ + C207=0.1мкФ
- **Снаббер первичный**: D201(BAS516) Анод→Drain/T201pin4, Катод→C208(10нФ)+D202+D203(2×BZT52C18 серия)→GND
- **Вторичная обмотка**: T201 pin2(dot)→GND, T201 pin3→C209 (прямо в CW, без отдельного выпрямителя)
- **CS цепь**: R205(0.1Ом) + C205(100пФ) параллельно, от Source Q201 к GND
- **RC-фильтр выхода**: C13a201‖C13b201+C14a201‖C14b201 (сглаживание) → R220→R231→R232 (3×100кОм) → C15a/b201, C16a/b201, C17a/b201 (к GND) → R233(10Ом)+C18a201‖C18b201 (ВЧ фильтр) → −HV_out; все ВВ конденсаторы: C1812X104K102T 2шт в серии = 50нФ/2кВ; C18: CC1206JKNPOCBN221 2шт в серии = 110пФ/2кВ
- **Обратная связь FB**: CW_out → R207+R209–R217 (10×100МОм 1206, суммарно 1ГОм) → FB; R208 (2.2МОм) от HV_CONTROL к FB; V_HV = −V_CTRL × 454.5; MAX1847 встроенный усилитель ошибки, MCP6001 не используется
- Мониторинг: R221–R230 (10×6.8МОм) + R219(110кОм) + TL431DBZ (U202, Vref_2.5V) → HV_MONITOR напрямую на MCU ADC PA0; R_th≈110кОм, время выборки ADC ≥247.5 цикла
- HV_ENABLE: активный HIGH (MCU HIGH = HV вкл); R204 (10кОм серия) + R203 (100кОм pull-down к GND) на SHDN MAX1847; без P-MOSFET
- ⚠ Снаббер: 4В запас (Vds 40В − 36В клamp) — достаточно при наличии C208
- ⚠ **Заливка силиконом** (вся плата после сборки и проверки)

### MCU board v1.6 (СХЕМА В KICAD ✓, ERC ✓)
`docs/adc_mcu_board.md`
- STM32G474MET6 (KiCad: U302), внутренние OPAMP1/2/3, ADC1 12-bit 4Msps
- Вход: U.FL J_sig_in (±431мВ от AD8000 на делителе)
- Питание делителя: JST PH J_pwr_div (±5V_A, AGND)
- **AD8000 убран** (перенесён на плату делителя в v1.6)
- Шейпер: R_cr_in=10к(R309) + C_diff=100п(C315) + R_diff=10к(R305) [CR], R_int=1к(R308) + C_int=510п(C316) + R_stab=1к(R306) [RC]
- TL431DBZ (KiCad: U301) — VREF+ 2.495В + делители DC-смещения
- W25Q64JVSSIQ (KiCad: U303) — SPI flash
- Кварц ABM8G-16МГц (KiCad: Y301)
- NTC: R303(10к) pull-up → NTC_OUT; NTC_IN → PB12
- HV_CONTROL: PA8 PWM → R302+R304 RC-фильтр → метка HV_CONTROL
- ERC: 2 ошибки исправлены (PWR_FLAG на GND удалён; PWR_FLAG добавлен на VDDA после FB301)

### Divider board (СХЕМА В KICAD ✓)
`docs/divider_board.md`
- ФЭУ: **R1307-01** (гибкие выводы, без цоколя), 11 выводов, J_PMT401 = Conn_01x11
- Делитель: R401–R414 (цепочка+базы), C401–C410 (bypass+накопительные), Q401/Q402/Q403 MMBTA42 (буферы Dy6/Dy7/Dy8)
- **AD8000 (U401)**: R415(Rin 50Ом) и R416(Rg 100Ом) **напрямую от P** (без GND на P); R417=Rfb(1.5к), C415=Cfb(1пФ NP0); C411-C414 децепинг ±5V_A
- ⚠ **НЕ ставить GND-символ на P** — R415(Rin) единственный путь тока делителя к GND; GND на P убивает сигнал
- Выход сигнала: метка AMP_OUT на OUTPUT AD8000; разъём J_sig (U.FL) — при разводке PCB
- −ВВ, питание ±5V_A, разъёмы TP1/TP2/J_pwr — при разводке PCB
- Диаметр ~42–45мм
- Нарисовано: R401–R414 ✓, C401–C410 ✓, Q401/Q402/Q403 ✓, J_PMT401 ✓, U401(AD8000YRDZ)+R415+R416+R417+C415+C411-C414 ✓, метка AMP_OUT на OUTPUT ✓
- Разъёмы (J_sig, J_pwr, TP1, TP2) — добавляются при разводке PCB, не в схеме

---

## Что осталось

1. **Разводка PCB** — все 4 схемы готовы, ERC 0 ошибок. Порядок разводки на усмотрение (предложение: начать с USB/Power board или Divider board)
2. **STM32 прошивка** — DMA ADC, пик-детектор, гистограмма, shproto/USB
3. **ПО на PC** — совместимость с BecqMoni (shproto протокол)

---

## LTspice запуск
```powershell
& "C:\Program Files\ADI\LTspice\LTspice.exe" -b "C:\gamma-spectrometer\simulation\sim2_shaper.cir"
python simulation/scripts/analyze_sim2.py simulation/sim2_shaper.raw
```

## Ключевые детали

- **opamp_g474.sub**: XU1 — `XU1 INM INP_off OUT opamp` (pin1=IN−, pin2=IN+)
- **.raw формат**: transient — time=8B double, vars=4B float; noise — все 4B float
- **AMS1117**: нужен танталовый конденсатор на выходе, с керамикой нестабилен
- **TPS60400 C110(VIN)/C111(CFLY)**: X7R обязательно, не Y5V (bypass VIN и летающий конденсатор)
- **MAX1847 POL**: подключить к VL (N-channel), VL bypass 0.47мкФ
- **MAX1847 COMP**: R201 (1МОм) + C201 (33нФ) последовательно к GND
- **MAX1847 FREQ**: только R202 к GND; R202=180кОм → f≈252кГц
- **HV_ENABLE**: активный HIGH (MCU HIGH = HV вкл); R204 (10кОм) серия + R203 (100кОм) pull-down к GND на SHDN; P-MOSFET не используется
- **+5V_HV**: шина сырого USB 5В через LC-фильтр (L103+C115+C116 на USB board) → HV board; не проходит на MCU board
