/* acq.c — учёт набора и команды сигнального тракта (см. acq.h). */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "acq.h"
#include "fmt.h"

uint32_t acq_rejected(const det_stats_t *s)
{
    return s->rej_pileup + s->rej_shape + s->rej_width + s->rej_budget;
}

void acq_init(acq_t *a, uint32_t fs_hz, const det_stats_t *st)
{
    memset(a, 0, sizeof *a);
    a->fs_hz = fs_hz ? fs_hz : 1u;
    a->a_prev = *st;
    a->r_prev = *st;
}

void acq_set_fs(acq_t *a, uint32_t fs_hz) { if (fs_hz) a->fs_hz = fs_hz; }

/* приращения набора с a_prev — в накопленное; a_prev = st */
static void account(acq_t *a, const det_stats_t *st)
{
    a->dead_s += (double)(st->dead_samples - a->a_prev.dead_samples) / (double)a->fs_hz;
    a->invalid += acq_rejected(st) - acq_rejected(&a->a_prev);
    a->a_prev = *st;
}

void acq_start(acq_t *a, uint32_t now_ms, const det_stats_t *st)
{
    if (a->run) return;
    a->run = true;
    a->since_ms = now_ms;
    a->a_prev = *st;
}

void acq_stop(acq_t *a, uint32_t now_ms, const det_stats_t *st)
{
    if (!a->run) return;
    account(a, st);
    a->acc_ms += now_ms - a->since_ms;
    a->run = false;
}

void acq_clear(acq_t *a, uint32_t now_ms, const det_stats_t *st)
{
    a->acc_ms = 0;
    a->since_ms = now_ms;
    a->dead_s = 0.0;
    a->invalid = 0;
    a->a_prev = *st;
}

void acq_tick_1s(acq_t *a, const det_stats_t *st, uint32_t busy_cyc, uint32_t win_cyc)
{
    a->cps_acc = st->accepted - a->r_prev.accepted;
    a->cps_rej = acq_rejected(st) - acq_rejected(&a->r_prev);
    a->cps_in = a->cps_acc + a->cps_rej;
    a->r_prev = *st;
    uint64_t c = win_cyc ? ((uint64_t)busy_cyc * 1000u + win_cyc / 2u) / win_cyc : 0u;
    a->cpu_x10 = (uint16_t)(c > 1000u ? 1000u : c);
    if (a->run) account(a, st);
}

uint32_t acq_real_ms(const acq_t *a, uint32_t now_ms)
{
    return a->acc_ms + (a->run ? now_ms - a->since_ms : 0u);
}

float acq_dead_s(const acq_t *a, const det_stats_t *st)
{
    double d = a->dead_s;
    if (a->run) d += (double)(st->dead_samples - a->a_prev.dead_samples) / (double)a->fs_hz;
    return (float)d;
}

float acq_live_s(const acq_t *a, uint32_t now_ms, const det_stats_t *st)
{
    float l = (float)acq_real_ms(a, now_ms) / 1000.0f - acq_dead_s(a, st);
    return l > 0.0f ? l : 0.0f;
}

uint32_t acq_invalid(const acq_t *a, const det_stats_t *st)
{
    return a->invalid + (a->run ? acq_rejected(st) - acq_rejected(&a->a_prev) : 0u);
}

void acq_noise(const uint16_t *s, size_t n, acq_noise_t *o)
{
    memset(o, 0, sizeof *o);
    o->mean = o->sigma = NAN;
    if (!s || !n) return;
    uint64_t s1 = 0, s2 = 0;
    uint16_t mn = 0xFFFFu, mx = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t v = s[i];
        s1 += v;
        s2 += (uint64_t)v * v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    double m = (double)s1 / (double)n;
    double var = (double)s2 / (double)n - m * m;
    if (var < 0.0) var = 0.0;
    o->n = (uint32_t)n;
    o->mean = (float)m;
    o->sigma = (float)sqrt(var);
    o->min = mn;
    o->max = mx;
    double lim = floor(m + ACQ_NOISE_OVER_K * sqrt(var));   /* целый s > lim ⇔ s > floor(lim) */
    if (lim >= 65535.0) return;
    uint16_t thr = (uint16_t)lim;
    for (size_t i = 0; i < n; i++)
        if (s[i] > thr) o->over++;
}

