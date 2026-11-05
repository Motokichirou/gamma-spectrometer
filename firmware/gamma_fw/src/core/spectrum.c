/* spectrum.c — гистограмма 8192 каналов (см. spectrum.h). */
#include "spectrum.h"

static uint32_t spec[SPECTRUM_CHANNELS];
static uint32_t total;

void spectrum_clear(void)
{
    for (uint32_t i = 0; i < SPECTRUM_CHANNELS; i++)
        spec[i] = 0;
    total = 0;
}

void spectrum_add(uint16_t ch)
{
    if (ch < SPECTRUM_CHANNELS) {
        spec[ch]++;
        total++;
    }
}

uint32_t spectrum_get(uint16_t ch)
{
    return (ch < SPECTRUM_CHANNELS) ? spec[ch] : 0;
}

const uint32_t *spectrum_data(void)
{
    return spec;
}

uint32_t spectrum_total(void)
{
    return total;
}
