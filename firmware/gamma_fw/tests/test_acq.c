/* Тесты acq.c: учёт времени набора, мёртвое/живое время, скорости, загрузка ЦП, -noise, команды
   -smp/-noise/-osc. */
#include <string.h>
#include <stdlib.h>
#include "t.h"
#include "acq.h"

#define FS 3466667u

static bool starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static void test_time_and_dead(void)
{
    acq_t a;
    det_stats_t st;
    memset(&st, 0, sizeof st);
    st.dead_samples = 1000000u;              /* до набора: не в счёт */
    st.rej_shape = 50u;
    acq_init(&a, FS, &st);

    st.dead_samples += FS;                   /* 1 с мёртвого вне набора — не в счёт */
    acq_start(&a, 1000u, &st);
    CHECK(a.run);
    CHECK(acq_real_ms(&a, 1000u) == 0u);
    st.dead_samples += FS / 10u;             /* 0.1 с мёртвого в наборе */
    st.rej_pileup += 7u;
    CHECK_NEAR(acq_dead_s(&a, &st), 0.1, 1e-4);
    CHECK_NEAR(acq_live_s(&a, 3000u, &st), 1.9, 1e-3);
    CHECK(acq_invalid(&a, &st) == 7u);
    acq_tick_1s(&a, &st, 0u, 0u);            /* учёт в накопленное */
    CHECK_NEAR(acq_dead_s(&a, &st), 0.1, 1e-4);
    CHECK(acq_invalid(&a, &st) == 7u);

    acq_stop(&a, 3000u, &st);
    CHECK(!a.run);
    CHECK(acq_real_ms(&a, 99999u) == 2000u); /* остановлено — время стоит */
    st.dead_samples += FS;                   /* вне набора — не в счёт */
    st.rej_width += 100u;
    CHECK_NEAR(acq_dead_s(&a, &st), 0.1, 1e-4);
    CHECK(acq_invalid(&a, &st) == 7u);

    acq_start(&a, 10000u, &st);              /* продолжение: время копится */
    acq_start(&a, 10500u, &st);              /* повторный -sta не сбивает отрезок */
    CHECK(acq_real_ms(&a, 11000u) == 3000u);
    st.dead_samples += FS / 2u;
    st.rej_budget += 3u;
    CHECK_NEAR(acq_live_s(&a, 11000u, &st), 3.0 - 0.6, 1e-3);
    CHECK(acq_invalid(&a, &st) == 10u);

    acq_clear(&a, 11000u, &st);              /* -rst при наборе: с нуля, набор идёт */
    CHECK(a.run);
    CHECK(acq_real_ms(&a, 12000u) == 1000u);
    CHECK_NEAR(acq_dead_s(&a, &st), 0.0, 1e-6);
    CHECK(acq_invalid(&a, &st) == 0u);

    /* живое не уходит в минус (перегрузка: мёртвого больше реального по округлению) */
    st.dead_samples += 10u * FS;
    CHECK(acq_live_s(&a, 12000u, &st) == 0.0f);
}

static void test_rates_cpu(void)
{
    acq_t a;
    det_stats_t st;
    memset(&st, 0, sizeof st);
    acq_init(&a, FS, &st);
    st.accepted = 1200u;
    st.rej_pileup = 30u; st.rej_shape = 10u; st.rej_width = 5u; st.rej_budget = 5u;
    acq_tick_1s(&a, &st, 15600000u, 156000000u);   /* 10 % */
    CHECK(a.cps_acc == 1200u);
    CHECK(a.cps_rej == 50u);
    CHECK(a.cps_in == 1250u);
    CHECK(a.cpu_x10 == 100u);
    acq_tick_1s(&a, &st, 0u, 156000000u);          /* за секунду ничего */
    CHECK(a.cps_in == 0u && a.cps_acc == 0u);
    CHECK(a.cpu_x10 == 0u);
    acq_tick_1s(&a, &st, 200000000u, 156000000u);  /* больше окна — потолок 100 % */
    CHECK(a.cpu_x10 == 1000u);
    acq_tick_1s(&a, &st, 1000u, 0u);               /* окно 0 — без деления на ноль */
    CHECK(a.cpu_x10 == 0u);
    /* переполнение счётчика принятых (uint32) — разность по модулю */
    st.accepted = 0xFFFFFFF0u;
    acq_tick_1s(&a, &st, 0u, 1u);
    st.accepted = 0x10u;
    acq_tick_1s(&a, &st, 0u, 1u);
    CHECK(a.cps_acc == 0x20u);
}

static void test_fs_change(void)
{
    acq_t a;
    det_stats_t st;
    memset(&st, 0, sizeof st);
    acq_init(&a, FS, &st);
    acq_set_fs(&a, 2736842u);                      /* -smp 6 */
    acq_start(&a, 0u, &st);
    st.dead_samples += 2736842u;
    CHECK_NEAR(acq_dead_s(&a, &st), 1.0, 1e-4);
    acq_set_fs(&a, 0u);                            /* 0 — игнор */
    CHECK(a.fs_hz == 2736842u);
}

