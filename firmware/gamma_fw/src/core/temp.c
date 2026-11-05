/* temp.c — Pt1000 и термокомпенсация (SPEC §8). */
#include "temp.h"
#include <math.h>

#define PT_FULL    (4095.0f * 8.0f)

pt_state_t pt_state(float raw)
{
    if (!(raw >= 0.0f) || raw > PT_RAW_MAX) return PT_OPEN;   /* узел у потолка АЦП (и NaN) */
    if (raw < PT_RAW_MIN) return PT_SHORT;                    /* узел у земли */
    return PT_OK;
}

const char *pt_state_name(pt_state_t st)
{
    return st == PT_OK ? "ok" : st == PT_SHORT ? "short" : "open";
}

float pt_resistance(float raw, float rpu)
{
    pt_state_t st = pt_state(raw);
    if (st != PT_OK) return st == PT_SHORT ? -2.0f : -1.0f;
    /* raw — код ADC4 при выходе усилителя x8; доля делителя = код/(4095·8) */
    float x = raw / PT_FULL;
    return rpu * x / (1.0f - x);
}

float pt_temp_c(float r_ohm)
{
    /* B·T² + A·T + (1 − R/R0) = 0, корень T >= 0 */
    float c = 1.0f - r_ohm / PT_R0;
    float d = PT_CVD_A * PT_CVD_A - 4.0f * PT_CVD_B * c;
    if (d < 0.0f) d = 0.0f;
    return (-PT_CVD_A + sqrtf(d)) / (2.0f * PT_CVD_B);
}

float pt_calibrate_rpu(float raw, float t_ref_c)
{
    if (pt_state(raw) != PT_OK) return -1.0f;
    float x = raw / PT_FULL;
    float r = PT_R0 * (1.0f + PT_CVD_A * t_ref_c + PT_CVD_B * t_ref_c * t_ref_c);
    return r * (1.0f - x) / x;
}

float tc_pmt_gain(float t_c, float t0, float k1, float k2)
{
    float dt = t_c - t0;
    float g = 1.0f + k1 * dt + k2 * dt * dt;
    if (g != g) return 1.0f;                     /* NaN (T неизвестна) — без поправки */
    if (g < TC_G_MIN) g = TC_G_MIN;
    if (g > TC_G_MAX) g = TC_G_MAX;
    return g;
}

float tc_corr(float t_c, float t0, float k1, float k2)
{
    return 1.0f / tc_pmt_gain(t_c, t0, k1, k2);
}

float tc_corr_raw(float raw, float rpu, float t0, float k1, float k2, pt_state_t *st)
{
    pt_state_t s = pt_state(raw);
    if (st) *st = s;
    if (s != PT_OK) return 1.0f;
    return tc_corr(pt_temp_c(pt_resistance(raw, rpu)), t0, k1, k2);
}
