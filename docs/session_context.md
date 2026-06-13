# Шпаргалка сессии — восстановление контекста после сжатия

> Обновлять перед каждым сжатием. Дата последнего обновления: 2026-06-13 (сессия 9, ~03:30).

---

## ⚡ БЫСТРЫЙ СТАРТ ПОСЛЕ СЖАТИЯ (состояние на 2026-06-13)

**Где проект:** схемы KiCad готовы (ERC 0, BOM с MPN полный), разводка PCB НЕ начата.
**Прошивка: этапы 1+2 + самотест РАБОТАЮТ на Nucleo-G474RE** (перемычка A2→A0 на Arduino-гребёнке).
BecqMoni подключается: COM6, 600000 бод, 8192 канала, статус зелёный, SN GS474-0001.

**Команды разработчика (проверенные):**
```bash
# Сборка (headless, IDE не нужна; путь -import строго с backslash!)
rm -rf /tmp/cube_hb_ws && "C:/ST/STM32CubeIDE_1.19.0/STM32CubeIDE/stm32cubeidec.exe" \
  --launcher.suppressErrors -nosplash \
  -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
  -data /tmp/cube_hb_ws -import 'C:\gamma-spectrometer\firmware\gamma_stage1' \
  -build gamma_stage1/Debug

# Прошивка
"C:/ST/STM32CubeIDE_1.19.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.200.202503041107/tools/bin/STM32_Programmer_CLI.exe" \
  -c port=SWD mode=UR reset=HWrst -w ".../Debug/gamma_stage1.elf" -v -rst

# Тест протокола (порт должен быть свободен от BecqMoni!)
PYTHONIOENCODING=utf-8 python -u firmware/tools/becqmoni_sim.py COM6
```

**Правки кода:** core-модули в `firmware/core/` — ПЕРВОИСТОЧНИК; после правки копировать
в `firmware/gamma_stage1/Core/Src|Inc/` (cp). main.c/it.c правятся только в USER CODE секциях.
Python-патчи файлов с кириллицей: писать скрипт во временный .py файл (Write) и запускать
`python file.py` — heredoc в bash ломает кодировку!

**Отладка на железе (без IDE):** PC ядра → nm+addr2line; переменные → STM32_Programmer_CLI
-c port=SWD mode=HOTPLUG -r32 <addr> <bytes>; адрес ADC-буфера брать из DMA CMAR (0x40020028),
не из nm. Дампы под halt бесполезны для волны (таймеры замирают, DAC молчит).

**Самотест:** -tst enc из командной строки BecqMoni → отчёт за ~6с.
Эталонный результат железа: k=2.0048 ch/code, INL 0.025%FS, FWHM@Cs-экв 0.66%.

**СЛЕДУЮЩИЕ ШАГИ (по приоритету):**
1. **Разводка PCB** — главный фронт. Порядок: USB/Power (простая) → Divider (Ø42мм,
   кастомный footprint B14-38 для R1307, HV-зазоры, C_par≤1пФ у VINM) → HV (зазоры,
   силикон) → MCU (4 слоя, аналог отдельно). Заказ компонентов — BOM готов (bom_fp_work.md).
2. Этап 3 прошивки (после железа): polarity −1, fit m=3, порог по реальному шуму,
   Pt1000-термокомпенсация (план: docs/reference_spectra.md), -inf формат с реального AtomSpectra.
3. Мелочь: фактический рейт генератора ~1.7× от mean_period (разобраться при настройке);
   выбросы в -tst stat на переходах режимов (артефакт переключения, не влияет на enc).

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

**Сессия 4 (2026-05-22):**
- GitHub репозиторий создан: https://github.com/Motokichirou/gamma-spectrometer (ПРИВАТНЫЙ)
- GitHub MCP сервер подключён (claude mcp add -s user, глобальный ~/.claude.json)
- README.md создан и запушен (коммит 363dfb9)
- Протокол shproto полностью изучен по исходникам BecqMoni (см. ниже)
- Footprints/MPN — подход согласован (см. ниже)

