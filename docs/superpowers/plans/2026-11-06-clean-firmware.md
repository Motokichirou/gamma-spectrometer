# Чистая прошивка боевой платы (STM32G474CBT6) — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Прошивка, с которой прибор настраивается из `gammapult` (ВВ, калибровки, термокомпенсация), сохраняет настройки и затем как готовое изделие набирает спектры в BecqMoni по shproto.

**Architecture:** Голые регистры CMSIS, без HAL. Переносимая логика (`src/core/`) не знает о железе и тестируется на ПК (MSVC); железо (`src/hw/`) — тонкие драйверы; `src/app/` склеивает их в главном цикле. Сигнал — ADC3 непрерывно по DMA, обработка порциями: быстрый проход + медленный путь вокруг импульсов, горячий код в CCM SRAM.

**Tech Stack:** C11, CMSIS (`firmware/gamma_stage1/Drivers/CMSIS`), arm-none-eabi-gcc из STM32CubeIDE 1.19 (`-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard`, newlib-nano), MSVC `cl` для тестов на ПК, Python 3 (`python -I`) для хостовых скриптов, STM32_Programmer_CLI по UART.

**Spec:** `firmware/board_g474cb/SPEC.md` (живое ТЗ; читать целиком). Протокол BecqMoni — `docs/ru/protocol.md` и upstream `C:\Claud Projects\BecqMoni` (`origin/master`).

## Global Constraints

- Первой строкой после сброса: **PB10 = 0, PA8 = 0** (выходы); ВВ включается только командой (SPEC §3).
- `SCB->VTOR = 0x08000000` в `SystemInit`; команда **`!boot`** распознаётся подстрокой в любом месте входного потока и переводит в системный загрузчик с выключенным ВВ (SPEC §4).
- Калибровка АЦП — не раньше **1.5 с** после сброса (SPEC §3).
- Тактирование: HSE 16 МГц, PLL M=2 N=39 R=2 → **SYSCLK 156 МГц**, boost, FLASH 4WS; ADC345 = PLLP/6 = **52 МГц**; ADC12 = PLLP, PRESC /2 = **26 МГц**. Отказ HSE → HSI 16 МГц и сообщение (SPEC §2).
- Сигнал — **ADC3_IN1**, 2.5 + 12.5 такта = **3.4667 Мвыб/с**; ADC1 под сигнал не использовать (SPEC §7.1).
- ШИМ HV_CONTROL: TIM1_CH1 на PA8 (AF6), 12 бит → **38.1 кГц**; амплитуда = +3.3V_D по VBAT/3, **усреднение τ ≈ 60 с** (SPEC §5).
- ВВ: `|V_cw| = 454.5·V_CONTROL`; монитор `|V| = (zero − V_mon)/k`, по умолчанию **zero = 2460 мВ, k = 1.640 мВ/В**; уставка выхода ≤ **1200 В**, CW ≤ **1300 В**; подъём **100 В/с**; подстройка по монитору (τ ≈ 1 с усреднение, мёртвая зона ±3 В, добавка −50…+150 В, масштаб при смене уставки); защита: монитор > **1300 В** → выкл; > уставки + **150 В** при неизменной уставке > **30 с** → выкл (SPEC §5).
- **D102 (PB6) — индикатор ВВ:** горит ⇔ ВВ включено, не мигает. Никаких переключаемых нагрузок на +3.3V_D при включённом ВВ; во время набора без WFI (SPEC §5, §7.2).
- Pt1000: OPAMP6 PGA ×8 от PB12 → ADC4_IN17; `x = raw/(4095·8)`, `R = R_pu·x/(1−x)`, CVD A = 3.9083e-3, B = −5.775e-7, R0 = 1000; R_pu по умолчанию **10 024.5 Ом** (SPEC §8).
- Внутренняя флеш: настройки — одна страница **0x0801 F000** (DBANK = 0, страница 4 КБ), CRC, проверка чтением; **писать только при остановленном наборе** (SPEC §7.9, §9). W25Q64 (SPI: PA15 CS, PB3 SCK, PB4 MISO, PB5 MOSI) — в deep power-down при старте.
- UART: USART1 PA9/PA10, **600 000 бод 8N1**, FIFO, передача через DMA; загрузчик — 115 200 8E1 (SPEC §9).
- Протокол: shproto (кадр `FF FE … CRC16 A5`, стаффинг FE/FD/A5). То, что умеет BecqMoni, — строго как он ждёт: `-sta/-sto/-rst` → `-ok\r\n`, `-stt`, `-mode`, `-inf`, `-cal`, `-cal i hex`, `-sho` → `-ok collecting`; пакеты 0x01 (offset u16 + каналы u32, маска 0x7FFFFFF), 0x04 (elapsed u32 **секунды**, cpu u16, cps u32, invalid u32), 0x03 текст. Остальное — сервисные команды `gammapult` (SPEC §9).
- Гистограмма 8192 канала (амплитуда в половинах LSB). Эталон формы — 64 фазы, под 3.4667 Мвыб/с (SPEC §7.4).

## Review Focus

