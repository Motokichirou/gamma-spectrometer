/*
 * hv_ctl.c — логика ВВ (SPEC §5). Перенос и переработка hv_* из board_g474cb/bringup/main.c,
 * проверенной на стенде 2026-11-05 (−1000 В ± 2 В на выходе при добавке ≈ +98 В).
 *
 * Отличия от прошивки запуска:
 *  - шаги рампы, фильтра монитора и интегратора считаются по фактическому dt (≤ 100 мс),
 *    а не «по тику», — пропуск тиков не ускоряет подъём;
 *  - защита по усреднённому монитору (шум цепочки 6.8 МОм ±40 В на 1200 В — SPEC §5);
 *  - у потолка АЦП монитор = «≈0, не точно»: подстройка не интегрирует;
 *  - подстройка ждёт, пока выход устоится после смены уставки (см. ниже «устоялся»).
 *
 * «Устоялся». После снижения уставки выход разряжается медленно (τ разряда 2…16 с) и монитор
 * долго стоит выше уставки. Если интегрировать сразу по окончании рампы, добавка уходит вниз,
 * стирает отмасштабированное значение и выход проседает на 30–40 В ниже уставки. Поэтому после
 * рампы: выдержка HOLD_MS, затем подстройка включается (и дальше не выключается до следующей
 * уставки), как только монитор либо вошёл в мёртвую зону, либо перестал идти к уставке:
 * идёт от неё, стоит, или идёт так медленно, что за SETTLE_TAU_S (больше наибольшей τ разряда)
 * не дошёл бы. Скорость оценивается разностью двух фильтров монитора: (mon_f − mon_slow)/SLOW_TAU_S.
 * У экспоненты с любой τ ≤ 16 с скорость ≥ |e|/16 с — значит, пока выход сам идёт к уставке,
 * подстройка ждёт; если добавка неверна, выход встаёт со смещением, скорость → 0 — подстройка пошла.
 * Условие должно держаться подряд SETTLE_CONFIRM_MS: иначе шум монитора (до ±40 В на 1200 В)
 * на мгновение роняет оценку скорости и включает подстройку, пока выход ещё разряжается.
 *
 * Защита "over". После большого снижения уставки без нагрузки (τ ≈ 16 с) выход к 30 с от команды
 * ещё на сотни вольт выше уставки (1200 → 100: ≈ 330 В), а 30 с SPEC считает от команды. Поэтому
 * "over" проверяется, только когда (а) с команды прошло > 30 с, (б) выход устоялся и (в) монитор
 * не падает. При настоящем перенапряжении монитор стоит или растёт — «устоялся» выставляется сразу
 * после выдержки, (в) выполнено; выход, сам разряжающийся к уставке, не проходит ни (б), ни (в).
 * "abs" (> 1300 В) от этого не зависит.
 *
 * Известное поведение без нагрузки: внешняя петля (τ ≈ 10 с) при инерции выхода τ ≈ 16 с даёт
 * перерегулирование, если после снижения уставки добавка неверна (модель 1000 → 500 при смене
 * коэффициента цепочки 0.916 → 0.98: выход доходит до ≈ 470 В). С делителем (τ ≈ 2 с) не проявляется.
 */
#include <stddef.h>
#include "hv_ctl.h"

#define DT_MAX_MS     100u
#define MON_TAU_S     1.0f    /* усреднение монитора */
#define TRIM_RATE     0.1f    /* (В trim)/(В ошибки · с): τ ≈ 10 с при выход ≈ 0.92·CW */
#define SLOW_TAU_S    5.0f    /* медленный фильтр монитора для оценки скорости */
#define HOLD_MS       6000u   /* выдержка после рампы: 3 с + 3·τ монитора */
#define SETTLE_TAU_S  20.0f   /* > τ разряда выхода (до 16 с) */
#define SETTLE_CONFIRM_MS 2000u /* условие «устоялся» держится подряд — шум монитора не обманет */

static float absf(float x) { return x < 0.0f ? -x : x; }

