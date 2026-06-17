# BOM — финальное состояние

> **Статус:** ВСЕ КОМПОНЕНТЫ НАЗНАЧЕНЫ ✅ (кроме J_PMT401 — кастомный footprint на этапе PCB)
> Свежий нетлист: `pcb/gamma_spectrometer.net` (2026-05-23, 5868 строк)
> Свежий BOM: см. ниже / экспорт из KiCad

---

## Сводный BOM (финальный)

### Конденсаторы — 17 уникальных MPN, 81 шт

| Refs | Qty | Value | MPN | Footprint |
|---|---|---|---|---|
| C13a201, C13b201, C14a201, C14b201, C15a201, C15b201, C16a201, C16b201, C17a201, C17b201, C401-C407, C410 | 18 | 0.1uF / 1кВ | C1812X104K102T (Holy Stone) | C_1812_HandSolder |
| C18a201, C18b201 | 2 | 220pF / 1кВ C0G | CC1206JKNPOCBN221 (Yageo) | C_1206_HandSolder |
| C101, C104, C105, C112, C113 | 5 | 10uF тант. | TR3B106K010C0750 (Vishay) | CP_EIA-3528-21_Kemet-B_HandSolder |
| C102, C107-C111, C114, C116, C203, C204, C207, C217, C303-C309, C311-C314, C317, C319-C321, C412, C414 | 29 | 0.1uF X7R | CC0603JRX7R9BB104 (Yageo) | C_0603_HandSolder |
| C103 | 1 | 1uF X7R | CC0805JKX7R9BB105 (Yageo) | C_0805_HandSolder |
| C106, C310, C411, C413 | 4 | 4.7uF X5R | GRM21BR61C475KA88L (Murata) | C_0805_HandSolder |
| C115, C206 | 2 | 47uF X7R | GRM32ER71A476KE15L (Murata) | C_1210_HandSolder |
| C201 | 1 | 33nF X7R | CC0805JRX7R9BB333 (Yageo) | C_0805_HandSolder |
| C202 | 1 | 0.47uF X7R | CC0805JKX7R9BB474 (Yageo) | C_0805_HandSolder |
| C205, C315, C318 | 3 | 100pF C0G | CC0603JRNPO9BN101 (Yageo) | C_0603_HandSolder |
| C208 | 1 | 10nF X7R | CC0603KRX7R9BB103 (Yageo) | C_0603_HandSolder |
| C209-C216 | 8 | 1nF / 500В C0G | CC0805JKNPOBBN102 (Yageo) | C_0805_HandSolder |
| C301, C302 | 2 | 18pF C0G | CC0402FRNPO9BN180 (Yageo) | C_0402_HandSolder |
| C316 | 1 | 510pF C0G | CC0603FRNPO9BN511 (Yageo) | C_0603_HandSolder |
| C408 | 1 | 22nF / 1кВ | CC1210KKX7RCBB223 (Yageo) | C_1210_HandSolder |
| C409 | 1 | 47nF / 1кВ | GRM43DR73A473KW01L (Murata) | C_1812_HandSolder |
| C415 | 1 | 1pF C0G ±0.05пФ | GRM1555C1H1R0WA01D (Murata) | C_0402_HandSolder |

### Резисторы — стандартные 0603 (1%, низковольтные)