1. **Повреждённые/пустые настройки во флеш** (первая прошивка, CRC не сходится, старая версия структуры) → работать на значениях по умолчанию, ВВ выключено, `-inf`/`-cal` отвечают, сообщение в сервисном статусе — тест в Task 3.
2. **Команды записи во время набора** (BecqMoni шлёт 11 × `-cal i hex`, пока идёт набор; `-save` из пульта) → ответ `-ok`, фактическая запись откладывается до `-sto`, набор не прерывается — тест в Task 7.
3. **Мусор и обрывы на UART** (кадр без FINISH, ESC в конце, неверный CRC, кадр длиннее буфера, `!boot` между кадрами) → кадр отбрасывается, парсер восстанавливается на следующем START, `!boot` срабатывает — тесты в Task 2 и Task 7.
4. **Перегрузка по скорости счёта** (поток импульсов выше 55 кимп/с, непрерывный «шум» над порогом) → потерянные порции считаются и уходят в мёртвое время, прошивка не виснет, команды обслуживаются — тест в Task 6 (детектор) и проверка на плате в Task 10.
5. **Монитор ВВ у потолка АЦП и сбой подстройки** (V_mon ≥ 4090 кодов при TL431 в плюсовом допуске; уставка меняется во время подстройки; монитор отстаёт после снижения уставки) → «≈0», добавка масштабируется, защита не срабатывает ложно — тесты в Task 4.

---

## Структура файлов

```
firmware/gamma_fw/
├── build.sh                 # ARM-сборка → gamma_fw.elf/.bin/.map (по образцу bringup/build.sh)
├── gamma_fw.ld              # линкер: FLASH 128K, RAM 96K @0x20000000, CCMRAM 32K @0x10000000 (код .ccmfunc + данные .ccmdata)
├── startup_stm32g474xx.s    # копия из bringup/
├── src/core/                # переносимое, без CMSIS, тестируется на ПК
│   ├── shproto.c/.h         # кадры, CRC16, стаффинг (порт core/shproto)
│   ├── spectrum.c/.h        # гистограмма 8192
│   ├── settings.c/.h        # структура настроек, умолчания, CRC32, сериализация, проверка
│   ├── becq_cal.c/.h        # 11 слов энергокалибровки BecqMoni, CRC-32 по ASCII, 40 регистров
│   ├── hv_ctl.c/.h          # ВВ: уставка→скважность, подъём, подстройка, защита, монитор
│   ├── temp.c/.h            # Pt1000: код→R→T, калибровка R_pu, термокомпенсация усиления
│   ├── detector.c/.h        # быстрый проход, медленный путь, подгонка эталона 64 фазы, χ², база, мёртвое время
│   ├── tpl_3467k.h          # эталон 64×7 Q14 под 3.4667 Мвыб/с (сгенерирован)
│   └── proto.c/.h           # команды и ответы (BecqMoni + сервис), пакеты 0x01/0x02/0x03/0x04; I/O через struct proto_port
├── src/hw/                  # CMSIS-драйверы
│   ├── clock.c/.h  gpio.c/.h  uart.c/.h  timebase.c/.h  boot.c/.h
│   ├── opamp.c/.h  adc_aux.c/.h  adc_sig.c/.h  hvpwm.c/.h  dac_test.c/.h
│   └── flash_int.c/.h  spiflash.c/.h
├── src/app/main.c           # инициализация, главный цикл, планировщик 10 мс / 1 с, связь core↔hw
├── tests/t.h                # мини-фреймворк: CHECK(cond), CHECK_NEAR(a,b,eps), итог и код возврата
├── tests/test_*.c           # по одному на модуль core/
├── tests/run_tests.cmd      # vcvars64 + cl каждого теста с нужными src/core/*.c, запуск, итог
└── tools/gfw.py             # хост: shproto-клиент (текстовые команды, приём 0x01/0x02/0x03/0x04), reflash через !boot
```

---

### Task 1: Каркас проекта, сборка ARM и тесты на ПК

**Files:**
- Create: `firmware/gamma_fw/build.sh`, `gamma_fw.ld`, `startup_stm32g474xx.s` (копия `firmware/board_g474cb/bringup/startup_stm32g474xx.s`), `src/app/main.c` (пустой цикл), `tests/t.h`, `tests/test_smoke.c`, `tests/run_tests.cmd`, `.gitignore` (elf/bin/map/obj/exe)

**Interfaces:**
- Produces: макросы `CHECK(cond)`, `CHECK_NEAR(a, b, eps)`, `T_DONE()` (печатает `N passed, M failed`, возвращает код ≠ 0 при сбое); секции линкера `.ccmfunc` (код, LMA во FLASH, VMA 0x10000000) и `.ccmdata`; символ-атрибут `#define CCM_FUNC __attribute__((section(".ccmfunc"), noinline))` в `src/core/ccm.h` (на ПК — пустой).

