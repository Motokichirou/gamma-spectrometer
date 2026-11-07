/*
 * temp.h — Pt1000 и термокомпенсация (SPEC §8). Переносимый модуль, без железа.
 *
 * Схема: R303 (R_pu) от VREF+ -> узел -> Pt1000 -> GND; узел через OPAMP6 PGA x8 -> ADC4_IN17.
 * x = raw/(4095·8), R = R_pu·x/(1−x); T по Каллендару — Ван Дюзену (T >= 0).
 *
 * Неисправности датчика по сырому коду: raw > PT_RAW_MAX — обрыв (узел у потолка АЦП; 4080 ≈ R 1.39 кОм,
 * ≈ +100 °C — выше датчик всё равно не меряется), raw < PT_RAW_MIN — замыкание или утечка: R < 700 Ом
 * при номинальном R_pu (≈ −76 °C, ниже реального диапазона). Прежний порог 5 % шкалы (R < 63 Ом)
 * не ловил оторванную перемычку (2026-11-07: R ≈ 170…580 Ом, «−105…−205 °C» при pt=ok). При неисправности
 * термокомпенсация выключается (поправка 1.0), в -gst — флаг pt_state_name() (приложение).
 *
 * Термокомпенсация — однозначно: k1, k2 описывают УСИЛЕНИЕ ФЭУ относительно t0,
 *   G(T) = 1 + k1·(T − t0) + k2·(T − t0)²   (k1 < 0 — усиление падает с ростом T),
 * а поправка канала — обратная величина: tc_corr = 1/G, канал = round(2·A·tc_corr)
 * (det_set_gain(tc_corr(...))). Так пик стоит на месте: A растёт в G раз, поправка делит.
 */
#ifndef TEMP_H
#define TEMP_H

#define PT_R0      1000.0f
#define PT_CVD_A   3.9083e-3f
#define PT_CVD_B   (-5.775e-7f)
#define PT_RPU_DEF 10024.5f   /* калибровка 2026-11-06, временная */
#define PT_RAW_MAX 4080.0f    /* выше — обрыв */
#define PT_R_MIN   700.0f     /* Ом: ниже — замыкание/утечка (≈ −76 °C) */
#define PT_RAW_MIN (8.0f * 4095.0f * PT_R_MIN / (PT_R_MIN + PT_RPU_DEF))   /* ≈ 2138 */
#define TC_G_MIN   0.5f       /* пределы G(T): вне — поправка ограничивается */
#define TC_G_MAX   2.0f

typedef enum { PT_OK = 0, PT_OPEN, PT_SHORT } pt_state_t;

/* Состояние датчика по сырому коду; NaN и отрицательные — обрыв. */
pt_state_t pt_state(float raw);
const char *pt_state_name(pt_state_t st);       /* "ok", "open", "short" */
/* raw — среднее кодов ADC4 (усиление 8). Сопротивление, Ом; −1 — обрыв, −2 — замыкание. */
float pt_resistance(float raw, float rpu);
/* T, °C, по CVD (T >= 0). */
float pt_temp_c(float r_ohm);
/* Новое R_pu, при котором raw даёт t_ref_c; < 0 — датчик неисправен (обрыв/замыкание). */
float pt_calibrate_rpu(float raw, float t_ref_c);
/* Усиление ФЭУ относительно t0: G = 1 + k1·dT + k2·dT² (в пределах TC_G_MIN…TC_G_MAX). */
float tc_pmt_gain(float t_c, float t0, float k1, float k2);
/* Поправка канала = 1/G (см. выше); для det_set_gain. */
float tc_corr(float t_c, float t0, float k1, float k2);
/* Поправка канала по сырому коду Pt1000: датчик неисправен — 1.0 (без термокомпенсации),
   состояние — в *st (может быть NULL). */
float tc_corr_raw(float raw, float rpu, float t0, float k1, float k2, pt_state_t *st);

#endif
