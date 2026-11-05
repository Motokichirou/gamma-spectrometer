/* detector.c — детектор импульсов: быстрый проход + медленный путь (см. detector.h). */
#include "detector.h"
#include "ccm.h"
#include <string.h>

#if defined(__ARM_FEATURE_SIMD32) && __ARM_FEATURE_SIMD32
#include <arm_acle.h>
#define UQSUB16(a, b)      ((uint32_t)__uqsub16((a), (b)))
#define SMLALD(a, b, acc)  ((int64_t)__smlald((int32_t)(a), (int32_t)(b), (acc)))
#else
/* ПК: скалярная модель тех же инструкций — тесты гоняют тот же цикл, что и МК */
static inline uint32_t UQSUB16(uint32_t a, uint32_t b)
{
    uint32_t lo = ((a & 0xFFFFu) > (b & 0xFFFFu)) ? (a & 0xFFFFu) - (b & 0xFFFFu) : 0u;
    uint32_t hi = ((a >> 16) > (b >> 16)) ? (a >> 16) - (b >> 16) : 0u;
    return lo | (hi << 16);
}
static inline int64_t SMLALD(uint32_t a, uint32_t b, int64_t acc)
{
    return acc + (int64_t)(int16_t)(a & 0xFFFFu) * (int16_t)(b & 0xFFFFu)
               + (int64_t)(int16_t)(a >> 16) * (int16_t)(b >> 16);
}
#endif

enum { DET_S_IDLE = 0, DET_S_PULSE, DET_S_POST, DET_S_GUARD };

static int32_t iround(float x) { return (int32_t)(x >= 0.0f ? x + 0.5f : x - 0.5f); }

/* int64 → float без библиотечного __aeabi_l2f (точно хватает при |x| < 2^47) */
static inline float i64f(int64_t x)
{
    return (float)(int32_t)(x >> 16) * 65536.0f + (float)(uint32_t)(x & 0xFFFF);
}

static int16_t clip16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

void det_init(det_t *d, const det_cfg_t *cfg, const int16_t (*tpl)[DET_NS], det_emit_fn emit)
{
    memset(d, 0, sizeof(*d));
    d->cfg = *cfg;
    if (d->cfg.min_w < 1u)          d->cfg.min_w = 1u;
    if (d->cfg.max_w > DET_MAX_W)   d->cfg.max_w = DET_MAX_W;
    if (d->cfg.max_w < d->cfg.min_w) d->cfg.max_w = d->cfg.min_w;
    d->chi2_max = (cfg->chi2_max > 0.0f) ? cfg->chi2_max : 4.0f * (float)DET_NS;
    d->gain = 1.0f;
    d->emit = emit;
    d->armed = 1;
    det_set_template(d, tpl);
}

void det_set_template(det_t *d, const int16_t (*tpl)[DET_NS])
{
    for (unsigned p = 0; p < DET_NPH; p++) {
        int32_t sh = 0, hh = 0;
        for (unsigned k = 0; k < DET_NS; k++) {
            sh += tpl[p][k];
            hh += (int32_t)tpl[p][k] * tpl[p][k];
        }
        d->tpl_sh[p]  = sh;
        d->tpl_ihh[p] = (hh > 0) ? 1.0f / (float)hh : 0.0f;
    }
    d->tpl = tpl;
}

void det_set_gain(det_t *d, float g) { d->gain = g; }
float det_baseline(const det_t *d) { return d->base; }

float det_sigma(const det_t *d)
{
    /* sqrt без libm: две итерации Ньютона от грубой оценки достаточно для статуса */
    float v = d->var, x;
    if (v <= 0.0f)
        return 0.0f;
    x = (v > 1.0f) ? v * 0.5f : 1.0f;
    for (int i = 0; i < 8; i++)
        x = 0.5f * (x + v / x);
    return x;
}

void det_get_stats(const det_t *d, det_stats_t *st) { *st = d->st; }
void det_add_lost(det_t *d, uint32_t n) { d->st.dead_samples += n; }

/* Первая порция: база по началу порции (без отсчётов выше среднего + порог). */
static void init_base(det_t *d, const uint16_t *s, size_t n)
{
    size_t m = (n < 256u) ? n : 256u;
    uint32_t sum = 0;
    for (size_t i = 0; i < m; i++) sum += s[i];
    float mean = (float)sum / (float)m;
    float lim = mean + (float)d->cfg.thr;
    float s1 = 0.0f, s2 = 0.0f;
    unsigned c = 0;
    for (size_t i = 0; i < m; i++) {
        if ((float)s[i] <= lim) { s1 += s[i]; c++; }
    }
    if (c) mean = s1 / (float)c;
    c = 0;
    for (size_t i = 0; i < m; i++) {
        float x = (float)s[i] - mean;
        if ((float)s[i] <= lim) { s2 += x * x; c++; }
    }
    d->base = mean;
    d->var  = c > 1u ? s2 / (float)(c - 1u) : 4.0f;
    d->bint = iround(mean);
    d->last = (uint16_t)(d->bint < 0 ? 0 : d->bint);
    d->init = 1;
}

