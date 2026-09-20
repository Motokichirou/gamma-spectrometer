/* dsp.c — детектор импульсов с LSQ-фитом вершины (см. dsp.h) */

#include "dsp.h"

/* Горячий путь: форсируем оптимизацию даже в Debug-сборке.
 * С -O0 обработка полубуфера (4096 сэмплов @ 2.83 Мвыб/с) не успевает
 * за 1.45 мс — ISR работает без пауз и душит главный цикл. */
#pragma GCC optimize ("O2")

void dsp_init(dsp_t *d, int8_t polarity, uint16_t threshold,
              uint16_t min_width, uint16_t max_width,
              uint8_t fit_halfwidth, dsp_emit_fn emit)
{
    d->polarity      = polarity;
    d->threshold     = threshold;
    d->min_width     = min_width;
    d->max_width     = (max_width < DSP_PULSE_BUF) ? max_width
                                                   : (DSP_PULSE_BUF - 1u);
    d->fit_halfwidth = fit_halfwidth;
    d->emit          = emit;
    d->baseline_q8   = 0;
    d->in_pulse      = false;
    d->falling       = false;
    d->pileup        = false;
    d->armed         = true;
    d->fall_min      = 0;
    d->width         = 0;
    d->prev_dev      = 0;
    d->peak          = 0;
    d->imax          = 0;
    d->buf_len       = 0;
    d->post_left     = 0;
    d->accepted      = 0;
    d->rejected      = 0;
    d->tpl           = 0;
    d->tpl_nph       = 0;
    d->tpl_ns        = 0;
    d->tpl_ipk       = 0;
    d->tpl_q         = 0;
}

bool dsp_set_template(dsp_t *d, const int16_t *tpl, uint8_t nph, uint8_t ns,
                      uint8_t ipk, uint8_t q)
{
    if (tpl == 0) {                 /* выключение: возврат к параболе */
        d->tpl = 0;
        return true;
    }
    /* ns — uint8_t, поэтому в буфер (DSP_PULSE_BUF = 256) помещается всегда */
    if (nph == 0u || nph > DSP_TPL_MAX_NPH || ns < DSP_TPL_MIN_PTS ||
        ipk >= ns || q == 0u || q > 15u)
        return false;

    /* суммы строк не зависят от данных — считаем один раз (горячий путь короче вдвое) */
    for (unsigned p = 0; p < nph; p++) {
        const int16_t *h = tpl + (size_t)p * (size_t)ns;
        int32_t sh = 0, hh = 0;
        for (unsigned k = 0; k < ns; k++) {
            sh += h[k];
            hh += (int32_t)h[k] * h[k];
        }
        d->tpl_sh[p] = sh;
        d->tpl_hh[p] = hh;
        float den = (float)ns * (float)hh - (float)sh * (float)sh;
        d->tpl_ivd[p] = (den > 0.0f) ? (1.0f / den) : 0.0f;   /* деления в горячем пути нет */
    }
    d->tpl     = tpl;
    d->tpl_nph = nph;
    d->tpl_ns  = ns;
    d->tpl_ipk = ipk;
    d->tpl_q   = q;
    return true;
}

uint16_t dsp_baseline(const dsp_t *d)
{
    return (uint16_t)(d->baseline_q8 >> 8);
}

/* Квадратичный LSQ-фит y = a·x² + b·x + c по окну вокруг максимума.
 * x отсчитывается от позиции максимума; окно может быть обрезано краями
 * импульса (общее решение 3×3 по Крамеру). Амплитуда = высота вершины параболы;
 * при вырожденной кривизне — сглаженный центр (c).
 * Возвращает амплитуду в половинах LSB. */
