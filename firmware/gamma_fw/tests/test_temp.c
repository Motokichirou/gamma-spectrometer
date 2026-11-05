/* Тесты temp.c: Pt1000, калибровка R_pu, термокомпенсация. */
#include <string.h>
#include "t.h"
#include "temp.h"

static void test_cvd(void)
{
    CHECK_NEAR(pt_temp_c(1000.0f), 0.0, 0.01);
    CHECK_NEAR(pt_temp_c(1097.35f), 25.0, 0.02);
    CHECK_NEAR(pt_temp_c(1194.0f), 50.0, 0.2);  /* ≈ таблица: 1194.0 Ом при 50 °C */
}

static void test_resistance_roundtrip(void)
{
    /* R 1096.6 Ом при R_pu 10000: x = R/(R+Rpu); raw = x·4095·8 */
    float x = 1096.6f / (1096.6f + 10000.0f);
    float raw = x * 4095.0f * 8.0f;
    CHECK_NEAR(pt_resistance(raw, 10000.0f), 1096.6, 0.1);
}

static void test_calibrate_rpu(void)
{
    /* замер 2026-11-06: R 1096.6 Ом при R_pu 10000 → для 25.5 °C R_pu ≈ 10024.5 */
    float x = 1096.6f / (1096.6f + 10000.0f);
    float raw = x * 4095.0f * 8.0f;
    CHECK_NEAR(pt_calibrate_rpu(raw, 25.5f), 10024.5, 1.0);
    /* после калибровки та же сырая сумма даёт эталонную температуру */
    float rpu = pt_calibrate_rpu(raw, 25.5f);
    CHECK_NEAR(pt_temp_c(pt_resistance(raw, rpu)), 25.5, 0.01);
}

static void test_open(void)
{
    CHECK(pt_resistance(4090.0f, 10024.5f) < 0.0f);   /* обрыв: узел у потолка */
    CHECK(pt_resistance(-1.0f, 10024.5f) < 0.0f);
    CHECK(pt_calibrate_rpu(4090.0f, 25.0f) < 0.0f);   /* калибровка по обрыву — отказ */
}

/* Термокомпенсация: k1, k2 — усиление ФЭУ относительно t0; поправка канала — обратная величина. */
static void test_tc_corr(void)
{
    CHECK_NEAR(tc_pmt_gain(30.0f, 25.0f, -0.002f, 0.0f), 0.99, 1e-6);
    CHECK_NEAR(tc_corr(30.0f, 25.0f, -0.002f, 0.0f), 1.0 / 0.99, 1e-6);   /* усиление упало — канал × 1.0101 */
    CHECK_NEAR(tc_corr(25.0f, 25.0f, -0.002f, 1e-5f), 1.0, 1e-9);
    CHECK_NEAR(tc_pmt_gain(35.0f, 25.0f, 0.0f, 1e-4f), 1.01, 1e-6);
    CHECK_NEAR(tc_corr(35.0f, 25.0f, 0.0f, 1e-4f), 1.0 / 1.01, 1e-6);
    /* пик на месте: A·G(T)·tc_corr(T) = A при любой T */
    for (float t = -10.0f; t <= 50.0f; t += 5.0f)
        CHECK_NEAR(580.0f * tc_pmt_gain(t, 25.0f, -0.003f, 2e-5f) * tc_corr(t, 25.0f, -0.003f, 2e-5f), 580.0, 1e-3);
    /* пределы и мусор */
    CHECK_NEAR(tc_corr(1000.0f, 25.0f, -0.002f, 0.0f), 1.0 / TC_G_MIN, 1e-6);
    CHECK_NEAR(tc_corr(1000.0f, 25.0f, 0.01f, 0.0f), 1.0 / TC_G_MAX, 1e-6);
    volatile float zero = 0.0f;
    CHECK_NEAR(tc_corr(zero / zero, 25.0f, -0.002f, 0.0f), 1.0, 1e-9);
}

/* Короткое замыкание и обрыв Pt1000: ошибка, поправка 1.0, флаг для -gst. */
static void test_pt_faults(void)
{
    pt_state_t st = PT_OK;
    CHECK(pt_state(3233.0f) == PT_OK);                    /* 25 °C */
    CHECK(pt_state(2430.0f) == PT_OK);                    /* −50 °C */
    CHECK(pt_state(0.0f) == PT_SHORT);
    CHECK(pt_state(150.0f) == PT_SHORT);                  /* < 0.05·4095 */
    CHECK(pt_state(0.05f * 4095.0f + 1.0f) == PT_OK);
    CHECK(pt_state(4090.0f) == PT_OPEN);
    volatile float zero = 0.0f;
    CHECK(pt_state(zero / zero) == PT_OPEN);
    CHECK(pt_resistance(10.0f, 10024.5f) == -2.0f);
    CHECK(pt_resistance(4090.0f, 10024.5f) == -1.0f);
    CHECK(pt_calibrate_rpu(10.0f, 25.0f) < 0.0f);         /* калибровка по замыканию — отказ */
    CHECK(strcmp(pt_state_name(PT_OK), "ok") == 0);
    CHECK(strcmp(pt_state_name(PT_SHORT), "short") == 0);
    CHECK(strcmp(pt_state_name(PT_OPEN), "open") == 0);
    /* поправка по сырому коду: неисправен — 1.0 и флаг */
    CHECK_NEAR(tc_corr_raw(10.0f, 10024.5f, 25.0f, -0.002f, 0.0f, &st), 1.0, 1e-9);
    CHECK(st == PT_SHORT);
    CHECK_NEAR(tc_corr_raw(4090.0f, 10024.5f, 25.0f, -0.002f, 0.0f, &st), 1.0, 1e-9);
    CHECK(st == PT_OPEN);
    /* исправен: 30 °C при R_pu 10024.5 → поправка 1/0.99 */
    float r30 = 1000.0f * (1.0f + PT_CVD_A * 30.0f + PT_CVD_B * 900.0f);
    float raw = r30 / (r30 + 10024.5f) * 4095.0f * 8.0f;
    CHECK_NEAR(tc_corr_raw(raw, 10024.5f, 25.0f, -0.002f, 0.0f, &st), 1.0 / 0.99, 1e-4);
    CHECK(st == PT_OK);
    CHECK_NEAR(tc_corr_raw(raw, 10024.5f, 25.0f, -0.002f, 0.0f, NULL), 1.0 / 0.99, 1e-4);
}

int main(void)
{
    test_cvd(); test_resistance_roundtrip(); test_calibrate_rpu(); test_open(); test_tc_corr(); test_pt_faults();
    return T_DONE();
}
