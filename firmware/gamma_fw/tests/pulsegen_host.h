/*
 * pulsegen_host.h — синтез потока отсчётов для тестов детектора (только ПК).
 *
 * Форма импульса — непрерывная, восстановленная из эталона [nph][7]: строки эталона —
 * отсчёты формы со сдвигом p/nph, выровненные по максимуму; из них собирается таблица
 * формы с шагом T/nph и линейная интерполяция между узлами. Поэтому импульс со случайной
 * (непрерывной) фазой не совпадает ни с одной строкой — как в жизни.
 */
#ifndef PULSEGEN_HOST_H
#define PULSEGEN_HOST_H

#include <stdint.h>
#include <stddef.h>

#define PG_NS 7

/* форма, заданная функцией (pg_init_fn): нормирована на пик 1.0, tau — отсчёты от опорного момента */
typedef float (*pg_shape_fn)(double tau);

typedef struct {
    uint32_t rng;
    pg_shape_fn fn;        /* ≠ NULL — форма из функции (pg_init_fn), иначе из эталона */
    int      pre, post;    /* pg_add_pulse пишет отсчёты floor(t) − pre … ceil(t) + post */
    int      nph;
    int      i0;           /* индекс узла τ = 0 (пик) в таблице */
    int      len;
    float    h[64 * PG_NS + 2];
} pulsegen_t;

void   pg_init(pulsegen_t *g, const int16_t (*tpl)[PG_NS], int nph, uint32_t seed);
/* форма из функции fn (длинный импульс, тесты разбора наложений); pre/post — см. pulsegen_t */
void   pg_init_fn(pulsegen_t *g, pg_shape_fn fn, int pre, int post, uint32_t seed);
/* форма, нормированная на пик 1.0; tau — время от пика в отсчётах */
float  pg_shape(const pulsegen_t *g, double tau);
double pg_uniform(pulsegen_t *g);                 /* [0, 1) */
double pg_gauss(pulsegen_t *g);                   /* N(0, 1) */
/* buf[k] += amp·shape(k − t_peak) */
void   pg_add_pulse(const pulsegen_t *g, float *buf, size_t n, double t_peak, float amp);
void   pg_add_spike(float *buf, size_t n, size_t at, int width, float amp);
void   pg_add_noise(pulsegen_t *g, float *buf, size_t n, float sigma);
/* out[k] = round(base + buf[k]), 0..4095 */
void   pg_quantize(const float *buf, uint16_t *out, size_t n, float base);

#endif
