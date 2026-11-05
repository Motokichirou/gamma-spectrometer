/*
 * test_det_equiv.c — новый детектор (src/core/detector.c) против прежнего (tests/detector_ref.c):
 * одинаковые потоки и вызовы, после каждой порции сверяются бит в бит выданные каналы, статистика
 * отбраковки, база и σ² (побитно), порог и замер авто-порога, суммы базы, счёт ступеньки, состояние
 * машины, записанные окна. Потоки — pulsegen_host: Cs/Am/Co и широкий спектр до перегрузки АЦП,
 * шум 1.2–1.8 LSB, 30…250 кимп/с (наложения, исчерпание бюджета), выбросы помехи 1–2 отсчёта,
 * ступеньки базы и перегрузка, порции разной длины (стыки), запись окон, авто-порог, порог/ширина/
 * усиление/эталон на лету, потерянные порции, режим самотеста ЦАП (tpl_dac, база заморожена).
 * Итог — не меньше 10^5 выданных каналов.
 *
 * Тот же тест на ARM (тот же тулчейн и флаги, что прошивка; там подгонка собрана с VFMS) —
 * tools/cyc/run.sh --equiv.
 */
#include "t.h"
#include "detector.h"
#include "detector_ref.h"
#include "tpl_3467k.h"
#include "tpl_dac.h"
#include "pulsegen_host.h"
#include <string.h>
#include <stdlib.h>

#define BLK   8192u
#define NBLK  40u
#define NMAX  (NBLK * BLK)
#define CHMAX 16384
#define CAPN  64u
#define CAPW  (8u + 24u)

static float    fbuf[NMAX];
static uint16_t sbuf[NMAX];
static pulsegen_t pg;
static det_t    dn;
static detref_t dr;
static uint16_t ch_n[CHMAX], ch_r[CHMAX];
static int      nn, nr;
static uint16_t capb_n[CAPN * CAPW], capb_r[CAPN * CAPW];
static det_cap_t    capi_n[CAPN];
static detref_cap_t capi_r[CAPN];
static int16_t  tpl_half[DET_NPH][DET_NS];

static long tot_pulses, tot_emit, tot_blocks, tot_fails;
static const char *cur_tag;

static void emit_n(uint16_t c) { if (nn < CHMAX) ch_n[nn] = c; nn++; }
static void emit_r(uint16_t c) { if (nr < CHMAX) ch_r[nr] = c; nr++; }

/* ---- одинаковые вызовы обоих ---- */
static void both_init(uint16_t thr, uint16_t max_fits, int dac, uint8_t minw, uint8_t maxw)
{
    det_cfg_t a = { thr, minw ? minw : DET_MINW_DEF, maxw ? maxw : DET_MAXW_DEF, 0.0f, max_fits };
    detref_cfg_t b = { thr, minw ? minw : DETREF_MINW_DEF, maxw ? maxw : DETREF_MAXW_DEF, 0.0f, max_fits };
    det_init(&dn, &a, dac ? tpl_dac : tpl_3467k, emit_n);
    detref_init(&dr, &b, dac ? tpl_dac : tpl_3467k, emit_r);
    memset(capb_n, 0, sizeof capb_n); memset(capb_r, 0, sizeof capb_r);
    memset(capi_n, 0, sizeof capi_n); memset(capi_r, 0, sizeof capi_r);
    nn = nr = 0;
}
static void both_capture(int on)
{
    if (on) {
        det_set_capture(&dn, capb_n, capi_n, 8, 24, CAPN);
        detref_set_capture(&dr, capb_r, capi_r, 8, 24, CAPN);
    } else {
        det_set_capture(&dn, NULL, NULL, 0, 0, 0);
        detref_set_capture(&dr, NULL, NULL, 0, 0, 0);
    }
}
static void both_hold(int on)      { det_set_base_hold(&dn, on); detref_set_base_hold(&dr, on); }
static void both_thr(uint16_t t)   { det_set_thr(&dn, t); detref_set_thr(&dr, t); }
static void both_width(uint8_t a, uint8_t b) { det_set_width(&dn, a, b); detref_set_width(&dr, a, b); }
static void both_gain(float g)     { det_set_gain(&dn, g); detref_set_gain(&dr, g); }
static void both_lost(uint32_t n)  { det_add_lost(&dn, n); detref_add_lost(&dr, n); }

