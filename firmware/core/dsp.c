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
    d->accepted      = 0;
    d->rejected      = 0;
}

uint16_t dsp_baseline(const dsp_t *d)
{
    return (uint16_t)(d->baseline_q8 >> 8);
}

/* Квадратичный LSQ-фит y = a·x² + b·x + c по окну вокруг максимума.
 * x отсчитывается от позиции максимума; окно может быть обрезано краями
 * импульса (общий解 3×3 по Крамеру). Амплитуда = высота вершины параболы;
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

            if (d->width > d->max_width) {
                d->in_pulse = false;
                d->armed    = false;
                d->rejected++;
            } else if (dev < thr) {
                d->in_pulse = false;
                d->armed    = false;   /* взвод — только после спада < thr/2 */
                if (!d->pileup && d->width >= d->min_width) {
                    d->accepted++;
                    if (d->emit)
                        d->emit(fit_amplitude(d));
                } else {
                    d->rejected++;
                }
            }
        }
    }

    d->baseline_q8 = baseline_q8;
}
