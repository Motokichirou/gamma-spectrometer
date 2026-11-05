/* Тесты hv_ctl: скважность, подъём, подстройка по монитору, защита, потолок монитора.
   Модель стека (замеры 2026-11-05, SPEC §5): CW = 454.5·V_CONTROL, V_CONTROL = скважность/4096·vdd;
   выход = 0.916·CW (нелинейность цепочки ОС), монитор V_mon = zero − k·|V_out|. */
#include <string.h>
#include "t.h"
#include "hv_ctl.h"

#define ZERO 2460.0f
#define K    1.640f
#define VDD  3296.0f

static float mon_mv_of(float volts) { return ZERO - K * volts; }
static float cw_of(uint16_t duty, float vdd) { return (float)duty / 4096.0f * vdd / 1000.0f * 454.5f; }

/* Прогон модели «выход = ratio·CW» (без инерции) на n тиков по 10 мс; возвращает последний выход. */
static float run_model(hv_ctl_t *h, float ratio, uint32_t *now, int n, float *out)
{
    for (int i = 0; i < n; i++) {
        *now += 10;
        uint16_t d = hv_tick(h, mon_mv_of(*out), VDD, *now);
        *out = ratio * cw_of(d, VDD);
    }
    return *out;
}

static void test_duty_math(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    CHECK(hv_set(&h, 1000.0f, 0) == 0);
    CHECK(hv_enabled(&h));
    uint16_t d = 0; uint32_t now = 0;
    /* монитор показывает ровно уставку — подстройке нечего делать, trim = 0 */
    for (int i = 0; i < 1100; i++) { now += 10; d = hv_tick(&h, mon_mv_of(1000.0f), VDD, now); }
    CHECK_NEAR(hv_ramp(&h), 1000.0, 1e-3);
    CHECK_NEAR(hv_trim(&h), 0.0, 1e-6);
    CHECK_NEAR(d, 2733, 1);
    /* выключено → скважность 0 */
    hv_off(&h, "test");
    CHECK(!hv_enabled(&h));
    CHECK(hv_tick(&h, mon_mv_of(0), VDD, now + 10) == 0);
    CHECK(hv_off_reason(&h) && strcmp(hv_off_reason(&h), "test") == 0);
    CHECK(hv_trip_reason(&h) == NULL);
}

static void test_range(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    CHECK(hv_set(&h, -1.0f, 0) == -1);
    CHECK(hv_set(&h, 1200.5f, 0) == -1);
    CHECK(!hv_enabled(&h));
    CHECK(hv_set(&h, 1200.0f, 0) == 0);
    CHECK(hv_enabled(&h));
}

static void test_ramp_100vps(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 500.0f, 0);
    uint32_t now = 0; float out = 0;
    for (int i = 1; i <= 500; i++) {
        now += 10;
        uint16_t d = hv_tick(&h, mon_mv_of(out), VDD, now);
        out = 0.94f * cw_of(d, VDD);
        CHECK(hv_ramp(&h) <= (float)i * 1.0f + 1e-3f);   /* не быстрее 100 В/с */
        if (i == 499) CHECK(hv_ramp(&h) < 500.0f);
    }
    CHECK_NEAR(hv_ramp(&h), 500.0, 1e-3);
    /* большой скачок времени (зависание цикла) не даёт рывка */
    hv_set(&h, 1000.0f, now);
    now += 5000;
    hv_tick(&h, mon_mv_of(out), VDD, now);
    CHECK(hv_ramp(&h) <= 500.0f + 10.0f + 1e-3f);
    /* спуск тоже 100 В/с */
    hv_set(&h, 0.0f, now);
    float r0 = hv_ramp(&h);
    now += 10; hv_tick(&h, mon_mv_of(out), VDD, now);
    CHECK_NEAR(hv_ramp(&h), r0 - 1.0, 1e-3);
}

