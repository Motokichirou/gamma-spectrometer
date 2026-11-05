/* Тесты detector.c и spectrum.c на синтетическом потоке (pulsegen_host). */
#include "t.h"
#include "detector.h"
#include "spectrum.h"
#include "tpl_3467k.h"
#include "pulsegen_host.h"
#include <string.h>
#include <stdlib.h>

#define BLK   8192u
#define NMAX  (80u * BLK)
#define BASE  645.0f

static float    fbuf[NMAX];
static uint16_t sbuf[NMAX];
static uint16_t chans[200000];
static int      nch;
static pulsegen_t pg;
static det_t    det;

static void emit(uint16_t c)
{
    if (nch < (int)(sizeof chans / sizeof chans[0]))
        chans[nch++] = c;
}

static void reset(uint16_t thr)
{
    det_cfg_t cfg = { thr, DET_MINW_DEF, DET_MAXW_DEF, 0.0f };
    det_init(&det, &cfg, tpl_3467k, emit);
    nch = 0;
}

static void feed(size_t n, size_t blk)
{
    for (size_t i = 0; i < n; i += blk)
        det_process_block(&det, sbuf + i, (n - i < blk) ? n - i : blk);
}

static void stats(double *mean, double *sd)
{
    double s = 0, s2 = 0;
    for (int i = 0; i < nch; i++) s += chans[i];
    *mean = nch ? s / nch : 0;
    for (int i = 0; i < nch; i++) s2 += (chans[i] - *mean) * (chans[i] - *mean);
    *sd = nch > 1 ? sqrt(s2 / (nch - 1)) : 0;
}

/* поток: 2 порции шума на разгон базы, затем npulse импульсов через step отсчётов */
static size_t make_train(int npulse, float amp, float sigma, size_t step)
{
    size_t n = 2 * BLK + (size_t)npulse * step + 64;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    for (int k = 0; k < npulse; k++)
        pg_add_pulse(&pg, fbuf, n, 2.0 * BLK + 20.0 + (double)k * step + pg_uniform(&pg), amp);
    pg_add_noise(&pg, fbuf, n, sigma);
    pg_quantize(fbuf, sbuf, n, BASE);
    return n;
}

static void test_amplitude_accuracy(void)
{
    pg_init(&pg, tpl_3467k, 64, 12345u);
    size_t n = make_train(2000, 580.0f, 1.8f, 48);
    reset(DET_THR_DEF);
    feed(n, BLK);
    double m, sd;
    stats(&m, &sd);
    printf("  Cs-137: принято %d, канал %.2f, sigma %.2f, база %.2f, sigma шума %.2f\n",
           nch, m, sd, det_baseline(&det), det_sigma(&det));
    CHECK(nch >= 1990 && nch <= 2000);
    CHECK_NEAR(m, 1160.0, 1.0);
    CHECK(sd <= 3.0);
    CHECK_NEAR(det_sigma(&det), 1.82, 0.15);
    det_stats_t st;
    det_get_stats(&det, &st);
    CHECK(st.accepted == (uint32_t)nch);
    CHECK(st.dead_samples > 2000u * 4u);
}

static void test_low_energy(void)
{
    pg_init(&pg, tpl_3467k, 64, 777u);
    size_t n = make_train(2000, 28.0f, 1.8f, 48);
    reset(9);
    feed(n, BLK);
    double m, sd;
    stats(&m, &sd);
    printf("  32 кэВ: принято %d из 2000, канал %.2f, sigma %.2f\n", nch, m, sd);
    CHECK(nch >= 1980 && nch <= 2005);
    CHECK_NEAR(m, 56.0, 1.0);
}

/*
 * Выбросы помехи 257 кГц: +12 LSB, ширина 1 и 2 отсчёта по очереди, каждые 7.7 мкс.
 * Шум 0.5 LSB: при σ 1.8 LSB выброс 2×12 по χ² от импульса ≈ 30 кэВ не отличить
 * (χ² медиана 14 у выброса против 99-го перцентиля 15 у настоящих импульсов).
 */
