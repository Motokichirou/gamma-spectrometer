# Документация

<!-- README-I18N:START -->
**Русский** | [English](../en/README.md)
<!-- README-I18N:END -->

Курированный набор документации по гамма-спектрометру на NaI(Tl).
Обзор проекта — в корневом [README](../../README.md).

| Документ | Содержание |
|---|---|
| [hardware.md](hardware.md) | Стек плат, цепь сигнала и обзор всех четырёх плат |
| [firmware.md](firmware.md) | Прошивка STM32G474: переносимое ядро, этапы, DSP-детектор, самотест |
| [protocol.md](protocol.md) | Справочник по последовательному протоколу shproto / AtomSpectra |
| [simulations.md](simulations.md) | Симуляции LTspice (AD8000, шейпер CR-RC², шум) и бюджет шумов |

> Детальные инженерные заметки по каждой плате (`docs/adc_mcu_board.md`,
> `docs/divider_board.md`, `docs/hv_board_schematic.md`,
> `docs/usb_power_board_schematic.md`) ведутся отдельно и более подробны.
> Документы здесь — курированная выжимка.
