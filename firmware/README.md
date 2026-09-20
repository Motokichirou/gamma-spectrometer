# Прошивка STM32G474 — гамма-спектрометр

## Этапы разработки

| Этап | Железо | Цель | Статус |
|---|---|---|---|
| **1** | Nucleo-G474RE | shproto через VCP → BecqMoni рисует тестовый спектр | ✅ работает |
| **2** | Nucleo-G474RE | DAC→ADC петля: синтетические импульсы, peak detect, реальная гистограмма | ✅ работает |
| 3 | Боевая MCU board | OPAMP-шейпер, реальные импульсы, HV, Pt1000-термокомпенсация | ⏳ ждёт железа |

Дополнительно реализованы: подсистема самотеста (`-tst`), настраиваемый порог DSP
(`-thr`) и энергокалибровка во flash (`-cal`/`-rcal`/`-wcal`, совместимая с BecqMoni —
см. [docs/ru/protocol.md](../docs/ru/protocol.md)).

## Структура

```
firmware/
├── core/                 # Переносимые модули (без HAL — компилируются где угодно)
│   ├── shproto.h/.c      # Протокол BecqMoni: CRC16 MODBUS, стаффинг, парсер, билдер
│   ├── spectrum.h/.c     # Гистограмма 8192
│   ├── dsp.h/.c          # Детектор импульсов: baseline, pile-up, LSQ-фит вершины
│   ├── pulsegen.h/.c     # Генератор тест-импульсов (источник самотеста)
│   ├── selftest.h/.c     # FSM самотеста ENC / линейности
│   └── app.h/.c          # Команды (-sta/-sto/-rst/-inf/-cal/-rcal/-wcal/-calclr/-thr/-tst) + спектр/статус 1 Гц
├── gamma_stage1/         # Проект CubeIDE (Nucleo): клей HAL, stage2_hw.c (DAC/ADC/DSP), cal_store.c (flash)
├── nucleo_stage1/
│   └── main_glue.c       # Шаблон интеграции в CubeMX-проект (копировать в USER CODE)
├── tools/                # becqmoni_sim.py, flash_stage1.cmd
└── README.md
```

`core/` не зависит от HAL — связь с железом через функции-порты (`app_port_uart_send`,
`app_port_millis`, `app_port_temperature_c`), которые реализуются в main.c. Слабые хуки
калибровки переопределяются в `cal_store.c`. Этот же код без изменений поедет на боевую
плату.

## Этап 1 — пошаговая инструкция

### 1. Установить STM32CubeIDE
https://www.st.com/en/development-tools/stm32cubeide.html (бесплатно, ~1 ГБ).

### 2. Создать проект
1. File → New → STM32 Project
2. Вкладка **Board Selector** → `NUCLEO-G474RE` → Next
3. "Initialize all peripherals with their default Mode?" → **Yes**
   (CubeMX сам назначит LPUART1 на PA2/PA3 — это VCP через ST-LINK)

### 3. Настроить в CubeMX (.ioc)
1. **Clock Configuration:** HCLK = **170 МГц**
   - источник PLL: HSI16, PLLM=4, PLLN=85, PLLR=2
   - CubeMX сам включит Boost mode (R1MODE) при 170 МГц
2. **LPUART1** (Connectivity): Asynchronous, **600000** бод, 8N1
3. **DMA** (вкладка DMA Settings у LPUART1): Add → `LPUART1_RX`,
   Mode = **Circular**, Data Width = Byte
4. Сгенерировать код (Ctrl+S → Generate)

### 4. Подключить core/
1. Скопировать `firmware/core/*.c` в `Core/Src/`, `firmware/core/*.h` в `Core/Inc/`
   (или: Project Properties → C/C++ General → Paths → добавить путь к `firmware/core`)
2. Перенести куски из `nucleo_stage1/main_glue.c` в `Core/Src/main.c`
   в соответствующие секции `USER CODE` (номера секций — в комментариях шаблона)

### 5. Собрать и прошить
Build (молоток) → Run (зелёная стрелка). Прошивается через встроенный ST-LINK.

