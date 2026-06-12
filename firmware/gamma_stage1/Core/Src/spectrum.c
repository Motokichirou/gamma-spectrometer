/* spectrum.c — гистограмма + тестовый генератор (см. spectrum.h) */

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

/* ---- тестовый генератор ------------------------------------------------ */

/* LCG: быстро и достаточно для демо */
static uint32_t rng_state = 0x12345678u;

static uint32_t rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/* Приближение нормального распределения: сумма 12 равномерных − 6 (ЦПТ).
 * Возвращает значение в "сигмах" × 256 (fixed point). */
static int32_t gauss_q8(void)
{
    int32_t acc = 0;
    for (int i = 0; i < 12; i++)
        acc += (int32_t)(rng() & 0xFFu);   /* 12 × U[0..255] */
    return acc - 12 * 128;                 /* центрировано, σ ≈ 73.9 (q8: ×256/73.9) */
}

/* канал = mu + sigma*N(0,1) */
static uint16_t gauss_channel(uint16_t mu, uint16_t sigma)
{
    /* gauss_q8: σ_raw ≈ 73.9 ⇒ масштаб sigma/73.9 */
    int32_t ch = (int32_t)mu + (gauss_q8() * (int32_t)sigma) / 74;
    if (ch < 0) ch = 0;
    if (ch >= (int32_t)SPECTRUM_CHANNELS) ch = SPECTRUM_CHANNELS - 1;
    return (uint16_t)ch;
}

void spectrum_test_accumulate(uint32_t n)
{
    /* Cs-137: канал 1138, FWHM 6.5% → σ ≈ 31 каналов
     * Am-241: канал 101,  FWHM ~11% → σ ≈ 5
     * Compton-континуум Cs-137: равномерно до края ~ch 800
     * Фон: равномерно 0..3000 */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = rng() % 100u;
        if (r < 35u)
            spectrum_add(gauss_channel(1138, 31));         /* фотопик Cs-137 */
        else if (r < 50u)
            spectrum_add(gauss_channel(101, 5));           /* Am-241 */
        else if (r < 85u)
            spectrum_add((uint16_t)(rng() % 800u));        /* комптон Cs-137 */
        else
            spectrum_add((uint16_t)(rng() % 3000u));       /* фон */
    }
}