- [ ] **Step 1:** Написать `tests/test_smoke.c`: `CHECK(1 + 1 == 2); return T_DONE();`.
- [ ] **Step 2:** `tests/run_tests.cmd` (по образцу `simulation/fix_c320/build_dsp.cmd`: vcvars64, `cl /nologo /O2 /utf-8 /W3 /I ..\src\core`), список тестов и их исходников — в начале файла; печатает `ALL TESTS PASSED` или имя упавшего. Run: `cmd /c tests\run_tests.cmd` → `ALL TESTS PASSED`.
- [ ] **Step 3:** `gamma_fw.ld` из `board_g474cb/STM32G474CBTX_FLASH.ld`: RAM 96K, CCMRAM 32K (0x10000000), копирование `.ccmfunc`/`.ccmdata` в `Reset_Handler` через символы `_siccm/_sccm/_eccm`. `build.sh` по образцу `bringup/build.sh` собирает `src/**/*.c` + startup → `gamma_fw.elf/.bin`, печатает `size`.
- [ ] **Step 4:** Run: `sh build.sh` → без ошибок, `gamma_fw.bin` создан; `arm-none-eabi-objdump -h gamma_fw.elf` показывает `.ccmfunc` с VMA 0x10000000.
- [ ] **Step 5:** Commit `fw: каркас gamma_fw — сборка ARM, линкер с CCM, тесты на ПК`.

### Task 2: shproto

**Files:**
- Create: `src/core/shproto.c/.h` (порт `firmware/core/shproto.c/.h` — API тот же), `tests/test_shproto.c`

**Interfaces:**
- Produces: `uint16_t shproto_crc16(const uint8_t*, size_t)`, `void shproto_rx_init(shproto_rx_t*)`, `bool shproto_rx_byte(shproto_rx_t*, uint8_t)`, `size_t shproto_build(uint8_t cmd, const uint8_t *pl, size_t len, uint8_t *out)`; константы `SHPROTO_CMD_HISTOGRAM 0x01`, `_PULSE 0x02`, `_TEXT 0x03`, `_STAT 0x04`, `SHPROTO_RX_MAX_PAYLOAD 256`.

- [ ] **Step 1: Тесты.** `test_crc_modbus`: `shproto_crc16("123456789")` == `0x4B37`. `test_roundtrip_stuffing`: payload `{0xFE,0xFD,0xA5,0x00,0xFF}` → build → скормить побайтно → один кадр, cmd и payload совпадают. `test_bad_crc_dropped`: испорченный байт → `false` на FINISH. `test_resync`: обрывок кадра без FINISH, затем полный кадр → принят только полный. `test_overlong_dropped`: payload 300 байт → отброшен, следующий нормальный кадр принят. `test_esc_at_end`: `… FD A5` → отброшен без зависания.
- [ ] **Step 2:** Run `run_tests.cmd` → FAIL (нет `shproto.c`).
- [ ] **Step 3:** Перенести `shproto.c/.h` из `firmware/core/` (проверить поведение на переполнении — кадр отбрасывается, парсер ждёт START).
- [ ] **Step 4:** Run → PASS.
- [ ] **Step 5:** Commit `fw: shproto с тестами на ресинхронизацию и переполнение`.

### Task 3: Настройки и энергокалибровка BecqMoni

**Files:**
- Create: `src/core/settings.c/.h`, `src/core/becq_cal.c/.h`, `tests/test_settings.c`, `tests/test_becq_cal.c`

**Interfaces:**
- Produces:
  ```c
  #define SETTINGS_MAGIC 0x47534631u   /* "GSF1" */
  #define SETTINGS_VERSION 1u
  typedef struct {
      uint32_t magic, version, size;          /* size = sizeof(settings_t) */
      float hv_mon_zero_mv, hv_mon_k_mv_per_v; /* 2460, 1.640 */
      float hv_setpoint_v; uint8_t hv_on_at_boot; /* 0, 0 — ВВ при старте не включать */
      float pt_rpu_ohm;                       /* 10024.5 */
      float tc_t0_c, tc_k1, tc_k2;            /* термокомпенсация: усиление ×(1+k1·dT+k2·dT²); 25, 0, 0 */
      uint16_t thr_codes;                     /* 0 = авто 5σ */
      uint32_t becq_words[11]; uint8_t becq_valid; /* слова -cal как прислал BecqMoni */
      int16_t tpl[64][7]; uint8_t tpl_valid;  /* эталон с реального тракта (Q14), иначе встроенный */
      uint32_t crc32;                         /* по всем полям до crc32 */
  } settings_t;
  void settings_defaults(settings_t *s);
  bool settings_valid(const settings_t *s);            /* magic, version, size, crc */
  void settings_seal(settings_t *s);                   /* пересчитать crc32 */
  uint32_t crc32_zip(const uint8_t *p, size_t n);      /* poly 0x04C11DB7 reflected, init/xorout 0xFFFFFFFF */
  /* becq_cal: */
  uint32_t becq_checksum(const uint32_t w[10]);        /* CRC-32 по ASCII "%08X"×10, docs/ru/protocol.md */
  bool becq_words_to_coef(const uint32_t w[11], int *order, double coef[5]);
  void becq_coef_to_words(int order, const double coef[5], uint32_t w[11]);
  size_t becq_format_registers(const uint32_t w[11], bool valid, const char *serial, char *out, size_t cap); /* 40 строк, [39]=serial */
  ```
