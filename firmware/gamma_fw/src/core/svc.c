/* svc.c — служебные замеры и сервисные команды (см. svc.h). */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "svc.h"
#include "temp.h"
#include "fmt.h"

static const svc_ctx_t *C;

/* ---------------- служебные замеры ---------------- */
void svc_aux_init(svc_aux_t *a, float vrefint_cal)
{
    memset(a, 0, sizeof *a);
    a->vrefint_cal = vrefint_cal;
    a->vref_now_mv = NAN;
    a->vref_mv = SVC_VREF_NOM_MV;
    a->vdd_mv = NAN;
    a->pt_raw = NAN;
    a->mon_mv = a->mon_mv_f = NAN;
}

float svc_vref_from_vrefint(float raw, float cal)
{
    if (!(raw > 0.0f) || !isfinite(raw) || !(cal > 0.0f)) return NAN;
    return 3000.0f * cal / raw;
}

float svc_vref_mv(const svc_aux_t *a) { return a->vref_seeded ? a->vref_mv : SVC_VREF_NOM_MV; }

static bool vref_in_norm(float v) { return fabsf(v - SVC_VREF_NOM_MV) <= SVC_VREF_TOL_MV; } /* NaN → false */

float svc_aux_10ms(svc_aux_t *a, float mon_raw, bool fresh, bool hv_on, uint32_t now_ms)
{
    uint32_t dms = now_ms - a->last_ms;
    if (dms > 100u) dms = 100u;
    a->last_ms = now_ms;
    if (hv_on) a->hv_off_since_ms = now_ms;
    if (fresh && isfinite(mon_raw) && mon_raw >= 0.0f) {
        a->mon_mv = mon_raw * svc_vref_mv(a) / 4095.0f;
        a->mon_fresh_ms = now_ms;
        a->mon_ever = true;
        if (!a->mon_seeded) { a->mon_mv_f = a->mon_mv; a->mon_seeded = true; }
    }
    if (!a->mon_ever || now_ms - a->mon_fresh_ms > SVC_MON_STALE_MS) return NAN;
    a->mon_mv_f += (a->mon_mv - a->mon_mv_f) * ((float)dms / 1000.0f / SVC_MON_TAU_S);
    return a->mon_mv;
}

bool svc_aux_1s(svc_aux_t *a, hv_ctl_t *hv, float vrefint_raw, float vbat_raw, float pt_raw)
{
    const float k_vref = 1.0f / SVC_VREF_TAU_S, k_vdd = 1.0f / SVC_VDD_TAU_S, k_pt = 1.0f / SVC_PT_TAU_S;
    bool recal = false;

    float v = svc_vref_from_vrefint(vrefint_raw, a->vrefint_cal);
    a->vref_now_mv = v;
    if (isfinite(v)) a->ready = true;
    if (vref_in_norm(v)) {
        if (!a->vref_seeded) { a->vref_mv = v; a->vref_seeded = true; }
        else a->vref_mv += (v - a->vref_mv) * k_vref;
        a->vref_ok = true;
        a->vref_bad = 0;
        a->recal_n = 0;
    } else {
        a->vref_ok = false;
        if (a->vref_bad < 255u) a->vref_bad++;
        if (hv_enabled(hv)) {
            if (a->vref_bad >= SVC_VREF_TRIP_N) hv_off(hv, "TRIP VREF+ out of range");
        } else if (isfinite(v) && a->recal_n < SVC_RECAL_MAX) {
            a->recal_n++;
            recal = true;
        }
    }

    if (isfinite(vbat_raw) && vbat_raw > 0.0f) {
        float vdd = vbat_raw * 3.0f * svc_vref_mv(a) / 4095.0f;
        if (!a->vdd_seeded) { a->vdd_mv = vdd; a->vdd_seeded = true; }
        else a->vdd_mv += (vdd - a->vdd_mv) * k_vdd;
    }

    if (isfinite(pt_raw)) {
        if (!a->pt_seeded || pt_state(pt_raw) != pt_state(a->pt_raw)) { a->pt_raw = pt_raw; a->pt_seeded = true; }
        else a->pt_raw += (pt_raw - a->pt_raw) * k_pt;
    }
    return recal;
}

bool svc_hv_allowed(const svc_aux_t *a)
{
    return a->ready && a->vref_ok && a->vdd_seeded && isfinite(a->vdd_mv) && a->mon_ever;
}