static void test_spike_rejected(void)
{
    pg_init(&pg, tpl_3467k, 64, 4242u);
    size_t n = 10 * BLK;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    const double period = 7.7e-6 * 3466666.67;  /* 26.7 отсчёта */
    int k = 0;
    for (double t = 2.0 * BLK; t < n - 4; t += period, k++)
        pg_add_spike(fbuf, n, (size_t)t, 1 + (k & 1), 12.0f);
    pg_add_noise(&pg, fbuf, n, 0.5f);
    pg_quantize(fbuf, sbuf, n, BASE);
    reset(9);
    feed(n, BLK);
    det_stats_t st;
    det_get_stats(&det, &st);
    printf("  выбросы %d: принято %u, форма %u, ширина %u\n", k, st.accepted, st.rej_shape,
           st.rej_width);
    CHECK(st.accepted == 0);
    CHECK(st.rej_shape > (uint32_t)k / 3);
    CHECK(st.rej_width > (uint32_t)k / 3);
}

static void test_pileup(void)
{
    pg_init(&pg, tpl_3467k, 64, 99u);
    const int npair = 300;
    size_t n = 2 * BLK + (size_t)npair * 64 + 64;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    for (int k = 0; k < npair; k++) {
        double t = 2.0 * BLK + 20.0 + k * 64.0 + pg_uniform(&pg);
        pg_add_pulse(&pg, fbuf, n, t, 580.0f);
        pg_add_pulse(&pg, fbuf, n, t + 4.0, 580.0f);
    }
    pg_add_noise(&pg, fbuf, n, 1.8f);
    pg_quantize(fbuf, sbuf, n, BASE);
    reset(DET_THR_DEF);
    feed(n, BLK);
    det_stats_t st;
    det_get_stats(&det, &st);
    printf("  наложения %d пар: принято %u, наложение %u, форма %u, ширина %u\n", npair,
           st.accepted, st.rej_pileup, st.rej_shape, st.rej_width);
    CHECK(st.accepted == 0);
    CHECK(st.rej_pileup == 2u * npair);
}

/* Импульс у стыка порций: вершина на последнем отсчёте и рядом; любое дробление потока. */
static void test_block_boundary(void)
{
    pg_init(&pg, tpl_3467k, 64, 5u);
    const size_t n = 4 * BLK;
    int ok_all = 1;
    for (int off = -6; off <= 6; off++) {
        for (int ph = 0; ph < 4; ph++) {
            memset(fbuf, 0, n * sizeof fbuf[0]);
            double tp = 3.0 * BLK - 1 + off + ph * 0.25;   /* off = 0: пик на последнем отсчёте */
            pg_add_pulse(&pg, fbuf, n, tp, 580.0f);
            pg_quantize(fbuf, sbuf, n, BASE);
            reset(DET_THR_DEF);
            feed(n, n);                                    /* одним куском — эталон */
            int ref_n = nch;
            uint16_t ref = nch ? chans[0] : 0;
            const size_t blks[] = { BLK, 7, 1, 333 };
            for (unsigned b = 0; b < sizeof blks / sizeof blks[0]; b++) {
                reset(DET_THR_DEF);
                feed(n, blks[b]);
                if (nch != 1 || ref_n != 1 || chans[0] != ref || abs((int)ref - 1160) > 3) {
                    printf("  стык: off %d ph %d блок %u: принято %d (%d), канал %u (%u)\n", off, ph,
                           (unsigned)blks[b], nch, ref_n, nch ? chans[0] : 0, ref);
                    ok_all = 0;
                }
            }
        }
    }
    CHECK(ok_all);
}