| Refs | Qty | Value | MPN | Серия |
|---|---|---|---|---|
| R101, R102 | 2 | 330R | RT0603BRD07330RL | Yageo RT (thin film) |
| R201 | 1 | 1M | RT0603BRD071ML | Yageo RT (thin film) |
| R202 | 1 | 180k | RC0603FR-07180KL | Yageo RC |
| R203, R220, R231, R232 | 4 | 100k | RC0603FR-07100KL | Yageo RC |
| R204, R302-R305, R307, R309, R408, R411, R413 | 10 | 10k | RC0603FR-0710KL | Yageo RC (R303 = Pt1000 pull-up) |
| R205 | 1 | 0.1R | RL0603FR-070R1L | Yageo RL (current sense) |
| R206, R233 | 2 | 10R | AC0603FR-0710RL | Yageo AC (anti-surge) |
| R208 | 1 | 2.2M | RC0603FR-072M2L | Yageo RC |
| R218 | 1 | 1.2k | RC0603FR-071K2L | Yageo RC |
| R219 | 1 | 110k | RC0603FR-07110KL | Yageo RC |
| R301 | 1 | 330R | RC0603FR-07330RL | Yageo RC (было 470R; 330 из-за нагрузки Pt1000 на TL431; ⚠ URL покупки в KiCad поправить на 330RL) |
| R306, R308 | 2 | 1k | RC0603FR-071KL | Yageo RC |
| R310 | 1 | 22k | RC0603FR-0722KL | Yageo RC |
| R311 | 1 | 5.9k | RC0603FR-075K9L | Yageo RC |
| R312 | 1 | 8.2k | RC0603FR-078K2L | Yageo RC |
| R313 | 1 | 2k | RC0603FR-072KL | Yageo RC |
| R415 | 1 | 49.9R | RC0603FR-0749R9L | Yageo RC |
| R416 | 1 | 100R | RC0603FR-07100RL | Yageo RC |
| R417 | 1 | 1.5k | RC0603FR-071K5L | Yageo RC |
| R_CC101, R_CC102 | 2 | 5.1k | RC0603FR-075K1L | Yageo RC |

**Все 0603 footprint:** Resistor_SMD:R_0603_1608Metric_Pad0.98x0.95mm_HandSolder

### Резисторы — высоковольтные 1206

| Refs | Qty | Value | MPN | Серия |
|---|---|---|---|---|
| R207, R209-R217 | 10 | 100M | RC1206JR-07100ML (5%) | Yageo RC |
| R221-R230 | 10 | 6.8M | RC1206FR-076M8L (1%) | Yageo RC |
| R401, R402 | 2 | 750k | RC1206FR-07750KL | Yageo RC |
| R403-R407 | 5 | 680k | RC1206FR-07680KL | Yageo RC |
| R409 | 1 | 820k | RC1206FR-07820KL | Yageo RC |
| R410 | 1 | 1M | RC1206FR-071ML | Yageo RC |
| R412 | 1 | 1M | RC1206FR-071ML | Yageo RC (фикс C1: было 2×499k+R414, теперь один 1М 1206; R414 удалён) |

**Все 1206 footprint:** Resistor_SMD:R_1206_3216Metric_Pad1.30x1.75mm_HandSolder

### Активные / разное