/* ---------------- -diag ---------------- */
const svc_node_t svc_nodes[SVC_DIAG_N] = {
    [SVC_N_VA]   = { "va",   "PA1", 1613.0f, 30.0f },   /* V_A на OA1+ */
    [SVC_N_OA1N] = { "oa1n", "PA3", 1613.0f, 30.0f },
    [SVC_N_OA1]  = { "oa1",  "PA2", 2418.0f, 50.0f },   /* = 1.5·V_A: TIA и коаксиал целы */
    [SVC_N_VA2]  = { "va2",  "PA7", 1613.0f, 30.0f },
    [SVC_N_OA2N] = { "oa2n", "PA5", 1613.0f, 30.0f },
    [SVC_N_OA2]  = { "oa2",  "PA6", 1613.0f, 50.0f },
    [SVC_N_VB]   = { "vb",   "PB0", 99.0f,   5.0f  },
    [SVC_N_BASE] = { "base", "PB1", 394.0f,  15.0f },   /* база АЦП */
    [SVC_N_MON]  = { "mon",  "PA0", 0.0f,    0.0f  },   /* монитор ВВ — без нормы */
};

int svc_diag_check(const svc_diag_t *d, char *bad, size_t cap)
{
    int n = 0;
    size_t w = 0;
    if (bad && cap) bad[0] = 0;
    for (int i = 0; i < SVC_DIAG_N; i++) {
        const svc_node_t *s = &svc_nodes[i];
        if (s->tol_mv <= 0.0f) continue;
        if (fabsf(d->mv[i] - s->nom_mv) <= s->tol_mv) continue;     /* NaN — вне нормы */
        n++;
        if (bad && w < cap) {
            int k = snprintf(bad + w, cap - w, "%s%s", w ? "," : "", s->key);
            if (k > 0) w += (size_t)k;
        }
    }
    return n;
}

/* ---------------- разбор и вывод ---------------- */
/* одно число и ничего после; true — ок */
static bool parse_one_float(const char *p, float *out)
{
    while (*p == ' ' || *p == '\t') p++;
    char *e;
    double d = strtod(p, &e);
    if (e == p) return false;
    while (*e == ' ' || *e == '\t') e++;
    if (*e || !isfinite(d)) return false;
    *out = (float)d;
    return true;
}

/* аргументы после имени команды: "" — нет аргументов; NULL — это не та команда */
static const char *args_of(const char *cmd, const char *name)
{
    size_t n = strlen(name);
    if (strncmp(cmd, name, n) != 0) return NULL;
    if (cmd[n] != 0 && cmd[n] != ' ' && cmd[n] != '\t') return NULL;
    const char *a = cmd + n;
    while (*a == ' ' || *a == '\t') a++;
    return a;
}

typedef struct { char *p; size_t cap, w; } wr_t;

static void put(wr_t *o, const char *key, const char *val)
{
    if (o->w >= o->cap) return;
    int k = snprintf(o->p + o->w, o->cap - o->w, "%s%s=%s", o->w ? " " : "", key, val);
    if (k > 0) o->w += (size_t)k;
    if (o->w >= o->cap) o->w = o->cap - 1u;
}
static void putf(wr_t *o, const char *key, float v, int dec)
{
    char b[24];
    fmt_fix(b, sizeof b, v, dec);
    put(o, key, b);
}
static void putu(wr_t *o, const char *key, uint32_t v)
{
    char b[12];
    snprintf(b, sizeof b, "%lu", (unsigned long)v);
    put(o, key, b);
}

