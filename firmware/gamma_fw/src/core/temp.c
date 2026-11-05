/* temp.c — Pt1000 и термокомпенсация (SPEC §8). */
#include "temp.h"
#include <math.h>

#define PT_FULL    (4095.0f * 8.0f)
#define PT_RAW_MAX 4080.0f   /* выше — узел у потолка АЦП: обрыв датчика (как в запуске, bringup) */

float pt_resistance(float raw, float rpu)
{
    if (!(raw >= 0.0f) || raw > PT_RAW_MAX) return -1.0f;
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
    if (!(raw > 0.0f) || raw > PT_RAW_MAX) return -1.0f;
    float x = raw / PT_FULL;
    float r = PT_R0 * (1.0f + PT_CVD_A * t_ref_c + PT_CVD_B * t_ref_c * t_ref_c);
    return r * (1.0f - x) / x;
}

float tc_gain(float t_c, float t0, float k1, float k2)
{
    float dt = t_c - t0;
    return 1.0f + k1 * dt + k2 * dt * dt;
}
