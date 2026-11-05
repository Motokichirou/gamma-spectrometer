/* Тесты svc.c: служебные замеры (VREF+, +3.3V_D, Pt1000, монитор) и сервисные команды. */
#include <string.h>
#include <stdlib.h>
#include "t.h"
#include "svc.h"
#include "temp.h"

#define CAL 1655.0f                                   /* типичная VREFINT_CAL (3.0 В) */
static float raw_for_vref(float vref) { return 3000.0f * CAL / vref; }

static hv_ctl_t   hv;
static settings_t live;
static svc_aux_t  aux;
static uint32_t   now_ms;
static svc_diag_t diag_stub;
static bool       diag_ok;
static svc_gst_t  gst_stub;

static uint32_t p_millis(void) { return now_ms; }
static bool p_diag(svc_diag_t *d) { if (!diag_ok) return false; *d = diag_stub; return true; }
static void p_gst(svc_gst_t *g) { *g = gst_stub; }
static const svc_ctx_t ctx = { &hv, &live, &aux, p_millis, p_gst, p_diag };

static char rep[512];
static const char *svc(const char *cmd)
{
    rep[0] = 0;
    if (!svc_handle(cmd, rep, sizeof rep)) strcpy(rep, "<not svc>");
    return rep;
}
static bool starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

/* значение ключа из строки -gst в buf; false — нет ключа */
static bool kv(const char *line, const char *key, char *buf, size_t cap)
{
    size_t n = strlen(key);
    for (const char *p = line; (p = strstr(p, key)) != NULL; p += n) {
        if ((p == line || p[-1] == ' ') && p[n] == '=') {
            const char *v = p + n + 1;
            size_t k = strcspn(v, " ");
            if (k >= cap) k = cap - 1;
            memcpy(buf, v, k); buf[k] = 0;
            return true;
        }
    }
    return false;
}

/* код монитора для |V| выхода при опоре vref (как его увидит АЦП) */
static float mon_raw_for(float v_out, float zero, float k, float vref) { return (zero - k * v_out) * 4095.0f / vref; }

static void reset(void)
{
    settings_defaults(&live);
    hv_init(&hv, live.hv_mon_zero_mv, live.hv_mon_k_mv_per_v);
    svc_aux_init(&aux, CAL);
    svc_init(&ctx);
    now_ms = 0;
    diag_ok = true;
    memset(&gst_stub, 0, sizeof gst_stub);
    gst_stub.flash = "none";
    for (int i = 0; i < SVC_DIAG_N; i++) diag_stub.mv[i] = svc_nodes[i].nom_mv;
}

/* служебные замеры «в норме»: n секунд по 100 тиков 10 мс; монитор — ВВ 0 */
static void run_aux(int secs, float vref_true, float vdd, float pt_raw)
{
    for (int s = 0; s < secs; s++) {
        for (int t = 0; t < 100; t++) {
            now_ms += 10;
            float m = svc_aux_10ms(&aux, mon_raw_for(0.0f, 2460.0f, 1.64f, vref_true), true, hv_enabled(&hv), now_ms);
            hv_tick(&hv, m, aux.vdd_mv, now_ms);
        }
        svc_aux_1s(&aux, &hv, raw_for_vref(vref_true), vdd / 3.0f * 4095.0f / vref_true, pt_raw);
    }
}

static void test_vref_math(void)
{
    CHECK_NEAR(svc_vref_from_vrefint(raw_for_vref(2500.0f), CAL), 2500.0, 0.01);
    CHECK_NEAR(svc_vref_from_vrefint(raw_for_vref(2508.0f), CAL), 2508.0, 0.01);
    CHECK(isnan(svc_vref_from_vrefint(0.0f, CAL)));
    CHECK(isnan(svc_vref_from_vrefint(NAN, CAL)));
    svc_aux_t a;
    svc_aux_init(&a, CAL);
    CHECK_NEAR(svc_vref_mv(&a), 2500.0, 0.0);          /* до первого замера — номинал */
    CHECK(!svc_hv_allowed(&a));
}