static void cmd_gst(char *reply, size_t cap)
{
    const hv_ctl_t *h = C->hv;
    const svc_aux_t *a = C->aux;
    svc_gst_t g;
    memset(&g, 0, sizeof g);
    g.base = g.sigma = NAN;
    g.flash = "none";
    if (C->fill_gst) C->fill_gst(&g);

    pt_state_t pst = a->pt_seeded ? pt_state(a->pt_raw) : PT_OPEN;
    float t = NAN;
    if (a->pt_seeded && pst == PT_OK) t = pt_temp_c(pt_resistance(a->pt_raw, C->live->pt_rpu_ohm));

    wr_t o = { reply, cap, 0 };
    reply[0] = 0;
    put(&o, "-ok gst hv", hv_enabled(h) ? "on" : "off");
    putf(&o, "hv_set", hv_setpoint(h), 0);
    putf(&o, "hv_ramp", hv_ramp(h), 0);
    putf(&o, "hv_out", hv_mon_avg(h), 1);
    putu(&o, "hv_ceil", hv_near_ceiling(h) ? 1u : 0u);
    putf(&o, "hv_trim", hv_trim(h), 1);
    putu(&o, "hv_pwm", g.pwm);
    put(&o, "hv_trip", hv_trip_reason(h) ? hv_trip_reason(h) : "-");
    putf(&o, "hv_zero", C->live->hv_mon_zero_mv, 1);
    putf(&o, "hv_k", C->live->hv_mon_k_mv_per_v, 4);
    putf(&o, "mon_mv", a->mon_mv_f, 1);
    putf(&o, "vdd", a->vdd_mv, 1);
    putf(&o, "vref", a->vref_now_mv, 1);
    put(&o, "vref_ok", a->vref_ok ? "1" : "0");
    putf(&o, "t", t, 2);
    put(&o, "pt", a->pt_seeded ? pt_state_name(pst) : "-");
    putf(&o, "pt_raw", a->pt_raw, 1);
    putf(&o, "pt_rpu", C->live->pt_rpu_ohm, 1);
    putf(&o, "base", g.base, 2);
    putf(&o, "sigma", g.sigma, 2);
    putu(&o, "cps_in", g.cps_in);
    putu(&o, "cps_acc", g.cps_acc);
    putu(&o, "rej", g.rej);
    putu(&o, "thr", g.thr);
    if (o.w + 48u < cap) {          /* причины отбраковки с пуска: форма/ширина/наложение/бюджет */
        int k = snprintf(reply + o.w, cap - o.w, " rej_swpb=%lu/%lu/%lu/%lu", (unsigned long)g.rej_shape,
                         (unsigned long)g.rej_width, (unsigned long)g.rej_pile, (unsigned long)g.rej_budget);
        if (k > 0) o.w += (size_t)k;
    }
    putf(&o, "live_s", g.live_s, 1);
    putf(&o, "real_s", g.real_s, 1);
    putf(&o, "cpu", g.cpu_pct, 1);
    putu(&o, "lost", g.lost);
    putu(&o, "overrun", g.overrun);
    put(&o, "flash", g.flash ? g.flash : "-");
    /* причина последнего выключения — последней: свободный текст с пробелами */
    if (hv_off_reason(h) && o.w + 10u < cap) {
        int k = snprintf(reply + o.w, cap - o.w, " hv_off=%s", hv_off_reason(h));
        if (k > 0) o.w += (size_t)k;
    }
}

static void cmd_diag(char *reply, size_t cap)
{
    svc_diag_t d;
    if (!C->diag || !C->aux->ready || !C->diag(&d)) { snprintf(reply, cap, "-err notready"); return; }
    wr_t o = { reply, cap, 0 };
    reply[0] = 0;
    putf(&o, "-ok diag vref", svc_vref_mv(C->aux), 0);
    for (int i = 0; i < SVC_DIAG_N; i++) putf(&o, svc_nodes[i].key, d.mv[i], 0);
    char bad[96];
    int n = svc_diag_check(&d, bad, sizeof bad);
    put(&o, "tract", n ? "FAIL" : "ok");
    if (n) put(&o, "bad", bad);
}

static void cmd_hv(const char *a, char *reply, size_t cap)
{
    hv_ctl_t *h = C->hv;
    if (!*a) {
        char s[16], m[16];
        fmt_fix(s, sizeof s, hv_setpoint(h), 0);
        fmt_fix(m, sizeof m, hv_mon_avg(h), 1);
        snprintf(reply, cap, "-ok hv %s set %s out %s", hv_enabled(h) ? "on" : "off", s, m);
        return;
    }
    uint32_t now = C->millis ? C->millis() : 0u;
    /* «выключено с» для -hvzero — и без тиков между включением и выключением */
    if (hv_enabled(h)) C->aux->hv_off_since_ms = now;
    if (strcmp(a, "off") == 0) { hv_off(h, "cmd"); snprintf(reply, cap, "-ok hv off"); return; }
    /* строго: целое и ничего после ("-hv abc" через atoi давал 0 — выключение, "12abc" → 12) */
    char *e;
    long v = strtol(a, &e, 10);
    while (*e == ' ' || *e == '\t') e++;
    if (e == a || *e || v < 0 || v > (long)HV_MAX_SET_V) { snprintf(reply, cap, "-err hv"); return; }
    if (v > 0 && !svc_hv_allowed(C->aux)) {
        snprintf(reply, cap, C->aux->ready && !C->aux->vref_ok ? "-err vref" : "-err notready");
        return;
    }
    if (v > 0 && !hv_cal_ok(h)) { snprintf(reply, cap, "-err hvcal"); return; }
    if (hv_set(h, (float)v, now) != 0) { snprintf(reply, cap, "-err hv"); return; }
    if (hv_enabled(h)) C->aux->hv_off_since_ms = now;
    snprintf(reply, cap, "-ok hv %ld", v);
}

