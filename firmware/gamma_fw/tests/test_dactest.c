/* test_dactest.c — тестовые импульсы ЦАП: машина таймера и команда -dac (core/dactest.c). */
#include <string.h>
#include <stdint.h>
#include "t.h"
#include "dactest.h"

#define CLK 156000000u

/* прогнать серию до конца: время ступенек вверх (такты от старта), проверки спуска */
typedef struct {
    uint32_t ups, downs, max_ticks, min_ticks, bad_code, hold_bad, dn_bad, max_dstep, nonmono;
    uint64_t t_up[70000];
} run_t;
static run_t R;

static void run(dact_t *d)
{
    memset(&R, 0, sizeof R);
    R.min_ticks = 1u << 30;
    uint64_t t = d->settle;                    /* таймер запущен с первым интервалом settle */
    uint16_t code, prev = DACT_BASE; uint32_t ticks;
    uint64_t last_up = 0, last_dn = 0;
    unsigned kdn = 0;
    int guard = 0;
    while (dact_tick(d, &code, &ticks)) {
        if (code == DACT_BASE + d->step) {
            if (prev != DACT_BASE) R.bad_code++;            /* подъём — только с базы */
            R.t_up[R.ups++] = t; last_up = t; kdn = 0;
        } else if (code < DACT_BASE || code > DACT_BASE + d->step) {
            R.bad_code++;
        } else if (code == DACT_BASE && prev == DACT_BASE) {
            /* продолжение длинной паузы (кусками по 65536) */
        } else {                                            /* ступенька спуска */
            if (code >= prev) R.nonmono++;
            if ((uint32_t)(prev - code) > R.max_dstep) R.max_dstep = prev - code;
            if (kdn == 0 && t - last_up != d->hold) R.hold_bad++;
            if (kdn > 0 && t - last_dn != d->dn) R.dn_bad++;
            last_dn = t;
            kdn++;
            if (code == DACT_BASE) { R.downs++; if (kdn != d->ndn) R.bad_code++; }
        }
        prev = code;
        if (ticks > R.max_ticks) R.max_ticks = ticks;
        if (ticks < R.min_ticks) R.min_ticks = ticks;
        t += ticks;
        if (++guard > 2000000) break;
    }
}

static void test_start_limits(void)
{
    dact_t d = { 0 };
    CHECK(!dact_start(&d, 0, 10, 100, CLK, 45));
    CHECK(!dact_start(&d, DACT_STEP_MAX + 1u, 10, 100, CLK, 45));
    CHECK(!dact_start(&d, 200, 0, 100, CLK, 45));
    CHECK(!dact_start(&d, 200, 10, DACT_PER_MIN - 1u, CLK, 45));
    CHECK(!dact_start(&d, 200, 10, DACT_PER_MAX + 1u, CLK, 45));
    CHECK(!dact_start(&d, 200, 10, 100, CLK, 0));
    /* наименьший период растёт со ступенькой: спуск по DACT_DN_MAX */
    CHECK(dact_ndn(300) == 1 && dact_ndn(301) == 2 && dact_ndn(DACT_STEP_MAX) == 13);
    CHECK(dact_per_min(200) == DACT_PER_MIN);
    CHECK(dact_per_min(DACT_STEP_MAX) == DACT_HOLD_US + 12u * DACT_DN_US + DACT_REST_MIN_US);
    CHECK(!dact_start(&d, DACT_STEP_MAX, 10, (uint16_t)(dact_per_min(DACT_STEP_MAX) - 1u), CLK, 45));
    CHECK(!dact_busy(&d));
    CHECK(dact_start(&d, DACT_STEP_MAX, 10, DACT_PER_MAX, CLK, 57));   /* край: 16 бит хватает */
    CHECK(d.settle <= 65536u && d.hold <= 65536u);
    CHECK(dact_busy(&d));
    CHECK(!dact_start(&d, 200, 10, 100, CLK, 45));                    /* занято */
    /* длинная пауза кусками: период 800 мкс = 124 800 тактов */
    dact_t e = { 0 };
    CHECK(dact_start(&e, DACT_STEP_MAX, 50, DACT_PER_MAX, CLK, 57));
    run(&e);
    CHECK(R.ups == 50 && R.downs == 50 && R.bad_code == 0 && R.max_ticks <= 65536u);
    CHECK_NEAR((double)(R.t_up[49] - R.t_up[0]) / 49.0, 124800.0 + 28.0, 8.0);
}

static void test_series(void)
{
    dact_t d = { 0 };
    CHECK(dact_start(&d, 200, 1000, 100, CLK, 45));
    CHECK(d.hold == 3900u && d.ndn == 1 && d.rest == 15600u - 3900u);
    run(&d);
    CHECK(R.ups == 1000 && R.downs == 1000 && d.done == 1000);
    CHECK(R.bad_code == 0 && R.hold_bad == 0 && R.nonmono == 0);
    CHECK(R.max_ticks <= 65536u);
    CHECK(!dact_busy(&d));
    uint16_t code; uint32_t ticks;
    CHECK(!dact_tick(&d, &code, &ticks) && code == DACT_BASE);        /* после конца — только «выкл» */
    /* период ≈ 100 мкс в среднем (добавка в среднем 22 такта) */
    double per = (double)(R.t_up[999] - R.t_up[0]) / 999.0;
    CHECK_NEAR(per, 15600.0 + 22.0, 3.0);

    /* большая ступенька: спуск лесенкой, ни одной ступеньки больше DACT_DN_MAX */
    dact_t e = { 0 };
    CHECK(dact_start(&e, 2700, 500, 100, CLK, 45));
    CHECK(e.ndn == 9 && e.dn == 468u);
    run(&e);
    CHECK(R.ups == 500 && R.downs == 500 && e.done == 500);
    CHECK(R.bad_code == 0 && R.hold_bad == 0 && R.dn_bad == 0 && R.nonmono == 0);
    CHECK(R.max_dstep <= DACT_DN_MAX && R.max_dstep >= 2700u / 9u);
    CHECK(R.min_ticks == 468u);
    per = (double)(R.t_up[499] - R.t_up[0]) / 499.0;
    CHECK_NEAR(per, 15600.0 + 22.0, 4.0);
}