- [ ] **Step 1: Тесты.** `test_defaults_valid`: после `defaults+seal` → `settings_valid` true, поля = значения из Global Constraints. `test_crc_corruption`: изменить 1 байт → false. `test_version_mismatch`: version = 2 → false. `test_size_fits_page`: `sizeof(settings_t) <= 4096`. `test_crc32_zip`: `crc32_zip("123456789")` == `0xCBF43926`. `test_becq_roundtrip`: coef `{1.5, 0.3456, 1e-6, 0, 0}`, order 2 → words → coef, равны бит в бит; `becq_checksum` совпадает со словом 10. `test_becq_registers`: ровно 40 строк, предпоследняя — серийник `GS474-0001`, при `valid=false` слова 0..10 = `00000000`.
- [ ] **Step 2:** Run → FAIL.
- [ ] **Step 3:** Реализовать. Формат слов и CRC — по `docs/ru/protocol.md` §«Энергокалибровка BecqMoni» и `firmware/gamma_stage1/Core/Src/cal_store.c` (`becq_checksum`, `double_to_u64`).
- [ ] **Step 4:** Run → PASS.
- [ ] **Step 5:** Commit `fw: настройки с CRC и энергокалибровка BecqMoni`.

### Task 4: Управление ВВ (логика)

**Files:**
- Create: `src/core/hv_ctl.c/.h`, `tests/test_hv_ctl.c`

**Interfaces:**
- Consumes: `settings_t` (zero, k).
- Produces:
  ```c
  typedef struct { /* состояние; поля закрыты, менять только функциями */ } hv_ctl_t;
  void  hv_init(hv_ctl_t*, float zero_mv, float k_mv_per_v);
  int   hv_set(hv_ctl_t*, float volts, uint32_t now_ms);  /* 0..1200; 0 ок, -1 вне диапазона; включает ENABLE;
                                                            повтор той же уставки — без изменений; 0 при выкл. — ENABLE не трогает */
  void  hv_set_vref(hv_ctl_t*, float vref_mv);              /* опора АЦП монитора для порога потолка, по умолч. 2500 */
  int   hv_set_cal(hv_ctl_t*, float zero_mv, float k_mv_per_v); /* только при выкл. ВВ (иначе -1), состояние не сбрасывает — для -hvzero/-hvk */
  void  hv_off(hv_ctl_t*, const char *why);
  /* шаг 10 мс: монитор (мВ), амплитуда ШИМ (мВ, уже усреднённая) → скважность 0..4095 */
  uint16_t hv_tick(hv_ctl_t*, float mon_mv, float vdd_mv, uint32_t now_ms);
  float hv_mon_volts(const hv_ctl_t*, float mon_mv);      /* |V|; при mon ≥ 4090 кодов-эквивалент → 0 */
  bool  hv_near_ceiling(const hv_ctl_t*);                  /* флаг «≈0, не точно»; обновляет hv_tick (hv_mon_volts — const) */
  bool  hv_enabled(const hv_ctl_t*);                       /* → PB10 и D102 */
  const char *hv_trip_reason(const hv_ctl_t*);             /* NULL, "abs", "over" */
  const char *hv_off_reason(const hv_ctl_t*);              /* последний why из hv_off/защиты */
  float hv_trim(const hv_ctl_t*); float hv_ramp(const hv_ctl_t*);
  float hv_setpoint(const hv_ctl_t*); float hv_mon_avg(const hv_ctl_t*);  /* уставка; усреднённый монитор |V| */
  ```
- [ ] **Step 1: Тесты** (значения из прошивки запуска, `bringup/main.c` и SPEC §5): `test_duty_math`: уставка 1000, trim 0, vdd 3296 → скважность 2733±1 (замер 2026-11-05: pwm 2733 при 3V3D 3298). `test_ramp_100vps`: 0→500 за 500 тиков по 10 мс, не быстрее. `test_trim_converges`: модель «выход = 0.916·CW» → после 60 с монитор в ±3 В от 1000, trim ≈ +88…+92 (точное 91.7; подход снизу встаёт на краю мёртвой зоны ±3 В — уточнено при ревью; +98 со стенда — при другом коэффициенте цепочки, см. `test_trim_rescale`). `test_deadband`: ошибка 2 В → trim не меняется. `test_trim_rescale`: при 1000 trim 98, `hv_set(500)` → trim 49. `test_cw_ceiling`: уставка 1200 при модели 0.916 → CW ≤ 1300, trim упирается. `test_trip_abs`: монитор 1301 В → выкл, причина "abs". `test_no_false_trip_after_lowering`: 1000→500, монитор спадает с τ = 2 с и 16 с → нет срабатывания, добавка не стирается (≥ 0.9 отмасштабированной), выход не проседает ниже 495 В (подстройка ждёт, пока выход устоится). `test_trip_over`: уставка стоит 31 с, монитор = уставка + 151 → выкл "over". `test_near_ceiling`: mon_mv соответствует 4092 кодам → `hv_mon_volts` = 0, флаг выставлен.
- [ ] **Step 2:** Run → FAIL.
- [ ] **Step 3:** Реализовать по SPEC §5 (константы — Global Constraints).
- [ ] **Step 4:** Run → PASS.
- [ ] **Step 5:** Commit `fw: логика ВВ — подъём, подстройка по монитору, защита`.

### Task 5: Температура и термокомпенсация

**Files:**
- Create: `src/core/temp.c/.h`, `tests/test_temp.c`