static void test_vref_norm_and_recal(void)
{
    reset();
    /* холодный старт: опора не устоялась (2593 «по VREFINT») — перекалибровка, но не больше 10 подряд */
    int n = 0;
    for (int i = 0; i < 15; i++) n += svc_aux_1s(&aux, &hv, raw_for_vref(2593.0f), NAN, NAN);
    CHECK(n == (int)SVC_RECAL_MAX);
    CHECK(aux.ready && !aux.vref_ok && !svc_hv_allowed(&aux));
    CHECK_NEAR(svc_vref_mv(&aux), 2500.0, 0.0);        /* негодные в пересчёт не идут */
    /* в норме — сглаженная опора, счётчик перекалибровок сброшен */
    CHECK(!svc_aux_1s(&aux, &hv, raw_for_vref(2507.0f), NAN, NAN));
    CHECK(aux.vref_ok);
    CHECK_NEAR(svc_vref_mv(&aux), 2507.0, 0.01);
    for (int i = 0; i < 10; i++) svc_aux_1s(&aux, &hv, raw_for_vref(2509.0f), NAN, NAN);
    CHECK_NEAR(svc_vref_mv(&aux), 2509.0 - 2.0 * pow(0.9, 10), 0.01);   /* τ 10 с */
    CHECK(svc_aux_1s(&aux, &hv, raw_for_vref(2480.0f), NAN, NAN));      /* снова вне — перекалибровать */
    /* замер не пришёл — не в норме, но и не перекалибровка */
    CHECK(!svc_aux_1s(&aux, &hv, NAN, NAN, NAN));
    CHECK(!aux.vref_ok);
}

static void test_vref_trip_with_hv(void)
{
    reset();
    run_aux(3, 2507.0f, 3296.0f, 1300.0f);
    CHECK(svc_hv_allowed(&aux));
    CHECK(strcmp(svc("-hv 500"), "-ok hv 500") == 0);
    CHECK(hv_enabled(&hv));
    /* 2 замера вне нормы и снова норма — ВВ держится, перекалибровки при ВВ нет */
    CHECK(!svc_aux_1s(&aux, &hv, raw_for_vref(2400.0f), NAN, NAN));
    CHECK(!svc_aux_1s(&aux, &hv, raw_for_vref(2400.0f), NAN, NAN));
    svc_aux_1s(&aux, &hv, raw_for_vref(2507.0f), NAN, NAN);
    CHECK(hv_enabled(&hv));
    /* 3 подряд — выкл */
    for (int i = 0; i < 3; i++) CHECK(!svc_aux_1s(&aux, &hv, raw_for_vref(2400.0f), NAN, NAN));
    CHECK(!hv_enabled(&hv));
    CHECK(hv_off_reason(&hv) && strstr(hv_off_reason(&hv), "VREF"));
    CHECK(strcmp(svc("-hv 500"), "-err vref") == 0);   /* и не включить, пока опора вне нормы */
}

