# Прошивка STM32G474 — гамма-спектрометр

## Этапы разработки

| Этап | Железо | Цель | Статус |
|---|---|---|---|
| **1** | Nucleo-G474RE | shproto через VCP → BecqMoni рисует тестовый спектр | 🔧 в работе |
| 2 | Nucleo-G474RE | DAC→ADC петля: синтетические импульсы, peak detect, реальная гистограмма | — |
| 3 | Боевая MCU board | OPAMP-шейпер, реальные импульсы, HV, Pt1000-термокомпенсация | — |

## Структура

```
firmware/
├── core/                 # Переносимые модули (без HAL — компилируются где угодно)
│   ├── shproto.h/.c      # Протокол BecqMoni: CRC16 MODBUS, стаффинг, парсер, билдер
│   ├── spectrum.h/.c     # Гистограмма 8192 + тестовый генератор (Cs-137 + Am-241 + фон)
│   └── app.h/.c          # Логика: -sta/-sto/-rst/-inf, отправка спектра+статуса 1 Гц
├── nucleo_stage1/
│   └── main_glue.c       # Шаблон интеграции в CubeMX-проект (копировать в USER CODE)
└── README.md
```

`core/` не зависит от HAL — связь с железом через 3 функции-порта (`app_port_uart_send`,
`app_port_millis`, `app_port_temperature_c`), которые реализуются в main.c.
Этот же код без изменений поедет на боевую плату.

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
Статус: [elapsed_ms u32][cpu_load u16][cps u32][invalid u32]
```

Полная спецификация: `docs/session_context.md` → «Протокол shproto».

## Дальше (этап 2)

- DAC1 (PA4) генерирует ступеньки → внешняя RC → ADC1 ловит
- DMA ADC 4 Мвыб/с, кольцевой буфер
- Peak detector: baseline, FindPeakNegative, pile-up rejection
- Замена `spectrum_test_accumulate()` на реальные события
- Архитектура DSP и термокомпенсация: `docs/adc_mcu_board.md` раздел 8