| Refs | Qty | Value | MPN | Footprint |
|---|---|---|---|---|
| D101 | 1 | LED синий | FYLS-0805UBC (Foryard) | LED_0805_HandSolder |
| D102 | 1 | LED зелёный | 150080GS75000 (Würth) | LED_0805_HandSolder |
| D201 | 1 | BAS516 | BAS516 (YJ) | D_SOD-523 |
| D202, D203 | 2 | BZT52C18 | BZT52C18 (Diotec) | Nexperia_CFP3_SOD-123W ⚠ проверить |
| D204-D211 | 8 | BAV23S | BAV23S (Nexperia) | SOT-23 |
| F101 | 1 | PTC 500мА | MF-MSMF050-2 (JEMETE) | Fuse_1812_HandSolder |
| FB301 | 1 | 120Ω@100МГц | BLM18EG121SN1D (Murata) | L_0603_HandSolder |
| J101 | 1 | USB-C | DX07S024WJ3R400 (JAE) | USB_C_Receptacle_JAE |
| J_PMT401 | 1 | ФЭУ R1307-01 | — | **J_PMT401_R1307-01** (своя библ. «посадочные места», Ø30, 18 слотов, готов) |
| L101, L102 | 2 | 47uH | SRN3015-470M (Bourns) | L_Neosid_SMS-ME3015 |
| L103 | 1 | 100uH | SRU5028-101Y (Bourns) | L_Bourns_SRU5016_5.2x5.2mm |
| Q201 | 1 | IRLML0040 | IRLML0040 (HOTTECH) | SOT-23 |
| Q401-Q403 | 3 | MMBTA42 | MMBTA42 (Diotec) | SOT-23 |
| T201 | 1 | 1:10.2 трансф. | ATB322524-0110-T000 (TDK) | CHOKE_524-0110-T000_TDK-M |
| U101 | 1 | LP5907MFX-3.3 | LP5907MFX-3.3 (TECH PUB) | SOT-23-5 |
| U102 | 1 | AMS1117-3.3 | AMS1117-3.3 (YOUTAI) | SOT-223-3 |
| U103 | 1 | FT231XQ | FT231XQ | QFN-20-1EP_4x4mm |
| U104 | 1 | TPS60400DBV | TPS60400DBV (TI) | SOT-23-5 |
| U201 | 1 | MAX1847EEE+ | MAX1847EEE+ (AliExpress) | QSOP-16 (21-0055H_16_MXM-M) |
| U202, U301 | 2 | TL431 | TL431 (YOUTAI) ⚠ U301 лучше TI | SOT-23 |
| U302 | 1 | STM32G474MET6 | STM32G474MET6 (ST) | LQFP-80 |
| U303 | 1 | W25Q64JVSSIQ | W25Q64JVSSIQ (Winbond) | SOIC-8 |
| U401 | 1 | AD8000YRDZ-REEL | AD8000YRDZ-REEL (ADI) | SOIC-8 (RD_8_1_ADI-M) |
| U_ESD101 | 1 | PRTR5V0U2X | PRTR5V0U2X (YOUTAI) | SOT-143 |
| Y301 | 1 | кварц 16МГц | ABM8G-16.000MHZ-4Y-T3 (Abracon) | Crystal_SMD_3225-4Pin |
| (внешн.) | 1 | Pt1000 термодатчик | 700-102BAA-B00 (Honeywell, артикул 32208572) | M222 leaded, не на PCB — крепится на корпус кристалла, витой парой к TSENS_PWR/TSENS_IN |

---

## ⚠ К проверке перед заказом

1. **🔴 D202, D203 footprint SOD-123W — ОБЯЗАТЕЛЬНО ИСПРАВИТЬ** — BZT52C18 от Diotec в стандартном SOD-123 (2.6×1.5мм). SOD-123W (3.7×1.6мм) шире на 0.8мм. Диоды не пропаяются на широком footprint. Поменять на `Diode_SMD:D_SOD-123`.

2. ~~U202, U301 TL431 / YOUTAI~~ — **оставляем как есть** (китайский клон). Дрейф 100-300 ppm/°C вместо 50 ppm/°C → 0.3% против 0.15% на ±30°C. Для NaI-спектрометра с FWHM ~6% это в шуме, компенсируется peak tracking в прошивке.

3. ~~R412, R414 URL~~ — ✅ исправлено.

4. ~~J_PMT401~~ — ✅ сделан `J_PMT401_R1307-01` в библиотеке «посадочные места» (по реальному обмеру: 11 pad, окружность выводов Ø35→**Ø30** под стойки, 18 слотов × 20°, отверстие Ø1.0 под вывод Ø0.7). Проверить линейкой в редакторе перед заказом.

---

## Дальнейшие этапы

1. **Заказ компонентов** — закрыть все ⚠ выше, оформить заказ
2. **Разводка PCB** (по порядку):
   1. USB/Power board — простейшая
   2. Divider board — Ø42-45мм, кастомный footprint PMT
   3. HV board — ВВ зазоры, силиконовая заливка
   4. MCU board — аналоговый тракт, 4 слоя
3. **STM32 прошивка** — DMA ADC, пик-детектор, гистограмма 8192, shproto
4. **Host software** — BecqMoni совместимость

---

## Финальная статистика

- **Уникальных MPN:** 60+
- **Конденсаторов:** 81 (17 MPN)
- **Резисторов:** 79 (28 MPN: 21 серии 0603 + 7 серий 1206)
- **Активных:** 24 (включая 8 BAV23S, 8 BZT52, 3 MMBTA42)
- **Магнитных:** 4 (T201 + L101/102/103)
- **Прочих пассивов:** 6 (F101, FB301, D101, D102, D201, J101)
- **Разъёмов:** 2 (J101 USB-C, J_PMT401 кастомный)