static void test_vdd_pt_filters(void)
{
    reset();
    float vr = 2500.0f;
    svc_aux_1s(&aux, &hv, raw_for_vref(vr), 3296.0f / 3.0f * 4095.0f / vr, 1300.0f);
    CHECK_NEAR(aux.vdd_mv, 3296.0, 0.1);               /* первый замер — сразу */
    for (int i = 0; i < 60; i++) svc_aux_1s(&aux, &hv, raw_for_vref(vr), 3306.0f / 3.0f * 4095.0f / vr, 1300.0f);
    CHECK_NEAR(aux.vdd_mv, 3296.0 + 10.0 * (1.0 - pow(1.0 - 1.0 / 60.0, 60)), 0.05);   /* τ 60 с */
    /* шум ±4 мВ (0.12 %) τ 60 с гасит до долей мВ — без ступенек ВВ (SPEC §5) */
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 600; i++) {
        float v = 3306.0f + ((i & 1) ? 4.0f : -4.0f);
        svc_aux_1s(&aux, &hv, raw_for_vref(vr), v / 3.0f * 4095.0f / vr, 1300.0f);
        if (i > 300) { if (aux.vdd_mv < lo) lo = aux.vdd_mv; if (aux.vdd_mv > hi) hi = aux.vdd_mv; }
    }
    CHECK(hi - lo < 0.3f);
    /* Pt: τ 10 с; смена состояния (обрыв) — фильтр заново */
    CHECK_NEAR(aux.pt_raw, 1300.0, 0.01);
    for (int i = 0; i < 10; i++) svc_aux_1s(&aux, &hv, raw_for_vref(vr), NAN, 1310.0f);
    CHECK_NEAR(aux.pt_raw, 1310.0 - 10.0 * pow(0.9, 10), 0.01);
    svc_aux_1s(&aux, &hv, raw_for_vref(vr), NAN, 4095.0f);
    CHECK_NEAR(aux.pt_raw, 4095.0, 0.0);
    CHECK(pt_state(aux.pt_raw) == PT_OPEN);
    svc_aux_1s(&aux, &hv, raw_for_vref(vr), NAN, 1300.0f);
    CHECK_NEAR(aux.pt_raw, 1300.0, 0.0);
}

static void test_mon_stale(void)
{
    reset();
    now_ms = 1000;
    CHECK(isnan(svc_aux_10ms(&aux, 4000.0f, false, false, now_ms)));   /* замеров ещё не было */
    float m = svc_aux_10ms(&aux, 4029.48f, true, false, now_ms);
    CHECK_NEAR(m, 4029.48 * 2500.0 / 4095.0, 0.01);
    now_ms += 50;
    CHECK(!isnan(svc_aux_10ms(&aux, 0.0f, false, false, now_ms)));     /* 50 мс — ещё свежий */
    now_ms += 10;
    CHECK(isnan(svc_aux_10ms(&aux, 0.0f, false, false, now_ms)));      /* застрял — NaN для hv_tick */
    /* сглаженный τ 1 с идёт к новому значению */
    for (int i = 0; i < 1000; i++) { now_ms += 10; svc_aux_10ms(&aux, 3000.0f, true, false, now_ms); }
    CHECK_NEAR(aux.mon_mv_f, 3000.0 * 2500.0 / 4095.0, 0.2);
}

static void test_stale_monitor_trips_hv(void)
{
    reset();
    run_aux(3, 2500.0f, 3296.0f, 1300.0f);
    CHECK(strcmp(svc("-hv 300"), "-ok hv 300") == 0);
    for (int t = 0; t < 10; t++) {                    /* АЦП монитора встал: свежих нет */
        now_ms += 10;
        float m = svc_aux_10ms(&aux, 0.0f, false, true, now_ms);
        hv_tick(&hv, m, aux.vdd_mv, now_ms);
    }
    CHECK(!hv_enabled(&hv));
    CHECK(hv_trip_reason(&hv) && strcmp(hv_trip_reason(&hv), "mon") == 0);
}

static void test_cmd_hv(void)
{
    reset();
    CHECK(strcmp(svc("-hv 500"), "-err notready") == 0);   /* служебные АЦП ещё не работают */
    CHECK(strcmp(svc("-hv 0"), "-ok hv 0") == 0);          /* 0 — квитирование, можно всегда */
    CHECK(strcmp(svc("-hv off"), "-ok hv off") == 0);
    run_aux(2, 2507.0f, 3296.0f, 1300.0f);
    CHECK(strcmp(svc("-hv abc"), "-err hv") == 0);
    CHECK(strcmp(svc("-hv 12abc"), "-err hv") == 0);
    CHECK(strcmp(svc("-hv 1201"), "-err hv") == 0);
    CHECK(strcmp(svc("-hv -5"), "-err hv") == 0);
    CHECK(!hv_enabled(&hv));
    CHECK(strcmp(svc("-hv  500 "), "-ok hv 500") == 0);
    CHECK(hv_enabled(&hv) && hv_setpoint(&hv) == 500.0f);
    CHECK(starts(svc("-hv"), "-ok hv on set 500"));
    CHECK(strcmp(svc("-hv off"), "-ok hv off") == 0);
    CHECK(!hv_enabled(&hv));
    CHECK(starts(svc("-hv"), "-ok hv off set 0"));
    /* негодная калибровка монитора — не включать */
    hv_init(&hv, NAN, 1.64f);
    CHECK(strcmp(svc("-hv 500"), "-err hvcal") == 0);
    CHECK(strcmp(svc("-hvx"), "<not svc>") == 0);         /* чужая команда — proto ответит -err unknown */
}