static uint16_t fit_amplitude(const dsp_t *d)
{
    int m  = d->fit_halfwidth;
    int lo = (int)d->imax - m;
    int hi = (int)d->imax + m;
    if (lo < 0) lo = 0;
    if (hi > (int)d->buf_len - 1) hi = (int)d->buf_len - 1;

    if (hi - lo + 1 < 3) {
        /* окно вырождено — fallback: пик как есть */
        return (uint16_t)(2 * (int32_t)d->peak);
    }

    float S0 = 0, S1 = 0, S2 = 0, S3 = 0, S4 = 0;
    float T0 = 0, T1 = 0, T2 = 0;
    for (int i = lo; i <= hi; i++) {
        float x  = (float)(i - (int)d->imax);
        float y  = (float)d->buf[i];
        float x2 = x * x;
        S0 += 1.0f;  S1 += x;      S2 += x2;
        S3 += x2 * x; S4 += x2 * x2;
        T0 += y;     T1 += x * y;  T2 += x2 * y;
    }

    /* Крамер для | S0 S1 S2 | (c)   (T0)
     *            | S1 S2 S3 | (b) = (T1)
     *            | S2 S3 S4 | (a)   (T2) */
    float det = S0 * (S2 * S4 - S3 * S3)
              - S1 * (S1 * S4 - S3 * S2)
              + S2 * (S1 * S3 - S2 * S2);
    float amp;
    if (det > 1e-3f || det < -1e-3f) {
        float c = (T0 * (S2 * S4 - S3 * S3)
                 - S1 * (T1 * S4 - S3 * T2)
                 + S2 * (T1 * S3 - S2 * T2)) / det;
        float b = (S0 * (T1 * S4 - T2 * S3)
                 - T0 * (S1 * S4 - S3 * S2)
                 + S2 * (S1 * T2 - T1 * S2)) / det;
        float a = (S0 * (S2 * T2 - S3 * T1)
                 - S1 * (S1 * T2 - S3 * T0)
                 + T0 * (S1 * S3 - S2 * S2)) / det;

        if (a < -1e-4f) {
            float xv = -b / (2.0f * a);          /* позиция вершины */
            if (xv > (float)-m && xv < (float)m)
                amp = c - (b * b) / (4.0f * a);  /* высота вершины */
            else
                amp = c;                         /* вершина вне окна — центр */
        } else {
            amp = c;   /* плоско/вогнуто: сглаженное значение центра */
        }
    } else {
        amp = (float)d->peak;
    }

    if (amp < 0.0f) amp = 0.0f;
    float amp2 = 2.0f * amp + 0.5f;
    if (amp2 > 65535.0f) amp2 = 65535.0f;
    return (uint16_t)amp2;
}

/* Подгонка окна импульса по эталону формы (см. dsp.h).
 * Для каждой суб-отсчётной фазы p решается МНК с ДВУМЯ параметрами:
 *     y ≈ A·h_p + B     (A — амплитуда, B — остаточное смещение базовой линии)
 *     A = (n·Σyh − Σy·Σh) / (n·Σh² − (Σh)²),   фаза выбирается по минимуму невязки,
 *     что равносильно максимуму (n·Σyh − Σy·Σh)² / (n·Σh² − (Σh)²).
 * Свободный член B нужен: хвост импульса уходит ниже нуля и утягивает EMA базовой
 * линии, а у слабых импульсов ошибка базы в 1 код давала бы −2 % по амплитуде.
 * С ним смещение ≈ 0 ценой роста σ с 0.35 % до 0.5 % (при 59.5 кэВ) — это много
 * меньше собственного разрешения сцинтиллятора.
 *
 * Цена на МК: суммы строк (Σh, Σh², 1/знаменатель) посчитаны в dsp_set_template,
 * в горячем пути остаётся Σy·h — одно умножение с накоплением на точку. Фаза ищется
 * в два этапа: грубо с шагом nph/8, затем уточнение вокруг лучшей (14 фаз из 32
 * вместо 32) — кривая невязки по фазе одномодальная, максимум не теряется.
 * Суммы берутся только по точкам окна, которые есть в буфере (у импульсов
 * у порога хвост обрезан). Возвращает амплитуду в половинах LSB, 0 — если не вышло. */
typedef struct {
    const dsp_t *d;
    int   base, k0, k1, ns;
    int32_t sy;
    bool  full;
    float best_score, best_amp;
} tpl_ctx_t;