static void test_set_zero_turns_off(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 100.0f, 0);
    uint32_t now = 0; float out = 0;
    run_model(&h, 0.98f, &now, 1500, &out);
    CHECK(hv_set(&h, 0.0f, now) == 0);
    CHECK(hv_enabled(&h));                /* сначала плавный спуск */
    run_model(&h, 0.98f, &now, 1100, &out);
    CHECK(!hv_enabled(&h));               /* дошли до 0 — ENABLE снят */
    CHECK(hv_trip_reason(&h) == NULL);
}

static void test_trim_converges(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 1000.0f, 0);
    uint32_t now = 0; float out = 0;
    run_model(&h, 0.916f, &now, 6000, &out);      /* 60 с */
    CHECK(hv_enabled(&h));
    CHECK_NEAR(out, 1000.0, 3.0);
    CHECK_NEAR(hv_mon_avg(&h), 1000.0, 3.0);
    /* точное значение 1000/0.916 − 1000 = 91.7; подход снизу останавливается на краю мёртвой зоны */
    CHECK(hv_trim(&h) > 88.0f && hv_trim(&h) < 92.5f);
}

static void test_deadband(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 1000.0f, 0);
    uint32_t now = 0;
    for (int i = 0; i < 1000; i++) { now += 10; hv_tick(&h, mon_mv_of(1000.0f), VDD, now); }
    CHECK_NEAR(hv_trim(&h), 0.0, 1e-6);
    for (int i = 0; i < 3000; i++) { now += 10; hv_tick(&h, mon_mv_of(998.0f), VDD, now); }
    CHECK_NEAR(hv_trim(&h), 0.0, 1e-6);           /* ошибка 2 В — внутри ±3 В */
    for (int i = 0; i < 3000; i++) { now += 10; hv_tick(&h, mon_mv_of(1002.0f), VDD, now); }
    CHECK_NEAR(hv_trim(&h), 0.0, 1e-6);
    for (int i = 0; i < 1000; i++) { now += 10; hv_tick(&h, mon_mv_of(990.0f), VDD, now); }
    CHECK(hv_trim(&h) > 0.0f);                    /* 10 В — уже правим */
}

static void test_trim_rescale(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 1000.0f, 0);
    uint32_t now = 0; float out = 0;
    /* коэффициент подобран так, чтобы подстройка встала на trim ≈ 98 (как на стенде 2026-11-05):
       остановка на краю мёртвой зоны, r·(1000 + 98) = 997 */
    run_model(&h, 997.0f / 1098.0f, &now, 9000, &out);
    float t = hv_trim(&h);
    CHECK(t > 96.0f && t < 99.5f);
    CHECK(hv_set(&h, 500.0f, now) == 0);
    CHECK_NEAR(hv_trim(&h), t * 0.5f, 1e-3);
    CHECK_NEAR(hv_setpoint(&h), 500.0, 1e-6);
}

static void test_cw_ceiling(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 1200.0f, 0);
    uint32_t now = 0; float out = 0;
    float cw_max = 0;
    for (int i = 0; i < 12000; i++) {
        now += 10;
        uint16_t d = hv_tick(&h, mon_mv_of(out), VDD, now);
        float cw = cw_of(d, VDD);
        if (cw > cw_max) cw_max = cw;
        out = 0.916f * cw;
    }
    CHECK(hv_enabled(&h));
    CHECK(cw_max <= 1300.0f + 0.4f);              /* 0.4 В — шаг ШИМ на CW */
    CHECK_NEAR(hv_trim(&h), 100.0, 0.5);          /* упёрлись: 1200 + 100 = 1300 */
    CHECK(hv_trip_reason(&h) == NULL);
}

static void test_trip_abs(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 1200.0f, 0);
    uint32_t now = 0; float out = 0;
    run_model(&h, 0.98f, &now, 1300, &out);
    CHECK(hv_enabled(&h));
    /* монитор показывает 1301 В — сразу после выхода на уставку (до 30 с) */
    for (int i = 0; i < 1000 && hv_enabled(&h); i++) { now += 10; hv_tick(&h, mon_mv_of(1301.0f), VDD, now); }
    CHECK(!hv_enabled(&h));
    CHECK(hv_trip_reason(&h) && strcmp(hv_trip_reason(&h), "abs") == 0);
    CHECK(hv_tick(&h, mon_mv_of(1301.0f), VDD, now + 10) == 0);
    /* новая уставка сбрасывает причину */
    CHECK(hv_set(&h, 100.0f, now) == 0);
    CHECK(hv_trip_reason(&h) == NULL);
}

