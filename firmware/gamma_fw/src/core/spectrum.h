/*
 * spectrum.h — гистограмма 8192 каналов (порт core/spectrum без тестового генератора).
 * Канал = амплитуда в половинах LSB (SPEC §7.4, §7.5).
 */
#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdint.h>

#define SPECTRUM_CHANNELS  8192u
#define SPECTRUM_MASK      0x7FFFFFFu  /* маска значений канала в пакете 0x01 shproto */

void     spectrum_clear(void);
void     spectrum_add(uint16_t channel);   /* +1 импульс; канал вне диапазона — мимо */
uint32_t spectrum_get(uint16_t channel);
const uint32_t *spectrum_data(void);
uint32_t spectrum_total(void);

#endif