static void tpl_eval(tpl_ctx_t *c, int p)
{
    const dsp_t *d = c->d;
    const int16_t *h = d->tpl + (size_t)p * (size_t)c->ns;
    int32_t yh = 0, hh, sh;
    float ivd;

    if (c->full) {
        for (int k = 0; k < c->ns; k++)
            yh += (int32_t)d->buf[c->base + k] * h[k];
        hh  = d->tpl_hh[p];
        sh  = d->tpl_sh[p];
        ivd = d->tpl_ivd[p];
    } else {
        hh = 0; sh = 0;
        for (int k = c->k0; k < c->k1; k++) {
            int32_t hv = h[k];
            yh += (int32_t)d->buf[c->base + k] * hv;
            hh += hv * hv;
            sh += hv;
        }
        float den = (float)(c->k1 - c->k0) * (float)hh - (float)sh * (float)sh;
        if (den <= 0.0f)
            return;
        ivd = 1.0f / den;
    }

    float num = (float)(c->k1 - c->k0) * (float)yh - (float)c->sy * (float)sh;
    if (num <= 0.0f || ivd <= 0.0f)
        return;
    float amp   = num * ivd;
    float score = num * amp;                     /* = num²/den */
    if (score > c->best_score) {
        c->best_score = score;
        c->best_amp   = amp;
    }
}

static uint16_t fit_template(const dsp_t *d)
{
    const int ns   = (int)d->tpl_ns;
    const int nph  = (int)d->tpl_nph;
    const int ipk  = (int)d->tpl_ipk;
    const int base = (int)d->imax - ipk;          /* индекс buf для k = 0 */
    int k0 = (base < 0) ? -base : 0;
    int k1 = ((int)d->buf_len - base < ns) ? (int)d->buf_len - base : ns;

    if (k1 - k0 < DSP_TPL_MIN_PTS)
        return 0;

    tpl_ctx_t c;
    c.d = d; c.base = base; c.k0 = k0; c.k1 = k1; c.ns = ns;
    c.full = (k0 == 0 && k1 == ns);
    c.best_score = 0.0f; c.best_amp = 0.0f;
    c.sy = 0;
    for (int k = k0; k < k1; k++)                 /* Σy — от фазы не зависит */
        c.sy += d->buf[base + k];

    int step = nph / 8;
    if (step < 1)
        step = 1;
    int best_p = -1;
    for (int p = 0; p < nph; p += step) {         /* грубый проход */
        float prev = c.best_score;
        tpl_eval(&c, p);
        if (c.best_score > prev)
            best_p = p;
    }
    if (best_p < 0)
        return 0;
    for (int dp = -step + 1; dp < step; dp++) {   /* уточнение вокруг лучшей фазы */
        if (dp == 0)
            continue;
        int p = best_p + dp;
        if (p < 0)      p += nph;
        if (p >= nph)   p -= nph;
        tpl_eval(&c, p);
    }
    if (c.best_score <= 0.0f)
        return 0;

    float amp2 = c.best_amp * (float)(1u << d->tpl_q) * 2.0f + 0.5f;  /* половины LSB */
    if (amp2 < 0.0f)     amp2 = 0.0f;
    if (amp2 > 65535.0f) amp2 = 65535.0f;
    return (uint16_t)amp2;
}

/* Оценка амплитуды: эталон формы, если задан и применим; иначе парабола. */
static uint16_t estimate_amplitude(const dsp_t *d)
{
    if (d->tpl) {
        uint16_t a = fit_template(d);
        if (a)
            return a;
    }
    return fit_amplitude(d);
}

