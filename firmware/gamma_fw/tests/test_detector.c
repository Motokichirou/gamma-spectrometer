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
    return T_DONE();
}