static void test_baseline_tracking(void)
{
    pg_init(&pg, tpl_3467k, 64, 31337u);
    const size_t ramp = 60 * BLK, n = ramp + 6 * BLK;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    for (size_t k = 0; k < n; k++)                         /* база 645 → 650 */
        fbuf[k] = (k < 2 * BLK) ? 0.0f
                : (k < ramp ? 5.0f * (float)(k - 2 * BLK) / (float)(ramp - 2 * BLK) : 5.0f);
    for (double t = 2.0 * BLK; t < n - 32; t += 60.0 + 80.0 * pg_uniform(&pg))
        pg_add_pulse(&pg, fbuf, n, t, 30.0f + 1500.0f * (float)pg_uniform(&pg));
    pg_add_noise(&pg, fbuf, n, 1.8f);
    pg_quantize(fbuf, sbuf, n, BASE);
    reset(DET_THR_DEF);
    feed(n, BLK);
    printf("  база %.3f (650), sigma %.3f\n", det_baseline(&det), det_sigma(&det));
    CHECK_NEAR(det_baseline(&det), 650.0, 0.5);
    CHECK_NEAR(det_sigma(&det), 1.82, 0.25);
}

static void test_overload(void)
{
    pg_init(&pg, tpl_3467k, 64, 8u);
    size_t n = 7 * BLK;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    for (size_t k = 2 * BLK; k < 5 * BLK; k++)            /* 3 порции выше порога */
        fbuf[k] = 50.0f;
    for (int k = 0; k < 20; k++)
        pg_add_pulse(&pg, fbuf, n, 5.0 * BLK + 200.0 + k * 300.0, 580.0f);
    pg_add_noise(&pg, fbuf, n, 1.8f);
    pg_quantize(fbuf, sbuf, n, BASE);
    reset(DET_THR_DEF);
    feed(n, BLK);
    det_stats_t st;
    det_get_stats(&det, &st);
    printf("  перегрузка: мёртвых %llu, ширина %u, принято %u, база %.2f\n",
           (unsigned long long)st.dead_samples, st.rej_width, st.accepted, det_baseline(&det));
    CHECK(st.dead_samples >= 3u * BLK - 64u);
    CHECK(st.rej_width >= 1u);
    CHECK(st.accepted >= 19u && st.accepted <= 20u);       /* после перегрузки снова считает */
    CHECK_NEAR(det_baseline(&det), BASE, 0.5);
    det_add_lost(&det, 8192u);
    det_stats_t st2;
    det_get_stats(&det, &st2);
    CHECK(st2.dead_samples == st.dead_samples + 8192u);
}

static void test_gain(void)
{
    pg_init(&pg, tpl_3467k, 64, 2024u);
    size_t n = make_train(500, 580.0f, 1.8f, 48);
    double m1, m2, sd;
    reset(DET_THR_DEF);
    feed(n, BLK);
    stats(&m1, &sd);
    reset(DET_THR_DEF);
    det_set_gain(&det, 0.99f);
    feed(n, BLK);
    stats(&m2, &sd);
    printf("  усиление: %.2f → %.2f (×%.4f)\n", m1, m2, m2 / m1);
    CHECK_NEAR(m2 / m1, 0.99, 0.0005);
}

static void test_spectrum(void)
{
    spectrum_clear();
    spectrum_add(0);
    spectrum_add(1160);
    spectrum_add(1160);
    spectrum_add(8191);
    spectrum_add(8192);                                    /* вне диапазона — мимо */
    CHECK(spectrum_get(1160) == 2);
    CHECK(spectrum_get(8191) == 1);
    CHECK(spectrum_get(9000) == 0);
    CHECK(spectrum_total() == 4);
    CHECK(spectrum_data()[0] == 1);
    spectrum_clear();
    CHECK(spectrum_total() == 0 && spectrum_get(1160) == 0);
}

int main(void)
{
    test_amplitude_accuracy();
    test_low_energy();
    test_spike_rejected();
    test_pileup();
    test_block_boundary();
    test_baseline_tracking();
    test_overload();
    test_gain();
    test_spectrum();
    return T_DONE();
}