/* 1000 → 500; выход отстаёт от CW с τ (разряд через делитель), коэффициент цепочки тот же 0.916.
   Добавка после масштабирования верна (0.916·(500 + 44.4) ≈ 498.7 — в мёртвой зоне), поэтому
   подстройка, пока монитор догоняет, не должна её стирать и ронять выход ниже уставки. */
static void lowering_case(float tau_s, int n_ticks)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 1000.0f, 0);
    uint32_t now = 0; float out = 0;
    run_model(&h, 0.916f, &now, 6000, &out);
    CHECK(hv_set(&h, 500.0f, now) == 0);
    float t_scaled = hv_trim(&h);
    CHECK(t_scaled > 40.0f);
    float trim_min = t_scaled, out_min = out;
    for (int i = 0; i < n_ticks; i++) {
        now += 10;
        uint16_t d = hv_tick(&h, mon_mv_of(out), VDD, now);
        float target = 0.916f * cw_of(d, VDD);
        out += (target - out) * (0.01f / tau_s);
        if (hv_trim(&h) < trim_min) trim_min = hv_trim(&h);
        if (out < out_min) out_min = out;
    }
    CHECK(hv_enabled(&h));
    CHECK(hv_trip_reason(&h) == NULL);
    CHECK(trim_min >= 0.9f * t_scaled);
    CHECK(out_min >= 500.0f - 5.0f);
    CHECK_NEAR(out, 500.0, 3.5);
}

static void test_no_false_trip_after_lowering(void)
{
    lowering_case(2.0f, 9000);    /* 90 с */
    lowering_case(16.0f, 15000);  /* 150 с */
}

/* Добавка неверна после снижения (цепочка сменила коэффициент) — подстройка всё же доходит. */
static void test_trim_after_lowering_wrong_ratio(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 1000.0f, 0);
    uint32_t now = 0; float out = 0;
    run_model(&h, 0.916f, &now, 6000, &out);
    hv_set(&h, 500.0f, now);
    for (int i = 0; i < 15000; i++) {
        now += 10;
        uint16_t d = hv_tick(&h, mon_mv_of(out), VDD, now);
        float target = 0.94f * cw_of(d, VDD);     /* нужна добавка ≈ 31.9, а не 44.4 */
        out += (target - out) * (0.01f / 16.0f);
    }
    CHECK(hv_trip_reason(&h) == NULL);
    CHECK_NEAR(out, 500.0, 3.5);
    CHECK(hv_trim(&h) < 36.0f);
}

/* Пульт повторяет ту же уставку раз в 10 с — защита "over" всё равно срабатывает. */
static void test_repeat_set_keeps_over_trip(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 1000.0f, 0);
    uint32_t now = 0; float out = 0;
    run_model(&h, 0.916f, &now, 1500, &out);
    float t = hv_trim(&h);
    for (int i = 0; i < 6000 && hv_enabled(&h); i++) {
        now += 10;
        if (i % 1000 == 0) { CHECK(hv_set(&h, 1000.0f, now) == 0); }
        hv_tick(&h, mon_mv_of(1200.0f), VDD, now);
        if (i == 0) CHECK_NEAR(hv_trim(&h), t, 1.0);   /* повтор не масштабирует и не сбрасывает trim */
    }
    CHECK(!hv_enabled(&h));
    CHECK(hv_trip_reason(&h) && strcmp(hv_trip_reason(&h), "over") == 0);
}

