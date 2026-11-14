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

static void reset_fits(uint16_t thr, uint16_t max_fits)
{
    det_cfg_t cfg = { thr, DET_MINW_DEF, DET_MAXW_DEF, 0.0f, max_fits };
    det_init(&det, &cfg, tpl_3467k, emit);
    nch = 0;
}

static void reset(uint16_t thr) { reset_fits(thr, 0); }

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
 * Выбросы помехи 257 кГц: ширина 1 и 2 отсчёта по очереди, каждые 7.7 мкс.
 * Одного χ² при σ 1.8 LSB мало (выброс 2×12 даёт χ² ≈ 14, 99-й перцентиль у импульсов ≈ 15),
 * выбросы отсеиваются по числу отсчётов окна выше thr/2 (DET_MIN_HALF): у выброса нет спада.
 * Регресс виден по проценту пропуска; допуск — 1 % выбросов шириной 2.
 */
static void spike_run(float amp, float sigma, uint32_t seed)
{
    pg_init(&pg, tpl_3467k, 64, seed);
    size_t n = 10 * BLK;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    const double period = 7.7e-6 * 3466666.67;  /* 26.7 отсчёта */
    int k = 0;
    for (double t = 2.0 * BLK; t < n - 4; t += period, k++)
        pg_add_spike(fbuf, n, (size_t)t, 1 + (k & 1), amp);
    pg_add_noise(&pg, fbuf, n, sigma);
    pg_quantize(fbuf, sbuf, n, BASE);
    reset_fits(9, 0xFFFFu);                         /* 130 тыс. выбросов/с — бюджет не мерим */
    feed(n, BLK);
    det_stats_t st;
    det_get_stats(&det, &st);
    printf("  выбросы %d × %.0f LSB, шум %.1f: принято %u (%.2f %% от ширины 2), форма %u, ширина %u\n",
           k, amp, sigma, st.accepted, 100.0 * st.accepted / (k / 2), st.rej_shape, st.rej_width);
    CHECK(st.accepted <= (uint32_t)(k / 2) / 100u);
    CHECK(st.rej_shape > (uint32_t)k / 3);
    CHECK_NEAR(det_baseline(&det), BASE, 0.5);
}

static void test_spike_rejected(void)
{
    spike_run(12.0f, 0.5f, 4242u);
    spike_run(12.0f, 1.8f, 4243u);                 /* шум базы по SPEC §7.3 */
    spike_run(20.0f, 1.8f, 4244u);
    spike_run(40.0f, 1.8f, 4245u);
}

/*
 * Потери у порога (thr = 9, σ 1.8). Отсечка выбросов (DET_MIN_HALF) их чуть увеличивает,
 * замер 2026-11-05 без неё → с ней: A 15 (≈17 кэВ) 74 → 59 %, A 20 (≈23 кэВ) 98.7 → 95 %,
 * A 28 (32 кэВ) без изменений. Пороги ниже — на регресс.
 */
static void test_threshold_losses(void)
{
    const float amps[] = { 12.0f, 15.0f, 20.0f, 28.0f };
    const int   min_ok[] = { 0, 1100, 1880, 1990 };   /* из 2000 */
    for (unsigned a = 0; a < sizeof amps / sizeof amps[0]; a++) {
        pg_init(&pg, tpl_3467k, 64, 600u + a);
        size_t n = make_train(2000, amps[a], 1.8f, 48);
        reset(DET_THR_DEF);
        feed(n, BLK);
        double m, sd;
        stats(&m, &sd);
        printf("  A = %.0f LSB: принято %d из 2000, канал %.1f\n", amps[a], nch, m);
        CHECK(nch >= min_ok[a] && nch <= 2005);
    }
}