static void test_noise(void)
{
    static uint16_t s[8192];
    acq_noise_t o;
    for (int i = 0; i < 8192; i++) s[i] = (uint16_t)(645 + ((i & 1) ? 1 : -1));   /* σ = 1 */
    acq_noise(s, 8192, &o);
    CHECK(o.n == 8192u);
    CHECK_NEAR(o.mean, 645.0, 1e-4);
    CHECK_NEAR(o.sigma, 1.0, 1e-4);
    CHECK(o.min == 644u && o.max == 646u);
    CHECK(o.over == 0u);
    s[100] = 700; s[101] = 700;                    /* выброс помехи 2 отсчёта */
    acq_noise(s, 8192, &o);
    CHECK(o.over == 2u);
    CHECK(o.max == 700u);
    CHECK(o.n == 8192u - (2u + ACQ_NOISE_PRE + ACQ_NOISE_POST));  /* выброс с окрестностью — вне тихих */
    CHECK_NEAR(o.sigma, 1.0, 1e-3);
    /* импульсы (σ всей порции ≫ 1): mean и σ — по базе, не по импульсам */
    for (int i = 0; i < 8192; i++) s[i] = (uint16_t)(645 + ((i & 1) ? 1 : -1));
    for (int p = 0; p < 20; p++)
        for (int k = 0; k < 30; k++) s[200 + p * 400 + k] = (uint16_t)(645 + 600 - 20 * k);
    acq_noise(s, 8192, &o);
    CHECK_NEAR(o.mean, 645.0, 0.01);
    CHECK_NEAR(o.sigma, 1.0, 0.01);
    CHECK(o.n < 8192u && o.n > 6000u);
    CHECK(o.over > 20u);
    CHECK(o.max == 1245u);
    /* выброс у самого начала и конца порции */
    for (int i = 0; i < 8192; i++) s[i] = (uint16_t)(645 + ((i & 1) ? 1 : -1));
    s[0] = 800; s[8191] = 800;
    acq_noise(s, 8192, &o);
    CHECK(o.n == 8192u - (1u + ACQ_NOISE_POST) - (1u + ACQ_NOISE_PRE));
    CHECK(o.over == 2u);
    for (int i = 0; i < 8192; i++) s[i] = 4095;    /* потолок, σ = 0 */
    acq_noise(s, 8192, &o);
    CHECK_NEAR(o.sigma, 0.0, 1e-6);
    CHECK(o.over == 0u);
    acq_noise(NULL, 0, &o);
    CHECK(o.n == 0u && isnan(o.mean));
}

static void test_osc_line(void)
{
    det_cap_t c = { 123456u, 1160u, 17u, 2, 580.25f, 645.5f };
    char b[128];
    acq_osc_line(b, sizeof b, 3u, &c);
    CHECK(strcmp(b, "osc i=3 pos=123456 ch=1160 amp=580.25 ph=17 pk=2 base=645.50") == 0);
}

/* ---- команды ---- */
static bool st_acq, st_ready = true, st_set_ok = true, st_noise_ok = true;
static uint8_t st_smp = 2;
static uint16_t st_armed;
static int st_raw_calls;
static bool c_acq(void) { return st_acq; }
static bool c_ready(void) { return st_ready; }
static uint8_t c_smp_get(void) { return st_smp; }
static bool c_smp_set(uint8_t v) { if (!st_set_ok) return false; st_smp = v; return true; }
static uint32_t c_fs(void) { return st_smp == 6 ? 2736842u : FS; }
static bool c_noise(acq_noise_t *o)
{
    if (!st_noise_ok) return false;
    memset(o, 0, sizeof *o);
    o->n = 8192; o->mean = 645.25f; o->sigma = 1.234f; o->min = 640; o->max = 652; o->over = 1;
    return true;
}
static bool c_raw(uint32_t *n) { st_raw_calls++; *n = 8192; return true; }
static bool c_arm(uint16_t n) { st_armed = n; return true; }
static int st_hs; static unsigned st_pga = 4;
static void c_opa_get(int *hs, unsigned *pga) { *hs = st_hs; *pga = st_pga; }
static void c_opa_set(int hs, unsigned pga) { st_hs = hs; st_pga = pga; }
static const acq_ctx_t ctx = { c_acq, c_ready, c_smp_get, c_smp_set, c_fs, c_noise, c_raw, c_arm, 64,
                               c_opa_get, c_opa_set };

static char rep[256];
static const char *cmd(const char *c)
{
    rep[0] = 0;
    if (!acq_handle(&ctx, c, rep, sizeof rep)) strcpy(rep, "<not acq>");
    return rep;
}