/* ---- сверка ---- */
static void compare(size_t blk)
{
    int bad = 0;
    if (nn != nr || memcmp(ch_n, ch_r, (size_t)(nn < CHMAX ? nn : CHMAX) * sizeof ch_n[0])) bad |= 1;
    det_stats_t a;
    detref_stats_t b;
    det_get_stats(&dn, &a);
    detref_get_stats(&dr, &b);
    if (memcmp(&a, &b, sizeof a)) bad |= 2;
    float fa = det_baseline(&dn), fb = detref_baseline(&dr), sa = det_sigma(&dn), sb = detref_sigma(&dr);
    if (memcmp(&dn.base, &dr.base, sizeof dn.base) || memcmp(&dn.var, &dr.var, sizeof dn.var)
        || memcmp(&fa, &fb, sizeof fa) || memcmp(&sa, &sb, sizeof sa) || dn.bint != dr.bint) bad |= 4;
    if (det_thr(&dn) != detref_thr(&dr) || det_thr_measuring(&dn) != detref_thr_measuring(&dr)) bad |= 8;
    if (dn.qn != dr.qn || dn.qsum != dr.qsum || dn.qsq != dr.qsq) bad |= 16;
    if (dn.stk_n != dr.stk_n || dn.stk_s1 != dr.stk_s1 || dn.stk_s2 != dr.stk_s2) bad |= 32;
    if (dn.state != dr.state || dn.pos0 != dr.pos0 || dn.fits_left != dr.fits_left) bad |= 64;
    if (det_capture_count(&dn) != detref_capture_count(&dr) || dn.cap_on != dr.cap_on
        || memcmp(capb_n, capb_r, sizeof capb_n) || memcmp(capi_n, capi_r, sizeof capi_n)) bad |= 128;
    tot_emit += nn;
    nn = nr = 0;
    if (bad) {
        if (tot_fails < 10)
            printf("  РАСХОЖДЕНИЕ: %s, порция %u, маска 0x%x; состояние %u/%u, ступенька %u/%u %d/%d "
                   "%lld/%lld, каналов %d/%d\n", cur_tag, (unsigned)blk, bad, dn.state, dr.state,
                   dn.stk_n, dr.stk_n, dn.stk_s1, dr.stk_s1, (long long)dn.stk_s2, (long long)dr.stk_s2,
                   nn, nr);
        tot_fails++;
    }
}

/* ---- поток ---- */
enum { K_CS, K_AM, K_CO, K_WIDE, K_DAC };

static float amp_of(int kind)
{
    double u = pg_uniform(&pg);
    switch (kind) {
    case K_AM:   return u < 0.8 ? (float)(52.0 + 3.0 * pg_gauss(&pg)) : (float)(12.0 + 288.0 * pg_uniform(&pg));
    case K_CO:   return u < 0.15 ? 1025.0f : u < 0.3 ? 1160.0f : (float)(15.0 + 985.0 * pg_uniform(&pg));
    case K_WIDE: return (float)(12.0 + 3600.0 * pg_uniform(&pg));       /* до перегрузки АЦП (4095) */
    case K_DAC:  return (float)(100.0 + 900.0 * pg_uniform(&pg));
    default:     return u < 0.24 ? 580.0f : (float)(15.0 + 485.0 * pg_uniform(&pg));
    }
}

/* nblk порций: первые 2 — только шум; пуассоновский поток rate имп/с */
static size_t make_stream(int kind, double rate, float sigma, unsigned nblk, uint32_t seed)
{
    pg_init(&pg, kind == K_DAC ? tpl_dac : tpl_3467k, 64, seed);
    size_t n = (size_t)nblk * BLK;
    memset(fbuf, 0, n * sizeof fbuf[0]);
    const double gap = 3466667.0 / rate;
    double t = 2.0 * BLK;
    for (;;) {
        t += -log(1.0 - pg_uniform(&pg)) * gap;
        if (t >= n - 32) break;
        float a = amp_of(kind);
        pg_add_pulse(&pg, fbuf, n, t, a);
        if (kind == K_DAC)                               /* спуск ЦАП — отрицательные импульсы */
            pg_add_pulse(&pg, fbuf, n, t + 52.0, -0.2f * a);
        tot_pulses++;
    }
    pg_add_noise(&pg, fbuf, n, sigma);
    pg_quantize(fbuf, sbuf, n, 645.0f);
    return n;
}