/* Ступенька базы +15 кодов (> порога) и обратно: база восстанавливается, импульсы снова идут. */
static void test_baseline_step(void)
{
    const float steps[] = { 15.0f, 50.0f, -15.0f };
    for (unsigned q = 0; q < sizeof steps / sizeof steps[0]; q++) {
        pg_init(&pg, tpl_3467k, 64, 1500u + q);
        const size_t n = 12 * BLK;
        memset(fbuf, 0, n * sizeof fbuf[0]);
        for (size_t k = 2 * BLK; k < n; k++)
            fbuf[k] = steps[q];
        /* импульсы только через 2 порции после ступеньки */
        for (int k = 0; k < 500; k++)
            pg_add_pulse(&pg, fbuf, n, 4.0 * BLK + 200.0 + k * 48.0 + pg_uniform(&pg), 580.0f);
        pg_add_noise(&pg, fbuf, n, 1.8f);
        pg_quantize(fbuf, sbuf, n, BASE);
        reset(DET_THR_DEF);
        feed(4 * BLK, BLK);
        float b2 = det_baseline(&det);
        for (size_t i = 4 * BLK; i < n; i += BLK)
            det_process_block(&det, sbuf + i, BLK);
        double m, sd;
        stats(&m, &sd);
        det_stats_t st;
        det_get_stats(&det, &st);
        printf("  ступенька %+.0f: база через 2 порции %.2f, в конце %.2f, скачков %u, принято %d, "
               "канал %.2f, мёртвых %llu\n", steps[q], b2, det_baseline(&det), st.base_jumps, nch, m,
               (unsigned long long)st.dead_samples);
        CHECK_NEAR(b2, BASE + steps[q], 0.5);
        CHECK_NEAR(det_baseline(&det), BASE + steps[q], 0.5);
        CHECK(st.base_jumps == 1u);
        CHECK(nch >= 495 && nch <= 500);
        CHECK_NEAR(m, 1160.0, 1.5);
        CHECK(st.dead_samples < 500u * 48u);
    }
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


/* Заморозка базы (самотест ЦАП): отрицательные импульсы и сдвиг «спокойных» базу не двигают,
 * ступенька вверх не пересчитывает её; после отпускания — снова следит. Импульсы при заморозке
 * считаются от замороженной базы. */
static void test_base_hold(void)
{
    pg_init(&pg, tpl_3467k, 64, 4242u);
    const size_t n = 12 * BLK;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    for (double t = 2.0 * BLK + 100; t < 8.0 * BLK - 200; t += 347.0 + 44.0 * pg_uniform(&pg)) {
        pg_add_pulse(&pg, fbuf, n, t, 500.0f);
        /* «спуск ЦАП» лесенкой; у синтетической формы недолёт −4 % — отрицательные импульсы дают
           выброс вверх, поэтому редкие и небольшие (на плате выброса нет, см. dactest.h) */
        for (int k = 0; k < 2; k++)
            pg_add_pulse(&pg, fbuf, n, t + 52.0 + 40.0 * k, -100.0f);
    }
    pg_add_noise(&pg, fbuf, n, 1.8f);
    pg_quantize(fbuf, sbuf, n, BASE);
    reset(DET_THR_DEF);
    feed(2 * BLK, BLK);
    CHECK_NEAR(det_baseline(&det), 645.0, 0.3);
    det_set_base_hold(&det, 1);
    for (size_t i = 2 * BLK; i < 8 * BLK; i += BLK) det_process_block(&det, sbuf + i, BLK);
    CHECK_NEAR(det_baseline(&det), 645.0, 0.3);
    CHECK_NEAR(det_sigma(&det), 1.8, 0.3);
    double m, sd;
    stats(&m, &sd);
    CHECK(nch > 100);
    printf("  заморозка базы: принято %d, канал %.2f (1000), база %.3f\n", nch, m, det_baseline(&det));
    CHECK_NEAR(m, 2.0 * 500.0, 3.0);                               /* от замороженной базы */
    det_set_base_hold(&det, 0);
    for (size_t i = 8 * BLK; i < n; i += BLK) det_process_block(&det, sbuf + i, BLK);
    CHECK_NEAR(det_baseline(&det), 645.0, 0.3);
    /* ступенька вверх при заморозке — без пересчёта (base_jumps не растёт) */
    for (size_t k = 0; k < 6 * BLK; k++) sbuf[k] = (uint16_t)(k < BLK ? 645 : 700);
    reset(DET_THR_DEF);
    det_process_block(&det, sbuf, BLK);
    det_set_base_hold(&det, 1);
    for (size_t i = BLK; i < 6 * BLK; i += BLK) det_process_block(&det, sbuf + i, BLK);
    det_stats_t st;
    det_get_stats(&det, &st);
    CHECK(st.base_jumps == 0);
    CHECK_NEAR(det_baseline(&det), 645.0, 0.01);
    det_set_base_hold(&det, 0);
    for (size_t i = 0; i < 2; i++) det_process_block(&det, sbuf + BLK, BLK);
    CHECK_NEAR(det_baseline(&det), 700.0, 0.5);
}

/*
 * Перегрузка: 3 порции на +50 и обратно. По сигналу это не отличить от ступеньки базы:
 * после DET_BASE_N залипших отсчётов база переезжает наверх, после спада — сразу обратно
 * (2 скачка). Мёртвое время — только до пересчёта; первая порция после спада ещё со старой
 * базой, поэтому импульсы — со следующей.
 */
/* Замена эталона на лету: эталон вдвое ниже ⇒ канал вдвое выше; база и счёт не сбрасываются. */
static int16_t tpl_half[DET_NPH][DET_NS];

static void test_template_swap(void)
{
    pg_init(&pg, tpl_3467k, 64, 77u);
    size_t n = make_train(1000, 580.0f, 1.8f, 48);
    size_t half = 2 * BLK + 500 * 48;                      /* граница — между импульсами */
    half -= half % BLK;
    reset(DET_THR_DEF);
    feed(half, BLK);
    int n1 = nch;
    double m1 = 0;
    for (int i = 0; i < n1; i++) m1 += chans[i];
    m1 /= n1 ? n1 : 1;
    float b1 = det_baseline(&det);
    for (unsigned p = 0; p < DET_NPH; p++)
        for (unsigned k = 0; k < DET_NS; k++)
            tpl_half[p][k] = (int16_t)(tpl_3467k[p][k] / 2);
    det_set_template(&det, tpl_half);
    CHECK_NEAR(det_baseline(&det), b1, 1e-6);
    for (size_t i = half; i < n; i += BLK)
        det_process_block(&det, sbuf + i, (n - i < BLK) ? n - i : BLK);
    double m2 = 0;
    for (int i = n1; i < nch; i++) m2 += chans[i];
    m2 /= (nch - n1) ? (nch - n1) : 1;
    det_stats_t st;
    det_get_stats(&det, &st);
    printf("  смена эталона: до %d имп., канал %.1f; после %d имп., канал %.1f\n", n1, m1,
           nch - n1, m2);
    CHECK(st.accepted == (uint32_t)nch && nch >= 990);
    CHECK_NEAR(m1, 1160.0, 1.5);
    CHECK_NEAR(m2, 2320.0, 3.0);
}

/* Ревью Fable: эталон с большими значениями (|h| > 17 500): Σh² не переполняется, детектор не слепнет. */
static int16_t tpl_big[DET_NPH][DET_NS];
static void test_template_large_values(void)
{
    for (unsigned p = 0; p < DET_NPH; p++)
        for (unsigned k = 0; k < DET_NS; k++) tpl_big[p][k] = 20000;
    reset(DET_THR_DEF);
    det_set_template(&det, tpl_big);
    CHECK_NEAR(det.tph[0].ihh, 1.0 / (7.0 * 20000.0 * 20000.0), 1e-14);
    for (unsigned k = 0; k < DET_NS; k++) tpl_big[0][k] = 32767;
    det_set_template(&det, tpl_big);
    CHECK_NEAR(det.tph[0].ihh * 7.0 * 32767.0 * 32767.0, 1.0, 1e-6);
    /* эталон ×2 (пик 32767 вместо 16384): импульсы принимаются, канал — вдвое меньше */
    for (unsigned p = 0; p < DET_NPH; p++)
        for (unsigned k = 0; k < DET_NS; k++) {
            int v = 2 * tpl_3467k[p][k];
            tpl_big[p][k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    pg_init(&pg, tpl_3467k, 64, 4242u);
    size_t n = make_train(500, 580.0f, 1.8f, 48);
    reset(DET_THR_DEF);
    det_set_template(&det, tpl_big);
    feed(n, BLK);
    double m, sd;
    stats(&m, &sd);
    printf("  эталон x2: принято %d, канал %.1f\n", nch, m);
    CHECK(nch >= 490);
    CHECK_NEAR(m, 580.0, 2.0);
}

/* ---- порог и ширина на лету, авто-порог ---- */

/* det_set_thr / det_set_width между порциями: база, σ и счёт не сбрасываются, новое — со следующего импульса */
static void test_set_thr_width(void)
{
    pg_init(&pg, tpl_3467k, 64, 4711u);
    size_t n = make_train(1500, 580.0f, 1.8f, 48);       /* 500 имп. на каждую треть */
    reset(DET_THR_DEF);
    size_t p1 = 2 * BLK + 500 * 48, p2 = 2 * BLK + 1000 * 48;
    feed(p1, 8);                                         /* мелкими кусками — граница точно между импульсами */
    det_stats_t a;
    det_get_stats(&det, &a);
    float b = det_baseline(&det), sg = det_sigma(&det);
    CHECK(a.accepted >= 498u);
    /* ширина 20…30 при пороге 9: у Cs-137 выше порога меньше 20 отсчётов — всё в отбраковку по ширине */
    det_set_width(&det, 20, 30);
    CHECK(det.cfg.min_w == 20 && det.cfg.max_w == 30);
    CHECK_NEAR(det_baseline(&det), b, 1e-6);
    CHECK_NEAR(det_sigma(&det), sg, 1e-6);
    for (size_t i = p1; i < p2; i += 8) det_process_block(&det, sbuf + i, (p2 - i < 8) ? p2 - i : 8);
    det_stats_t m;
    det_get_stats(&det, &m);
    printf("  ширина 20…30: принято %u, по ширине %u\n", m.accepted - a.accepted, m.rej_width - a.rej_width);
    CHECK(m.accepted == a.accepted);                     /* счёт не сброшен и не вырос */
    CHECK(m.rej_width - a.rej_width >= 495u);
    /* обратно 2…16 и порог 200: Cs-137 (580) по-прежнему проходит, канал тот же */
    det_set_width(&det, DET_MINW_DEF, DET_MAXW_DEF);
    det_set_thr(&det, 200);
    CHECK(det_thr(&det) == 200 && !det_thr_measuring(&det));
    int n0 = nch;
    for (size_t i = p2; i < n; i += 8) det_process_block(&det, sbuf + i, (n - i < 8) ? n - i : 8);
    double s1 = 0;
    for (int i = n0; i < nch; i++) s1 += chans[i];
    printf("  порог 200: принято %d, канал %.1f\n", nch - n0, (nch - n0) ? s1 / (nch - n0) : 0.0);
    CHECK(nch - n0 >= 495);
    CHECK_NEAR(s1 / (nch - n0 ? nch - n0 : 1), 1160.0, 2.0);
    det_stats_t z;
    det_get_stats(&det, &z);
    CHECK(z.accepted == (uint32_t)nch);
    /* предел: thr > DET_THR_AUTO_MAX режется; ширина — пределы det_init */
    det_set_thr(&det, 5000);
    CHECK(det_thr(&det) == DET_THR_AUTO_MAX);
    det_set_width(&det, 0, 200);
    CHECK(det.cfg.min_w == 1 && det.cfg.max_w == DET_MAX_W);
    det_set_width(&det, 10, 3);
    CHECK(det.cfg.min_w == 10 && det.cfg.max_w == 10);
    /* низкий порог отсекает малые импульсы: A 15 при пороге 30 не проходит */
    pg_init(&pg, tpl_3467k, 64, 4712u);
    n = make_train(500, 15.0f, 1.8f, 48);
    reset(DET_THR_DEF);
    feed(2 * BLK, BLK);
    det_set_thr(&det, 30);
    for (size_t i = 2 * BLK; i < n; i += BLK) det_process_block(&det, sbuf + i, (n - i < BLK) ? n - i : BLK);
    CHECK(nch == 0);
}

/* Поток шума σ (и импульсы rate через step отсчётов, 0 — без) с порогом «авто»: порог → round(5σ). */
static void auto_case(float sigma, size_t step, int expect, int tol)
{
    pg_init(&pg, tpl_3467k, 64, 900u + (uint32_t)(sigma * 10.0f) + (uint32_t)step);
    const size_t n = 6 * BLK;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    if (step)
        for (double t = 30.0; t < n - 32; t += (double)step)
            pg_add_pulse(&pg, fbuf, n, t + pg_uniform(&pg), 580.0f);
    pg_add_noise(&pg, fbuf, n, sigma);
    pg_quantize(fbuf, sbuf, n, BASE);
    reset(0);
    CHECK(det_thr(&det) == DET_THR_DEF && det_thr_measuring(&det));
    feed(n, BLK);
    printf("  авто-порог: σ %.1f%s → порог %u (ожидание %d), σ детектора %.2f\n", sigma,
           step ? " + импульсы" : "", det_thr(&det), expect, det_sigma(&det));
    CHECK(!det_thr_measuring(&det));
    CHECK(abs((int)det_thr(&det) - expect) <= tol);
}

static void test_auto_thr(void)
{
    auto_case(1.8f, 0, 9, 1);
    auto_case(1.8f, 116, 9, 1);           /* ≈ 30 кимп/с Cs-137 во время замера */
    auto_case(4.0f, 0, 20, 1);
    auto_case(6.0f, 0, 30, 2);            /* σ много больше прежнего порога 9 — замер повторяется */
    auto_case(0.3f, 0, (int)DET_THR_AUTO_MIN, 0);
    /* повторный замер по det_set_thr(0) при старте набора: шум вырос 1.8 → 4 */
    pg_init(&pg, tpl_3467k, 64, 31u);
    const size_t n = 8 * BLK;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    pg_add_noise(&pg, fbuf, 4 * BLK, 1.8f);
    pg_add_noise(&pg, fbuf + 4 * BLK, 4 * BLK, 4.0f);
    pg_quantize(fbuf, sbuf, n, BASE);
    reset(0);
    feed(4 * BLK, BLK);
    CHECK(det_thr(&det) == 9 || det_thr(&det) == 10 || det_thr(&det) == 8);
    det_stats_t a;
    det_get_stats(&det, &a);
    det_set_thr(&det, 0);                 /* «-sta» */
    CHECK(det_thr_measuring(&det));
    for (size_t i = 4 * BLK; i < n; i += BLK) det_process_block(&det, sbuf + i, BLK);
    printf("  повторный замер: порог %u\n", det_thr(&det));
    CHECK(!det_thr_measuring(&det));
    CHECK(abs((int)det_thr(&det) - 20) <= 1);
    /* фиксированный порог отменяет замер */
    det_set_thr(&det, 0);
    det_set_thr(&det, 12);
    CHECK(!det_thr_measuring(&det) && det_thr(&det) == 12);
}

/* ---- бюджет времени ---- */

/* Пуассоновский поток rate имп/с (3.4667 Мвыб/с), амплитуды 30…1500, σ 1.8; две порции разгона. */
static size_t make_poisson(double rate, int nblk, uint32_t seed, float amp_fixed)
{
    pg_init(&pg, tpl_3467k, 64, seed);
    size_t n = (size_t)nblk * BLK;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    const double mean_gap = 3466667.0 / rate;
    double t = 2.0 * BLK;
    for (;;) {
        double u = pg_uniform(&pg);
        t += -log(1.0 - u) * mean_gap;
        if (t >= n - 32) break;
        float a = amp_fixed > 0 ? amp_fixed : 30.0f + 1470.0f * (float)pg_uniform(&pg);
        pg_add_pulse(&pg, fbuf, n, t, a);
    }
    pg_add_noise(&pg, fbuf, n, 1.8f);
    pg_quantize(fbuf, sbuf, n, BASE);
    return n;
}

static void test_budget_250k(void)
{
    CHECK(DET_MAX_FITS_DEF == 198 && DET_MAX_FITS_BOARD == 95);
    const int nblk = 22;
    size_t n = make_poisson(250000.0, nblk, 2501u, 580.0f);
    reset(DET_THR_DEF);
    det_stats_t prev, st;
    feed(2 * BLK, BLK);
    det_get_stats(&det, &prev);
    int over = 0;
    uint64_t dead_min = 0;
    for (int b = 2; b < nblk; b++) {
        det_process_block(&det, sbuf + (size_t)b * BLK, BLK);
        det_get_stats(&det, &st);
        uint32_t fits = (st.accepted - prev.accepted) + (st.rej_shape - prev.rej_shape);
        if (fits > DET_MAX_FITS_DEF) over = 1;
        if (st.rej_budget > prev.rej_budget) dead_min += 1;
        prev = st;
    }
    double m, sd;
    stats(&m, &sd);
    uint32_t trig = st.accepted + st.rej_shape + st.rej_width + st.rej_pileup + st.rej_budget;
    printf("  250 кимп/с: принято %u, наложение %u, форма %u, ширина %u, бюджет %u; "
           "мёртвых %.1f %%; канал %.1f ± %.1f\n", st.accepted, st.rej_pileup, st.rej_shape, st.rej_width,
           st.rej_budget, 100.0 * (double)st.dead_samples / (double)(n - 2 * BLK), m, sd);
    CHECK(!over);                                         /* подгонок на порцию ≤ бюджета */
    CHECK(st.rej_budget > 1000u);                         /* бюджет исчерпывался */
    CHECK(dead_min >= (uint64_t)(nblk - 2) / 2);
    CHECK(st.dead_samples <= (uint64_t)(n - 2 * BLK));
    CHECK(st.accepted > 1000u);
    /* принятые: пик на месте (часть — на хвостах соседей, как при любой загрузке; это не бюджет) */
    int inpk = 0;
    for (int i = 0; i < nch; i++) inpk += abs((int)chans[i] - 1160) <= 8;
    printf("    в пике 1160 ± 8: %.1f %%, база %.2f\n", 100.0 * inpk / (nch ? nch : 1), det_baseline(&det));
    CHECK(inpk > nch / 2);
    CHECK_NEAR(det_baseline(&det), BASE, 0.5);           /* спокойные отсчёты пропуска — без хвостов */
    /* триггеров в сумме — около числа импульсов (наложения склеивают часть) */
    CHECK(trig > (uint32_t)(250000.0 * (nblk - 2) * BLK / 3466667.0 / 2.0));
    /* при 30 кимп/с бюджет не мешает */
    n = make_poisson(30000.0, 12, 3001u, 0.0f);
    reset(DET_THR_DEF);
    feed(n, BLK);
    det_get_stats(&det, &st);
    printf("  30 кимп/с: принято %u, бюджет %u\n", st.accepted, st.rej_budget);
    CHECK(st.rej_budget == 0u);
}

/* Малый бюджет (10): в порции 50 разнесённых импульсов — 10 подогнано, 40 в rej_budget,
   остаток порции от 11-го триггера — в мёртвое время; следующая порция — снова 10. */
static void test_budget_exact(void)
{
    pg_init(&pg, tpl_3467k, 64, 77u);
    const size_t n = 4 * BLK;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    for (int b = 2; b < 4; b++)
        for (int k = 0; k < 50; k++)
            pg_add_pulse(&pg, fbuf, n, (double)b * BLK + 100.0 + k * 150.0 + pg_uniform(&pg), 580.0f);
    pg_add_noise(&pg, fbuf, n, 1.8f);
    pg_quantize(fbuf, sbuf, n, BASE);
    reset_fits(DET_THR_DEF, 10);
    feed(2 * BLK, BLK);
    det_stats_t a, b, c;
    det_get_stats(&det, &a);
    det_process_block(&det, sbuf + 2 * BLK, BLK);
    det_get_stats(&det, &b);
    /* 11-й триггер — первый отсчёт выше база + thr после 10-го импульса */
    size_t i11 = 0;
    {
        int cnt = 0, armed = 1;
        for (size_t k = 2 * BLK; k < 3 * BLK; k++) {
            int dev = (int)sbuf[k] - (int)BASE;
            if (armed && dev > (int)DET_THR_DEF) { if (++cnt == 11) { i11 = k - 2 * BLK; break; } armed = 0; }
            else if (!armed && dev < (int)DET_THR_DEF / 2) armed = 1;
        }
    }
    printf("  бюджет 10: принято %u, бюджет %u, мёртвых %llu (остаток от 11-го %u)\n",
           b.accepted - a.accepted, b.rej_budget - a.rej_budget,
           (unsigned long long)(b.dead_samples - a.dead_samples), (unsigned)(BLK - i11));
    CHECK(b.accepted - a.accepted == 10u);
    CHECK(b.rej_budget - a.rej_budget == 40u);
    CHECK(b.dead_samples - a.dead_samples >= BLK - i11);
    CHECK(b.dead_samples - a.dead_samples <= BLK - i11 + 10u * 40u);
    det_process_block(&det, sbuf + 3 * BLK, BLK);
    det_get_stats(&det, &c);
    CHECK(c.accepted - b.accepted == 10u);
    CHECK(c.rej_budget - b.rej_budget == 40u);
    CHECK_NEAR(det_baseline(&det), BASE, 0.5);
}

/* ---- запись окон ---- */

#define CAP_PRE  8u
#define CAP_POST 24u
#define CAP_MAX  600u
static uint16_t cap_buf[CAP_MAX * (CAP_PRE + CAP_POST)];
static det_cap_t cap_info[CAP_MAX];

/* первый отсчёт выше bint + thr начиная с from (как триггер детектора) */
static size_t find_trig(size_t from, size_t n, int bint, int thr)
{
    for (size_t k = from; k < n; k++)
        if ((int)sbuf[k] > bint + thr) return k;
    return n;
}

static void test_capture(void)
{
    pg_init(&pg, tpl_3467k, 64, 1234u);
    size_t n = make_train(300, 580.0f, 1.8f, 60);
    int ok_all = 1;
    const size_t blks[] = { BLK, 7, 333 };
    int ref_n = 0;
    for (unsigned bi = 0; bi < sizeof blks / sizeof blks[0]; bi++) {
        reset(DET_THR_DEF);
        feed(2 * BLK, BLK);
        det_set_capture(&det, cap_buf, cap_info, CAP_PRE, CAP_POST, CAP_MAX);
        int n0 = nch;
        for (size_t i = 2 * BLK; i < n; i += blks[bi])
            det_process_block(&det, sbuf + i, (n - i < blks[bi]) ? n - i : blks[bi]);
        unsigned cn = det_capture_count(&det);
        if (bi == 0) ref_n = nch - n0;
        printf("  запись окон (порция %u): принято %d, окон %u\n", (unsigned)blks[bi], nch - n0, cn);
        CHECK(cn == (unsigned)(nch - n0) && (int)cn == ref_n);
        int bint = (int)(det_baseline(&det) + 0.5f);
        double worst = 0;
        for (unsigned r = 0; r < cn; r++) {
            const det_cap_t *ci = &cap_info[r];
            const uint16_t *w = cap_buf + (size_t)r * (CAP_PRE + CAP_POST);
            size_t t = find_trig(ci->pos > 4 ? ci->pos - 4 : 0, n, bint, DET_THR_DEF);
            if (t != ci->pos) { ok_all = 0; printf("  окно %u: триггер %u, ожидался %u\n", r, ci->pos, (unsigned)t); continue; }
            if (memcmp(w, sbuf + ci->pos - CAP_PRE, (CAP_PRE + CAP_POST) * sizeof w[0]) != 0) {
                ok_all = 0; printf("  окно %u: отсчёты не совпали\n", r); continue;
            }
            /* канал — тот же, что ушёл в emit (окна идут в порядке приёма; база между вариантами
               дробления порций обновляется в разные моменты, поэтому сравнивать между ними нельзя) */
            if (ci->channel != chans[n0 + r] || ci->phase >= DET_NPH || ci->peak < 0 || ci->peak > 6
                || fabs(ci->amp * 2.0 - ci->channel) > 1.0) {
                ok_all = 0;
                printf("  окно %u: канал %u (%u), фаза %u, пик %d, A %.2f\n", r, ci->channel, chans[n0 + r],
                       ci->phase, ci->peak, ci->amp);
                continue;
            }
            /* фаза и положение максимума: эталон в окне ложится на отсчёты с невязкой ≈ шум */
            for (unsigned k = 0; k < DET_NS; k++) {
                int idx = (int)CAP_PRE + ci->peak - (int)DET_IPK + (int)k;
                double y = (double)w[idx] - ci->base;
                double h = ci->amp * tpl_3467k[ci->phase][k] / 16384.0;
                if (fabs(y - h) > worst) worst = fabs(y - h);
            }
        }
        printf("    наибольшая невязка эталона в окне %.1f LSB\n", worst);
        CHECK(worst < 10.0);
    }
    CHECK(ok_all);

    /* предел max_n: записывается ровно столько, дальше — не пишется; det_set_capture перезапускает счёт */
    reset(DET_THR_DEF);
    feed(2 * BLK, BLK);
    det_set_capture(&det, cap_buf, cap_info, CAP_PRE, CAP_POST, 5);
    for (size_t i = 2 * BLK; i < n; i += BLK)
        det_process_block(&det, sbuf + i, (n - i < BLK) ? n - i : BLK);
    CHECK(det_capture_count(&det) == 5);
    det_set_capture(&det, NULL, NULL, 0, 0, 0);
    CHECK(det_capture_count(&det) == 0);

    /* наложения не записываются */
    pg_init(&pg, tpl_3467k, 64, 99u);
    const int npair = 50;
    n = 2 * BLK + (size_t)npair * 64 + 64;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    for (int k = 0; k < npair; k++) {
        double t = 2.0 * BLK + 20.0 + k * 64.0 + pg_uniform(&pg);
        pg_add_pulse(&pg, fbuf, n, t, 580.0f);
        pg_add_pulse(&pg, fbuf, n, t + 4.0, 580.0f);
    }
    pg_add_noise(&pg, fbuf, n, 1.8f);
    pg_quantize(fbuf, sbuf, n, BASE);
    reset(DET_THR_DEF);
    feed(2 * BLK, BLK);
    det_set_capture(&det, cap_buf, cap_info, CAP_PRE, CAP_POST, CAP_MAX);
    for (size_t i = 2 * BLK; i < n; i += BLK)
        det_process_block(&det, sbuf + i, (n - i < BLK) ? n - i : BLK);
    CHECK(det_capture_count(&det) == 0);

    /* импульс, за которым в окне +24 идёт второй (через 18 отсчётов, оба принимаются): окно
       первого испорчено и не пишется, второе — пишется */
    pg_init(&pg, tpl_3467k, 64, 5u);
    n = 4 * BLK;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    pg_add_pulse(&pg, fbuf, n, 3.0 * BLK + 0.3, 580.0f);
    pg_add_pulse(&pg, fbuf, n, 3.0 * BLK + 18.3, 300.0f);
    pg_quantize(fbuf, sbuf, n, BASE);
    reset(DET_THR_DEF);
    det_set_capture(&det, cap_buf, cap_info, CAP_PRE, CAP_POST, CAP_MAX);
    feed(n, BLK);
    printf("  два импульса через 18: принято %d, окон %u\n", nch, det_capture_count(&det));
    CHECK(nch == 2);
    CHECK(det_capture_count(&det) == 1);
    CHECK(cap_info[0].pos > 3 * BLK + 10);
}

static void test_overload(void)
{
    pg_init(&pg, tpl_3467k, 64, 8u);
    size_t n = 8 * BLK;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    for (size_t k = 2 * BLK; k < 5 * BLK; k++)            /* 3 порции выше порога */
        fbuf[k] = 50.0f;
    for (int k = 0; k < 20; k++)
        pg_add_pulse(&pg, fbuf, n, 6.0 * BLK + 200.0 + k * 300.0, 580.0f);
    pg_add_noise(&pg, fbuf, n, 1.8f);
    pg_quantize(fbuf, sbuf, n, BASE);
    reset(DET_THR_DEF);
    feed(n, BLK);
    det_stats_t st;
    det_get_stats(&det, &st);
    printf("  перегрузка: мёртвых %llu, ширина %u, скачков базы %u, принято %u, база %.2f\n",
           (unsigned long long)st.dead_samples, st.rej_width, st.base_jumps, st.accepted,
           det_baseline(&det));
    CHECK(st.dead_samples >= DET_BASE_N && st.dead_samples < DET_BASE_N + 20u * 64u);
    CHECK(st.rej_width >= 1u);
    CHECK(st.base_jumps == 2u);
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

/* канал спектра насыщается на 2^32−1, а не обнуляется (SPEC_logger.md §2.7) */
static void test_spectrum_saturates(void)
{
    spectrum_clear();
    uint32_t *s = (uint32_t *)spectrum_data();
    s[100] = 0xFFFFFFFEu;
    spectrum_add(100);
    spectrum_add(100);
    spectrum_add(100);
    CHECK(spectrum_get(100) == 0xFFFFFFFFu);
    spectrum_add(8192);                        /* вне диапазона — молча мимо */
    CHECK(spectrum_get(8191) == 0u);
    spectrum_clear();
}

/* ======================================================================================
 * Разбор хвостовых наложений (det_set_tail / det_set_pile). Длинный импульс шейпера V2: форма —
 * замер по 10 564 одиночным Cs (tools/pileup/out/S.npz, нормирована на пик), узлы через отсчёт
 * τ = −3…+72 от момента CFD 50 %, между узлами — кубический сплайн Катмулла — Рома (до τ = −1 —
 * линейно, без выброса перед фронтом); после +72 выброс в минус спадает с τ = 40 отсчётов (на
 * приборе ≈ 450 — укорочен, чтобы не тянуть базу между парами). Эталон 64 × 7 и таблица хвоста
 * 16 × 64 строятся по этой же форме, как это сделает пульт по одиночным импульсам.
 * ====================================================================================== */
static const float lp_knot[76] = {                 /* τ = −3, −2, …, +72 */
    0.00000f, 0.00000f, 0.18006f, 0.50387f, 0.75450f, 0.90838f, 0.98324f, 0.99969f, 0.97727f, 0.92881f,
    0.86435f, 0.79142f, 0.71551f, 0.63995f, 0.56806f, 0.50098f, 0.43888f, 0.38259f, 0.33184f, 0.28603f,
    0.24593f, 0.21069f, 0.17983f, 0.15293f, 0.12973f, 0.10979f, 0.09262f, 0.07777f, 0.06502f, 0.05406f,
    0.04469f, 0.03678f, 0.03011f, 0.02436f, 0.01946f, 0.01534f, 0.01187f, 0.00893f, 0.00650f, 0.00451f,
    0.00288f, 0.00152f, 0.00041f, -0.00045f, -0.00119f, -0.00182f, -0.00235f, -0.00280f, -0.00316f, -0.00344f,
    -0.00372f, -0.00394f, -0.00412f, -0.00426f, -0.00439f, -0.00452f, -0.00463f, -0.00472f, -0.00479f, -0.00484f,
    -0.00488f, -0.00492f, -0.00494f, -0.00494f, -0.00495f, -0.00498f, -0.00499f, -0.00500f, -0.00501f, -0.00502f,
    -0.00502f, -0.00502f, -0.00500f, -0.00500f, -0.00500f, -0.00499f,
};
#define LP_T0   (-3)
#define LP_NK   76
#define LP_BASE 620.0f
#define LP_SIG  3.0f
#define LP_THR  15u                                  /* 5σ */

static double lp_max = 1.0;                          /* максимум сырой формы */
static double lp_xs;                                 /* f(xs) = f(xs + 1): граница «своего» отсчёта максимума */
#define LP_E(ph) (lp_xs - 0.15 + 1.3 * (ph) / DET_NPH) /* фаза эталона → отсчёт максимума − опорный момент */
static int16_t tplL[DET_NPH][DET_NS];
static det_tail_t tailL;

static double lp_raw(double tau)
{
    double u = tau - LP_T0;
    if (u <= 0.0) return 0.0;
    if (u >= LP_NK - 1) return lp_knot[LP_NK - 1] * exp(-(u - (LP_NK - 1)) / 40.0);
    int i = (int)u;
    double f = u - i;
    if (tau < -1.0) return lp_knot[i] + (lp_knot[i + 1] - lp_knot[i]) * f;
    double y0 = i > 0 ? lp_knot[i - 1] : 0.0, y1 = lp_knot[i], y2 = lp_knot[i + 1];
    double y3 = (i + 2 < LP_NK) ? lp_knot[i + 2] : y2;
    double m1 = 0.5 * (y2 - y0), m2 = 0.5 * (y3 - y1);
    double f2 = f * f, f3 = f2 * f;
    return (2 * f3 - 3 * f2 + 1) * y1 + (f3 - 2 * f2 + f) * m1 + (-2 * f3 + 3 * f2) * y2 + (f3 - f2) * m2;
}

static float lp_shape(double tau) { return (float)(lp_raw(tau) / lp_max); }

static void lp_setup(void)
{
    double m = 0.0, tm = 0.0;
    for (double t = 0.0; t < 8.0; t += 1.0 / 4096) {
        double v = lp_raw(t);
        if (v > m) { m = v; tm = t; }
    }
    lp_max = m;
    double a = tm - 1.0, b = tm;                     /* f(x) − f(x + 1): + слева, − справа */
    for (int k = 0; k < 60; k++) {
        double c = 0.5 * (a + b);
        if (lp_raw(c) - lp_raw(c + 1.0) > 0.0) b = c; else a = c;
    }
    lp_xs = 0.5 * (a + b);
    /* фаза p: отсчёт максимума отстоит от опорного момента на e = xs − 0.15 + 1.3·(p + 0.5)/64. Как у
       эталона с тракта (tpl_cs.h: 1.22 отсчёта), фазы перекрывают больше отсчёта: у вершины два отсчёта
       почти равны, и шум выбирает максимум «не тот» — без перекрытия такой импульс уходил бы в брак
       по форме (≈ 0.5 %) */
    for (int p = 0; p < (int)DET_NPH; p++) {
        double e = LP_E(p + 0.5);
        for (int j = 0; j < (int)DET_NS; j++)
            tplL[p][j] = (int16_t)lround(16384.0 * lp_shape(e - DET_IPK + j));
    }
    /* строка r — фазы 4r…4r+3, середина */
    for (int r = 0; r < (int)DET_TL_ROWS; r++) {
        double e = LP_E(4.0 * r + 2.0);
        for (int j = 1; j <= (int)DET_TL_LEN; j++) {
            long v = lround(32768.0 * lp_shape(e + j));
            tailL.s[r][j - 1] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
    }
    for (int p = 0; p < (int)DET_NPH; p++) tailL.row[p] = (uint8_t)(p / 4);
}

/* поправка DNL в тестах: широкий код 2047 (как ADC3 прибора: 2048…2061 не выдаются) */
#define GAP_LO 2047u                                 /* широкий код */
#define GAP_HI 2061u                                 /* последний пропущенный */
static int gap_on;                                   /* портить поток: коды GAP_LO+1…GAP_HI → GAP_LO */
static const det_dnl_t *dnl_g;                       /* таблица для reset_long/reset_dnl; NULL — без */
static unsigned dnl_ng;
static uint32_t dnl_map_g[DET_DNL_WORDS];
/* центр входного отрезка кода 2047 — [2046.5, 2061.5): 2054, сдвиг +7 LSB = 112/16 */
static const det_dnl_t dnl_gap[1] = { { GAP_LO, 112 } };

static void gap_apply(uint16_t *b, size_t n)
{
    if (!gap_on) return;
    for (size_t i = 0; i < n; i++)
        if (b[i] > GAP_LO && b[i] <= GAP_HI) b[i] = (uint16_t)GAP_LO;
}

static void reset_long(int pile)
{
    det_cfg_t cfg = { LP_THR, DET_MINW_BOARD, DET_MAXW_BOARD, 0.0f, 0 };
    det_init(&det, &cfg, (const int16_t (*)[DET_NS])tplL, emit);
    if (pile) det_set_tail(&det, &tailL);
    if (dnl_g) det_set_dnl(&det, dnl_g, dnl_ng, dnl_map_g);
    nch = 0;
}

/* шум на всём отрезке, импульсы — по списку (t — опорный момент, вершина ≈ t + 3.7) */
static void lp_stream(size_t n, const double *t, const float *a, int np, uint32_t seed)
{
    pg_init_fn(&pg, lp_shape, 4, 330, seed);
    memset(fbuf, 0, n * sizeof fbuf[0]);
    for (int k = 0; k < np; k++)
        pg_add_pulse(&pg, fbuf, n, t[k], a[k]);
    pg_add_noise(&pg, fbuf, n, LP_SIG);
    pg_quantize(fbuf, sbuf, n, LP_BASE);
    gap_apply(sbuf, n);
}

#define LP_STEP 400u                                 /* шаг пар: хвост и выброс в минус спадают */
#define LP_NPMAX 400
static double lp_t[2 * LP_NPMAX];
static float  lp_a[2 * LP_NPMAX];

typedef struct {
    int    both, one, zero, more;                    /* пар с 2 / 1 / 0 / >2 каналами */
    double e1, e2;                                   /* средняя ошибка A (коды) у пар, где оба */
    uint32_t rec, acc, pile, front, wid;             /* приращения счётчиков */
    uint64_t dead;
} lp_res_t;

/* npair пар: A1 в t, A2 в t + dlt (вершина — вершина); по пачке на пару — каналы пары видны */
static lp_res_t lp_pairs(int pile, int npair, float a1, float a2, double dlt, uint32_t seed, int rnd)
{
    pg_init_fn(&pg, lp_shape, 4, 330, seed);
    size_t n = 2 * BLK + (size_t)npair * LP_STEP;
    int np = 0;
    for (int k = 0; k < npair; k++) {
        double t = 2.0 * BLK + 50.0 + (double)k * LP_STEP + (rnd ? pg_uniform(&pg) : 0.0);
        lp_t[np] = t; lp_a[np++] = a1;
        if (a2 > 0.0f) { lp_t[np] = t + dlt; lp_a[np++] = a2; }
    }
    lp_stream(n, lp_t, lp_a, np, seed + 1u);
    reset_long(pile);
    feed(2 * BLK, BLK);
    det_stats_t s0, s1;
    det_get_stats(&det, &s0);
    lp_res_t r;
    memset(&r, 0, sizeof r);
    double s1e = 0.0, s2e = 0.0;
    for (int k = 0; k < npair; k++) {
        int c0 = nch;
        det_process_block(&det, sbuf + 2 * BLK + (size_t)k * LP_STEP, LP_STEP);
        int c = nch - c0;
        if (c == 2) {
            r.both++;
            s1e += chans[c0] / 2.0 - a1;
            s2e += chans[c0 + 1] / 2.0 - a2;
        } else if (c == 1) r.one++;
        else if (c == 0) r.zero++;
        else r.more++;
    }
    det_get_stats(&det, &s1);
    r.e1 = r.both ? s1e / r.both : 0.0;
    r.e2 = r.both ? s2e / r.both : 0.0;
    r.rec = s1.recovered - s0.recovered;
    r.acc = s1.accepted - s0.accepted;
    r.pile = s1.rej_pileup - s0.rej_pileup;
    r.front = s1.rej_front - s0.rej_front;
    r.wid = s1.rej_width - s0.rej_width;
    r.dead = s1.dead_samples - s0.dead_samples;
    return r;
}

/* выключенный разбор: det_set_tail(NULL) — тот же det_t бит в бит, что и без вызова; таблица при
   det_set_pile(0) — те же каналы и счётчики */
static uint16_t chans_b[200000];
static int      nch_b;
static det_t    det_b;
static void emit_b(uint16_t c) { if (nch_b < (int)(sizeof chans_b / sizeof chans_b[0])) chans_b[nch_b++] = c; }

static void test_pile_off_equiv(void)
{
    /* поток: одиночные и пары на Δ 6…60, 40 кимп/с, порции разной длины */
    pg_init_fn(&pg, lp_shape, 4, 330, 2024u);
    size_t n = 30 * BLK;
    int np = 0;
    for (double t = 2.0 * BLK; t < n - 400 && np < 2 * LP_NPMAX - 1; t += 60.0 + 300.0 * pg_uniform(&pg)) {
        lp_t[np] = t; lp_a[np++] = (float)(20.0 + 1200.0 * pg_uniform(&pg));
        if (pg_uniform(&pg) < 0.5) {
            lp_t[np] = t + 6.0 + 54.0 * pg_uniform(&pg); lp_a[np++] = (float)(20.0 + 1200.0 * pg_uniform(&pg));
        }
    }
    lp_stream(n, lp_t, lp_a, np, 2025u);
    for (int mode = 0; mode < 2; mode++) {
        det_cfg_t cfg = { LP_THR, DET_MINW_BOARD, DET_MAXW_BOARD, 0.0f, 0 };
        det_init(&det, &cfg, (const int16_t (*)[DET_NS])tplL, emit);
        det_init(&det_b, &cfg, (const int16_t (*)[DET_NS])tplL, emit);
        det_b.emit = emit;                           /* один и тот же адрес — для memcmp */
        if (mode == 0) det_set_tail(&det_b, NULL);
        else { det_set_tail(&det_b, &tailL); det_set_pile(&det_b, 0); det_b.emit = emit_b; }
        nch = 0; nch_b = 0;
        int bad_t = 0, bad_s = 0;
        size_t i = 0, k = 0;
        static const size_t blks[] = { BLK, 3000, 77, BLK, 5000, 1, 8191, 4096 };
        while (i < n) {
            size_t b = blks[k++ % (sizeof blks / sizeof blks[0])];
            if (b > n - i) b = n - i;
            if (mode == 0) {
                int c0 = nch;
                det_process_block(&det, sbuf + i, b);
                int c1 = nch;                        /* каналы det_b — следом в тот же chans */
                det_process_block(&det_b, sbuf + i, b);
                if (nch - c1 != c1 - c0 || memcmp(chans + c0, chans + c1, (size_t)(c1 - c0) * 2)) bad_s++;
                nch = c0 + (c1 - c0);                /* копия от det_b не нужна */
                if (memcmp(&det, &det_b, sizeof det)) bad_t++;
            } else {
                det_process_block(&det, sbuf + i, b);
                det_process_block(&det_b, sbuf + i, b);
                det_stats_t a, c;
                det_get_stats(&det, &a);
                det_get_stats(&det_b, &c);
                if (memcmp(&a, &c, sizeof a)) bad_s++;
            }
            i += b;
        }
        if (mode == 1) {
            if (nch != nch_b || memcmp(chans, chans_b, (size_t)nch * 2)) bad_s++;
            CHECK(det_pile_active(&det_b) == 0);
        }
        det_stats_t st;
        det_get_stats(&det, &st);
        printf("  разбор выкл (%s): импульсов %d, принято %u, наложения %u, расхождений det_t %d, "
               "каналов/счёта %d\n", mode ? "таблица, -pile off" : "det_set_tail(NULL)", np, st.accepted,
               st.rej_pileup, bad_t, bad_s);
        CHECK(bad_t == 0);
        CHECK(bad_s == 0);
        CHECK(st.recovered == 0 && st.rej_front == 0);
        CHECK(st.accepted > 500u);
    }
}

static void test_tail_pairs(void)
{
    static const float a2s[2] = { 600.0f, 60.0f };
    int bad_acc = 0, bad_e = 0, bad_glue = 0;
    for (int q = 0; q < 2; q++) {
        /* «оба приняты» — не хуже пар без перекрытия (Δ 200): одиночный импульс и сам изредка уходит в
           брак по форме — шум выбирает максимумом соседний отсчёт вершины (у A 60 ≈ 1.5 %) */
        lp_res_t c = lp_pairs(1, 200, 600.0f, a2s[q], 200.0, 4900u + q, 1);
        printf("  пары 600 + %.0f (200 на Δ; без перекрытия, Δ 200: оба %d): Δ: оба / ср. ошибка A1, A2, коды / "
               "восстановлено\n   ", a2s[q], c.both);
        for (int dlt = 8; dlt <= 60; dlt++) {
            lp_res_t r = lp_pairs(1, 200, 600.0f, a2s[q], dlt, 5000u + 97u * dlt + q, 1);
            if (dlt <= 16 || dlt % 4 == 0)
                printf(" %d: %d/%+.2f/%+.2f/%u", dlt, r.both, r.e1, r.e2, r.rec);
            if (dlt >= 14) {
                if (r.both < c.both - 4) { bad_acc++; printf(" [мало: %d]", r.both); }
                if (fabs(r.e2) > 0.005 * a2s[q] + 2.0 || fabs(r.e1) > 0.005 * 600.0 + 2.0) {
                    bad_e++; printf(" [ошибка %+.2f/%+.2f]", r.e1, r.e2);
                }
                CHECK(r.rec == (uint32_t)r.both);
            }
            if (dlt <= 11) {
                if (r.rec != 0u || r.both + r.more != 0) { bad_glue++; printf(" [два: %d, вост. %u]", r.both, r.rec); }
            }
        }
        printf("\n");
    }
    CHECK(bad_acc == 0);
    CHECK(bad_e == 0);
    CHECK(bad_glue == 0);
}

static void test_front_k2(void)
{
    /* опорный момент целый: максимум на τ = 4 (фаза ≈ 42), спад — к imax + 5; триггер второго
       (600) — на imax₂ − 5, т. е. imax₁ + 6 при Δ = 11 и imax₁ + 7 при Δ = 12 */
    for (int dlt = 11; dlt <= 12; dlt++) {
        lp_res_t r = lp_pairs(1, 100, 600.0f, 600.0f, dlt, 777u + dlt, 0);
        printf("  фронт k = 2, Δ = %d (триггер на imax₁ + %d): принято %u, наложения %u (фронт %u)\n",
               dlt, dlt - 5, r.acc, r.pile, r.front);
        CHECK(r.acc == 0u);
        CHECK(r.pile == 200u);
        CHECK(r.front == 200u);
        CHECK(r.rec == 0u);
    }
    /* Δ = 14: триггер на imax₁ + 9 — за проверкой фронта, оба приняты */
    lp_res_t r = lp_pairs(1, 100, 600.0f, 600.0f, 14, 790u, 0);
    printf("  Δ = 14 (триггер на imax₁ + 9): оба %d, принято %u, фронт %u\n", r.both, r.acc, r.front);
    CHECK(r.both == 100 && r.front == 0u);
}

static void test_tail_after_threshold(void)
{
    for (int k = 0; k < 2; k++) {
        double dlt = k ? 45.0 : 30.0;
        lp_res_t on = lp_pairs(1, 200, 600.0f, 60.0f, dlt, 3030u + k, 1);
        lp_res_t off = lp_pairs(0, 200, 600.0f, 60.0f, dlt, 3030u + k, 1);
        printf("  600 + 60 на Δ = %.0f: разбор — оба %d, ошибка A2 %+.2f кода (%+.1f %%); без разбора — оба %d, "
               "%+.2f (%+.1f %%)\n", dlt, on.both, on.e2, 100.0 * on.e2 / 60.0, off.both, off.e2,
               100.0 * off.e2 / 60.0);
        CHECK(on.both >= 198);
        CHECK(fabs(on.e2) <= 1.0);
    }
}

/* GUARD после TAIL (хвосты уже ≤ thr/2, но ещё не ушли за +64): взвод и триггер — по остатку s − база − хвост,
   с допуском |pred|/32, как в TAIL. По сырым отсчётам шум срабатывал на thr − pred ≈ thr/2: ложные «импульсы»
   уходили в брак по ширине, а настоящий второй импульс за ними (Δ 30…49) терялся в GUARD без взвода */
static void test_guard_after_tail(void)
{
    /* одиночные 1500: ни лишних каналов, ни брака по ширине на хвосте (шум σ 3, порог 15) */
    lp_res_t s = lp_pairs(1, 400, 1500.0f, 0.0f, 0.0, 9090u, 1);
    printf("  одиночные 1500 (400): принято %u, лишних каналов %d, брак по ширине %u, наложения %u\n",
           s.acc, s.more, s.wid, s.pile);
    CHECK(s.acc >= 398u);
    CHECK(s.more == 0);
    CHECK(s.wid == 0u);
    /* пары 1500 + 100 на Δ 30…49: второй ловится. Без вести (без брака) — ни одного; в брак по форме
       уходит, как одиночный A 100 (шум выбирает максимумом соседний отсчёт вершины, ≈ 1 %) */
    int silent = 0, extra = 0;
    uint32_t wid = 0, shp = 0;
    for (int dlt = 30; dlt <= 49; dlt++) {
        lp_res_t r = lp_pairs(1, 100, 1500.0f, 100.0f, dlt, 9100u + dlt, 1);
        const uint32_t rej = det.st.rej_shape + r.pile + r.wid;   /* det после lp_pairs — этот прогон */
        silent += 100 - r.both - r.more - (int)rej;
        extra += r.more;
        wid += r.wid;
        shp += det.st.rej_shape;
        if (fabs(r.e2) > 0.005 * 100.0 + 2.0) { printf("  [Δ %d: ошибка A2 %+.2f]", dlt, r.e2); CHECK(0); }
    }
    printf("  пары 1500 + 100, Δ 30…49 (по 100): потеряно без брака %d, лишних %d, брак по ширине %u, "
           "по форме %u\n", silent, extra, wid, shp);
    CHECK(silent == 0);
    CHECK(extra == 0);
    CHECK(wid == 0u);
    CHECK(shp <= 40u);
}

/* бюджет при разборе (cfg.max_fits_pile): действует, пока разбор включён; перебор — в rej_budget */
static void test_pile_budget(void)
{
    pg_init_fn(&pg, lp_shape, 4, 330, 9300u);
    const size_t n = 4 * BLK;
    int np = 0;
    for (int b = 2; b < 4; b++)
        for (int k = 0; k < 30; k++) { lp_t[np] = (double)b * BLK + 100.0 + k * 250.0; lp_a[np++] = 600.0f; }
    lp_stream(n, lp_t, lp_a, np, 9301u);
    det_stats_t st[2];
    for (int pile = 0; pile < 2; pile++) {
        det_cfg_t cfg = { LP_THR, DET_MINW_BOARD, DET_MAXW_BOARD, 0.0f, 25, 12 };
        det_init(&det, &cfg, (const int16_t (*)[DET_NS])tplL, emit);
        CHECK(det.max_fits == 25u);
        if (pile) { det_set_tail(&det, &tailL); CHECK(det.max_fits == 12u); }
        feed(n, BLK);
        det_get_stats(&det, &st[pile]);
    }
    printf("  бюджет 25 / при разборе 12 (по 30 импульсов в порции): принято %u / %u, бюджет %u / %u\n",
           st[0].accepted, st[1].accepted, st[0].rej_budget, st[1].rej_budget);
    CHECK(st[0].accepted == 50u && st[0].rej_budget == 10u);
    CHECK(st[1].accepted == 24u && st[1].rej_budget == 36u);
    det_set_pile(&det, 0);
    CHECK(det.max_fits == 25u);
    det_set_pile(&det, 1);
    CHECK(det.max_fits == 12u);
    det_set_tail(&det, NULL);
    CHECK(det.max_fits == 25u);
}

static void test_tail_across_blocks(void)
{
    /* 10 пар Δ = 20 в одной порции (меньше DET_BASE_N отсчётов — база между частями не обновится);
       вариант Б режет порцию между импульсами пары на imax₁ + c, c = −1 … 18 */
    static const int cut[10] = { -1, 2, 5, 6, 7, 8, 10, 13, 15, 18 };
    pg_init_fn(&pg, lp_shape, 4, 330, 4040u);
    size_t n = 4 * BLK;
    int np = 0;
    for (int k = 0; k < 10; k++) {
        double t = 2.0 * BLK + 30.0 + 400.0 * k;
        lp_t[np] = t; lp_a[np++] = 600.0f;
        lp_t[np] = t + 20.0; lp_a[np++] = (k & 1) ? 60.0f : 600.0f;
    }
    lp_stream(n, lp_t, lp_a, np, 4041u);
    reset_long(1);
    feed(n, BLK);
    int na = nch;
    static uint16_t ca[64];
    memcpy(ca, chans, (size_t)(na < 64 ? na : 64) * 2);
    det_stats_t sa, sb;
    det_get_stats(&det, &sa);

    reset_long(1);
    feed(2 * BLK, BLK);
    size_t i = 2 * BLK;
    for (int k = 0; k < 10; k++) {
        size_t c = (size_t)(lp_t[2 * k] + 4.0) + (size_t)(long)cut[k];   /* imax₁ = t + 4 */
        det_process_block(&det, sbuf + i, c - i);
        i = c;
    }
    det_process_block(&det, sbuf + i, 3 * BLK - i);
    det_process_block(&det, sbuf + 3 * BLK, BLK);
    det_get_stats(&det, &sb);
    printf("  стык порций между импульсами пары: принято %u / %u, восстановлено %u / %u, каналы %s\n",
           sa.accepted, sb.accepted, sa.recovered, sb.recovered,
           (na == nch && !memcmp(ca, chans, (size_t)na * 2)) ? "совпали" : "РАЗНЫЕ");
    CHECK(sa.accepted == 20u && sa.recovered == 10u);
    CHECK(na == nch && memcmp(ca, chans, (size_t)na * 2) == 0);
    CHECK(memcmp(&sa, &sb, sizeof sa) == 0);
}

static void test_tail_reset_on_lost(void)
{
    /* импульс 1 в конце порции (принят, хвост ещё идёт — TAIL), потеря 1 отсчёта, в следующей порции —
       другой поток с импульсом 2 (вершина на 12-м отсчёте, j ≈ 23 от импульса 1): старый хвост вычитать
       нельзя — без сброса A₂ вышла бы ниже на ≈ 30 кодов */
    int ch[2] = { -1, -1 };
    for (int v = 0; v < 2; v++) {
        int np = 0;
        if (v == 0) { lp_t[np] = 3.0 * BLK - 14.0; lp_a[np++] = 600.0f; }     /* imax₁ = 3·BLK − 10 */
        lp_stream(3 * BLK, lp_t, lp_a, np, 6061u);
        static float tmp[BLK];
        pulsegen_t g2;
        pg_init_fn(&g2, lp_shape, 4, 330, 6062u);
        memset(tmp, 0, sizeof tmp);
        pg_add_pulse(&g2, tmp, BLK, 8.0, 600.0f);
        pg_add_noise(&g2, tmp, BLK, LP_SIG);
        pg_quantize(tmp, sbuf + 3 * BLK, BLK, LP_BASE);
        reset_long(1);
        feed(3 * BLK, BLK);
        int c3 = nch;
        if (v == 0) CHECK(det.tl_n == 1);
        det_add_lost(&det, 1);
        CHECK(det.tl_n == 0);
        det_process_block(&det, sbuf + 3 * BLK, BLK);
        if (v == 0) CHECK(c3 == 1);
        if (nch - c3 == 1) ch[v] = chans[c3];
    }
    printf("  потеря порции посреди хвоста: канал второго %d, в чистом потоке %d\n", ch[0], ch[1]);
    CHECK(ch[0] >= 0 && ch[1] >= 0);
    CHECK(abs(ch[0] - ch[1]) <= 2);
}

/* Потеря посреди импульса при разборе: поток на самом деле непрерывен, но детектор думает, что пропало
   n отсчётов (pos0 += n). Импульс 1 стоит на стыке порций (PULSE) — его bpos в старой нумерации, хвост
   лёг бы на n отсчётов раньше и проверка фронта по нему дала бы ложный брак (rej_front += 2). Начатый
   импульс бросается, как при включении разбора; импульс 2 через 200 — как в чистом потоке. */
static void test_lost_mid_pulse(void)
{
    int ch[2] = { -1, -1 }, nc[2] = { 0, 0 };
    det_stats_t st = { 0 };
    for (int v = 0; v < 2; v++) {                    /* v = 0: с потерей и импульсом 1; v = 1: только импульс 2 */
        int np = 0;
        if (v == 0) { lp_t[np] = 3.0 * BLK - 6.0; lp_a[np++] = 800.0f; }   /* вершина ≈ 3·BLK − 2 */
        lp_t[np] = 3.0 * BLK + 200.0; lp_a[np++] = 600.0f;
        lp_stream(4 * BLK, lp_t, lp_a, np, 9091u);
        reset_long(1);
        feed(3 * BLK, BLK);
        if (v == 0) {
            CHECK(det.state == 1u /* PULSE */);
            det_add_lost(&det, 10);
        }
        det_process_block(&det, sbuf + 3 * BLK, BLK);
        nc[v] = nch;
        if (nch >= 1) ch[v] = chans[nch - 1];
        if (v == 0) det_get_stats(&det, &st);
    }
    printf("  потеря посреди импульса: каналов %d (чистый %d), второй %d / %d, rej_front %u, rej_pileup %u\n",
           nc[0], nc[1], ch[0], ch[1], st.rej_front, st.rej_pileup);
    CHECK(st.rej_front == 0u);
    CHECK(nc[0] == 1 && nc[1] == 1);                 /* импульс 1 брошен, лишнего события нет */
    CHECK(ch[0] >= 0 && abs(ch[0] - ch[1]) <= 2);
}

/* Разбор выключился посреди импульса: импульс бросается (буфер — с вычтенным хвостом), следующий — как
   в чистом потоке */
static void test_pile_off_mid_pulse(void)
{
    int ch[2] = { -1, -1 }, nc[2] = { 0, 0 };
    for (int v = 0; v < 2; v++) {
        int np = 0;
        if (v == 0) { lp_t[np] = 3.0 * BLK - 6.0; lp_a[np++] = 800.0f; }
        lp_t[np] = 3.0 * BLK + 200.0; lp_a[np++] = 600.0f;
        lp_stream(4 * BLK, lp_t, lp_a, np, 9092u);
        reset_long(1);
        feed(3 * BLK, BLK);
        if (v == 0) {
            CHECK(det.state == 1u /* PULSE */);
            det_set_pile(&det, 0);
            CHECK(det.state == 3u /* GUARD */);
        } else {
            det_set_pile(&det, 0);
        }
        det_process_block(&det, sbuf + 3 * BLK, BLK);
        nc[v] = nch;
        if (nch >= 1) ch[v] = chans[nch - 1];
    }
    printf("  выкл разбора посреди импульса: каналов %d (чистый %d), второй %d / %d\n", nc[0], nc[1], ch[0], ch[1]);
    CHECK(nc[0] == 1 && nc[1] == 1);
    CHECK(ch[0] >= 0 && abs(ch[0] - ch[1]) <= 2);
}

static void test_no_tail_after_reject(void)
{
    /* зашкал: 5000 кодов при базе 620 — срезан на 4095, отбраковка по форме; через 40 — импульс 300 */
    int ch[2] = { -1, -1 }, nc[2];
    det_stats_t st[2];
    for (int pile = 0; pile < 2; pile++) {
        size_t n = 3 * BLK;
        lp_t[0] = 2.0 * BLK + 100.0; lp_a[0] = 5000.0f;
        lp_t[1] = lp_t[0] + 40.0;    lp_a[1] = 300.0f;
        lp_stream(n, lp_t, lp_a, 2, 7070u);
        reset_long(pile);
        feed(n, BLK);
        det_get_stats(&det, &st[pile]);
        nc[pile] = nch;
        if (nch >= 1) ch[pile] = chans[nch - 1];
    }
    printf("  зашкал + импульс через 40: каналов %d / %d (без разбора / с разбором), второй %d / %d, "
           "форма %u / %u, восстановлено %u\n", nc[0], nc[1], ch[0], ch[1], st[0].rej_shape, st[1].rej_shape,
           st[1].recovered);
    CHECK(nc[0] == 1 && nc[1] == 1);
    CHECK(ch[0] == ch[1]);
    /* 300 → канал 600, минус выброс в минус зашкала (≈ −0.35 % от 5000 на +40…+46): ≈ 566 */
    CHECK(ch[0] >= 540 && ch[0] <= 600);
    CHECK(st[0].rej_shape >= 1u && st[1].rej_shape >= 1u);
    CHECK(st[1].recovered == 0u);
}

static void test_dead_time_peel(void)
{
    /* одиночные: мёртвое время с разбором не больше, чем без */
    uint64_t dd[2];
    uint32_t acc[2];
    for (int pile = 0; pile < 2; pile++) {
        pg_init_fn(&pg, lp_shape, 4, 330, 8080u);
        size_t n = 2 * BLK + 300u * 300u;
        int np = 0;
        for (int k = 0; k < 300; k++) {
            lp_t[np] = 2.0 * BLK + 50.0 + 300.0 * k + pg_uniform(&pg);
            lp_a[np++] = (float)(100.0 + 900.0 * pg_uniform(&pg));
        }
        lp_stream(n, lp_t, lp_a, np, 8081u);
        reset_long(pile);
        feed(2 * BLK, BLK);
        det_stats_t s0, s1;
        det_get_stats(&det, &s0);
        for (size_t i = 2 * BLK; i < n; i += BLK)
            det_process_block(&det, sbuf + i, (n - i < BLK) ? n - i : BLK);
        det_get_stats(&det, &s1);
        dd[pile] = s1.dead_samples - s0.dead_samples;
        acc[pile] = s1.accepted - s0.accepted;
    }
    /* пары Δ = 20: отсчёты TAIL живые — мёртвы только «триггер … imax + 5 + k» у каждого */
    lp_res_t on = lp_pairs(1, 200, 600.0f, 600.0f, 20.0, 8282u, 1);
    lp_res_t off = lp_pairs(0, 200, 600.0f, 600.0f, 20.0, 8282u, 1);
    printf("  мёртвое время: одиночные %llu / %llu (без / с разбором, принято %u / %u); пары Δ 20 — "
           "%llu / %llu, на пару %.1f / %.1f\n", (unsigned long long)dd[0], (unsigned long long)dd[1],
           acc[0], acc[1], (unsigned long long)off.dead, (unsigned long long)on.dead, off.dead / 200.0,
           on.dead / 200.0);
    CHECK(dd[1] <= dd[0]);
    CHECK(acc[1] == acc[0]);
    CHECK(on.both == 200 && on.rec == 200u);
    CHECK(on.dead <= 200u * 30u);
    CHECK(on.dead < off.dead);
}


/* ---- поправка DNL (det_set_dnl) ---- */

/* Короткий импульс, вершина 2040…2070 (через весь широкий код): без поправки канал у задетых импульсов
   съезжает вниз, с таблицей — возвращается. Поток один и тот же, импульсы сверяются попарно (все приняты во
   всех прогонах). Пустая таблица и таблица мимо окна — каналы и det_t бит в бит, как без вызова. */
#define DNL_NP 3000
static uint16_t dnl_ch[3][DNL_NP];
static void dnl_short_run(int corrupt, const det_dnl_t *tab, unsigned n_tab, uint16_t *out, int *acc)
{
    pg_init(&pg, tpl_3467k, 64, 777u);
    size_t n = 2 * BLK + (size_t)DNL_NP * 48u + 64;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    for (int k = 0; k < DNL_NP; k++)
        pg_add_pulse(&pg, fbuf, n, 2.0 * BLK + 20.0 + (double)k * 48 + pg_uniform(&pg),
                     (float)(1395.0 + 30.0 * pg_uniform(&pg)));
    pg_add_noise(&pg, fbuf, n, 1.8f);
    pg_quantize(fbuf, sbuf, n, BASE);
    gap_on = corrupt;
    gap_apply(sbuf, n);
    gap_on = 0;
    reset(DET_THR_DEF);
    if (tab) det_set_dnl(&det, tab, n_tab, dnl_map_g);
    feed(n, BLK);
    *acc = nch;
    if (out) memcpy(out, chans, (size_t)(nch < DNL_NP ? nch : DNL_NP) * 2);
}

static void test_dnl_short(void)
{
    int a[3];
    dnl_short_run(0, NULL, 0, dnl_ch[0], &a[0]);
    dnl_short_run(1, NULL, 0, dnl_ch[1], &a[1]);
    dnl_short_run(1, dnl_gap, 1, dnl_ch[2], &a[2]);
    CHECK(a[0] == DNL_NP && a[1] == DNL_NP && a[2] == DNL_NP);
    /* задетые порчей импульсы: сдвиг канала без таблицы и с таблицей относительно чистого потока */
    int nh = 0;
    double s1 = 0, s2 = 0, q1 = 0, q2 = 0;
    for (int k = 0; k < DNL_NP && a[0] == DNL_NP && a[1] == DNL_NP && a[2] == DNL_NP; k++) {
        if (dnl_ch[1][k] == dnl_ch[0][k]) continue;
        const double e1 = (double)dnl_ch[1][k] - dnl_ch[0][k], e2 = (double)dnl_ch[2][k] - dnl_ch[0][k];
        nh++; s1 += e1; s2 += e2; q1 += e1 * e1; q2 += e2 * e2;
    }
    const double m1 = nh ? s1 / nh : 0, m2 = nh ? s2 / nh : 0;
    const double r1 = nh ? sqrt(q1 / nh) : 0, r2 = nh ? sqrt(q2 / nh) : 0;
    printf("  DNL (широкий код 2047, вершина 2040…2070): задето %d из %d; сдвиг канала без таблицы %+.2f (СКО %.2f), "
           "с таблицей %+.2f (СКО %.2f)\n", nh, DNL_NP, m1, r1, m2, r2);
    CHECK(nh > DNL_NP / 10);
    CHECK(m1 < -2.0);                                /* порча: канал вниз */
    CHECK(fabs(m2) < 0.15 * fabs(m1));               /* таблица убирает смещение */
    CHECK(r2 < 0.6 * r1);

    /* без порчи и с таблицей мимо окна — каналы бит в бит, как без таблицы */
    static const det_dnl_t miss[2] = { { 100u, 40 }, { 4095u, -16 } };
    int a0, a1;
    dnl_short_run(0, NULL, 0, dnl_ch[1], &a0);
    det_t d0 = det;
    dnl_short_run(0, miss, 2, dnl_ch[2], &a1);
    CHECK(a1 == a0 && memcmp(dnl_ch[1], dnl_ch[2], sizeof dnl_ch[1]) == 0);
    CHECK(det.dnl_n == 2u);
    /* пустая таблица: det_t тот же, что без вызова */
    dnl_short_run(0, miss, 0, dnl_ch[2], &a1);
    CHECK(a1 == a0 && memcmp(dnl_ch[1], dnl_ch[2], sizeof dnl_ch[1]) == 0);
    CHECK(det.dnl_n == 0u && det.dnl == NULL && det.dnl_map == NULL);
    CHECK(memcmp(&d0, &det, sizeof det) == 0);
    /* карта: коды вне 0…4095 пропускаются, бит стоит ровно у кодов списка */
    static const det_dnl_t bad[3] = { { 4096u, 16 }, { 31u, 8 }, { 32u, -8 } };
    det_set_dnl(&det, bad, 3, dnl_map_g);
    unsigned bits = 0;
    for (unsigned w = 0; w < DET_DNL_WORDS; w++)
        for (unsigned b = 0; b < 32; b++) bits += (dnl_map_g[w] >> b) & 1u;
    CHECK(bits == 2u && (dnl_map_g[0] >> 31) == 1u && (dnl_map_g[1] & 1u) == 1u);
    det_set_dnl(&det, bad, 1, dnl_map_g);            /* только негодный код — поправки нет */
    CHECK(det.dnl_n == 0u);
}

/* Разбор наложений: второй импульс пары лежит на хвосте первого, его вершина (с хвостом) — в широком коде.
   Сырой код отсчёта окна = остаток + база + хвост: без учёта хвоста (здесь ≈ 150 кодов) код в карте не нашёлся бы. */
static void test_dnl_pile(void)
{
    const float a1 = 1000.0f;
    const double dlt = 16.0;
    double tm = 0.0, mx = 0.0;                       /* вершина формы относительно опорного момента */
    for (double t = 0.0; t < 8.0; t += 1.0 / 1024) if (lp_shape(t) > mx) { mx = lp_shape(t); tm = t; }
    const float tail = a1 * lp_shape(dlt + tm);
    lp_res_t r[3][2];
    for (int v = 0; v < 2; v++) {                    /* вершина второго у начала и у середины широкого кода */
        const float a2 = (v ? 2054.0f : 2049.0f) - LP_BASE - tail;
        for (int q = 0; q < 3; q++) {
            gap_on = q > 0;
            dnl_g = q == 2 ? dnl_gap : NULL;
            dnl_ng = 1;
            r[q][v] = lp_pairs(1, 300, a1, a2, dlt, 6100u + v, 1);
        }
    }
    gap_on = 0; dnl_g = NULL; dnl_ng = 0;
    double e1 = 0, e2 = 0;
    for (int v = 0; v < 2; v++) {
        printf("  DNL при разборе: хвост первого %.1f, вершина второго %d: ошибка A2 чистый %+.2f (%d пар), "
               "испорчен %+.2f (%d), с таблицей %+.2f (%d), восстановлено %u/%u/%u\n", tail, v ? 2054 : 2049,
               r[0][v].e2, r[0][v].both, r[1][v].e2, r[1][v].both, r[2][v].e2, r[2][v].both,
               r[0][v].rec, r[1][v].rec, r[2][v].rec);
        CHECK(r[0][v].both > 250 && r[1][v].both > 250 && r[2][v].both > 250);
        CHECK(r[2][v].rec > 250u);
        CHECK(fabs(r[2][v].e1 - r[0][v].e1) < 0.5);      /* первый ниже широкого кода — не тронут */
        e1 += r[1][v].e2 - r[0][v].e2;
        e2 += r[2][v].e2 - r[0][v].e2;
    }
    CHECK(tail > 100.0f);
    CHECK(e1 / 2 < -1.0);                            /* порча: A2 вниз */
    CHECK(fabs(e2) < 0.3 * fabs(e1));                /* таблица убирает смещение (в среднем по двум положениям) */
}

int main(void)
{
    test_spectrum_saturates();
    test_amplitude_accuracy();
    test_low_energy();
    test_spike_rejected();
    test_threshold_losses();
    test_pileup();
    test_block_boundary();
    test_baseline_tracking();
    test_baseline_step();
    test_base_hold();
    test_template_swap();
    test_template_large_values();
    test_overload();
    test_gain();
    test_set_thr_width();
    test_auto_thr();
    test_budget_250k();
    test_budget_exact();
    test_capture();
    test_spectrum();
    lp_setup();
    test_pile_off_equiv();
    test_tail_pairs();
    test_front_k2();
    test_tail_after_threshold();
    test_guard_after_tail();
    test_pile_budget();
    test_tail_across_blocks();
    test_tail_reset_on_lost();
    test_lost_mid_pulse();
    test_pile_off_mid_pulse();
    test_no_tail_after_reject();
    test_dead_time_peel();
    test_dnl_short();
    test_dnl_pile();
    return T_DONE();
}