**Interfaces:**
- Produces: `float pt_resistance(float raw, float rpu)` (raw — среднее кодов ADC4, усиление 8), `float pt_temp_c(float r_ohm)` (CVD, T ≥ 0), `float pt_calibrate_rpu(float raw, float t_ref_c)` (возвращает новое R_pu), `float tc_gain(float t_c, float t0, float k1, float k2)` (= 1 + k1·dT + k2·dT²).
- [ ] **Step 1: Тесты:** `pt_temp_c(1000)` = 0.00±0.01; `pt_temp_c(1097.35)` = 25.0±0.02; при rpu 10000 R 1096.6 Ом даёт `pt_calibrate_rpu` для 25.5 °C ≈ 10024.5±1 (замер 2026-11-06); `tc_gain(30, 25, -0.002, 0)` = 0.99; `pt_resistance(4090, …)` → признак обрыва (возврат < 0).
- [ ] **Step 2:** FAIL. **Step 3:** реализовать по SPEC §8. **Step 4:** PASS.
- [ ] **Step 5:** Commit `fw: Pt1000 и термокомпенсация`.

### Task 6: Детектор и эталон 64 фазы

**Files:**
- Modify: `simulation/fix_c320/gen_template.py` — аргументы `--fs` (по умолчанию 4e6, поведение прежнее), `--nph` (32), `--out`, `--name`.
- Create: `src/core/tpl_3467k.h` (генерация), `src/core/detector.c/.h`, `src/core/spectrum.c/.h` (порт), `tests/test_detector.c`, `tests/pulsegen_host.c/.h` (синтез потока: эталон × амплитуда, случайная фаза, белый шум σ, выбросы-помеха 1–2 отсчёта)

**Interfaces:**
- Consumes: эталон `[64][7]` Q14, `CCM_FUNC`.
- Produces:
  ```c
  typedef void (*det_emit_fn)(uint16_t channel);          /* 0..8191, половины LSB */
  typedef struct { uint16_t thr; uint8_t min_w, max_w; float chi2_max; } det_cfg_t;
  void det_init(det_t*, const det_cfg_t*, const int16_t (*tpl)[7], det_emit_fn);
  void det_process_block(det_t*, const uint16_t *s, size_t n); /* порция; импульс на стыке переносится */
  float det_baseline(const det_t*); float det_sigma(const det_t*);
  typedef struct { uint32_t accepted, rej_pileup, rej_shape, rej_width; uint64_t dead_samples; } det_stats_t;
  void det_get_stats(const det_t*, det_stats_t*);
  void det_set_gain(det_t*, float g);                     /* термокомпенсация: канал = round(2·A·g) */
  ```
- [ ] **Step 1:** Сгенерировать эталон: `python -I simulation/fix_c320/gen_template.py --fs 3466666.67 --nph 64 --out firmware/gamma_fw/src/core/tpl_3467k.h --name tpl_3467k`. Проверка: `python -I simulation/fix_c320/gen_template.py` без аргументов даёт `firmware/core/dsp_template.h` байт в байт (`git diff --exit-code firmware/core/dsp_template.h`). ⚠ Проверка честная, только если скрипт дошёл до записи (`записано: …` в выводе): до правок по ревью Task 6 он падал в консоли cp1251 на `print('×')`, и `git diff` проходил тривиально. Теперь скрипт сам переводит stdout в UTF-8; на старой версии — `python -I -X utf8`.
- [ ] **Step 2: Тесты.** `test_amplitude_accuracy`: 2000 импульсов A = 580 кодов (Cs-137), случайная фаза, σ = 1.8 LSB → средний канал 1160±1, разброс σ ≤ 3 канала. `test_low_energy`: A = 28 кодов (≈32 кэВ), thr = 9 → регистрируется ≥ 99 %. `test_spike_rejected`: выбросы +12 LSB шириной 1–2 отсчёта каждые 7.7 мкс → 0 принятых, `rej_shape` растёт. `test_pileup`: два импульса через 4 отсчёта → оба отбракованы как наложение. `test_block_boundary`: импульс с вершиной на последнем отсчёте порции → принят ровно один раз с верной амплитудой. `test_baseline_tracking`: база 645 → 650 медленно → `det_baseline` ±0.5. `test_overload`: непрерывное превышение порога 3 порции → нет зависания, `dead_samples` растёт. `test_gain`: `det_set_gain(0.99)` → канал ×0.99.
- [ ] **Step 3:** FAIL.
- [ ] **Step 4:** Реализовать. Быстрый проход: поиск первого отсчёта > база + thr по словам uint32 (два отсчёта; на ARM — `__UQSUB16`, на ПК — скалярный вариант с тем же результатом), сумма для базы по спокойным участкам. Медленный путь — машина состояний из `firmware/core/dsp.c` (гистерезис, min/max ширина, повторный рост = наложение), но вызывается только от найденного отсчёта; подгонка эталона двухэтапная (грубо по 16 фазам, затем ±2 соседних из 64), МНК `y ≈ A·h + B`, χ² = невязка/σ², отбраковка при χ² > `chi2_max` (по умолчанию 4·n). Функции `det_process_block` и подгонка — `CCM_FUNC`.
- [ ] **Step 5:** PASS.
- [ ] **Step 6:** Commit `fw: детектор — быстрый проход, подгонка 64 фазы, χ²; эталон 3.467 Мвыб/с`.

