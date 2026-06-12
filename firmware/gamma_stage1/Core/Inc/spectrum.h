/*
 * spectrum.h — гистограмма 8192 каналов + тестовый генератор спектра
 *
 * Тестовый генератор (этап 1, Nucleo без аналогового тракта):
 * синтезирует события по PDF, похожей на реальный спектр Cs-137 + Am-241,
 * чтобы BecqMoni рисовал «живой» накапливающийся спектр.
 */
#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdint.h>
#include <stddef.h>

#define SPECTRUM_CHANNELS  8192u
#define SPECTRUM_MASK      0x7FFFFFFu  /* маска значений канала в shproto */

void     spectrum_clear(void);
void     spectrum_add(uint16_t channel);            /* +1 импульс в канал */
uint32_t spectrum_get(uint16_t channel);
const uint32_t *spectrum_data(void);
uint32_t spectrum_total(void);

/* Тестовый генератор: добавить n синтетических событий
 * (Am-241 @ ch 101, Cs-137 @ ch 1138 FWHM 6.5%, фон). */
void spectrum_test_accumulate(uint32_t n);

#endif /* SPECTRUM_H */