**Сессия 9 (2026-06-13, ночь): 🏁 ЭТАП 2 ПРОШИВКИ РАБОТАЕТ — реальный тракт DAC→ADC→DSP**
- Тракт: TIM7 (пуассон, мкс-разрешение) → DAC1+DMA (форма CR-RC², 48 сэмплов @1МГц) → перемычка PA4(A2)→PA0(A0) → ADC1 2.83Мвыб/с circular DMA → ISR → baseline-трекер → LSQ-фит вершины → PUR → гистограмма 8192 → shproto → BecqMoni
- Тестовое распределение амплитуд = РЕАЛЬНОЕ Cs-137 (inverse-CDF из черники, 512 узлов в pulsegen.c)
- Результат: ~6000 cps чистых / ~8500 грязных, 29% PUR — физично для наложений
- **Уроки железа (всё найдено отладкой по SWD: чтение PC, переменных, регистров, дамп буферов):**
  - DAC на AHB2: DMA-записи ТОЛЬКО 32-битные (halfword → TEIF). ST-пример DAC_SignalsGeneration использует WORD
  - DAC-канал НЕ выключать между импульсами: пин hi-Z, кикбэк ADC стягивает провод к 0, baseline рушится
  - EMA baseline — только в коридоре ±threshold
  - dsp_process: #pragma O2 даже в Debug (-O0 ISR душит main)
  - Планировщик импульсов: ISR TIM7 с приоритетом ВЫШЕ ADC; в main нельзя (TX спектра блокирует 0.55с/с)
- **DSP (переносимый, поедет на боевую плату):** амплитуда = вершина LSQ-параболы по 2m+1 точкам вокруг максимума (этап 2: m=8; боевая @4Мвыб/с: m=3); выход в полу-LSB = готовый канал; 12-бит ADC честно заполняет 8192 канала
- Отладочные приёмы: headless build → flash CLI → nm+addr2line по PC → чтение переменных r32 → dump_image буферов. Цикл правка-прошивка-проверка ~40 сек
- Известная мелочь: фактический рейт ~1.7× от расчётного по mean_period (некритично, разобраться при настройке)
- Хвост спектра: inverse-CDF 512→1024 узлов + последний узел = истинный максимум (была обрезка на канале 5772 = квантиль 99.9%); верхние каналы заполняются ✓
- **САМОТЕСТ-ПОДСИСТЕМА (финал сессии 9):** команды -tst off/spec/mono/stat/enc (вводятся из командной строки BecqMoni). -tst enc: автотест 5 амплитуд × 1000 событий → отчёт центроид/σ/FWHM + наклон + INL. Первый отчёт железа: k=2.0048 ch/code, INL 0.025%FS, FWHM @ Cs-экв = 0.66% (электроника+DSP пренебрежимы vs 6.3% ФЭУ — sim3 подтверждён железом!)
- **Упрочнение детектора** (баги найдены ОФЛАЙН-симУЛЯЦИЕЙ точного алгоритма в Python — мощный приём!): гистерезис взвода (триггер после спада <thr/2 — убил призраков хвоста), ретриггер-логика только при развитом пике ≥2×thr (шум на лестнице DAC давал ложный pile-up), falling сбрасывается новым максимумом, повторный рост меряется от минимума спада. Порог этапа 2: 45 (5σ железного шума), min_width 8
- Линкер: -u _printf_float обязателен (nano printf молча давал пустые float и ломал отчёты)
- ⚠ Дампы памяти под halt бесполезны для волны ADC: halt замораживает таймеры → DAC молчит → буфер перезаписывается плоским baseline за мс. Адрес буфера брать из DMA CMAR, а не из nm (после пересборок отличается)
- **ИТОГ СЕССИИ: этапы 1 и 2 прошивки + самотест полностью работают на Nucleo.** Дальше: разводка PCB (железо), этап 3 = пересадка на боевую MCU board (полярность −1, окно фита m=3, Pt1000-термокомпенсация)