### Task 7: Протокол и команды

**Files:**
- Create: `src/core/proto.c/.h`, `tests/test_proto.c`

**Interfaces:**
- Consumes: shproto, settings, becq_cal, hv_ctl, temp, det stats, spectrum.
- Produces:
  ```c
  typedef struct { uint32_t elapsed_s; uint16_t cpu_x10; uint32_t cps; uint32_t invalid; } proto_status_t;
  typedef struct {                       /* железо/приложение, на ПК — заглушки */
      void (*tx)(const uint8_t *p, size_t n);
      uint32_t (*millis)(void);
      bool (*settings_commit)(const settings_t *s); /* запись во флеш; вызывается только при остановленном наборе */
      void (*acq_start)(void); void (*acq_stop)(void); void (*acq_clear)(void);
      void (*enter_bootloader)(void);
      void (*service)(const char *cmd, char *reply, size_t cap); /* сервисные команды, см. ниже */
      void (*fill_status)(proto_status_t *st);       /* время, cpu (0.1 %), cps, invalid */
  } proto_port_t;
  void proto_init(const proto_port_t*, settings_t *live);
  void proto_rx(const uint8_t *p, size_t n);      /* сюда же ищется "!boot" */
  void proto_tick_1s(void);                       /* при наборе: спектр 0x01 кусками по 512 + статус 0x04 */
  void proto_send_pulse(const uint16_t *s, size_t n); /* 0x02 — осциллограф для gammapult */
  ```
  Команды BecqMoni (точно как ждёт upstream): `-sta`, `-sto`, `-rst` → `-ok\r\n`; `-stt` → `-ok collecting\r\n`/`-ok\r\n`; `-mode …` → `-ok\r\n`; `-sho` → `-ok collecting\r\n`; `-inf` → **`GammaSpec v1 RISE <r> FALL <f> T1 <t> F <fs> VERSION 100`** (позиции 3, 5, 9 = r, f, fs — старый BecqMoni; ключи `RISE/FALL/F` — новый); `-cal`, `-cal i`, `-cal i hex` — по `docs/ru/protocol.md`.
  Сервисные (для `gammapult`, ответ `-ok …`/`-err …`): `-hv <В>`, `-hv off`, `-hvzero`, `-hvk <мВ/В>`, `-thr <кодов|0>`, `-ptcal <°C>`, `-tc <t0> <k1> <k2>`, `-gst` (одна строка `key=value`: hv_set, hv_out, hv_trim, hv_trip, t, vref, base, sigma, cps_in, cps_acc, rej, live_s, real_s, cpu, lost, flash), `-diag` (напряжения узлов), `-osc <n>` (n импульсов пакетами 0x02), `-tplw <фаза> <7 чисел>` / `-tplok` / `-tplclr` (по `-tplok`/`-tplclr` — `det_set_template()`: пересчёт Σh и 1/Σh² без сброса базы и счёта; строки пишутся в отдельный буфер, не в таблицу, на которую смотрит идущий набор), `-dac <код> <n>` (самотест), `-save`, `-defaults`.
- [ ] **Step 1: Тесты** (порт-заглушки пишут ответы в буфер, разбор через `shproto_rx_byte`): `test_sta_sto_rst`; `test_inf_both_parsers`: токены [3], [5], [9] — числа r, f, fs **и** пары `RISE r`, `FALL f`, `F fs` присутствуют; `test_cal_read_40_regs`; `test_cal_write_sequence`: 11 × `-cal i hex` → `becq_valid`, слова совпадают; `test_cal_write_during_acq_deferred`: при наборе `settings_commit` не вызывается, ответы `-ok`, после `-sto` вызван ровно один раз; `test_save_during_acq`: `-save` при наборе → `-ok deferred`, коммит после `-sto`; `test_boot_substring`: байты `xx!boot` вне кадра и разорванные между двумя вызовами `proto_rx` (`!bo` + `ot`) → `enter_bootloader` вызван; поток без `!boot` → не вызван; `test_status_payload`: 14 байт, elapsed в секундах; `test_spectrum_chunks`: 16 кадров по 512 каналов, offset 0…7680, маска 27 бит; `test_unknown_cmd`: незнакомая команда → тишина (как этап 1), сервисная незнакомая `-xyz` → `-err unknown`.
- [ ] **Step 2:** FAIL. **Step 3:** реализовать (`firmware/core/app.c` — образец для BecqMoni-части). **Step 4:** PASS.
- [ ] **Step 5:** Commit `fw: протокол — команды BecqMoni и сервисные, отложенная запись`.

### Task 8: Железо — такт, GPIO, UART, загрузчик (первое включение)

**Files:**
- Create: `src/hw/clock.c/.h`, `gpio.c/.h`, `timebase.c/.h`, `uart.c/.h`, `boot.c/.h`; Modify: `src/app/main.c`; Create: `tools/gfw.py`