/*
 * Обновление базы и σ² по накопленным спокойным отсчётам (EMA, α = 1/2 на ≥4096 отсчётов).
 * Суммы приводятся к сдвигу bint в целых (без double и библиотечных вызовов): Σ(s−b), Σ(s−b)².
 * Среднее ушло больше чем на порог (ступенька базы вниз) — база ставится сразу, без EMA.
 */
static void update_base(det_t *d)
{
    if (d->qn < DET_BASE_N)
        return;
    const int64_t b = d->bint, n = (int64_t)d->qn;
    const int64_t s1 = (int64_t)d->qsum - n * b;
    const int64_t s2 = (int64_t)d->qsq - 2 * b * (int64_t)d->qsum + n * b * b;
    const float fn = (float)d->qn;
    const float m = i64f(s1) / fn;
    float v = i64f(s2) / fn - m * m;
    if (v < 0.0f) v = 0.0f;
    const float mb = (float)d->bint + m;
    const float dm = mb - d->base;
    if (dm > (float)d->cfg.thr || dm < -(float)d->cfg.thr) {
        d->base = mb;
        d->var  = v;
        d->st.base_jumps++;
    } else {
        d->base += 0.5f * dm;
        d->var  += 0.5f * (v - d->var);
    }
    d->bint  = iround(d->base);
    d->qn = 0; d->qsum = 0; d->qsq = 0;
}

static inline void stuck_reset(det_t *d) { d->stk_n = 0; d->stk_s1 = 0; d->stk_s2 = 0; }

/*
 * Ступенька базы вверх больше порога: DET_BASE_N отсчётов медленного пути подряд не ниже
 * thr/2 — база по ним, начатый импульс бросается, детектор в IDLE со взводом.
 */
static void rebase_stuck(det_t *d)
{
    const float fn = (float)d->stk_n;
    const float m = (float)d->stk_s1 / fn;
    float v = i64f(d->stk_s2) / fn - m * m;
    d->base = (float)d->bint + m;
    d->var  = (v > 0.0f) ? v : 0.0f;
    d->bint = iround(d->base);
    d->qn = 0; d->qsum = 0; d->qsq = 0;
    d->state = DET_S_IDLE;
    d->armed = 1;
    d->guard_left = 0;
    d->st.base_jumps++;
    stuck_reset(d);
}

/* Подгонка эталона по окну buf[imax−IPK … +NS). Возвращает 1 и канал, 0 — отбраковка. */
CCM_FUNC static int fit(det_t *d, uint16_t *ch)
{
    const int16_t *y = d->buf + (d->imax - DET_IPK);
    const float frac = d->pbase - (float)d->pbint;      /* y_ист = y − frac */
    const int32_t thr = d->cfg.thr;
    int32_t sy = 0, syy = 0;
    for (unsigned k = 0; k < DET_NS; k++) {
        sy  += y[k];
        syy += (int32_t)y[k] * y[k];
    }
    /* выброс 1–2 отсчёта: подряд выше thr/2 вокруг пика меньше DET_MIN_HALF отсчётов */
    unsigned run = 1;
    for (unsigned k = DET_IPK + 1u; k < DET_NS && 2 * (int32_t)y[k] > thr; k++) run++;
    for (int k = (int)DET_IPK - 1; k >= 0 && 2 * (int32_t)y[k] > thr; k--) run++;
    if (run < DET_MIN_HALF)
        return 0;
    const float Syy = (float)syy - 2.0f * frac * (float)sy + (float)DET_NS * frac * frac;

    float best = 0.0f, best_yh = 0.0f;
    int bp = -1;
    for (int p = 0; p < (int)DET_NPH; p += (int)DET_COARSE) {     /* грубо: 16 фаз */
        const int16_t *h = d->tpl[p];
        int32_t yh = 0;
        for (unsigned k = 0; k < DET_NS; k++) yh += (int32_t)y[k] * h[k];
        float YH = (float)yh - frac * (float)d->tpl_sh[p];
        if (YH <= 0.0f) continue;
        float sc = YH * YH * d->tpl_ihh[p];
        if (sc > best) { best = sc; best_yh = YH; bp = p; }
    }
    if (bp < 0)
        return 0;
    const int c = bp;
    for (int dp = -2; dp <= 2; dp++) {                             /* уточнение ±2 из 64 */
        if (dp == 0) continue;
        int p = (c + dp) & (int)(DET_NPH - 1u);
        const int16_t *h = d->tpl[p];
        int32_t yh = 0;
        for (unsigned k = 0; k < DET_NS; k++) yh += (int32_t)y[k] * h[k];
        float YH = (float)yh - frac * (float)d->tpl_sh[p];
        if (YH <= 0.0f) continue;
        float sc = YH * YH * d->tpl_ihh[p];
        if (sc > best) { best = sc; best_yh = YH; bp = p; }
    }

    const float A = best_yh * d->tpl_ihh[bp] * (float)(1u << DET_Q);  /* коды АЦП */
    float resid = Syy - best;
    if (resid < 0.0f) resid = 0.0f;
    float var = (d->var > DET_VAR_MIN) ? d->var : DET_VAR_MIN;
    float ea = DET_SHAPE_EPS * A;
    if (A <= 0.0f || resid > d->chi2_max * (var + ea * ea))
        return 0;
    float c2 = 2.0f * A * d->gain + 0.5f;
    if (c2 < 0.0f) c2 = 0.0f;
    if (c2 > (float)(DET_CHANNELS - 1u)) c2 = (float)(DET_CHANNELS - 1u);
    *ch = (uint16_t)c2;
    return 1;
}

