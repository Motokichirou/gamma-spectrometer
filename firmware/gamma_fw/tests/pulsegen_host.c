/* pulsegen_host.c — синтез потока для тестов детектора (см. pulsegen_host.h). */
#include "pulsegen_host.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Строка p эталона — отсчёты формы в моменты (j − IPK + p/nph)·T от пика, пока максимум
 * стоит на «своём» отсчёте; после точки переключения p* (отсчёт слева от пика становится
 * больше правого) окно сдвинуто на отсчёт: (j − IPK − 1 + p/nph)·T. p* видно по скачку
 * вниз столбца 0. В узлах T/nph каждая точка формы встречается ровно один раз.
 */
void pg_init(pulsegen_t *g, const int16_t (*tpl)[PG_NS], int nph, uint32_t seed)
{
    int ps = nph;
    for (int p = 0; p + 1 < nph; p++)
        if (tpl[p][0] - tpl[p + 1][0] > 3000) { ps = p + 1; break; }
    g->rng = seed ? seed : 1u;
    g->nph = nph;
    g->len = PG_NS * nph;
    g->i0  = 2 * nph - ps;
    static unsigned char filled[64 * PG_NS + 2];
    for (int i = 0; i < g->len; i++) filled[i] = 0;
    for (int p = 0; p < nph; p++) {
        int sh = (p < ps) ? 1 : 2;
        for (int j = 0; j < PG_NS; j++) {
            int i = (j - sh) * nph + p + g->i0;
            if (i < 0 || i >= g->len || filled[i]) {
                printf("pg_init: неверная развёртка эталона (p=%d j=%d i=%d)\n", p, j, i);
                exit(2);
            }
            filled[i] = 1;
            g->h[i] = (float)tpl[p][j] / 16384.0f;
        }
    }
}

float pg_shape(const pulsegen_t *g, double tau)
{
    double u = tau * g->nph + g->i0;
    if (u < 0.0)
        return 0.0f;
    if (u >= g->len - 1) {           /* хвост: линейно к нулю за 8 отсчётов */
        double k = 1.0 - (u - (g->len - 1)) / (8.0 * g->nph);
        return (k > 0.0) ? (float)(g->h[g->len - 1] * k) : 0.0f;
    }
    int i = (int)u;
    double f = u - i;
    return (float)(g->h[i] * (1.0 - f) + g->h[i + 1] * f);
}

double pg_uniform(pulsegen_t *g)
{
    /* xorshift32 */
    uint32_t x = g->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g->rng = x;
    return (x >> 8) * (1.0 / 16777216.0);
}

double pg_gauss(pulsegen_t *g)
{
    double u1 = pg_uniform(g), u2 = pg_uniform(g);
    if (u1 < 1e-12) u1 = 1e-12;
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

void pg_add_pulse(const pulsegen_t *g, float *buf, size_t n, double t_peak, float amp)
{
    long k0 = (long)floor(t_peak) - 4, k1 = (long)ceil(t_peak) + 16;
    if (k0 < 0) k0 = 0;
    if (k1 > (long)n) k1 = (long)n;
    for (long k = k0; k < k1; k++)
        buf[k] += amp * pg_shape(g, (double)k - t_peak);
}

void pg_add_spike(float *buf, size_t n, size_t at, int width, float amp)
{
    for (int k = 0; k < width && at + (size_t)k < n; k++)
        buf[at + k] += amp;
}

void pg_add_noise(pulsegen_t *g, float *buf, size_t n, float sigma)
{
    for (size_t k = 0; k < n; k++)
        buf[k] += sigma * (float)pg_gauss(g);
}

void pg_quantize(const float *buf, uint16_t *out, size_t n, float base)
{
    for (size_t k = 0; k < n; k++) {
        long v = lround(base + buf[k]);
        out[k] = (uint16_t)(v < 0 ? 0 : (v > 4095 ? 4095 : v));
    }
}
