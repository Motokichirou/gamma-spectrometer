/* selftest.c — встроенный самотест тракта (см. selftest.h) */

#include "selftest.h"
#include "pulsegen.h"
#include "app.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/* ---- накопитель статистики откликов ---- */
typedef struct {
    volatile uint32_t n;
    volatile uint64_t sum;
    volatile uint64_t sumsq;
    volatile uint16_t min, max;
    volatile bool     active;
} stats_t;

static stats_t st;

static void stats_reset(void)
{
    st.n = 0; st.sum = 0; st.sumsq = 0;
    st.min = 0xFFFF; st.max = 0;
    st.active = true;
}

void selftest_feed(uint16_t a)
{
    if (!st.active)
        return;
    st.n++;
    st.sum   += a;
    st.sumsq += (uint64_t)a * a;
    if (a < st.min) st.min = a;
    if (a > st.max) st.max = a;
}

static void stats_get(float *mean, float *sigma, uint32_t *n)
{
    uint32_t cn   = st.n;
    uint64_t csum = st.sum, csq = st.sumsq;
    *n = cn;
    if (cn < 2) { *mean = 0; *sigma = 0; return; }
    float m = (float)csum / (float)cn;
    float v = ((float)csq - (float)csum * m) / (float)(cn - 1);
    *mean  = m;
    *sigma = (v > 0.0f) ? sqrtf(v) : 0.0f;
}

/* ---- автотест ENC: FSM ---- */
#define ENC_POINTS    5u
#define ENC_EVENTS    1000u
#define ENC_PERIOD_US 1000u    /* 1 кГц: без наложений */
#define ENC_SETTLE_MS 150u     /* пауза на установление baseline */

static const uint16_t enc_amps[ENC_POINTS] = { 150, 300, 800, 1600, 2800 };

static enum { ST_IDLE, ST_SETTLE, ST_COLLECT } enc_state = ST_IDLE;
static uint8_t  enc_pt;
static uint32_t enc_t0;
static float    enc_mean[ENC_POINTS], enc_sig[ENC_POINTS];
static pg_mode_t saved_mode;

static void enc_start_point(uint32_t now)
{
    pulsegen_set_mode(PG_MONO, enc_amps[enc_pt], ENC_PERIOD_US);
    st.active = false;          /* settle: статистику не копим */
    enc_t0    = now;
    enc_state = ST_SETTLE;
}

static void enc_finish(void)
{
    /* линейность: МНК-прямая ch(amp), макс. отклонение в % шкалы */
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (uint32_t i = 0; i < ENC_POINTS; i++) {
        float x = (float)enc_amps[i], y = enc_mean[i];
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    float nb  = (float)ENC_POINTS;
    float den = nb * sxx - sx * sx;
    float k   = (nb * sxy - sx * sy) / den;
    float b   = (sy - k * sx) / nb;
    float inl = 0;
    for (uint32_t i = 0; i < ENC_POINTS; i++) {
        float r = enc_mean[i] - (k * (float)enc_amps[i] + b);
        if (r < 0) r = -r;
        if (r > inl) inl = r;
    }
    float inl_pct = 100.0f * inl / 8192.0f;

    char rep[360];
    int o = snprintf(rep, sizeof(rep), "ENC test (%u ev/pt @%u Hz):\r\n",
                     (unsigned)ENC_EVENTS, (unsigned)(1000000u / ENC_PERIOD_US));
    for (uint32_t i = 0; i < ENC_POINTS; i++) {
        o += snprintf(rep + o, sizeof(rep) - (size_t)o,
                      "A=%4u: ch=%7.1f sig=%5.2f FWHM=%5.2f ch\r\n",
                      enc_amps[i], (double)enc_mean[i], (double)enc_sig[i],
                      (double)(2.355f * enc_sig[i]));
    }
    snprintf(rep + o, sizeof(rep) - (size_t)o,
             "k=%.4f ch/code  INL max=%.3f%%FS\r\n-ok",
             (double)k, (double)inl_pct);
    app_send_text(rep);

    pulsegen_set_mode(saved_mode, 0, 0);
    st.active = false;
    enc_state = ST_IDLE;
}

void selftest_poll(uint32_t now)
{
    switch (enc_state) {
    case ST_SETTLE:
        if (now - enc_t0 >= ENC_SETTLE_MS) {
            stats_reset();
            enc_state = ST_COLLECT;
        }
        break;
    case ST_COLLECT:
        if (st.n >= ENC_EVENTS) {
            uint32_t n;
            stats_get(&enc_mean[enc_pt], &enc_sig[enc_pt], &n);
            enc_pt++;
            if (enc_pt >= ENC_POINTS)
                enc_finish();
            else
                enc_start_point(now);
        }
        break;
    default:
        break;
    }
}

void selftest_init(void)
{
    st.active = false;
    enc_state = ST_IDLE;
}

bool selftest_command(const char *args)
{
    while (*args == ' ') args++;

    if (strncmp(args, "off", 3) == 0) {
        pulsegen_set_mode(PG_OFF, 0, 0);
        st.active = false;
        app_send_text("-ok");
        return true;
    }
    if (strncmp(args, "spec", 4) == 0) {
        pulsegen_set_mode(PG_SPECTRUM, 0, 0);
        st.active = false;
        app_send_text("-ok");
        return true;
    }
    if (strncmp(args, "mono", 4) == 0) {
        const char *p = args + 4;
        uint16_t amp = (uint16_t)strtoul(p, (char **)&p, 10);
        uint32_t per = (uint32_t)strtoul(p, (char **)&p, 10);
        if (amp == 0) { app_send_text("-err amp?"); return true; }
        if (per == 0) per = 1000u;
        pulsegen_set_mode(PG_MONO, amp, per);
        stats_reset();
        app_send_text("-ok");
        return true;
    }
    if (strncmp(args, "stat", 4) == 0) {
        float m, s;
        uint32_t n;
        stats_get(&m, &s, &n);
        char rep[120];
        snprintf(rep, sizeof(rep),
                 "n=%lu mean=%.1f sig=%.2f FWHM=%.2f min=%u max=%u\r\n-ok",
                 (unsigned long)n, (double)m, (double)s, (double)(2.355f * s),
                 st.min, st.max);
        app_send_text(rep);
        return true;
    }
    if (strncmp(args, "enc", 3) == 0) {
        if (enc_state != ST_IDLE) { app_send_text("-err busy"); return true; }
        saved_mode = pulsegen_mode();
        enc_pt = 0;
        enc_start_point(app_port_millis());
        app_send_text("-ok enc started, ~6s");
        return true;
    }
    app_send_text("-err use: off|spec|mono <amp> [t_us]|stat|enc");
    return true;
}