CCM_FUNC static void finish(det_t *d)
{
    uint16_t ch;
    if (fit(d, &ch)) {
        d->st.accepted++;
        if (d->emit) d->emit(ch);
    } else {
        d->st.rej_shape++;
    }
}

static inline void to_guard(det_t *d)
{
    d->state = DET_S_GUARD;
    d->guard_left = DET_GUARD;
    d->armed = 0;
}

/* Начало импульса: pre — отсчёт до триггера (нужен окну эталона). */
static inline void start_pulse(det_t *d, uint16_t pre)
{
    d->state = DET_S_PULSE;
    d->pbint = d->bint;
    d->pbase = d->base;
    d->len = 1;
    d->buf[0] = clip16((int32_t)pre - d->pbint);
    d->peak = d->buf[0];
    d->imax = 0;
    d->lpeak = 0;
    d->falling = 0;
    d->pileup = 0;
    d->npile = 0;
    d->width = 0;
    d->post_left = 0;
}

/* Отсчёт внутри импульса (логика core/dsp.c). */
CCM_FUNC static void pulse_sample(det_t *d, int32_t dev)
{
    const int32_t thr = d->cfg.thr;
    d->st.dead_samples++;
    d->buf[d->len] = clip16(dev);
    if (dev > d->peak) { d->peak = clip16(dev); d->imax = d->len; }
    d->len++;
    if (dev > thr) d->width++;

    /* повторный рост на спаде = наложение; только для развитого пика (>= 2·thr) */
    if (!d->falling && dev > d->lpeak) d->lpeak = clip16(dev);
    if (d->lpeak >= 2 * thr) {
        if (!d->falling) {
            if (dev < (d->lpeak * 3) / 4) { d->falling = 1; d->fall_min = clip16(dev); }
        } else if (dev < d->fall_min) {
            d->fall_min = clip16(dev);
        } else {
            int32_t rise = thr;
            if (d->lpeak / 4 > rise) rise = d->lpeak / 4;
            if (dev > d->fall_min + rise && dev > (d->lpeak * 3) / 5) {
                d->pileup = 1;
                d->npile++;
                d->falling = 0;
                d->lpeak = clip16(dev);
            }
        }
    }

    if (d->width > d->cfg.max_w) {
        d->st.rej_width++;
        to_guard(d);
    } else if (dev <= thr) {                      /* спад ниже порога — конец импульса */
        if (d->pileup) {
            d->st.rej_pileup += 1u + d->npile;
            to_guard(d);
        } else if (d->width < d->cfg.min_w) {
            d->st.rej_width++;
            to_guard(d);
        } else {
            int need = (int)d->imax + (int)DET_NS - (int)DET_IPK - (int)d->len;
            if (need > 0) {
                d->state = DET_S_POST;            /* дописать хвост окна эталона */
                d->post_left = (uint8_t)need;
                d->armed = (dev < thr / 2);
            } else {
                finish(d);
                to_guard(d);
            }
        }
    }
}

