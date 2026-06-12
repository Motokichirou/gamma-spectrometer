/*
 * dsp.h — детектор импульсов: baseline-трекер, LSQ-фит вершины, PUR
 *
 * Переносимый модуль (без HAL). Этап 2 (Nucleo) и боевая плата.
 *
 * Оценка амплитуды: квадратичный LSQ-фит (парабола) по окну 2m+1 сэмплов
 * вокруг максимума; амплитуда = высота вершины параболы. Это несмещённая
 * суб-LSB оценка (12-бит ADC → 8192 канала, как заложено в ТЗ).
 * Если вершина плоская (кривизна ~0) — берётся сглаженное значение центра.
 *
 * Полярность: +1 = импульсы вверх (этап 2), −1 = вниз (боевая плата).
 */
#ifndef DSP_H
#define DSP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DSP_PULSE_BUF 256   /* макс. сэмплов одного импульса (≥ max_width) */

/* Валидный импульс. Аргумент — амплитуда в ПОЛОВИНАХ LSB ADC
 * (для 12-бит ADC это готовый индекс канала 0..8190). */
typedef void (*dsp_emit_fn)(uint16_t amplitude_half_lsb);

typedef struct {
    /* конфигурация */
    int8_t   polarity;       /* +1 / -1 */
    uint16_t threshold;      /* порог отклонения от baseline, ADC-коды */
    uint16_t min_width;      /* мин. ширина импульса, сэмплы */
    uint16_t max_width;      /* макс. ширина (дольше = pile-up/наводка) */
    uint8_t  fit_halfwidth;  /* m: окно фита = 2m+1 точек вокруг максимума
                                (боевая плата @4Мвыб/с: m=3 → 7 точек;
                                 этап 2 @2.83Мвыб/с, широкий пик: m=8) */
    dsp_emit_fn emit;

    /* состояние */
    int32_t  baseline_q8;    /* baseline × 256 (EMA в коридоре ±threshold) */
    bool     in_pulse;
    bool     falling;
    bool     pileup;
    uint16_t width;
    uint16_t prev_dev;
    int16_t  peak;
    uint16_t imax;           /* индекс максимума в buf */
    uint16_t buf_len;
    int16_t  buf[DSP_PULSE_BUF];   /* отклонения сэмплов импульса */

    /* статистика */
    uint32_t accepted;
    uint32_t rejected;
} dsp_t;

void dsp_init(dsp_t *d, int8_t polarity, uint16_t threshold,
              uint16_t min_width, uint16_t max_width,
              uint8_t fit_halfwidth, dsp_emit_fn emit);

void dsp_process(dsp_t *d, const uint16_t *samples, size_t n);

uint16_t dsp_baseline(const dsp_t *d);

#endif /* DSP_H */