void hv_init(hv_ctl_t *h, float zero_mv, float k_mv_per_v)
{
    h->zero_mv = zero_mv;
    h->k_mv_per_v = k_mv_per_v;
    h->vref_mv = HV_VREF_MV;
    h->target = h->ramp = h->trim = 0.0f;
    h->mon_f = h->mon_slow = 0.0f;
    h->mon_seeded = h->near_ceil = h->en = false;
    h->at_target = h->settled = h->over_armed = false;
    h->settle_ms = 0;
    h->last_ms = h->changed_ms = h->at_target_ms = 0;
    h->trip = h->off_why = NULL;
}

int hv_set_vref(hv_ctl_t *h, float vref_mv)
{
    if (!(vref_mv >= 2000.0f && vref_mv <= 3400.0f)) return -1;   /* и NaN */
    h->vref_mv = vref_mv;
    return 0;
}

int hv_set_cal(hv_ctl_t *h, float zero_mv, float k_mv_per_v)
{
    if (h->en || !(k_mv_per_v > 0.0f) || !(zero_mv == zero_mv)) return -1;
    h->zero_mv = zero_mv;
    h->k_mv_per_v = k_mv_per_v;
    h->mon_seeded = false;            /* фильтры монитора — заново, по новой калибровке */
    return 0;
}

static void clamp_trim(hv_ctl_t *h)
{
    if (h->trim < HV_TRIM_MIN_V) h->trim = HV_TRIM_MIN_V;
    if (h->trim > HV_TRIM_MAX_V) h->trim = HV_TRIM_MAX_V;
    if (h->ramp + h->trim > HV_MAX_CW_V) h->trim = HV_MAX_CW_V - h->ramp;
}

int hv_set(hv_ctl_t *h, float volts, uint32_t now_ms)
{
    if (!(volts >= 0.0f && volts <= HV_MAX_SET_V)) return -1;   /* и NaN */
    if (!h->en && volts == 0.0f) {            /* уже выключено: ENABLE не трогаем, квитируем аварию */
        h->trip = NULL;                       /* off_why остаётся — см. hv_ctl.h */
        return 0;
    }
    if (h->en && volts == h->target) return 0; /* повтор: не откладывать "over", не трогать trim */
    if (!h->en) {
        h->ramp = 0.0f;
        h->trim = 0.0f;
        h->en = true;
        h->last_ms = now_ms;
    } else if (h->target > 0.0f) {
        h->trim *= volts / h->target;          /* нелинейность ОС ≈ пропорциональна уставке */
    } else {
        h->trim = 0.0f;
    }
    h->target = volts;
    h->changed_ms = now_ms;
    h->at_target = h->settled = h->over_armed = false;
    h->settle_ms = 0;
    h->trip = NULL;
    h->off_why = NULL;
    clamp_trim(h);
    return 0;
}

void hv_off(hv_ctl_t *h, const char *why)
{
    h->en = false;
    h->target = h->ramp = h->trim = 0.0f;
    h->at_target = h->settled = h->over_armed = false;
    h->settle_ms = 0;
    h->off_why = why;
}

static void trip(hv_ctl_t *h, const char *reason, const char *why)
{
    hv_off(h, why);
    h->trip = reason;
}

float hv_mon_volts(const hv_ctl_t *h, float mon_mv)
{
    if (mon_mv >= HV_MON_CEIL_CODE * h->vref_mv / 4095.0f) return 0.0f;
    return (h->zero_mv - mon_mv) / h->k_mv_per_v;
}

static uint16_t duty_of(const hv_ctl_t *h, float vdd_mv)
{
    if (!h->en || !(vdd_mv > 0.0f)) return 0;
    float cw = h->ramp + h->trim;
    if (cw > HV_MAX_CW_V) cw = HV_MAX_CW_V;
    float d = cw / HV_K_SET * 1000.0f / vdd_mv * HV_PWM_PERIOD;
    if (!(d > 0.0f)) return 0;
    if (d > HV_PWM_PERIOD - 1.0f) d = HV_PWM_PERIOD - 1.0f;
    return (uint16_t)(d + 0.5f);
}

