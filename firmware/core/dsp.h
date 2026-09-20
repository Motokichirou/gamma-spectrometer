/*
 * dsp.h — детектор импульсов: baseline-трекер, LSQ-фит вершины, PUR
 *
 * Переносимый модуль (без HAL). Этап 2 (Nucleo) и боевая плата.
 *
 * Оценка амплитуды — два режима:
 *   1) эталон формы (dsp_set_template): подгонка по таблице NPH суб-отсчётных фаз.
 *      Для каждой фазы — МНК по двум параметрам y ≈ A·h + B (амплитуда и остаточное
 *      смещение базы), фаза берётся по минимуму невязки. Свободный член B делает оценку
 *      нечувствительной к ошибке трекера базовой линии (её тянет вниз отрицательный
 *      хвост импульса; без B ошибка в 1 код давала бы −2 % при 59.5 кэВ).
 *      Амплитуда = пик АНАЛОГОВОГО импульса в кодах АЦП. Нужен на боевой плате:
 *      импульс короткий (FWHM 2.56 отсчёта @4 Мвыб/с), парабола занижает на 12–15 %
 *      и даёт разброс ~3 % по фазе прихода. Таблица — firmware/core/dsp_template.h.
 *   2) без эталона (по умолчанию) — квадратичный LSQ-фит (парабола) по окну 2m+1
 *      сэмплов вокруг максимума; амплитуда = высота вершины параболы. Годится для
 *      широкого пика петли Nucleo (этап 2). Если вершина плоская — центр окна.
 *
 * Полярность: +1 = импульсы вверх, −1 = вниз.
 * ВЕЗДЕ НУЖЕН +1 — и на петле Nucleo, и на боевой плате: анод ФЭУ забирает ток,
 * выход TIA растёт, импульс на АЦП идёт ВВЕРХ от baseline. amplitude = sample - baseline.
 * ⛔ Прежний комментарий «−1 = вниз (боевая плата)» ОШИБОЧЕН (исправлено 2026-08-07).
 */
#ifndef DSP_H
#define DSP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DSP_PULSE_BUF 256   /* макс. сэмплов одного импульса (≥ max_width) */
#define DSP_TPL_MIN_PTS 3   /* меньше точек в окне — подгонка по эталону не делается */
#define DSP_TPL_MAX_NPH 32  /* потолок числа фаз эталона; пред-суммы = 12 байт на фазу */

/* Валидный импульс. Аргумент — амплитуда в ПОЛОВИНАХ LSB ADC
 * (для 12-бит ADC это готовый индекс канала 0..8190). */
typedef void (*dsp_emit_fn)(uint16_t amplitude_half_lsb);

typedef struct {
    /* конфигурация */
    int8_t   polarity;       /* +1 / -1 */
    uint16_t threshold;      /* порог отклонения от baseline, ADC-коды */
    uint16_t min_width;      /* мин. ширина импульса, сэмплы */
    uint16_t max_width;      /* макс. ширина (дольше = pile-up/наводка) */
    uint8_t  fit_halfwidth;  /* m: окно фита = 2m+1 точек вокруг максимума
                                (боевая плата @4Мвыб/с: m=3 → 7 точек;
                                 этап 2 @2.83Мвыб/с, широкий пик: m=8) */
    dsp_emit_fn emit;

    /* эталон формы (NULL = парабола) */
    const int16_t *tpl;      /* таблица [nph][ns], формат Q(tpl_q) */
    uint8_t  tpl_nph;        /* число суб-отсчётных фаз */
    uint8_t  tpl_ns;         /* отсчётов в окне */
    uint8_t  tpl_ipk;        /* позиция максимума строки в окне */
    uint8_t  tpl_q;          /* формат: 1.0 == (1 << tpl_q) */
    int32_t  tpl_sh[DSP_TPL_MAX_NPH];   /* Σh по строке   — для полного окна */
    int32_t  tpl_hh[DSP_TPL_MAX_NPH];   /* Σh² по строке  — считаются в dsp_set_template */
    float    tpl_ivd[DSP_TPL_MAX_NPH];  /* 1/(ns·Σh² − (Σh)²) — знаменатель МНК, полное окно */

    /* состояние */
    int32_t  baseline_q8;    /* baseline × 256 (EMA в коридоре ±threshold) */
    bool     in_pulse;
    bool     falling;
    bool     pileup;
    bool     armed;          /* гистерезис: новый триггер только после спада < thr/2 */
    int16_t  fall_min;       /* минимум спада (для детекта повторного роста) */
    uint16_t width;
    uint16_t prev_dev;
    int16_t  peak;
    uint16_t imax;           /* индекс максимума в buf */
    uint16_t buf_len;
    uint8_t  post_left;      /* дозапись хвоста после спада (нужна эталону) */
    int16_t  buf[DSP_PULSE_BUF];   /* отклонения сэмплов импульса */

    /* статистика */
    uint32_t accepted;
    uint32_t rejected;
} dsp_t;

void dsp_init(dsp_t *d, int8_t polarity, uint16_t threshold,
              uint16_t min_width, uint16_t max_width,
              uint8_t fit_halfwidth, dsp_emit_fn emit);

void dsp_process(dsp_t *d, const uint16_t *samples, size_t n);

uint16_t dsp_baseline(const dsp_t *d);

/* Включить подгонку по эталону формы. tpl == NULL возвращает параболу.
 * tpl: таблица [nph][ns] в формате Q(q), максимум каждой строки — в позиции ipk.
 * Готовая таблица боевой платы — dsp_template.h (генератор: simulation/fix_c320/gen_template.py).
 * Возвращает false при негодных параметрах (эталон тогда не включается). */
bool dsp_set_template(dsp_t *d, const int16_t *tpl, uint8_t nph, uint8_t ns,
                      uint8_t ipk, uint8_t q);

#endif /* DSP_H */