**Сессия 8 (2026-06-11): ПОЛНЫЙ АУДИТ НЕТЛИСТА — найдены и исправлены 2 критические ошибки**
- Прочитаны все 191 цепь нетлиста, сверены с документацией. Найдено:
- 🔴 **UART был прямой, а не крест**: FTDI TXD↔PA9(TX), RXD↔PA10(RX) — два выхода вместе. ERC не ловил (пины MCU bidirectional). ИСПРАВЛЕНО: PA9→UART_RX (на FTDI RXD), PA10→UART_TX (от FTDI TXD)
- 🔴 **Буферы делителя были мертвы**: узлы цепочки слиты с динодами DY6/7/8 → база через R на собственном эмиттере → V_BE=0, MMBTA42 заперты навсегда. ИСПРАВЛЕНО: динод = только эмиттер; узел цепочки → R_базы → база; накопит. C между динодными линиями; коллекторы ступенью выше. Контрольные цепи в divider_board.md
- 🟡 R301: 470→**330 Ом** (ратиометрический Pt1000 добавил 0.225мА нагрузки на катод TL431, остаток тока был на грани 1мА минимума)
- 🟡 R414 низ цепочки идёт на GND напрямую (не через P/Rin как в старых доках) — это правильнее, доки обновлены; DC анода ≈0 (не 6.5мВ)
- Pt1000 подключён ратиометрически: R303 от катода TL431 (U301-K) — дрейф опоры сокращается
- Новые правила в cheatsheet_mistakes.md: #7 UART крест, #8 динод=эмиттер, #9 ток TL431
- ~~URL покупки R301~~ — исправлен пользователем ✓
- **Приехала Nucleo-G474RE!** Начат этап 1 прошивки:
  - `firmware/core/` — переносимые модули: shproto.c/h (CRC16+стаффинг+парсер+билдер), spectrum.c/h (гистограмма 8192 + тестовый генератор Cs-137/Am-241), app.c/h (логика -sta/-sto/-rst/-inf + отправка 1Гц)
  - `firmware/nucleo_stage1/main_glue.c` — шаблон для CubeMX-проекта (LPUART1=VCP 600000, DMA circular RX)
  - `firmware/tools/becqmoni_sim.py` — мини-BecqMoni на Python для проверки до настоящего BecqMoni
  - Всё компилируется чисто ARM GCC из CubeIDE 1.19 (-Wall -Wextra -Werror); протокол протестирован (CRC эталон 0x4B37, раунд-трип, стаффинг)
  - CubeIDE уже установлен у пользователя (C:/ST/STM32CubeIDE_1.19.0)
  - План этапов в firmware/README.md
  - **Проект gamma_stage1 СОЗДАН, ИНТЕГРИРОВАН, СОБРАН (2026-06-12/13):**
    - Путь: `firmware/gamma_stage1/` (в репо). FW-пакет ST заблокирован для RU → STM32Cube_FW_G4 V1.6.1 склонирован с GitHub в `C:/Users/motok/STM32Cube/Repository/`
    - ⚠ BSP-нюанс: на Nucleo-G474RE VCP = **LPUART1** через `BSP_COM_Init(COM1)` (НЕ USART2!). Вся интеграция в USER CODE секциях main.c: переинициализация 115200→600000, ручной DMA RX (канал DMA1_Ch1, request LPUART1_RX, handle назван hdma_usart2_rx под сгенерированный обработчик в it.c)
    - Headless-сборка: `stm32cubeidec.exe -application org.eclipse.cdt.managedbuilder.core.headlessbuild -data <tmp> -import 'C:\...\gamma_stage1' -cleanBuild gamma_stage1` (путь с backslash! иначе "No file system for scheme C") — 0 ошибок
    - **✅ ЭТАП 1 ЗАВЕРШЁН (2026-06-13, ночь):** плата прошита и отвечает по shproto на COM6: -sta/-sto/-rst/-inf → -ok, спектр 500 cps, пики на каналах 101 и 1138 как задумано
    - **Эпопея прошивки (диагноз для истории): виноват был USB-кабель.** Симптомы: чтение/SRAM-запись работают, пакетные операции (flash erase/program, апгрейд ST-Link) рвут связь («Unable to get core ID», «Fail reading CTRL/STAT», «corrupted firmware data»). Перепробовано: режимы UR/HWrst, частоты до 50кГц, ELF/BIN, CubeProgrammer 2.20/2.18, OpenOCD, ручное стирание регистрами. Замена кабеля — всё заработало с первого раза
    - Полезные пути: прошивка = `firmware/tools/flash_stage1.cmd`; headless-сборка см. выше; тест = `PYTHONIOENCODING=utf-8 python -u firmware/tools/becqmoni_sim.py COM6`
    - ⚠ ST-Link остался на старой прошивке V3J9 (апгрейд падал из-за кабеля) — повторить апгрейд с новым кабелем (Help → ST-LINK Upgrade или STLinkUpgrade.jar), не критично
    - **🏁 ЭТАП 1 ПОЛНОСТЬЮ ЗАКРЫТ (2026-06-13 ~01:00):** BecqMoni подключён (зелёный статус, SN GS474-0001), спектр набран и экспортирован в XML (78 с, 502 cps, пики 101/1140). Файл: `firmware/stage1_first_spectrum.xml`
    - **Найдено на железе и исправлено:**
      - Статус «Неизвестно» → BecqMoni проверяет устройство командой `-cal`: нужен ОДИН текстовый кадр, split("\r\n") > 2 строк, предпоследняя = серийник
      - `-inf` для dead time: токены [3]=rise int, [5]=fall int, [9]=частота
      - **elapsed в cmd=0x04 — СЕКУНДЫ, не мс** (TimeSpan.FromSeconds) — таймер бежал ×1000
    - **СЛЕДУЮЩИЙ ШАГ — этап 2:** DAC1 (PA4) генерирует импульсы → ADC1+DMA 4 Мвыб/с ловит → baseline + peak detect + PUR → реальная гистограмма вместо синтетики. Архитектура DSP: docs/adc_mcu_board.md раздел 8

