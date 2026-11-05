/* dactest.c — см. dactest.h */
#include "dactest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICKS_MAX 65536u            /* 16-битный таймер: интервал = ARR + 1 */

bool dact_start(dact_t *d, uint16_t step, uint16_t n, uint16_t period_us, uint32_t tick_hz, uint16_t jit)
{
    if (!d || dact_busy(d)) return false;
    if (step < 1u || step > DACT_STEP_MAX || n < 1u) return false;
    if (period_us < DACT_PER_MIN || period_us > DACT_PER_MAX || jit < 1u) return false;
    uint32_t half = (uint32_t)((uint64_t)period_us * tick_hz / 2000000u);
    uint32_t settle = (uint32_t)((uint64_t)DACT_SETTLE_US * tick_hz / 1000000u);
    if (half < 1u || half + jit - 1u > TICKS_MAX || settle < 1u || settle > TICKS_MAX) return false;
    d->step = step;
    d->n = n;
    d->period_us = period_us;
    d->half = half;
    d->settle = settle;
    d->jit = jit;
    if (d->rnd == 0u) d->rnd = 0x9E3779B9u;
    d->done = 0;
    d->phase = DACT_SETTLE;
    return true;
}

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

bool dact_tick(dact_t *d, uint16_t *code, uint32_t *ticks)
{
    switch ((dact_phase_t)d->phase) {
    case DACT_SETTLE:
    case DACT_LOW:                  /* пауза кончилась — ступенька вверх */
        *code = (uint16_t)(DACT_BASE + d->step);
        *ticks = d->half;
        d->phase = DACT_HIGH;
        return true;
    case DACT_HIGH: {               /* ступенька вниз */
        *code = DACT_BASE;
        uint16_t k = (uint16_t)(d->done + 1u);
        d->done = k;
        if (k >= d->n) {
            *ticks = d->half;       /* дать спаду отработать, затем выкл */
            d->phase = DACT_TAIL;
        } else {
            *ticks = d->half + xorshift(&d->rnd) % d->jit;
            d->phase = DACT_LOW;
        }
        return true;
    }
    case DACT_TAIL:
    case DACT_IDLE:
    default:
        *code = DACT_BASE;
        *ticks = 1u;
        d->phase = DACT_IDLE;
        return false;
    }
}

void dact_abort(dact_t *d)
{
    d->phase = DACT_IDLE;
}

/* ---------------- команда ---------------- */
static bool parse_u(const char **p, unsigned long *v)
{
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (*s < '0' || *s > '9') return false;
    char *e;
    *v = strtoul(s, &e, 10);
    if (*e != 0 && *e != ' ' && *e != '\t') return false;
    *p = e;
    return true;
}

bool dact_handle(const dact_ctx_t *c, const char *cmd, char *reply, size_t cap)
{
    if (!c || !cap || strncmp(cmd, "-dac", 4) != 0) return false;
    const char *a = cmd + 4;
    if (*a != 0 && *a != ' ' && *a != '\t') return false;
    while (*a == ' ' || *a == '\t') a++;

    if (*a == 0) {
        if (c->busy && c->busy())
            snprintf(reply, cap, "-ok dac busy done=%u n=%u", (unsigned)c->done(), (unsigned)c->total());
        else
            snprintf(reply, cap, "-ok dac idle");
        return true;
    }
    if (strncmp(a, "off", 3) == 0 && (a[3] == 0 || a[3] == ' ' || a[3] == '\t')) {
        if (c->stop) c->stop();
        snprintf(reply, cap, "-ok dac off done=%u", (unsigned)(c->done ? c->done() : 0u));
        return true;
    }
    unsigned long step, n, per = DACT_PER_DEF;
    const char *p = a;
    bool ok = parse_u(&p, &step) && parse_u(&p, &n);
    if (ok) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p) ok = parse_u(&p, &per);
        while (*p == ' ' || *p == '\t') p++;
        ok = ok && *p == 0;
    }
    if (!ok || step < 1u || step > DACT_STEP_MAX || n < 1u || n > 65535u
        || per < DACT_PER_MIN || per > DACT_PER_MAX) {
        snprintf(reply, cap, "-err dac <1..%u> <1..65535> [%u..%u]",
                 (unsigned)DACT_STEP_MAX, (unsigned)DACT_PER_MIN, (unsigned)DACT_PER_MAX);
        return true;
    }
    if (c->busy && c->busy()) { snprintf(reply, cap, "-err dac busy"); return true; }
    if (!c->start || !c->start((uint16_t)step, (uint16_t)n, (uint16_t)per)) {
        snprintf(reply, cap, "-err dac");
        return true;
    }
    snprintf(reply, cap, "-ok dac step=%lu n=%lu period=%lu", step, n, per);
    return true;
}