uint16_t hv_tick(hv_ctl_t *h, float mon_mv, float vdd_mv, uint32_t now_ms)
{
    uint32_t dms = now_ms - h->last_ms;
    if (dms > DT_MAX_MS) dms = DT_MAX_MS;
    h->last_ms = now_ms;
    float dt = (float)dms / 1000.0f;

    /* монитор: всегда, и при выключенном ВВ (статус) */
    h->near_ceil = mon_mv >= HV_MON_CEIL_CODE * h->vref_mv / 4095.0f;
    float v = hv_mon_volts(h, mon_mv);
    if (!h->mon_seeded) { h->mon_f = h->mon_slow = v; h->mon_seeded = true; }
    else {
        h->mon_f += (v - h->mon_f) * (dt / MON_TAU_S);
        h->mon_slow += (h->mon_f - h->mon_slow) * (dt / SLOW_TAU_S);
    }

    if (!h->en) return 0;

    /* рампа 100 В/с */
    float step = HV_RAMP_VPS * (float)dms / 1000.0f;
    if (h->ramp < h->target) { h->ramp += step; if (h->ramp > h->target) h->ramp = h->target; }
    else if (h->ramp > h->target) { h->ramp -= step; if (h->ramp < h->target) h->ramp = h->target; }
    clamp_trim(h);                                /* hv_trim() не выше потолка CW и во время рампы */

    if (h->target == 0.0f && h->ramp == 0.0f) {   /* спуск до нуля закончен */
        hv_off(h, "set 0");
        return 0;
    }

    /* подстройка — только на стоящей уставке, после того как выход устоялся, по достоверному монитору */
    float e = h->target - h->mon_f;
    float slope = (h->mon_f - h->mon_slow) / SLOW_TAU_S;       /* В/с */
    if (h->ramp == h->target && !h->at_target) { h->at_target = true; h->at_target_ms = now_ms; }
    if (h->at_target && !h->settled && !h->near_ceil && now_ms - h->at_target_ms >= HOLD_MS) {
        bool approaching = e * slope > 0.0f && absf(slope) * SETTLE_TAU_S >= absf(e);
        if (absf(e) <= HV_DEADBAND_V || !approaching) {
            h->settle_ms += dms;
            if (h->settle_ms >= SETTLE_CONFIRM_MS) h->settled = true;
        } else {
            h->settle_ms = 0;
        }
    }
    if (h->settled && !h->near_ceil) {
        if (absf(e) > HV_DEADBAND_V)
            h->trim += TRIM_RATE * dt * e;
        clamp_trim(h);
    }

    /* защита */
    if (h->mon_f > HV_TRIP_ABS_V) {
        trip(h, "abs", "TRIP |V| > 1300 V by monitor");
        return 0;
    }
    /* выход разряжается медленно (τ ≈ 2…16 с): уставка стоит > 30 с, выход устоялся, монитор не падает.
       Флаг ставится один раз — переполнение now_ms − changed_ms (49.7 сут) защиту не снимает. */
    if (!h->over_armed && now_ms - h->changed_ms > HV_TRIP_OVER_MS) h->over_armed = true;
    if (h->ramp == h->target && h->over_armed && h->settled && slope >= 0.0f
        && h->mon_f > h->target + HV_TRIP_OVER_V) {
        trip(h, "over", "TRIP monitor > setpoint + 150 V");
        return 0;
    }
    return duty_of(h, vdd_mv);
}

bool hv_near_ceiling(const hv_ctl_t *h) { return h->near_ceil; }
bool hv_enabled(const hv_ctl_t *h) { return h->en; }
const char *hv_trip_reason(const hv_ctl_t *h) { return h->trip; }
const char *hv_off_reason(const hv_ctl_t *h) { return h->off_why; }
float hv_trim(const hv_ctl_t *h) { return h->trim; }
float hv_ramp(const hv_ctl_t *h) { return h->ramp; }
float hv_setpoint(const hv_ctl_t *h) { return h->target; }
float hv_mon_avg(const hv_ctl_t *h) { return h->mon_f; }