typedef void (*hook_fn)(unsigned k);

/* поток порциями длины splits[k % ns]; hook(k) — перед порцией k; сверка — после каждой */
static void run(const char *tag, size_t n, const size_t *splits, unsigned ns, hook_fn hook)
{
    cur_tag = tag;
    size_t i = 0;
    unsigned k = 0;
    while (i < n) {
        size_t m = splits[k % ns];
        if (m > n - i) m = n - i;
        if (hook) hook(k);
        det_process_block(&dn, sbuf + i, m);
        detref_process_block(&dr, sbuf + i, m);
        compare(k);
        i += m;
        k++;
        tot_blocks++;
    }
}

static const size_t SPL_A[] = { BLK };
static const size_t SPL_B[] = { BLK, 7, 1, 333, 4096, 5000 };
static const size_t SPL_C[] = { 1000, 3, 2048, 8191 };
static const size_t SPL_D[] = { 1, 2, 7, 64, 31 };

static void hook_cap2(unsigned k)  { if (k == 2) both_capture(1); }
static void hook_otf(unsigned k)                  /* на лету: порог, ширина, усиление, эталон, потери */
{
    switch (k) {
    case 3:  both_capture(1); break;
    case 6:  both_thr(30); break;
    case 9:  both_width(3, 10); break;
    case 12: both_gain(0.987f); break;
    case 15: det_set_template(&dn, tpl_half); detref_set_template(&dr, tpl_half); break;
    case 18: both_lost(8192); break;
    case 20: both_thr(0); break;                  /* авто */
    case 24: det_set_template(&dn, tpl_3467k); detref_set_template(&dr, tpl_3467k); both_width(1, 64); break;
    case 27: both_capture(0); break;
    case 30: both_capture(1); both_thr(9); break;
    default: break;
    }
}
static void hook_dac(unsigned k)
{
    if (k == 2) both_hold(1);
    if (k == 3) both_capture(1);
    if (k == 30) both_hold(0);
}