static void test_cmds(void)
{
    CHECK(strcmp(cmd("-smp"), "-ok smp=2.5 fs=3466667") == 0);
    CHECK(strcmp(cmd("-smp 6"), "-ok smp=6.5 fs=2736842") == 0);
    CHECK(st_smp == 6);
    CHECK(strcmp(cmd("-smp 2"), "-ok smp=2.5 fs=3466667") == 0);
    CHECK(strcmp(cmd("-smp 4"), "-err smp") == 0);
    CHECK(strcmp(cmd("-smp 2x"), "-err smp") == 0);
    CHECK(strcmp(cmd("-smp abc"), "-err smp") == 0);
    st_acq = true;
    CHECK(strcmp(cmd("-smp 6"), "-err acq") == 0);   /* при наборе — нельзя */
    CHECK(st_smp == 2);
    CHECK(starts(cmd("-smp"), "-ok smp=2.5"));      /* запрос — можно */
    st_acq = false;
    st_set_ok = false;
    CHECK(strcmp(cmd("-smp 6"), "-err adc") == 0);
    st_set_ok = true;
    st_ready = false;
    CHECK(strcmp(cmd("-smp 6"), "-err notready") == 0);
    CHECK(strcmp(cmd("-noise"), "-err notready") == 0);
    CHECK(strcmp(cmd("-osc 5"), "-err notready") == 0);
    st_ready = true;

    CHECK(strcmp(cmd("-noise"),
                 "-ok noise smp=2.5 fs=3466667 n=8192 mean=645.25 sigma=1.234 min=640 max=652 over=1") == 0);
    CHECK(strcmp(cmd("-noise 3"), "-err noise") == 0);
    st_noise_ok = false;
    CHECK(strcmp(cmd("-noise"), "-err notready") == 0);
    st_noise_ok = true;
    st_acq = true;
    CHECK(starts(cmd("-noise"), "-ok noise"));      /* шум можно и при наборе */
    st_acq = false;

    CHECK(strcmp(cmd("-osc 8"), "-ok osc 8") == 0);
    CHECK(st_armed == 8);
    CHECK(strcmp(cmd("-osc"), "-ok osc 64") == 0);  /* без числа — сколько вмещает */
    CHECK(st_armed == 64);
    CHECK(starts(cmd("-osc 65"), "-err osc"));
    CHECK(starts(cmd("-osc 0"), "-err osc"));
    CHECK(starts(cmd("-osc -1"), "-err osc"));
    CHECK(starts(cmd("-osc 3z"), "-err osc"));
    CHECK(strcmp(cmd("-osc raw"), "-ok osc raw 8192") == 0);
    CHECK(st_raw_calls == 1);
    st_acq = true;
    st_armed = 0;
    CHECK(strcmp(cmd("-osc 4"), "-err acq") == 0);
    CHECK(st_armed == 0);
    CHECK(strcmp(cmd("-osc raw"), "-err acq") == 0);
    CHECK(st_raw_calls == 1);
    st_acq = false;

    CHECK(strcmp(cmd("-opa"), "-ok opa hs=0 pga=4") == 0);
    CHECK(strcmp(cmd("-opa hs 1"), "-ok opa hs=1 pga=4") == 0);
    CHECK(strcmp(cmd("-opa pga 16"), "-ok opa hs=1 pga=16") == 0);
    CHECK(strcmp(cmd("-opa pga 5"), "-err opa") == 0);
    CHECK(strcmp(cmd("-opa hs 2"), "-err opa") == 0);
    CHECK(strcmp(cmd("-opa foo 1"), "-err opa") == 0);
    CHECK(strcmp(cmd("-opa hs"), "-err opa") == 0);
    CHECK(strcmp(cmd("-opa hs 0x"), "-err opa") == 0);
    st_acq = true;
    CHECK(strcmp(cmd("-opa hs 0"), "-err acq") == 0);
    CHECK(st_hs == 1);
    CHECK(strcmp(cmd("-opa"), "-ok opa hs=1 pga=16") == 0);
    st_acq = false;
    CHECK(strcmp(cmd("-opa pga 4"), "-ok opa hs=1 pga=4") == 0);
    CHECK(strcmp(cmd("-opa hs 0"), "-ok opa hs=0 pga=4") == 0);

    CHECK(strcmp(cmd("-oscx"), "<not acq>") == 0);   /* чужие команды — не наши */
    CHECK(strcmp(cmd("-gst"), "<not acq>") == 0);
    CHECK(strcmp(cmd("-smpl 2"), "<not acq>") == 0);
}

/* время набора дольше 49.7 сут (uint32 мс): с acq_fold_time раз в секунду (здесь — раз в сутки) не переполняется */
static void test_long_time(void)
{
    det_stats_t st;
    memset(&st, 0, sizeof st);
    acq_t a;
    acq_init(&a, 3466667u, &st);
    uint32_t now = 123u;
    acq_start(&a, now, &st);
    for (int d = 0; d < 60; d++) { now += 86400000u; acq_fold_time(&a, now); }   /* millis заворачивается */
    CHECK(acq_real_ms(&a, now) == 60ull * 86400000ull);
    CHECK(acq_real_ms(&a, now + 500u) == 60ull * 86400000ull + 500u);
    acq_stop(&a, now + 1000u, &st);
    CHECK(acq_real_ms(&a, now + 99999u) == 60ull * 86400000ull + 1000u);
}

int main(void)
{
    test_long_time();
    test_time_and_dead();
    test_rates_cpu();
    test_fs_change();
    test_noise();
    test_osc_line();
    test_cmds();
    return T_DONE();
}