static void cmd_hvzero(char *reply, size_t cap)
{
    hv_ctl_t *h = C->hv;
    svc_aux_t *a = C->aux;
    if (hv_enabled(h)) { snprintf(reply, cap, "-err hv_on"); return; }
    uint32_t now = C->millis ? C->millis() : 0u;
    if (now - a->hv_off_since_ms < SVC_HVZERO_WAIT_MS) {
        snprintf(reply, cap, "-err wait %lu s",
                 (unsigned long)((SVC_HVZERO_WAIT_MS - (now - a->hv_off_since_ms) + 999u) / 1000u));
        return;
    }
    float z = a->mon_mv_f;
    if (!a->ready || !a->mon_seeded || !isfinite(z)) { snprintf(reply, cap, "-err notready"); return; }
    if (z >= HV_MON_CEIL_CODE * svc_vref_mv(a) / 4095.0f) { snprintf(reply, cap, "-err ceiling"); return; }
    if (z < SVC_HVZERO_MIN_MV) { snprintf(reply, cap, "-err range"); return; }
    if (hv_set_cal(h, z, C->live->hv_mon_k_mv_per_v) != 0) { snprintf(reply, cap, "-err hvcal"); return; }
    C->live->hv_mon_zero_mv = z;
    char b[16];
    fmt_fix(b, sizeof b, z, 1);
    snprintf(reply, cap, "-ok hvzero %s", b);
}

static void cmd_hvk(const char *a, char *reply, size_t cap)
{
    char b[16];
    if (*a) {
        float k;
        if (!parse_one_float(a, &k) || k < SVC_HVK_MIN || k > SVC_HVK_MAX) { snprintf(reply, cap, "-err hvk"); return; }
        if (hv_enabled(C->hv)) { snprintf(reply, cap, "-err hv_on"); return; }
        if (hv_set_cal(C->hv, C->live->hv_mon_zero_mv, k) != 0) { snprintf(reply, cap, "-err hvcal"); return; }
        C->live->hv_mon_k_mv_per_v = k;
    }
    fmt_fix(b, sizeof b, C->live->hv_mon_k_mv_per_v, 4);
    snprintf(reply, cap, "-ok hvk %s", b);
}

static void cmd_ptcal(const char *a, char *reply, size_t cap)
{
    float t;
    if (!parse_one_float(a, &t) || t < SVC_PTCAL_TMIN || t > SVC_PTCAL_TMAX) { snprintf(reply, cap, "-err ptcal"); return; }
    if (!C->aux->pt_seeded) { snprintf(reply, cap, "-err notready"); return; }
    pt_state_t st = pt_state(C->aux->pt_raw);
    if (st != PT_OK) { snprintf(reply, cap, "-err pt %s", pt_state_name(st)); return; }
    float rpu = pt_calibrate_rpu(C->aux->pt_raw, t);
    if (!(rpu >= SVC_RPU_MIN && rpu <= SVC_RPU_MAX)) { snprintf(reply, cap, "-err ptcal range"); return; }
    C->live->pt_rpu_ohm = rpu;
    char b[16];
    fmt_fix(b, sizeof b, rpu, 1);
    snprintf(reply, cap, "-ok ptcal rpu %s", b);
}

void svc_init(const svc_ctx_t *ctx) { C = ctx; }

bool svc_handle(const char *cmd, char *reply, size_t cap)
{
    const char *a;
    if (!C || !cap) return false;
    if ((a = args_of(cmd, "-hv")) != NULL)          cmd_hv(a, reply, cap);
    else if ((a = args_of(cmd, "-hvzero")) != NULL) { if (*a) snprintf(reply, cap, "-err hvzero"); else cmd_hvzero(reply, cap); }
    else if ((a = args_of(cmd, "-hvk")) != NULL)    cmd_hvk(a, reply, cap);
    else if ((a = args_of(cmd, "-ptcal")) != NULL)  cmd_ptcal(a, reply, cap);
    else if ((a = args_of(cmd, "-gst")) != NULL)    cmd_gst(reply, cap);
    else if ((a = args_of(cmd, "-diag")) != NULL)   cmd_diag(reply, cap);
    else return false;
    return true;
}
