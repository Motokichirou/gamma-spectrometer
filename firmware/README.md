# Прошивка STM32G474 — гамма-спектрометр

Боевая прошивка — [`gamma_fw/`](gamma_fw/): голые регистры CMSIS, без HAL. Переносимая логика лежит
в `gamma_fw/src/core/` и покрыта тестами на ПК, драйверы железа — в `gamma_fw/src/hw/`.
ТЗ и журнал находок по плате — [`board_g474cb/SPEC.md`](board_g474cb/SPEC.md).
Обзор (тракт, детектор, команды) — [`docs/ru/firmware.md`](../docs/ru/firmware.md).

## Быстрый старт

- **Сборка:** `sh firmware/gamma_fw/build.sh` (arm-none-eabi-gcc). Скрипт проверяет бюджет ОЗУ и кладёт
  `gamma_fw.bin` рядом. Опытная сборка с ВВ до 1350 В:
  `EXTRA=-DHV_EXPERIMENT_1500 OUT=gamma_fw_hv1500 ./build.sh` (в приборе не оставлять).
- **Тесты на ПК:** `firmware\gamma_fw\tests\run_tests.cmd`.
- **Обновление по USB:** `python -I firmware/gamma_fw/tools/gfw.py flash firmware/gamma_fw/gamma_fw.bin`.
  Прошивка получает `!boot`, выключает ВВ и уходит в системный загрузчик (USART1 через FT231X).
- **Сервис:** `python -I firmware/gamma_fw/tools/gfw.py cmd -- "-gst"`. Команды: `-diag`, `-hv <В>` / `-hv off`,
  `-hvzero`, `-hvk`, `-ptcal <°C>`, `-thr`, `-noise`, `-smp`, `-opa`, `-dac`, `-osc`, `-tplw/-tplok/-tplclr`,
  `-tc`, `-save`, `-defaults`.
- **Эталоны формы:** `tools/tpl_pmt.py collect|build` — эталон с реального тракта (ФЭУ под ВВ, источник);
  `tools/tpl_dac.py --code 900` — эталон импульса ЦАП. Вшитые умолчания — `src/core/tpl_pmt.h`, `src/core/tpl_dac.h`.
- **BecqMoni:** устройство AtomSpectra VCP, 600 000 бод, 8192 канала.

## Окна детектора под шейпер

Окна детектора задаются на уровне платы: `DET_MINW_BOARD` / `DET_MAXW_BOARD` в `src/core/detector.h`
(сейчас 4 / 56 отсчётов) и `DET_GUARD` через `build.sh` (40). Тесты алгоритма гоняют прежние
умолчания (2 / 16 / 16) на модельном коротком импульсе `tpl_3467k.h`. Если меняется шейпер, нужно
переснять оба эталона и пересмотреть окна, `PROTO_INF_RISE/FALL` и константы самотеста ЦАП (`dactest.h`).

## История

Этапы 1–2 (Nucleo-G474RE: shproto, синтетические импульсы, первая гистограмма) и прошивка запуска
платы лежат в теге `archive-2026-11`.
