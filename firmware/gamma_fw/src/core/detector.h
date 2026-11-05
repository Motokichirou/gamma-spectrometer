/*
 * detector.h — детектор импульсов сигнального АЦП (SPEC §7.1–7.4). Переносимый модуль.
 *
 * Обработка порциями в два уровня:
 *  1) быстрый проход: поиск первого отсчёта выше база + порог (по два отсчёта за слово,
 *     на ARM — UQSUB16) и суммы Σs, Σs² по спокойным участкам для базы и σ шума;
 *  2) медленный путь — только от найденного отсчёта: машина состояний из core/dsp.c
 *     (гистерезис, мин./макс. ширина, повторный рост на спаде = наложение), затем
 *     подгонка эталона 64 фазы × 7 отсчётов (грубо по 16 фазам, уточнение ±2) и
 *     отбраковка по χ² (выбросы помехи 257 кГц шириной 1–2 отсчёта).
 *
 * Подгонка: y = s − база (база — среднее по спокойным участкам), МНК с ОДНИМ параметром
 * y ≈ A·h. Свободный член B (как в dsp.c) здесь не нужен: база меряется по тысячам
 * отсчётов, а не EMA, которую тянет хвост; с B разброс амплитуды на 30 % больше
 * (σ 4.0 против 2.9 канала у Cs-137 при σ шума 1.8 LSB).
 *
 * χ² = невязка / (σ² + (ε·A)²), ε = DET_SHAPE_EPS: член ε·A учитывает неточность эталона
 * (шаг фазы 1/64 отсчёта даёт до 1.2 % от A) — без него крупные импульсы
 * отбраковывались бы целиком (χ² до 500 у 3.5 МэВ).
 *
 * Импульс на стыке порций переносится: состояние живёт в det_t между вызовами.
 * Канал = round(2·A·gain), 0…8191 (половины LSB).
 */
#ifndef DETECTOR_H
#define DETECTOR_H

#include <stdint.h>
#include <stddef.h>

#define DET_NS        7u     /* отсчётов в окне эталона */
#define DET_IPK       1u     /* позиция максимума в окне */
#define DET_NPH       64u    /* фаз эталона */
#define DET_Q         14u    /* формат эталона Q14 */
#define DET_COARSE    4u     /* шаг грубого поиска фазы (64/4 = 16 фаз) */
#define DET_GUARD     16u    /* отсчётов после импульса не идут в базу (отрицательный хвост) */
#define DET_MAX_W     64u    /* потолок max_w */
#define DET_BUF       (DET_IPK + DET_MAX_W + DET_NS + 2u)
#define DET_SHAPE_EPS 0.01f  /* точность эталона относительно A (для χ²) */
#define DET_VAR_MIN   0.25f  /* нижний предел σ² (квантование, тихий вход) */
#define DET_CHANNELS  8192u

/* умолчания (SPEC §7.3) */
#define DET_THR_DEF   9u     /* ≈ 5σ шума базы */
#define DET_MINW_DEF  2u
#define DET_MAXW_DEF  16u

typedef void (*det_emit_fn)(uint16_t channel);          /* 0..8191, половины LSB */

typedef struct {
    uint16_t thr;            /* порог над базой, коды АЦП */
    uint8_t  min_w, max_w;   /* ширина — число отсчётов выше порога */
    float    chi2_max;       /* <= 0 — по умолчанию 4·DET_NS */
} det_cfg_t;

typedef struct {
    uint32_t accepted, rej_pileup, rej_shape, rej_width;
    uint64_t dead_samples;
} det_stats_t;

typedef struct {
    /* конфигурация */
    det_cfg_t cfg;
    float     chi2_max;
    float     gain;
    const int16_t (*tpl)[DET_NS];
    det_emit_fn emit;
    float     tpl_ihh[DET_NPH];   /* 1/Σh² по строке */
    int32_t   tpl_sh[DET_NPH];    /* Σh по строке */

    /* база и шум */
    int       init;
    float     base, var;
    int32_t   bint;               /* round(base) на текущую порцию */
    uint16_t  last;               /* последний отсчёт предыдущей порции (до-триггерный) */
    uint32_t  qn;                 /* спокойные отсчёты с прошлого обновления базы */
    uint64_t  qsum, qsq;          /* Σs, Σs² по ним */
    int32_t   pbint;              /* база, от которой считан текущий импульс */
    float     pbase;

    /* медленный путь */
    uint8_t   state;              /* DET_S_* в detector.c */
    uint8_t   armed;
    uint8_t   falling, pileup, npile;
    uint8_t   post_left;
    uint16_t  width;              /* отсчётов выше порога */
    uint16_t  guard_left;
    int16_t   peak, lpeak, fall_min;
    uint16_t  imax, len;
    int16_t   buf[DET_BUF];       /* s − bint */

    det_stats_t st;
} det_t;

void  det_init(det_t *d, const det_cfg_t *cfg, const int16_t (*tpl)[DET_NS], det_emit_fn emit);
/* порция; импульс на стыке переносится в следующую */
void  det_process_block(det_t *d, const uint16_t *s, size_t n);
/* потерянная порция (переполнение DMA): n отсчётов — в мёртвое время */
void  det_add_lost(det_t *d, uint32_t n);
float det_baseline(const det_t *d);
float det_sigma(const det_t *d);
void  det_get_stats(const det_t *d, det_stats_t *st);
/* термокомпенсация: канал = round(2·A·g) */
void  det_set_gain(det_t *d, float g);

#endif