/* ---- смена порога/ширины между порциями посреди импульса и максимум на buf[0] (ревью slowpath) ---- */
static size_t fed;
static void feed_to(size_t b)                     /* sbuf[fed, b) — одной порцией, сверка */
{
    det_process_block(&dn, sbuf + fed, b - fed);
    detref_process_block(&dr, sbuf + fed, b - fed);
    compare((unsigned)tot_blocks);
    tot_blocks++;
    fed = b;
}
static void put_dev(size_t at, const int *dv, size_t k)   /* отсчёты bint + dv[] */
{
    for (size_t q = 0; q < k; q++) sbuf[at + q] = (uint16_t)(dn.bint + dv[q]);
}
/* шум σ 1.2 вокруг 645 на 3 порции; первые 2 — разгон базы */
static void quiet_stream(uint32_t seed, uint16_t thr, uint8_t maxw)
{
    size_t n = 3u * BLK;
    pg_init(&pg, tpl_3467k, 64, seed);
    memset(fbuf, 0, n * sizeof fbuf[0]);
    pg_add_noise(&pg, fbuf, n, 1.2f);
    pg_quantize(fbuf, sbuf, n, 645.0f);
    both_init(thr, 0, 0, 1, maxw);
    fed = 0;
    feed_to(2u * BLK);
}
static void test_midpulse(void)
{
    const size_t P = 2u * BLK + 100u;
    det_stats_t s0, s1;

    /* 1. порог снижен 1000 → 30 при поднятом последнем отсчёте порции: импульс «37 37 34 31 28»
       с до-триггерным buf[0] = 680 выше всех ⇒ imax = 0, окно подгонки ушло бы за начало буфера
       (прежде: выход за массив, на ПК — аварийное завершение) ⇒ отбраковка по форме */
    cur_tag = "imax = 0";
    quiet_stream(7001, 1000, 16);
    { static const int a[] = { 680 }; put_dev(P - 1, a, 1); }
    feed_to(P);
    both_thr(30);
    { static const int a[] = { 37, 37, 34, 31, 28 }; put_dev(P, a, 5); }
    det_get_stats(&dn, &s0);
    feed_to(P + 5);
    feed_to(P + 200);
    det_get_stats(&dn, &s1);
    CHECK(s1.rej_shape == s0.rej_shape + 1u && s1.accepted == s0.accepted);

    /* 2. порог вырос 9 → 200 на спаде (falling, lpeak 188 < 2·thr): повторный рост уже не
       наложение — импульс доходит до хвоста окна, как в прежнем коде */
    cur_tag = "порог вырос на спаде";
    quiet_stream(7002, 9, 16);
    { static const int a[] = { 50, 120, 188, 130 }; put_dev(P, a, 4); }
    feed_to(P);
    feed_to(P + 4);
    both_thr(200);
    { static const int a[] = { 368, 579, 291, 156 }; put_dev(P + 4, a, 4); }
    det_get_stats(&dn, &s0);
    feed_to(P + 200);
    det_get_stats(&dn, &s1);
    CHECK(s1.rej_pileup == s0.rej_pileup);

    /* 3. max_w уменьшена 64 → 10 при набранной ширине 20: отбраковка по ширине на следующем
       отсчёте, и выше, и ниже порога (прежде plim переполнялся и импульс принимался) */
    for (unsigned v = 0; v < 2; v++) {
        cur_tag = v ? "max_w 64 → 10, отсчёт выше порога" : "max_w 64 → 10, отсчёт ниже порога";
        quiet_stream(7003 + v, 9, 64);
        int a[21];
        for (unsigned q = 0; q < 20; q++) a[q] = 100;
        put_dev(P, a, 20);
        feed_to(P);
        feed_to(P + 20);
        both_width(1, 10);
        a[0] = v ? 100 : 5; a[1] = 5;
        put_dev(P + 20, a, 2);
        det_get_stats(&dn, &s0);
        feed_to(P + 200);
        det_get_stats(&dn, &s1);
        CHECK(s1.rej_width == s0.rej_width + 1u);
    }

    /* 4. граница в общем режиме: порог 9 → 100 на спаде (lpeak 188 < 200 ⇒ общий режим) и
       max_w 16 → 6; импульс кончается при ширине ровно 6 — не шире max_w, не отбраковка */
    cur_tag = "ширина = max_w в общем режиме";
    quiet_stream(7005, 9, 16);
    { static const int a[] = { 50, 120, 188, 130 }; put_dev(P, a, 4); }
    feed_to(P);
    feed_to(P + 4);
    both_thr(100);
    both_width(1, 6);
    { static const int a[] = { 150, 140, 5 }; put_dev(P + 4, a, 3); }
    det_get_stats(&dn, &s0);
    feed_to(P + 200);
    det_get_stats(&dn, &s1);
    CHECK(s1.rej_width == s0.rej_width && s1.rej_pileup == s0.rej_pileup);
    feed_to(3u * BLK);
}