void dsp_process(dsp_t *d, const uint16_t *samples, size_t n)
{
    int32_t baseline_q8 = d->baseline_q8;
    const int32_t pol   = d->polarity;
    const int32_t thr   = d->threshold;

    for (size_t i = 0; i < n; i++) {
        int32_t s = (int32_t)samples[i];

        if (baseline_q8 == 0)
            baseline_q8 = s << 8;   /* стартовая инициализация */

        int32_t dev = pol * (s - (baseline_q8 >> 8));

        if (!d->in_pulse) {
            if (d->armed && dev > thr) {
                d->in_pulse = true;
                d->falling  = false;
                d->pileup   = false;
                d->width    = 1;
                d->peak     = (int16_t)dev;
                d->imax     = 0;
                d->buf_len  = 1;
                d->buf[0]   = (int16_t)dev;
                d->post_left = 0;
            } else {
                /* гистерезис: взвод триггера после спада ниже thr/2 —
                 * хвост импульса + шум не рождают призрачных событий */
                if (dev < thr / 2)
                    d->armed = true;
                if (dev > -thr && dev <= thr) {
                    /* EMA только в коридоре: ни импульсы, ни провалы
                     * входа не утаскивают baseline */
                    baseline_q8 += ((s << 8) - baseline_q8) >> 8;
                }
            }
        } else {
            d->width++;

            if (d->buf_len < DSP_PULSE_BUF) {
                d->buf[d->buf_len] = (int16_t)((dev > -32768 && dev < 32767)
                                               ? dev : 0);
                if (dev > (int32_t)d->peak) {
                    d->peak = (int16_t)dev;
                    d->imax = d->buf_len;
                    if (!d->pileup)
                        d->falling = false;   /* новый максимум = ещё растём */
                }
                d->buf_len++;
            }

            /* ретриггер-логика только для развитого пика (>=2*thr):
             * иначе шум на раннем подъёме даёт ложный pile-up */
            if ((int32_t)d->peak >= 2 * thr) {
                if (!d->falling && dev < ((int32_t)d->peak * 3) / 4) {
                    d->falling  = true;
                    d->fall_min = (int16_t)dev;
                } else if (d->falling) {
                    if (dev < (int32_t)d->fall_min) {
                        d->fall_min = (int16_t)dev;
                    } else {
                        int32_t rise_thr = thr;
                        if ((int32_t)d->peak / 4 > rise_thr)
                            rise_thr = (int32_t)d->peak / 4;
                        if (dev > (int32_t)d->fall_min + rise_thr &&
                            dev > ((int32_t)d->peak * 3) / 5)
                            d->pileup = true;   /* повторный рост от минимума спада */
                    }
                }
            }

            if (d->post_left) {
                /* дозапись хвоста: у слабых импульсов он уходит ниже порога,
                 * а окну эталона нужны все точки — иначе подгонка по обрезку
                 * даёт систематический сдвиг вверх (+1.4 % при 59.5 кэВ) */
                if (--d->post_left == 0 || d->buf_len >= DSP_PULSE_BUF) {
                    d->post_left = 0;
                    d->in_pulse  = false;
                    d->accepted++;
                    if (d->emit)
                        d->emit(estimate_amplitude(d));
                }
            } else if (d->width > d->max_width) {
                d->in_pulse = false;
                d->armed    = false;
                d->rejected++;
            } else if (dev < thr) {
                d->armed = false;      /* взвод — только после спада < thr/2 */
                if (!d->pileup && d->width >= d->min_width) {
                    int need = 0;
                    if (d->tpl) {      /* сколько точек окна эталона ещё не записано */
                        need = (int)d->imax + (int)d->tpl_ns - (int)d->tpl_ipk
                             - (int)d->buf_len;
                        if (need > (int)d->tpl_ns) need = (int)d->tpl_ns;
                        if (need < 0) need = 0;
                    }
                    if (need > 0 && d->buf_len < DSP_PULSE_BUF) {
                        d->post_left = (uint8_t)need;   /* завершим после дозаписи */
                    } else {
                        d->in_pulse = false;
                        d->accepted++;
                        if (d->emit)
                            d->emit(estimate_amplitude(d));
                    }
                } else {
                    d->in_pulse = false;
                    d->rejected++;
                }
            }
        }
    }

    d->baseline_q8 = baseline_q8;
}