### 6. Проверить с BecqMoni
1. Узнать номер COM-порта ST-LINK VCP (Диспетчер устройств → Ports)
2. BecqMoni: настройки устройства → AtomSpectra (shproto), COM-порт, **600000 бод**
3. **Каналов: 8192** (иначе BecqMoni будет даунсемплить)
4. Подключиться → старт набора
5. Ожидаемо: накапливающийся спектр с пиками на каналах **101** (как Am-241 59.5 кэВ)
   и **1138** (как Cs-137 662 кэВ), ~500 cps

### Если не работает
- Терминалом (PuTTY, 600000 8N1) на COM-порт: должен идти бинарный поток после `-sta`
- Проверить что LPUART1, а не USART2 подключён к VCP (UM2505, солдер-бриджи SB)
- BecqMoni шлёт при подключении `-sto`→`-rst`→`-sta` и ждёт `-ok` на каждую

## Протокол shproto (краткая шпаргалка)

```
Кадр: 0xFF 0xFE [CMD][PAYLOAD...][CRC_LSB][CRC_MSB] 0xA5
Стаффинг: 0xFE/0xFD/0xA5 внутри кадра → 0xFD + ~байт
CRC16 MODBUS (init 0xFFFF, poly 0xA001) по нестаффленным CMD+PAYLOAD
CMD: 0x01 спектр [offset u16][ch u32 ×N]; 0x03 текст; 0x04 статус (триггер отрисовки!)
Статус: [elapsed_СЕК u32][cpu_load u16][cps u32][invalid u32]  (elapsed в СЕКУНДАХ!)
```

Полная спецификация: `docs/session_context.md` → «Протокол shproto».

## Этап 2 — выполнено ✅

Петля DAC→ADC замкнута целиком на кристалле Nucleo (перемычка PA4/A2 → PA0/A0):
TIM7 (пуассон) → DAC1+DMA (форма CR-RC²) → ADC1 circular DMA @2.83 Мвыб/с → DSP
(`dsp.c`: baseline, pile-up, LSQ-фит вершины) → гистограмма. Подробности и архитектура
термокомпенсации — в [docs/ru/firmware.md](../docs/ru/firmware.md) и
`docs/adc_mcu_board.md` (раздел 8).

## Оценка амплитуды: эталон формы (2026-09-21)

На боевой плате импульс короткий — FWHM 2.56 отсчёта при 4 Мвыб/с, и парабола по 5 точкам занижает
амплитуду на 10–15 %, а главное даёт разброс по фазе прихода: электронное FWHM 7.4 % — вровень с
собственным разрешением NaI. Поэтому `dsp.c` умеет подгонять окно импульса по таблице эталона формы:

```c
#include "dsp_template.h"
dsp_init(&dsp, +1, thr, min_w, max_w, fit_m, emit);
dsp_set_template(&dsp, &dsp_template[0][0], DSP_TPL_NPH, DSP_TPL_NS, DSP_TPL_IPK, DSP_TPL_Q);
```

Без `dsp_set_template` работает прежняя парабола — петля Nucleo (этап 2, широкий пик) не затронута.
Таблица: 32 суб-отсчётные фазы × 7 отсчётов в Q14, генератор `simulation/fix_c320/gen_template.py`
(форма — из LTspice; позже её можно снять с реального тракта усреднением ~10 000 импульсов).
Для каждой фазы решается МНК `y ≈ A·h + B`; свободный член делает оценку нечувствительной к ошибке
трекера базовой линии. Итог: смещение −0.02 %, электронное FWHM 0.44 % при 30 кимп/с, линейность
±0.03 % до 3.5 МэВ; цена ~18 % ЦП при 30 кимп/с. Проверки — `simulation/fix_c320/test_template.py`
и `mc.py --template`.

## Дальше (этап 3)

- Перенос на боевую MCU board: внутренние OPAMP-шейпер CR-RC², реальные импульсы ФЭУ.
- Вызвать `dsp_set_template()` при инициализации боевого тракта.
- Термокомпенсация по Pt1000 (g(T)), управление ВВ.
- Сервисное ПО `gammapult` (см. [host/README.md](../host/README.md)).