static void test_equiv(void)
{
    test_midpulse();
    static const float sig[] = { 1.2f, 1.5f, 1.8f };
    static const double rates[] = { 30000.0, 100000.0 };
    static const char *kn[] = { "Cs", "Am", "Co", "широкий" };
    char tag[96];
    uint32_t seed = 1000;
    for (int kind = K_CS; kind <= K_WIDE; kind++)
        for (unsigned si = 0; si < 3; si++)
            for (unsigned ri = 0; ri < 2; ri++)
                for (unsigned v = 0; v < 2; v++) {
                    size_t n = make_stream(kind, rates[ri], sig[si], NBLK, seed++);
                    snprintf(tag, sizeof tag, "%s σ%.1f %.0f кимп/с вар.%u", kn[kind], sig[si],
                             rates[ri] / 1e3, v);
                    both_init(DET_THR_DEF, 0, 0, 0, 0);
                    const unsigned sv = (unsigned)(kind + si + ri + v) % 3u;
                    if (sv == 0) run(tag, n, SPL_A, 1, v ? hook_cap2 : NULL);
                    else if (sv == 1) run(tag, n, SPL_B, 6, v ? hook_cap2 : NULL);
                    else run(tag, n, SPL_C, 4, v ? hook_cap2 : NULL);
                }
    /* бюджет: 250 кимп/с при штатном бюджете и при 20 подгонках на порцию */
    for (unsigned v = 0; v < 2; v++) {
        size_t n = make_stream(K_CS, 250000.0, 1.8f, NBLK, seed++);
        both_init(DET_THR_DEF, v ? 20 : 0, 0, 0, 0);
        run(v ? "бюджет 20" : "бюджет 250 кимп/с", n, v ? SPL_C : SPL_A, v ? 4 : 1, hook_cap2);
    }
    /* выбросы помехи 1–2 отсчёта, ступенька базы вверх/вниз, перегрузка шумом и уровнем */
    for (unsigned v = 0; v < 3; v++) {
        size_t n = make_stream(K_CS, 30000.0, 1.8f, NBLK, seed++);
        memset(fbuf, 0, n * sizeof fbuf[0]);
        int k = 0;
        for (double t = 2.0 * BLK; t < n - 4; t += 26.7, k++)
            pg_add_spike(fbuf, n, (size_t)t, 1 + (k & 1), 12.0f + 7.0f * (float)(k % 5));
        for (size_t q = 10 * BLK; q < 14 * BLK; q++) fbuf[q] += (v == 0) ? 50.0f : (v == 1 ? -15.0f : 15.0f);
        for (size_t q = 20 * BLK; q < 22 * BLK; q++) fbuf[q] += 3000.0f;             /* в потолок АЦП */
        pg_add_noise(&pg, fbuf + 25 * BLK, 3 * BLK, 8.0f);                           /* шум много больше порога */
        for (size_t q = 0; q < n; q++) {
            long w = lround(645.0 + fbuf[q]) + (long)sbuf[q] - 645;
            sbuf[q] = (uint16_t)(w < 0 ? 0 : (w > 4095 ? 4095 : w));
        }
        both_init(DET_THR_DEF, 0, 0, 0, 0);
        run(v == 0 ? "выбросы + ступенька +50" : v == 1 ? "выбросы + ступенька −15" : "выбросы + ступенька +15",
            n, v == 2 ? SPL_B : SPL_A, v == 2 ? 6 : 1, hook_cap2);
    }
    /* авто-порог с самого начала при σ 1.8, 4, 6 и импульсах */
    for (unsigned v = 0; v < 3; v++) {
        size_t n = make_stream(K_CS, 30000.0, v == 0 ? 1.8f : (v == 1 ? 4.0f : 6.0f), NBLK, seed++);
        both_init(0, 0, 0, 0, 0);
        run("авто-порог", n, SPL_A, 1, NULL);
    }
    /* порог, ширина, усиление, эталон, запись окон, потери — на лету */
    for (unsigned p = 0; p < DET_NPH; p++)
        for (unsigned q = 0; q < DET_NS; q++) tpl_half[p][q] = (int16_t)(tpl_3467k[p][q] / 2);
    {
        size_t n = make_stream(K_CO, 60000.0, 1.5f, NBLK, seed++);
        both_init(DET_THR_DEF, 0, 0, 0, 0);
        run("на лету", n, SPL_B, 6, hook_otf);
    }
    /* мелкие порции: стыки на каждом отсчёте импульса */
    {
        size_t n = make_stream(K_CS, 30000.0, 1.8f, 8, seed++);
        both_init(DET_THR_DEF, 0, 0, 0, 0);
        run("мелкие порции", n, SPL_D, 5, hook_cap2);
    }
    /* самотест ЦАП: tpl_dac, порог 30, ширина до 64, база заморожена */
    for (unsigned v = 0; v < 2; v++) {
        size_t n = make_stream(K_DAC, 20000.0, 1.5f, NBLK, seed++);
        both_init(30, 0, 1, 2, 64);
        run("самотест ЦАП", n, v ? SPL_C : SPL_A, v ? 4 : 1, hook_dac);
    }
    printf("  сверка с прежним детектором: порций %ld, импульсов в потоках %ld, выдано каналов %ld "
           "(каждым), расхождений %ld\n", tot_blocks, tot_pulses, tot_emit, tot_fails);
    CHECK(tot_fails == 0);
    CHECK(tot_emit >= 100000);
}

int main(void)
{
    test_equiv();
    return T_DONE();
}