/* -hv 500 / 1000 от команды до монитора: модель выхода 0.94·CW (нелинейная ОС), τ 2 с (делитель),
   опора 2507 «по VREFINT» — та же, по которой калиброван монитор */
static void test_hv_end_to_end(void)
{
    reset();
    const float vref = 2507.0f, vdd = 3296.0f;
    run_aux(3, vref, vdd, 1300.0f);
    float vout = 0.0f;
    const float targets[2] = { 500.0f, 1000.0f };
    for (int k = 0; k < 2; k++) {
        char c[16];
        snprintf(c, sizeof c, "-hv %d", (int)targets[k]);
        CHECK(starts(svc(c), "-ok hv"));
        for (int t = 0; t < 9000; t++) {               /* 90 с */
            now_ms += 10;
            float m = svc_aux_10ms(&aux, mon_raw_for(vout, 2460.0f, 1.64f, vref), true, hv_enabled(&hv), now_ms);
            uint16_t d = hv_tick(&hv, m, aux.vdd_mv, now_ms);
            float cw = (float)d / 4096.0f * vdd / 1000.0f * 454.5f;
            float ratio = (targets[k] > 700.0f) ? 0.916f : 0.94f;
            vout += (ratio * cw - vout) * (0.01f / 2.0f);
            if (t % 100 == 99) svc_aux_1s(&aux, &hv, raw_for_vref(vref), vdd / 3.0f * 4095.0f / vref, 1300.0f);
        }
        CHECK(hv_enabled(&hv));
        CHECK_NEAR(vout, targets[k], 5.0);
        CHECK_NEAR(hv_mon_avg(&hv), targets[k], 5.0);
    }
    char b[32];
    CHECK(kv(svc("-gst"), "hv_out", b, sizeof b));
    CHECK_NEAR(atof(b), 1000.0, 5.0);
    CHECK(strcmp(svc("-hv off"), "-ok hv off") == 0);
    CHECK(!hv_enabled(&hv));
}

static void test_cmd_hvzero(void)
{
    reset();
    run_aux(3, 2500.0f, 3296.0f, 1300.0f);
    CHECK(starts(svc("-hvzero"), "-err wait"));        /* < 120 с с запуска (выход мог остаться заряжен) */
    CHECK(strcmp(svc("-hvzero 5"), "-err hvzero") == 0);
    run_aux(120, 2500.0f, 3296.0f, 1300.0f);
    CHECK(strcmp(svc("-hv 300"), "-ok hv 300") == 0);
    CHECK(strcmp(svc("-hvzero"), "-err hv_on") == 0);
    svc("-hv off");
    run_aux(10, 2500.0f, 3296.0f, 1300.0f);
    CHECK(starts(svc("-hvzero"), "-err wait 110 s"));     /* отсчёт от выключения */
    /* монитор 2445 мВ при выключенном и разряженном ВВ */
    for (int s = 0; s < 130; s++)
        for (int t = 0; t < 100; t++) { now_ms += 10; svc_aux_10ms(&aux, 2445.0f * 4095.0f / 2500.0f, true, false, now_ms); }
    CHECK(strcmp(svc("-hvzero"), "-ok hvzero 2445.0") == 0);
    CHECK_NEAR(live.hv_mon_zero_mv, 2445.0, 0.05);
    CHECK_NEAR(hv_mon_volts(&hv, 2445.0f), 0.0, 0.05);  /* калибровка ушла в hv_ctl */
    /* у потолка АЦП (TL431 в плюсовом допуске) и слишком низко — отказ, калибровка прежняя */
    for (int t = 0; t < 1000; t++) { now_ms += 10; svc_aux_10ms(&aux, 4093.0f, true, false, now_ms); }
    CHECK(strcmp(svc("-hvzero"), "-err ceiling") == 0);
    for (int t = 0; t < 1000; t++) { now_ms += 10; svc_aux_10ms(&aux, 2200.0f * 4095.0f / 2500.0f, true, false, now_ms); }
    CHECK(strcmp(svc("-hvzero"), "-err range") == 0);
    CHECK_NEAR(live.hv_mon_zero_mv, 2445.0, 0.05);
}