/* hv_set(0) при выключенном ВВ не включает ENABLE даже на один тик. */
static void test_set_zero_when_off(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    CHECK(hv_set(&h, 0.0f, 0) == 0);
    CHECK(!hv_enabled(&h));
    CHECK(hv_tick(&h, mon_mv_of(0), VDD, 10) == 0);
    CHECK(!hv_enabled(&h));
}

/* Калибровка монитора: только при выключенном ВВ, остальное состояние не сбрасывает. */
static void test_set_cal(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    CHECK(hv_set_cal(&h, 2470.0f, 1.650f) == 0);
    CHECK_NEAR(hv_mon_volts(&h, 2470.0f - 1.650f * 800.0f), 800.0, 1e-3);
    CHECK(hv_set_cal(&h, 2470.0f, 0.0f) == -1);
    hv_set(&h, 500.0f, 0);
    CHECK(hv_set_cal(&h, ZERO, K) == -1);
    CHECK(hv_enabled(&h));
    CHECK_NEAR(hv_setpoint(&h), 500.0, 1e-6);
    CHECK_NEAR(hv_mon_volts(&h, 2470.0f - 1.650f * 800.0f), 800.0, 1e-3);
}

static void test_trip_over(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 1000.0f, 0);
    uint32_t now = 0; float out = 0;
    run_model(&h, 0.916f, &now, 3100, &out);      /* 31 с с момента уставки */
    CHECK(hv_enabled(&h));
    for (int i = 0; i < 1000 && hv_enabled(&h); i++) { now += 10; hv_tick(&h, mon_mv_of(1151.0f), VDD, now); }
    CHECK(!hv_enabled(&h));
    CHECK(hv_trip_reason(&h) && strcmp(hv_trip_reason(&h), "over") == 0);
}

static void test_no_over_trip_before_30s(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 1000.0f, 0);
    uint32_t now = 0; float out = 0;
    run_model(&h, 0.916f, &now, 1500, &out);      /* 15 с */
    for (int i = 0; i < 1000; i++) { now += 10; hv_tick(&h, mon_mv_of(1151.0f), VDD, now); }   /* до 25 с */
    CHECK(hv_enabled(&h));
}

static void test_near_ceiling(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    float mv4092 = 4092.0f * 2500.0f / 4095.0f;  /* VREF+ = 2.500 В (REF35250) */
    CHECK_NEAR(hv_mon_volts(&h, mv4092), 0.0, 1e-9);
    CHECK_NEAR(hv_mon_volts(&h, mon_mv_of(500.0f)), 500.0, 1e-3);
    CHECK(!hv_near_ceiling(&h));
    hv_tick(&h, mv4092, VDD, 10);
    CHECK(hv_near_ceiling(&h));
    hv_tick(&h, mon_mv_of(10.0f), VDD, 20);
    CHECK(!hv_near_ceiling(&h));
    /* другая опора */
    hv_set_vref(&h, 2490.0f);
    CHECK_NEAR(hv_mon_volts(&h, 4091.0f * 2490.0f / 4095.0f), 0.0, 1e-9);
    /* у потолка подстройка не идёт: ВВ малое, монитор «≈0, не точно» */
    hv_init(&h, ZERO + 40.0f, K);                 /* TL431 в плюсе: ноль выше потолка */
    hv_set(&h, 5.0f, 0);
    for (int i = 0; i < 3000; i++) hv_tick(&h, mv4092, VDD, 10u * (uint32_t)(i + 1));
    CHECK_NEAR(hv_trim(&h), 0.0, 1e-6);
}

int main(void)
{
    test_duty_math();
    test_range();
    test_ramp_100vps();
    test_set_zero_turns_off();
    test_trim_converges();
    test_deadband();
    test_trim_rescale();
    test_cw_ceiling();
    test_trip_abs();
    test_no_false_trip_after_lowering();
    test_trim_after_lowering_wrong_ratio();
    test_repeat_set_keeps_over_trip();
    test_set_zero_when_off();
    test_set_cal();
    test_trip_over();
    test_no_over_trip_before_30s();
    test_near_ceiling();
    return T_DONE();
}
