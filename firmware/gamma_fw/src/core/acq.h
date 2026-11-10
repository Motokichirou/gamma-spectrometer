/*
 * acq.h — учёт набора и сигнального тракта (SPEC §7.1, §7.5, §7.6, §7.10). Переносимый модуль.
 *
 * Учёт (acq_t): реальное время набора (-sta … -sto, накапливается, -rst обнуляет), мёртвое время
 * и отбракованные — по приращениям счётчиков детектора ЗА ВРЕМЯ НАБОРА (детектор работает и вне
 * набора: база, σ, осциллограф), живое = реальное − мёртвое. Мёртвое — det_stats_t.dead_samples
 * (импульсы, перегрузка, потерянные порции через det_add_lost), пересчёт в секунды — по частоте
 * выборки на момент учёта (acq_set_fs; -smp меняется только вне набора).
 * Раз в секунду acq_tick_1s: скорости за прошедшую секунду (вход = принятые + отбракованные,
 * принятые, отбракованные — независимо от набора) и загрузка ЦП = такты обработки / такты окна.
 *
 * Статус 0x04: elapsed = реальное время, с; cpu = acq.cpu_x10; cps = принятые за секунду;
 * invalid = отбракованные за набор с последнего -rst.
 *
 * Сервисные команды сигнального тракта (acq_handle; ответ одной строкой, proto допишет \r\n):
 *   -smp            → -ok smp=2.5 fs=3466667
 *   -smp 2|6        → выборка 2.5 / 6.5 такта; только вне набора (иначе -err acq)
 *   -opa            → -ok opa hs=0 pga=4   (быстрый режим OPAMP1–3, PGA OPAMP3; опыт, во флеш НЕ пишется)
 *   -opa hs 0|1 | -opa pga 2|4|8|16 → то же после смены; только вне набора (иначе -err acq)
 *   -noise          → -ok noise smp=… fs=… n=… mean=… sigma=… min=… max=… over=…
 *                     по следующей порции. mean и σ — по «тихим» отсчётам: без отсчётов выше
 *                     mean + 5σ и их окрестности −8…+48 (порог уточняется итерациями), n — их
 *                     число (без импульсов — 8192); min/max — по всей порции, over — отсчётов
 *                     порции выше итогового mean + 5σ (импульсы и выбросы помехи)
 *   -osc <n>        → записать n импульсов окном −8…+24 (1…max); по готовности или через
 *                     ACQ_OSC_TIMEOUT_MS приложение шлёт их кадрами 0x02 и строки acq_osc_line,
 *                     в конце "-ok osc done <k>". Только вне набора.
 *   -osc raw        → сырая порция 8192 отсчёта кадрами 0x02 (АЦП на время передачи
 *                     остановлен), затем ответ "-ok osc raw 8192". Только вне набора.
 */
#ifndef ACQ_H
#define ACQ_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "detector.h"

#define ACQ_OSC_TIMEOUT_MS 10000u
#define ACQ_NOISE_OVER_K   5.0f
#define ACQ_NOISE_PRE      8u      /* -noise: окрестность выброса, исключаемая из тихих: до … */
#define ACQ_NOISE_POST     48u     /* … и после (хвост шейпера ≈ 10 мкс) */
#define ACQ_NOISE_ITER     4

typedef struct {
    /* итоги последней секунды */
    uint32_t cps_in, cps_acc, cps_rej;
    uint16_t cpu_x10;
    /* набор */
    bool     run;
    uint32_t since_ms;             /* реальное время: начало текущего отрезка (millis) */
    uint64_t acc_ms;               /* накоплено (64 бита: набор дольше 49.7 сут, SPEC_logger.md §2.7) */
    double   dead_s;               /* мёртвое время набора, учтённое к a_prev */
    uint32_t invalid;              /* отбракованные за набор, учтённые к a_prev */
    det_stats_t a_prev;            /* счётчики на момент последнего учёта набора */
    det_stats_t r_prev;            /* счётчики на прошлый тик (скорости) */
    uint32_t fs_hz;
} acq_t;

uint32_t acq_rejected(const det_stats_t *s);   /* наложения + форма + ширина + бюджет */

void acq_init(acq_t *a, uint32_t fs_hz, const det_stats_t *st);
void acq_set_fs(acq_t *a, uint32_t fs_hz);     /* звать вне набора */
void acq_start(acq_t *a, uint32_t now_ms, const det_stats_t *st);
void acq_stop(acq_t *a, uint32_t now_ms, const det_stats_t *st);
void acq_clear(acq_t *a, uint32_t now_ms, const det_stats_t *st);   /* -rst: время и счёт с нуля */
/* busy_cyc — такты обработки сигнала за окно, win_cyc — такты окна (0 — загрузка 0) */
void acq_tick_1s(acq_t *a, const det_stats_t *st, uint32_t busy_cyc, uint32_t win_cyc);
uint64_t acq_real_ms(const acq_t *a, uint32_t now_ms);
/* раз в секунду: идущий отрезок — в накопленное (отрезок длиннее 49.7 сут не переполнит millis) */
void     acq_fold_time(acq_t *a, uint32_t now_ms);
float    acq_dead_s(const acq_t *a, const det_stats_t *st);           /* с текущим отрезком */
float    acq_live_s(const acq_t *a, uint32_t now_ms, const det_stats_t *st);
uint32_t acq_invalid(const acq_t *a, const det_stats_t *st);

/* статистика порции для -noise */
typedef struct { uint32_t n; float mean, sigma; uint16_t min, max; uint32_t over; } acq_noise_t;
void acq_noise(const uint16_t *s, size_t n, acq_noise_t *o);

/* строка сведений об окне осциллографа i (без \r\n) */
void acq_osc_line(char *out, size_t cap, unsigned i, const det_cap_t *c);

/* приложение для команд */
typedef struct {
    bool     (*acquiring)(void);
    bool     (*ready)(void);               /* сигнальный АЦП запущен */
    uint8_t  (*smp_get)(void);             /* 2 или 6 */
    bool     (*smp_set)(uint8_t smp);      /* 2 или 6; false — АЦП не ответил */
    uint32_t (*fs)(void);                  /* текущая выборка, Гц */
    bool     (*noise)(acq_noise_t *o);     /* следующая порция; false — нет (таймаут) */
    bool     (*osc_raw)(uint32_t *n);      /* выслать сырую порцию; n — отсчётов */
    bool     (*osc_arm)(uint16_t n);       /* начать запись n окон */
    uint16_t osc_max;
    void     (*opa_get)(int *hs, unsigned *pga);   /* NULL — команды -opa нет */
    void     (*opa_set)(int hs, unsigned pga);     /* pga 2|4|8|16; не сохраняется во флеш */
} acq_ctx_t;

/* true — команда сигнального тракта (ответ в reply), false — не наша. */
bool acq_handle(const acq_ctx_t *c, const char *cmd, char *reply, size_t cap);

#endif
