/*
 * selftest.h — встроенный самотест тракта тестовым генератором
 *
 * Прямое назначение генератора по ТЗ (adc_mcu_board.md §3.2): самотест
 * работоспособности + измерение шума электроники/DSP без статистики
 * детектора. На Nucleo источник DAC→перемычка→ADC; на боевой плате —
 * DAC→R_inj/C_inj→шейпер. Команды и метрики одни и те же.
 *
 * Команды (shproto MODE_TEXT; можно слать из командной строки BecqMoni):
 *   -tst off                 тишина
 *   -tst spec                эмуляция источника (распределение черники)
 *   -tst mono <amp> [t_us]   фикс. амплитуда (DAC-коды), период (мкс, дефолт 1000)
 *   -tst stat                статистика mono-режима с момента включения
 *   -tst enc                 автотест: 5 амплитуд × 1000 имп. → отчёт:
 *                            центроид/σ/FWHM каждой точки + макс. нелинейность
 */
#ifndef SELFTEST_H
#define SELFTEST_H

#include <stdint.h>
#include <stdbool.h>

void selftest_init(void);

/* из emit-колбэка DSP: каждый принятый импульс (amplitude = полу-LSB) */
void selftest_feed(uint16_t amplitude_half_lsb);

/* из главного цикла: ведёт FSM автотеста, шлёт отчёты */
void selftest_poll(uint32_t now_ms);

/* обработка "-tst ..."; args — строка после "-tst" (может начинаться с пробела).
 * Возвращает true, если команда распознана (ответ уже отправлен). */
bool selftest_command(const char *args);

#endif /* SELFTEST_H */