/* фаза ступеньки относительно отсчётов АЦП (45 тактов) — равномерна; без добавки — 3 значения */
static void test_phase_coverage(void)
{
    dact_t d = { 0 };
    unsigned hist[45] = { 0 };
    CHECK(dact_start(&d, 300, 9000, 100, CLK, 45));
    run(&d);
    for (uint32_t i = 0; i < R.ups; i++) hist[R.t_up[i] % 45u]++;
    unsigned mn = 1u << 30, mx = 0;
    for (int i = 0; i < 45; i++) { if (hist[i] < mn) mn = hist[i]; if (hist[i] > mx) mx = hist[i]; }
    CHECK(mn >= 140 && mx <= 260);                                     /* 200 в среднем */

    dact_t z = { 0 };
    unsigned h2[45] = { 0 }, distinct = 0;
    CHECK(dact_start(&z, 300, 900, 100, CLK, 1));
    run(&z);
    for (uint32_t i = 0; i < R.ups; i++) h2[R.t_up[i] % 45u]++;
    for (int i = 0; i < 45; i++) distinct += h2[i] != 0;
    CHECK(distinct <= 3);
}

static void test_abort(void)
{
    dact_t d = { 0 };
    uint16_t code; uint32_t ticks;
    CHECK(dact_start(&d, 200, 100, 100, CLK, 45));
    CHECK(dact_tick(&d, &code, &ticks));
    dact_abort(&d);
    CHECK(!dact_busy(&d));
    CHECK(!dact_tick(&d, &code, &ticks));
    CHECK(dact_start(&d, 200, 100, 100, CLK, 45));                   /* снова можно */
}

/* ---- команда ---- */
static int st_calls, st_busy, st_fail, stop_calls;
static uint16_t st_step, st_n, st_per, st_done = 7;
static bool f_start(uint16_t s, uint16_t n, uint16_t p)
{
    st_calls++; st_step = s; st_n = n; st_per = p;
    if (st_fail) return false;
    st_busy = 1;
    return true;
}
static void f_stop(void) { stop_calls++; st_busy = 0; }
static bool f_busy(void) { return st_busy != 0; }
static uint16_t f_done(void) { return st_done; }
static uint16_t f_total(void) { return st_n; }
static const dact_ctx_t ctx = { f_start, f_stop, f_busy, f_done, f_total };

static void test_cmd(void)
{
    char r[96];
    CHECK(!dact_handle(&ctx, "-dacx 1 2", r, sizeof r));
    CHECK(!dact_handle(&ctx, "-gst", r, sizeof r));
    CHECK(dact_handle(&ctx, "-dac", r, sizeof r) && strcmp(r, "-ok dac idle") == 0);
    CHECK(dact_handle(&ctx, "-dac 200 10000", r, sizeof r));
    CHECK(strcmp(r, "-ok dac step=200 n=10000 period=100") == 0);
    CHECK(st_calls == 1 && st_step == 200 && st_n == 10000 && st_per == DACT_PER_DEF);
    CHECK(dact_handle(&ctx, "-dac", r, sizeof r) && strcmp(r, "-ok dac busy done=7 n=10000") == 0);
    CHECK(dact_handle(&ctx, "-dac 100 5", r, sizeof r) && strcmp(r, "-err dac busy") == 0);
    CHECK(st_calls == 1);
    CHECK(dact_handle(&ctx, "-dac off", r, sizeof r) && strcmp(r, "-ok dac off done=7") == 0);
    CHECK(stop_calls == 1 && !st_busy);
    CHECK(dact_handle(&ctx, "-dac  1600 \t 500  250 ", r, sizeof r));
    CHECK(strcmp(r, "-ok dac step=1600 n=500 period=250") == 0 && st_per == 250);
    st_busy = 0;
    const char *bad[] = { "-dac 0 10", "-dac 3696 10", "-dac 200", "-dac 200 0", "-dac 200 65536",
                          "-dac 200 10 44", "-dac 200 10 801", "-dac 200 10 100 5", "-dac a b",
                          "-dac -5 10", "-dac 200 1e3", "-dac offx" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(dact_handle(&ctx, bad[i], r, sizeof r));
        CHECK(strncmp(r, "-err dac <1..3695>", 18) == 0);
    }
    CHECK(st_calls == 2);
    CHECK(dact_handle(&ctx, "-dac 3000 10 50", r, sizeof r) && strcmp(r, "-err dac period>=72") == 0);
    CHECK(st_calls == 2);
    st_fail = 1;
    CHECK(dact_handle(&ctx, "-dac 200 10", r, sizeof r) && strcmp(r, "-err dac") == 0);
    st_fail = 0;
}

int main(void)
{
    test_start_limits();
    test_series();
    test_phase_coverage();
    test_abort();
    test_cmd();
    return T_DONE();
}