static void test_cmd_hvk(void)
{
    reset();
    run_aux(2, 2500.0f, 3296.0f, 1300.0f);
    CHECK(strcmp(svc("-hvk"), "-ok hvk 1.6400") == 0);
    CHECK(strcmp(svc("-hvk 1.615"), "-ok hvk 1.6150") == 0);
    CHECK_NEAR(live.hv_mon_k_mv_per_v, 1.615, 1e-6);
    CHECK_NEAR(hv_mon_volts(&hv, 2460.0f - 1.615f * 1000.0f), 1000.0, 0.01);
    CHECK(strcmp(svc("-hvk 9"), "-err hvk") == 0);
    CHECK(strcmp(svc("-hvk x"), "-err hvk") == 0);
    CHECK(strcmp(svc("-hvk 1.6 2"), "-err hvk") == 0);
    svc("-hv 200");
    CHECK(strcmp(svc("-hvk 1.64"), "-err hv_on") == 0);
    CHECK_NEAR(live.hv_mon_k_mv_per_v, 1.615, 1e-6);
}

static void test_cmd_ptcal(void)
{
    reset();
    CHECK(strcmp(svc("-ptcal 25.5"), "-err notready") == 0);
    /* замер 2026-11-06: R 1096.6 Ом при R_pu 10000 → для 25.5 °C R_pu ≈ 10024.5 */
    float x = 1096.6f / (1096.6f + 10000.0f);
    float raw = x * 4095.0f * 8.0f;
    run_aux(2, 2500.0f, 3296.0f, raw);
    CHECK(starts(svc("-ptcal 25.5"), "-ok ptcal rpu 1002"));
    CHECK_NEAR(live.pt_rpu_ohm, 10024.5, 1.0);
    char b[32];
    CHECK(kv(svc("-gst"), "t", b, sizeof b));
    CHECK_NEAR(atof(b), 25.5, 0.01);
    CHECK(strcmp(svc("-ptcal 100"), "-err ptcal") == 0);
    CHECK(strcmp(svc("-ptcal"), "-err ptcal") == 0);
    CHECK(strcmp(svc("-ptcal 60"), "-err ptcal range") == 0);   /* R_pu вне 9…11 кОм — не та точка */
    run_aux(1, 2500.0f, 3296.0f, 4095.0f);
    CHECK(strcmp(svc("-ptcal 25"), "-err pt open") == 0);
    CHECK(kv(svc("-gst"), "pt", b, sizeof b) && strcmp(b, "open") == 0);
    CHECK(kv(rep, "t", b, sizeof b) && strcmp(b, "nan") == 0);
}

