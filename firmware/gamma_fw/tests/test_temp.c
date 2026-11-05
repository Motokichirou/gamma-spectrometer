/* Тесты temp.c: Pt1000, калибровка R_pu, термокомпенсация. */
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

static void test_tc_gain(void)
{
    CHECK_NEAR(tc_gain(30.0f, 25.0f, -0.002f, 0.0f), 0.99, 1e-6);
    CHECK_NEAR(tc_gain(25.0f, 25.0f, -0.002f, 1e-5f), 1.0, 1e-9);
    CHECK_NEAR(tc_gain(35.0f, 25.0f, 0.0f, 1e-4f), 1.01, 1e-6);
}

int main(void)
{
    test_cvd(); test_resistance_roundtrip(); test_calibrate_rpu(); test_open(); test_tc_gain();
    return T_DONE();
}