void acq_osc_line(char *out, size_t cap, unsigned i, const det_cap_t *c)
{
    char amp[16], base[16];
    fmt_fix(amp, sizeof amp, c->amp, 2);
    fmt_fix(base, sizeof base, c->base, 2);
    snprintf(out, cap, "osc i=%u pos=%lu ch=%u amp=%s ph=%u pk=%d base=%s", i,
             (unsigned long)c->pos, (unsigned)c->channel, amp, (unsigned)c->phase, (int)c->peak, base);
}

/* ---------------- команды ---------------- */
static const char *args_of(const char *cmd, const char *name)
{
    size_t n = strlen(name);
    if (strncmp(cmd, name, n) != 0) return NULL;
    if (cmd[n] != 0 && cmd[n] != ' ' && cmd[n] != '\t') return NULL;
    const char *a = cmd + n;
    while (*a == ' ' || *a == '\t') a++;
    return a;
}

static const char *smp_name(uint8_t smp) { return smp == 6u ? "6.5" : "2.5"; }

static void cmd_smp(const acq_ctx_t *c, const char *a, char *reply, size_t cap)
{
    if (*a) {
        char *e;
        long v = strtol(a, &e, 10);
        while (*e == ' ' || *e == '\t') e++;
        if (e == a || *e || (v != 2 && v != 6)) { snprintf(reply, cap, "-err smp"); return; }
        if (c->acquiring && c->acquiring()) { snprintf(reply, cap, "-err acq"); return; }
        if (!c->ready || !c->ready()) { snprintf(reply, cap, "-err notready"); return; }
        if (!c->smp_set || !c->smp_set((uint8_t)v)) { snprintf(reply, cap, "-err adc"); return; }
    }
    snprintf(reply, cap, "-ok smp=%s fs=%lu", smp_name(c->smp_get ? c->smp_get() : 2u),
             (unsigned long)(c->fs ? c->fs() : 0u));
}

static void cmd_noise(const acq_ctx_t *c, const char *a, char *reply, size_t cap)
{
    acq_noise_t o;
    if (*a) { snprintf(reply, cap, "-err noise"); return; }
    if (!c->ready || !c->ready() || !c->noise || !c->noise(&o)) { snprintf(reply, cap, "-err notready"); return; }
    char m[16], s[16];
    fmt_fix(m, sizeof m, o.mean, 2);
    fmt_fix(s, sizeof s, o.sigma, 3);
    snprintf(reply, cap, "-ok noise smp=%s fs=%lu n=%lu mean=%s sigma=%s min=%u max=%u over=%lu",
             smp_name(c->smp_get ? c->smp_get() : 2u), (unsigned long)(c->fs ? c->fs() : 0u),
             (unsigned long)o.n, m, s, (unsigned)o.min, (unsigned)o.max, (unsigned long)o.over);
}

static void cmd_osc(const acq_ctx_t *c, const char *a, char *reply, size_t cap)
{
    if (strcmp(a, "raw") == 0) {
        uint32_t n = 0;
        if (c->acquiring && c->acquiring()) { snprintf(reply, cap, "-err acq"); return; }
        if (!c->ready || !c->ready() || !c->osc_raw || !c->osc_raw(&n)) { snprintf(reply, cap, "-err notready"); return; }
        snprintf(reply, cap, "-ok osc raw %lu", (unsigned long)n);
        return;
    }
    long v = c->osc_max;
    if (*a) {
        char *e;
        v = strtol(a, &e, 10);
        while (*e == ' ' || *e == '\t') e++;
        if (e == a || *e || v < 1 || v > (long)c->osc_max) { snprintf(reply, cap, "-err osc 1..%u|raw", (unsigned)c->osc_max); return; }
    }
    if (c->acquiring && c->acquiring()) { snprintf(reply, cap, "-err acq"); return; }
    if (!c->ready || !c->ready() || !c->osc_arm || !c->osc_arm((uint16_t)v)) { snprintf(reply, cap, "-err notready"); return; }
    snprintf(reply, cap, "-ok osc %ld", v);
}

bool acq_handle(const acq_ctx_t *c, const char *cmd, char *reply, size_t cap)
{
    const char *a;
    if (!c || !cap) return false;
    if ((a = args_of(cmd, "-smp")) != NULL)        cmd_smp(c, a, reply, cap);
    else if ((a = args_of(cmd, "-noise")) != NULL) cmd_noise(c, a, reply, cap);
    else if ((a = args_of(cmd, "-osc")) != NULL)   cmd_osc(c, a, reply, cap);
    else return false;
    return true;
}
