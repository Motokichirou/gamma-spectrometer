/* dactest.c — см. dactest.h */
#include "dactest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICKS_MAX 65536u            /* 16-битный таймер: интервал = ARR + 1 */

uint16_t dact_ndn(uint16_t step)
{
    return (uint16_t)((step + DACT_DN_MAX - 1u) / DACT_DN_MAX);
}

uint16_t dact_per_min(uint16_t step)
{
    uint32_t n = dact_ndn(step);
    uint32_t p = DACT_HOLD_US + (n ? n - 1u : 0u) * DACT_DN_US + DACT_REST_MIN_US;
    return (uint16_t)(p < DACT_PER_MIN ? DACT_PER_MIN : p);
}

bool dact_start(dact_t *d, uint16_t step, uint16_t n, uint16_t period_us, uint32_t tick_hz, uint16_t jit)
{
    if (!d || dact_busy(d)) return false;
    if (step < 1u || step > DACT_STEP_MAX || n < 1u) return false;
    if (period_us < dact_per_min(step) || period_us > DACT_PER_MAX || jit < 1u) return false;
    const uint32_t tpu = tick_hz / 1000000u;            /* тактов на мкс */
    const uint16_t ndn = dact_ndn(step);
    uint32_t hold = DACT_HOLD_US * tpu, dn = DACT_DN_US * tpu;
    uint32_t per = (uint32_t)((uint64_t)period_us * tick_hz / 1000000u);
    uint32_t used = hold + (uint32_t)(ndn - 1u) * dn;
    if (tpu < 1u || per <= used) return false;
    uint32_t rest = per - used;
    uint32_t settle = (uint32_t)((uint64_t)DACT_SETTLE_US * tick_hz / 1000000u);
    if (hold > TICKS_MAX || dn > TICKS_MAX || settle < 1u || settle > TICKS_MAX) return false;
    d->step = step;
    d->n = n;
    d->period_us = period_us;
    d->hold = hold;
    d->dn = dn;
    d->rest = rest;
    d->ndn = ndn;
    d->k = 0;
    d->wait = 0;
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

/* пауза длиннее 16-битного таймера — кусками (ЦАП держит базу, запись того же кода безвредна) */
static uint32_t take_wait(dact_t *d)
{
    uint32_t t = d->wait > TICKS_MAX ? TICKS_MAX : d->wait;
    d->wait -= t;
    return t;
}

bool dact_tick(dact_t *d, uint16_t *code, uint32_t *ticks)
{
    if ((d->phase == DACT_LOW || d->phase == DACT_TAIL) && d->wait) {
        *code = DACT_BASE;
        *ticks = take_wait(d);
        return true;
    }
    switch ((dact_phase_t)d->phase) {
    case DACT_SETTLE:
    case DACT_LOW:                  /* пауза кончилась — ступенька вверх */
        *code = (uint16_t)(DACT_BASE + d->step);
        *ticks = d->hold;
        d->k = 0;
        d->phase = DACT_DOWN;
        return true;
    case DACT_DOWN: {               /* очередная ступенька спуска: равные доли step */
        uint16_t k = (uint16_t)(d->k + 1u);
        d->k = k;
        *code = (uint16_t)(DACT_BASE + d->step
                           - (uint16_t)(((uint32_t)d->step * k + d->ndn / 2u) / d->ndn));
        if (k < d->ndn) {
            *ticks = d->dn;
            return true;
        }
        *code = DACT_BASE;          /* последняя — точно на базу */
        uint16_t m = (uint16_t)(d->done + 1u);
        d->done = m;
        if (m >= d->n) {
            d->wait = d->rest;      /* дать спуску отзвучать, затем выкл */
            d->phase = DACT_TAIL;
        } else {
            d->wait = d->rest + xorshift(&d->rnd) % d->jit;
            d->phase = DACT_LOW;
        }
        *ticks = take_wait(d);
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
    if (per < dact_per_min((uint16_t)step)) {
        snprintf(reply, cap, "-err dac period>=%u", (unsigned)dact_per_min((uint16_t)step));
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