**Сессия 7 (2026-06-10): AD4084 / DPP-анализ → решение: v1 сейчас, v2 потом**
- Пользователь предложил рассмотреть внешний АЦП без аналоговой обработки (DPP). Изучены даташиты: LTC2387-18, LTC2387-16, **AD4084** (16-bit 20 MSPS, Rev 0 07/2025) — все в `docs/refs/`
- AD4084 детально прочитан (94 стр.), полный анализ: `docs/ad4084_dpp_design.md`
- Ключевые находки AD4084: Event Detection (аппаратный триггер) + FIFO 16K (mode 10 = захват окна после события) + SPI для MCU (Figure 51 = наш кейс); НО: интерфейс 1.1В (нужны трансляторы), SPI = только FIFO (непрерывный поток — только LVDS), REFIN = внешний 3.0В (ADR4530)
- Парадокс: DPP на SPI даёт ~5-10 kcps (мёртвое время чтения FIFO) — МЕНЬШЕ чем v1 (30 kcps); FWHM не улучшается (ФЭУ-лимит)
- AD8000 может качать AD4084 напрямую (без FDA): сдвиг baseline +2.8В инжекцией через R_inj≈2.7к от −5V_A, LO_THRESHOLD, потеря 1 бита некритична
- **РЕШЕНИЕ: v1 (аналоговый шейпер) в железо. v2 (AD4084) = будущий апгрейд, заменой только MCU board в стеке. ТЗ готово в ad4084_dpp_design.md**

**Сессия 6 (2026-05-23, вечер): архитектурные решения + Pt1000**
- Архитектура подтверждена: **shproto через UART/FT231XQ** (НЕ аудиоустройство), MCU делает peak detection + накопление гистограммы, BecqMoni отображает/экспортирует
- **Главное ТЗ прошивки = термокомпенсация** (полный план в `docs/reference_spectra.md` секция "Архитектура прошивки и термокомпенсация")
- Принцип: 1 прибор = 1 кристалл = 1 ФЭУ = 1 калибровка (вшита в flash под конкретное железо)
- **NTC → Pt1000**: переход с Murata NCP18XH103 (β-формула, дрейф) на Honeywell **700-102BAA-B00** (артикул 32208572): Pt 1кΩ@0°C, ±0.15%, M222 leaded, -50..+300°C
  - Резолюция через hardware oversampling ADC4 256× → 16 бит → 0.02°C/бит
  - Долговременная стабильность >10 лет vs NTC ~2°C/10 лет
  - Формула Callendar-Van Dusen (или линейная для нашего диапазона)
- Проанализированы реальные спектры пользователя (черника Cs-137, CsI Маринелли, Lu-176, Am-241, Ra-226, Th-232) — сохранено в `docs/reference_spectra.md` как baseline для сравнения после запуска прошивки
- Замечено: текущий Sypher показывает 5.05% FWHM на NaI 40×40 + R1306 — это близко к физическому пределу. Наш план 6-6.5% на NaI 63×63 + R1307 — реалистичный.