**Interfaces:**
- Produces: `bool clock_init(void)` (true = HSE, false = HSI); `void gpio_safe_init(void)` (первым: PB10 = 0, PA8 = 0, PB6 = 0 выходы; остальные — аналог); `uint32_t millis(void)`, `uint32_t cycles(void)` (DWT); `void uart_init(uint32_t baud)`, `void uart_tx(const uint8_t*, size_t)` (кольцо + DMA, неблокирующее, при переполнении — ждать), `size_t uart_rx(uint8_t *dst, size_t cap)` (кольцо из прерывания RXFNE/IDLE); `void boot_enter_system(void)` (как `bringup/main.c::jump_to_bootloader` + PLL/HSE выкл, TIM1/ADC/DMA сброс).
- [ ] **Step 1:** `tools/gfw.py`: `gfw.py cmd "<текст>" [--port COM4] [--baud 600000]` — кадр 0x03, печать текстовых ответов; `gfw.py listen <с>` — печать 0x03 и счёт 0x01/0x04; `gfw.py flash <bin>` — `!boot` на 600000, затем `STM32_Programmer_CLI -c port=COM4 br=115200 P=EVEN db=8 sb=1 -w <bin> 0x08000000 -v -g 0x08000000`.
- [ ] **Step 2:** Реализовать драйверы; `main.c`: `gpio_safe_init`, `clock_init`, timebase, uart 600000, `proto_init` с портом (`service` пока отвечает `-err notyet`), цикл `proto_rx(uart_rx…)`, `proto_tick_1s`.
- [ ] **Step 3: Проверка на плате** (ВВ выключено). Первая заливка — через прошивку запуска: `python -I firmware/board_g474cb/bringup/tools/reflash.py firmware/gamma_fw/gamma_fw.bin`. Run: `python -I firmware/gamma_fw/tools/gfw.py cmd "-inf"` → строка `GammaSpec v1 RISE … FALL … T1 … F 3466667 VERSION 100`; `gfw.py cmd "-sta"` → `-ok`; `gfw.py listen 3` → ≥ 2 кадров 0x04 в секунду-период; `gfw.py flash gamma_fw.bin` → перепрошивка через `!boot` работает.
- [ ] **Step 4:** Commit `fw: такт 156 МГц, UART 600000 с DMA, shproto на плате, !boot`.

### Task 9: Служебные АЦП, ОУ, ВВ и Pt1000 на плате

**Files:**
- Create: `src/hw/opamp.c/.h`, `adc_aux.c/.h`, `hvpwm.c/.h`; Modify: `src/app/main.c`, `tools/gfw.py`

**Interfaces:**
- Produces: `void opamp_init(void)` (OPAMP1/2 внешняя обвязка, OPAMP3 PGA×4 от PB0, OPAMP6 PGA×8 от PB12 → внутрь); `void adc_aux_init(void)` (после 1.5 с; ADC2 26 МГц, ADC4/5 52 МГц, калибровка); `void adc_aux_poll(adc_aux_t *out)` — неблокирующий опрос: `mon_mv` (ADC2_IN1, 640.5 такта), `pt_raw` (ADC4_IN17, 256 отсчётов), `vrefint_raw` (ADC4_IN18), `vbat_mv` (ADC5_IN17), `vref_mv`; `void hvpwm_init(void)`, `void hvpwm_set(uint16_t duty)`, `void hv_enable_pin(bool on)` (PB10 и PB6 вместе).
- [ ] **Step 1:** Реализовать; `main.c`: задача 10 мс — `adc_aux_poll` → `hv_tick` → `hvpwm_set`, `hv_enable_pin(hv_enabled())`; 1 с — VREF+ (2500±15 мВ, иначе ошибка и ВВ не включать), VBAT с усреднением τ 60 с, Pt1000 со сглаживанием τ 10 с; сервис `-hv`, `-hvzero`, `-hvk`, `-ptcal`, `-gst`, `-diag`.
- [ ] **Step 2: Проверка на плате** (стек с делителем, без ФЭУ; человек видит D102). `gfw.py cmd "-gst"` → vref 2500±15, t ≈ комнатная, hv_out ≈ 0. `-diag` → V_A 1613±30, OA1 2418±50, V_B 99±5, база 394±15 мВ (SPEC §6). `-hv 500` → через 30 с hv_out 500±5, D102 горит; `-hv 1000` → 1000±5; `-hv off` → D102 гаснет, hv_out < 10 за 10 с. ⚠ Монитор теперь на ADC2/26 МГц/24.6 мкс: сравнить hv_out с прежним (bringup на −1000 давал −996…−999 при trim ≈ 98); при расхождении > 1 % — перекалибровать `-hvk` мультиметром (SPEC §5) и записать в SPEC.
- [ ] **Step 3:** Commit `fw: ВВ с подстройкой, Pt1000, служебные АЦП на ADC2/4/5`.

### Task 10: Сигнальный АЦП, детектор на плате, первые замеры

**Files:**
- Create: `src/hw/adc_sig.c/.h`; Modify: `src/app/main.c`