static void test_cmd_gst(void)
{
    reset();
    run_aux(5, 2507.0f, 3296.0f, 1300.0f);           /* монитор успел пересчитаться по опоре 2507 */
    gst_stub.base = 645.25f; gst_stub.sigma = 1.18f; gst_stub.cps_in = 120; gst_stub.cps_acc = 110;
    gst_stub.rej = 3; gst_stub.lost = 0; gst_stub.overrun = 0; gst_stub.live_s = 9.5f; gst_stub.real_s = 10.0f;
    gst_stub.cpu_pct = 7.2f; gst_stub.pwm = 0; gst_stub.flash = "defaults";
    gst_stub.thr = 7; gst_stub.rej_shape = 4000000000u; gst_stub.rej_width = 4000000000u;
    gst_stub.rej_pile = 4000000000u; gst_stub.rej_budget = 4000000000u;
    const char *g = svc("-gst");
    CHECK(starts(g, "-ok gst "));
    CHECK(strlen(g) < 500);
    static const char *keys[] = { "hv", "hv_set", "hv_out", "hv_trim", "hv_trip", "t", "vref", "base",
                                  "sigma", "cps_in", "cps_acc", "rej", "live_s", "real_s", "cpu", "lost",
                                  "overrun", "thr", "rej_swpb",
                                  "flash", "vdd", "mon_mv", "hv_zero", "hv_k", "pt_rpu", "hv_pwm" };
    char b[64];
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        bool ok = kv(g, keys[i], b, sizeof b);
        if (!ok) printf("  нет ключа %s\n", keys[i]);
        CHECK(ok);
    }
    CHECK(kv(g, "hv", b, sizeof b) && strcmp(b, "off") == 0);
    CHECK(kv(g, "vref", b, sizeof b) && strcmp(b, "2507.0") == 0);
    CHECK(kv(g, "vdd", b, sizeof b) && fabs(atof(b) - 3296.0) < 0.2);
    CHECK(kv(g, "hv_out", b, sizeof b) && fabs(atof(b)) < 0.5);
    CHECK(kv(g, "hv_trip", b, sizeof b) && strcmp(b, "-") == 0);
    CHECK(kv(g, "base", b, sizeof b) && strcmp(b, "645.25") == 0);
    CHECK(kv(g, "flash", b, sizeof b) && strcmp(b, "defaults") == 0);
    CHECK(kv(g, "thr", b, sizeof b) && strcmp(b, "7") == 0);
    CHECK(kv(g, "rej_swpb", b, sizeof b) && strcmp(b, "4000000000/4000000000/4000000000/4000000000") == 0);
    CHECK(strstr(g, "hv_off=") == NULL);
    svc("-hv 100");
    svc("-hv off");
    g = svc("-gst");
    CHECK(strstr(g, " hv_off=cmd") != NULL);
}

static void test_cmd_diag(void)
{
    reset();
    CHECK(strcmp(svc("-diag"), "-err notready") == 0);  /* АЦП ещё не запущены */
    run_aux(1, 2507.0f, 3296.0f, 1300.0f);
    const char *d = svc("-diag");
    char b[32];
    CHECK(starts(d, "-ok diag vref=2507"));
    CHECK(kv(d, "tract", b, sizeof b) && strcmp(b, "ok") == 0);
    CHECK(kv(d, "base", b, sizeof b) && strcmp(b, "394") == 0);
    CHECK(kv(d, "oa1", b, sizeof b) && strcmp(b, "2418") == 0);
    diag_stub.mv[SVC_N_VB] = 120.0f;
    diag_stub.mv[SVC_N_BASE] = 430.0f;
    diag_stub.mv[SVC_N_MON] = 1000.0f;                 /* монитор без нормы */
    d = svc("-diag");
    CHECK(kv(d, "tract", b, sizeof b) && strcmp(b, "FAIL") == 0);
    CHECK(kv(d, "bad", b, sizeof b) && strcmp(b, "vb,base") == 0);
    diag_stub.mv[SVC_N_VB] = NAN;
    CHECK(svc_diag_check(&diag_stub, NULL, 0) == 2);
    diag_ok = false;
    CHECK(strcmp(svc("-diag"), "-err notready") == 0);
}

int main(void)
{
    test_vref_math();
    test_vref_norm_and_recal();
    test_vref_trip_with_hv();
    test_vdd_pt_filters();
    test_mon_stale();
    test_stale_monitor_trips_hv();
    test_cmd_hv();
    test_hv_end_to_end();
    test_cmd_hvzero();
    test_cmd_hvk();
    test_cmd_ptcal();
    test_cmd_gst();
    test_cmd_diag();
    return T_DONE();
}