/* Медленный путь от s[i]; выход — когда состояние стало IDLE или кончилась порция. */
CCM_FUNC static size_t slow(det_t *d, const uint16_t *s, size_t i, size_t n)
{
    const int32_t thr = d->cfg.thr;
    for (; i < n; i++) {
        uint16_t v = s[i];
        if (d->state == DET_S_IDLE)
            return i;
        {                                            /* ступенька базы вверх: см. rebase_stuck */
            int32_t db = (int32_t)v - d->bint;
            if (db < thr / 2) {                      /* то же условие, что взвод в GUARD */
                stuck_reset(d);
            } else {
                d->stk_n++;
                d->stk_s1 += db;
                d->stk_s2 += (int64_t)db * db;
                if (d->stk_n >= DET_BASE_N) {
                    rebase_stuck(d);
                    return i + 1;
                }
            }
        }
        switch (d->state) {
        case DET_S_PULSE:
            pulse_sample(d, (int32_t)v - d->pbint);
            break;
        case DET_S_POST: {
            int32_t dev = (int32_t)v - d->pbint;
            if (d->armed && dev > thr) {           /* новый импульс до конца окна */
                d->st.rej_pileup++;
                start_pulse(d, i ? s[i - 1] : d->last);
                pulse_sample(d, (int32_t)v - d->pbint);
                break;
            }
            if (dev < thr / 2) d->armed = 1;
            d->st.dead_samples++;
            d->buf[d->len++] = clip16(dev);
            if (--d->post_left == 0) {
                finish(d);
                int a = d->armed;
                to_guard(d);
                d->armed = (uint8_t)a;
            }
            break;
        }
        case DET_S_GUARD: {
            int32_t dev = (int32_t)v - d->bint;
            if (dev < thr / 2) d->armed = 1;
            if (d->armed && dev > thr) {
                start_pulse(d, i ? s[i - 1] : d->last);
                pulse_sample(d, (int32_t)v - d->pbint);
                break;
            }
            if (!d->armed) d->st.dead_samples++;
            if (d->guard_left) d->guard_left--;
            if (d->guard_left == 0 && d->armed) {
                d->state = DET_S_IDLE;
                return i + 1;
            }
            break;
        }
        default:
            return i;
        }
    }
    return i;
}

/* Быстрый проход: первый отсчёт > bint + thr; суммы Σs, Σs² по пройденным. */
CCM_FUNC static size_t fast_scan(det_t *d, const uint16_t *s, size_t i, size_t n)
{
    int32_t l = d->bint + (int32_t)d->cfg.thr;
    const uint32_t L = (uint32_t)(l < 0 ? 0 : (l > 65535 ? 65535 : l));
    const uint32_t LL = L | (L << 16);
    uint32_t sum = 0;
    int64_t sq = 0;
    size_t j = i;

    if (j < n && ((uintptr_t)(s + j) & 2u) && s[j] <= L) {   /* выравнивание на слово */
        sum += s[j]; sq += (int64_t)s[j] * s[j]; j++;
    }
    while (j + 2u <= n) {
        uint32_t w;
        memcpy(&w, s + j, 4);
        if (UQSUB16(w, LL)) break;                             /* один из двух выше порога */
        sum += (w & 0xFFFFu) + (w >> 16);
        sq = SMLALD(w, w, sq);
        j += 2u;
    }
    while (j < n && s[j] <= L) {                               /* хвост и точное место */
        sum += s[j]; sq += (int64_t)s[j] * s[j]; j++;
    }
    d->qsum += sum;
    d->qsq  += (uint64_t)sq;
    d->qn   += (uint32_t)(j - i);
    if (j < n && j > i) {           /* отсчёт перед триггером — уже фронт, из базы убрать */
        uint32_t p = s[j - 1];
        d->qsum -= p; d->qsq -= (uint64_t)p * p; d->qn--;
    }
    return j;
}

CCM_FUNC void det_process_block(det_t *d, const uint16_t *s, size_t n)
{
    if (n == 0)
        return;
    if (!d->init)
        init_base(d, s, n);
    size_t i = 0;
    while (i < n) {
        if (d->state != DET_S_IDLE) {
            i = slow(d, s, i, n);
            continue;
        }
        size_t j = fast_scan(d, s, i, n);
        if (j >= n)
            break;
        start_pulse(d, j ? s[j - 1] : d->last);
        stuck_reset(d);
        i = j;                       /* триггерный отсчёт — через slow (учёт ступеньки базы) */
    }
    d->last = s[n - 1];
    update_base(d);
}
