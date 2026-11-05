/*
 * hv_ctl.h — логика высокого напряжения (SPEC §5). Переносимый модуль, без железа.
 *
 * Железо (src/hw) само выставляет PB10/D102 по hv_enabled() и CCR1 ШИМ по hv_tick().
 * Знаки: все напряжения — модули (|V|), выход ВВ отрицательный.
 *
 *  - уставка выхода 0…1200 В, подъём/спуск 100 В/с;
 *  - уставка CW = ramp + trim ≤ 1300 В; |V_cw| = 454.5·V_CONTROL, V_CONTROL = скважность/4096·vdd;
 *  - подстройка trim по усреднённому монитору (τ ≈ 1 с), мёртвая зона ±3 В, τ ≈ 10 с,
 *    пределы −50…+150 В, масштаб пропорционально при смене уставки; только на стоящей уставке;
 *  - монитор |V| = (zero − V_mon)/k; у потолка АЦП (≥ 4090 кодов) — «≈0, не точно»;
 *  - защита по усреднённому монитору: > 1300 В → выкл ("abs");
 *    > уставки + 150 В при уставке, стоящей > 30 с → выкл ("over").
 */
#ifndef HV_CTL_H
#define HV_CTL_H

#include <stdint.h>
#include <stdbool.h>

#define HV_MAX_SET_V     1200.0f  /* уставка выхода */
#define HV_MAX_CW_V      1300.0f  /* C210–C217 по 500 В */
#define HV_TRIP_ABS_V    1300.0f
#define HV_TRIP_OVER_V   150.0f
#define HV_TRIP_OVER_MS  30000u
#define HV_RAMP_VPS      100.0f
#define HV_K_SET         454.5f   /* |V_cw| / V_CONTROL */
#define HV_PWM_PERIOD    4096.0f  /* 12 бит: скважность 0…4095 */
#define HV_DEADBAND_V    3.0f
#define HV_TRIM_MIN_V    (-50.0f)
#define HV_TRIM_MAX_V    150.0f
#define HV_MON_CEIL_CODE 4090.0f
#define HV_VREF_MV       2500.0f  /* REF35250 по умолчанию */

/* Состояние; поля закрыты, менять только функциями. */
typedef struct {
    float zero_mv, k_mv_per_v, vref_mv;
    float target, ramp, trim;     /* уставка выхода, текущая по рампе, добавка к CW */
    float mon_f;                  /* усреднённый монитор, |V| */
    bool  mon_seeded, near_ceil, en;
    uint32_t last_ms, changed_ms;
    const char *trip, *off_why;
} hv_ctl_t;

void  hv_init(hv_ctl_t *h, float zero_mv, float k_mv_per_v);
/* Опора АЦП монитора (для порога «потолка»); по умолчанию 2500 мВ. */
void  hv_set_vref(hv_ctl_t *h, float vref_mv);
/* 0…1200 В; 0 — ок, −1 — вне диапазона. Включает ENABLE; 0 В — плавный спуск и выключение. */
int   hv_set(hv_ctl_t *h, float volts, uint32_t now_ms);
void  hv_off(hv_ctl_t *h, const char *why);
/* Шаг 10 мс: монитор (мВ), амплитуда ШИМ (мВ, уже усреднённая) → скважность 0…4095. */
uint16_t hv_tick(hv_ctl_t *h, float mon_mv, float vdd_mv, uint32_t now_ms);
/* |V| по монитору; при V_mon ≥ 4090 кодов-эквивалент → 0 (флаг — hv_near_ceiling после hv_tick). */
float hv_mon_volts(const hv_ctl_t *h, float mon_mv);
bool  hv_near_ceiling(const hv_ctl_t *h);
bool  hv_enabled(const hv_ctl_t *h);              /* → PB10 и D102 */
const char *hv_trip_reason(const hv_ctl_t *h);    /* NULL, "abs", "over" */
const char *hv_off_reason(const hv_ctl_t *h);     /* последний why из hv_off/защиты, NULL — не выключали */
float hv_trim(const hv_ctl_t *h);
float hv_ramp(const hv_ctl_t *h);
float hv_setpoint(const hv_ctl_t *h);
float hv_mon_avg(const hv_ctl_t *h);              /* усреднённый монитор, |V| */

#endif