**Задачи на завтра (2026-05-24):**
1. 🔥 **KiCad** — пользователь сам обновит MCU board: NTC → Pt1000, переименовать NTC_OUT/IN → TSENS_PWR/IN, value R303 оставить 10к pullup
2. **Hantek DSO2D15** — подключение к ПК (драйверы / Device Manager) — перенесено с сегодня
3. **Структура `firmware/`** проекта под STM32G474MET6 (CubeMX + skeleton)
4. **Заказ компонентов** включая Pt1000 700-102BAA-B00 (если ещё не оформлен)

**Сессия 5 (2026-05-23): ✅ ВЕСЬ BOM НАЗНАЧЕН**
- Полный BOM с MPN и footprint'ами заполнен для всех 4 плат
- Свежий нетлист экспортирован (`pcb/gamma_spectrometer.net`, 5868 строк)
- Финальный BOM с группировкой и URL покупки: `bom_fp_work.md`
- Все КОНДЕНСАТОРЫ (81 шт, 17 MPN), РЕЗИСТОРЫ (79 шт, 28 MPN), LED (2), ИНДУКТИВНОСТИ (3), ФЕРРИТ (1), ПРЕДОХРАНИТЕЛЬ (1) — назначены
- **Ключевые решения сессии 5:**
  - Стандартный bypass: 100нФ X7R 0603 (вместо 0402 — удобнее паять руками)
  - 4.7мкФ X5R 0805 (один MPN GRM21BR61C475KA88L на C106/C310/C411/C413)
  - 47мкФ X7R 1210 — консолидация C115 и C206 на один MPN (GRM32ER71A476KE15L)
  - Танталовые 10мкФ: Vishay TR3B106K010C0750 (ESR 0.75 Ом, удовлетворяет AMS1117 < 1 Ом)
  - 1пФ NP0 для Cfb AD8000: GRM1555C1H1R0WA01D (±0.05пФ — лучшее по точности)
  - HV ёмкости 1кВ X7R 1812 (Holy Stone C1812X104K102T) — 18 шт (CW + накопит + RC-фильтр + bypass делителя)
  - HV-конденсатор 220пФ C0G 1кВ 1206 (CC1206JKNPOCBN221) — 2 шт для ВЧ фильтра выхода ВВ
  - LED 0805 (D101 синий FYLS, D102 зелёный Würth) — корпус 0805 вместо 0402 для удобства
  - Феррит FB301: BLM18EG121SN1 — **корпус 0603, НЕ 0402** (исправлено в ходе сессии)
  - Резисторы: Yageo RC0603 для стандартных, RC1206 для HV (R207/R209-R217 100M в 5% JR-серии, остальные 1% FR-серии)
  - R415: 49.9Ω E96 вместо 50Ω (в каталоге нет точно 50)
  - L101/L102: Bourns SRN3015-470M (47мкГн 400мА), footprint Neosid SMS-ME3015 (геометрия идентична)
  - L103: Bourns SRU5028-101Y (100мкГн 470мА), footprint L_Bourns_SRU5016 (XY совпадает, разная высота)
- **Изменения от исходных доков:**
  - U103 чип сменён: **FT231XS → FT231XQ** (QFN-20 вместо SSOP-16)
  - Многие 0402 bypass → 0603 (удобство пайки)
  - LED 0402 → 0805
- **Что осталось:**
  - 🔴 D202/D203 — footprint SOD-123W → нужно SOD-123 (Diotec корпус уже)
  - J_PMT401 — кастомный footprint B14-38 (на этапе разводки)
  - PCB разводка всех 4 плат
  - Прошивка STM32G474
  - Host software (BecqMoni совместимость)
- **Отложенные/принятые компромиссы:**
  - U202, U301 TL431 / YOUTAI (китайский клон) — оставлен, прошивка компенсирует через peak tracking
  - MAX1847 с AliExpress (риск клона/б/у)
  - AMS1117 / TPS60400 / TL431 / PRTR — китайские суппайеры YOUTAI

## Протокол shproto — полная спецификация (из BecqMoni исходников)