**Interfaces:**
- Produces: `void adc_sig_init(uint8_t smp_code)` (ADC3_IN1, 52 МГц, SMPR = 2.5 или 6.5 такта, непрерывно, DMA по кругу 16384, прерывания HT/TC); `const uint16_t *adc_sig_take(size_t *n)` (готовая порция 8192 или NULL); `uint32_t adc_sig_lost(void)` (порции, не забранные вовремя). Сервис: `-smp 2|6` (смена выборки), `-noise` (σ и среднее по 8192 «тихим» отсчётам), `-osc <n>`.
- [ ] **Step 1:** Реализовать; главный цикл: `adc_sig_take` → `det_process_block` (DWT до/после → cpu в статус 0x04, единицы 0.1 %), эмиттер → `spectrum_add`, `det_set_gain(tc_gain(T))`.
- [ ] **Step 2: Проверка на плате — первые замеры по SPEC §7.1** (записать в SPEC §7.1 таблицей): `-noise` при `-smp 2` и `-smp 6`, ВВ выкл и −1000 В → среднее и σ базы; при 2.5 такта ожидаем σ ≈ 1.2 LSB без ВВ, ≈ 1.8 на −1000 с экраном. cpu при пустом потоке ≤ 10 %; `lost` = 0 за 60 с.
- [ ] **Step 3:** Commit `fw: сигнальный ADC3 по DMA, детектор на плате, замеры шума`.

### Task 11: Тестовые импульсы ЦАП, осциллограф, эталон с тракта

**Files:**
- Create: `src/hw/dac_test.c/.h`; Modify: `src/app/main.c`, `tools/gfw.py`

**Interfaces:**
- Produces: `void dac_test_pulses(uint16_t step_code, uint16_t count, uint16_t period_us)` (DAC1_OUT1 на PA4: ступенька вверх-вниз, после — PA4 в аналог/Hi-Z, SPEC §7.7); сервис `-dac <код> <n>`; `gfw.py osc <n> --out pulses.npy`; `gfw.py tpl pulses.npy` — выровнять по фазе, усреднить, отправить 64×7 через `-tplw …`, затем `-tplok` (хранится в `settings.tpl` после `-save`).
- [ ] **Step 1:** Реализовать.
- [ ] **Step 2: Проверка на плате** (без ФЭУ): `-sta`, `-dac 200 10000` → пик в спектре, σ пика ≤ 3 канала; 5 ступенек 100…1600 кодов → положения пиков линейны (R² ≥ 0.9999, отклонение ≤ 0.2 %) при `-smp 2`; если нелинейно — повторить на `-smp 6` и записать вывод в SPEC §7.1. `gfw.py osc 1000` → форма совпадает с эталоном (невязка ≤ 2 %).
- [ ] **Step 3:** Commit `fw: тестовые импульсы ЦАП, осциллограф 0x02, загрузка эталона`.

### Task 12: Флеш — настройки и W25Q64

**Files:**
- Create: `src/hw/flash_int.c/.h`, `src/hw/spiflash.c/.h`; Modify: `src/app/main.c`

**Interfaces:**
- Produces: `bool flash_settings_load(settings_t *s)` (читает 0x0801 F000, `settings_valid`, иначе false); `bool flash_settings_store(const settings_t *s)` (проверка DBANK = 0 по `FLASH->OPTR`, стирание страницы, запись двойными словами, сверка чтением; вызывается **только** из `proto_port.settings_commit`, а тот — только при остановленном наборе); `void spiflash_sleep(void)` (SPI1 на PB3/4/5, CS PA15: команда 0xB9, затем пины в аналог, CS — подтяжка вверх/выход 1).
- [ ] **Step 1:** Реализовать; старт: `flash_settings_load` → иначе `settings_defaults` + флаг `flash=defaults` в `-gst`; `spiflash_sleep()` до включения ВВ.
- [ ] **Step 2: Проверка на плате:** `-hvk 1.64`, `-ptcal 25.3`, `-save` → `-ok`; переподключить USB → `-gst` показывает сохранённое, `flash=ok`; `-save` во время набора → `-ok deferred`, после `-sto` сохранено; на −1000 В во время `-save` (набор остановлен) — монитор не прыгает больше чем на 5 В.
- [ ] **Step 3:** Commit `fw: настройки во внутренней флеш, W25Q64 в глубоком сне`.

### Task 13: Приёмка с BecqMoni и документация

**Files:**
- Modify: `firmware/board_g474cb/SPEC.md` (§10 журнал, результаты замеров), `firmware/README.md` (раздел «gamma_fw — боевая прошивка»), `CLAUDE.md` (строка в разделе MCU board)

- [ ] **Step 1: Приёмка** (человек за компьютером): собрать актуальный BecqMoni (`C:\Claud Projects\BecqMoni-pr`, `origin/master`); устройство AtomSpectra VCP, COM4, 600000, 8192 канала. Проверить: «Мёртвое время» по `-inf` заполняется; старт/стоп/сброс; спектр от `-dac` (или от ФЭУ с источником) обновляется раз в секунду, время и имп/с правильные; запись/чтение калибровки «Записать в устройство»/«Читать с устройства» во время набора и без; после переподключения калибровка на месте.
- [ ] **Step 2:** Записать результаты и отличия от ТЗ в SPEC §10 и README; в CLAUDE.md — одна строка о статусе `gamma_fw`.
- [ ] **Step 3:** Commit `docs: gamma_fw — приёмка с BecqMoni`.
