/*
 * temp.h — Pt1000 и термокомпенсация (SPEC §8). Переносимый модуль, без железа.
 *
 * Схема: R303 (R_pu) от VREF+ -> узел -> Pt1000 -> GND; узел через OPAMP6 PGA x8 -> ADC4_IN17.
 * x = raw/(4095·8), R = R_pu·x/(1−x); T по Каллендару — Ван Дюзену (T >= 0).
 */
#ifndef TEMP_H
#define TEMP_H

#define PT_R0      1000.0f
#define PT_CVD_A   3.9083e-3f
#define PT_CVD_B   (-5.775e-7f)
#define PT_RPU_DEF 10024.5f   /* калибровка 2026-11-06, временная */

/* raw — среднее кодов ADC4 (усиление 8). Сопротивление, Ом; < 0 — обрыв/вне шкалы. */
float pt_resistance(float raw, float rpu);
/* T, °C, по CVD (T >= 0). */
float pt_temp_c(float r_ohm);
/* Новое R_pu, при котором raw даёт t_ref_c; < 0 — raw вне шкалы (обрыв). */
float pt_calibrate_rpu(float raw, float t_ref_c);
/* Усиление ФЭУ относительно t0: 1 + k1·dT + k2·dT². */
float tc_gain(float t_c, float t0, float k1, float k2);

#endif