### Фрейм
```
0xFF  0xFE  [CMD][PAYLOAD...][CRC_LSB][CRC_MSB]  0xA5
```
- 0xFF — keepalive-префикс (каждые 1000 мс таймером + перед каждой командой)
- 0xFE — START (reset parser)
- 0xFD — ESC (следующий байт инвертирован ~byte)
- 0xA5 — FINISH
- CRC16 MODBUS: init=0xFFFF, poly=0xA001, считается по [CMD+PAYLOAD], CRC-байты тоже stuffed

### Команды PC → device (cmd=0x03, payload=ASCII)
| Команда | Ответ | Действие |
|---|---|---|
| `-sta` | `-ok` | Start накопление |
| `-sto` | `-ok` | Stop |
| `-rst` | `-ok` | Reset spectrum |
| `-inf` | текст с `T1 <°C>` | Инфо + температура |

**Startup sequence (BecqMoni):** `-sto` → wait `-ok` → `-rst` → wait `-ok` → `-sta` → wait `-ok`

### Пакеты device → PC

**cmd=0x01** — спектр:
```
[offset_lo][offset_hi] [ch_b0][ch_b1][ch_b2][ch_b3]...
```
- offset: uint16 LE (с какого канала)
- каналы: uint32 LE, маска & 0x7FFFFFF
- можно слать частями (разные offset)

**cmd=0x04** — статус (ТРИГГЕР обновления экрана BecqMoni!):
```
[elapsed 4B LE — ⚠ СЕКУНДЫ, не мс!][cpu_load 2B LE][cps 4B LE][invalid_pulses 4B LE]
```
- elapsed в СЕКУНДАХ (BecqMoni: `TimeSpan.FromSeconds(ElapsedTime)`; выяснено на железе 2026-06-13 — таймер бежал ×1000), маска 0x7FFFFFF
- ⚠ BecqMoni обновляет спектр НА ЭКРАНЕ только при получении cmd=0x04 (не 0x01!)

**cmd=0x03** — текстовый ответ (для `-ok`, `-inf`, etc.)

### Важно для прошивки
- BecqMoni настроить на 8192 каналов (иначе будет downsampling: суммирует группы)
- Температура: ответ `-inf` должен содержать `T1 <число>` (парсится split("T1 ")[1].split(' ')[0])
- Каждые ~1 сек: отправить cmd=0x01 (полный спектр) → затем cmd=0x04 (статус)
- Incoming 0xFF (keepalive от PC) — игнорировать

## Подход к назначению footprints / MPN (согласован сессия 5)

**Рабочий процесс:**
1. Claude называет один компонент (номинал + корпус + требования)
2. Пользователь находит в каталоге (LCSC / Mouser / Digi-Key)
3. Вносит MPN и ссылку на покупку в свойства символа KiCad (Edit Symbol Fields)
4. Переходим к следующему компоненту

**Поля KiCad для заполнения:** `MPN` (или `Part Number`), `Purchase URL` (или `Datasheet`/`URL`).

**Порядок плат:** USB/Power → HV → MCU → Divider (от простой к сложной).

---

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
- U103=FT231XQ (QFN-20-1EP_4x4mm — финальный выбор сессии 5)
- U101=LP5907 (KiCad U101), U102=AMS1117 (символ TO-252, footprint→SOT-223 при разводке)
- U104=TPS60400DBV
- C112, C113 на −5V шинах: **полярность обратная** (+ к GND)
- +5V_HV: через L103(100мкГн)+C115(47мкФ)+C116(100нФ) LC-фильтр от +5V_USB
- TPS60400: C111=летающий конд.(CFLY, X7R!), C110=bypass VIN (X7R!), C112=выход (тант., + к GND)

### MCU board — СХЕМА ЗАВЕРШЕНА ✓

**Компоненты:**
- U302 = STM32G474MET6 (LQFP-80, 12×12мм)
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
- Pt1000 (700-102BAA-B00, внешний на кристалле): R303(10к) pull-up → TSENS_PWR метка; TSENS_IN метка → PB12 (ADC4 oversampling 256×)
- HV_ENABLE метка → PB10; HV_MONITOR метка → PA0
- PA9 (USART1_TX) → метка **UART_RX** (на вход FTDI RXD); PA10 (USART1_RX) → метка **UART_TX** (с выхода FTDI TXD) — КРЕСТ, исправлено сессия 8; LED → PB6
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
| R1 | R301 | 330Ω (было 470, изменено сессия 8 — нагрузка Pt1000) | TL431DBZ bias |
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
