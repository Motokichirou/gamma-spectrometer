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

/* Шум монитора (SPEC §5: ±5…10 В на 1000 В, до ±40 В на 1200): равномерный ±amp, LCG. */
static uint32_t lcg_ = 12345u;
static float noise(float amp)
{
    lcg_ = lcg_ * 1664525u + 1013904223u;
    return amp * ((float)(lcg_ >> 8) / 8388608.0f - 1.0f);
}
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
    /* 70 с: рампа 10 с + выдержка/оценка скорости ≈ 18 с + подтверждение 2 с + интегратор τ ≈ 10 с */
    run_model(&h, 0.916f, &now, 7000, &out);
    CHECK(hv_enabled(&h));
    /* ±3 В мёртвой зоны + шаг ШИМ (0.4 В на CW): подход снизу встаёт на 997.0 ± шаг */
    CHECK_NEAR(out, 1000.0, 3.5);
    CHECK_NEAR(hv_mon_avg(&h), 1000.0, 3.5);
    /* точное значение 1000/0.916 − 1000 = 91.7; подход снизу останавливается на краю мёртвой зоны */
    CHECK(hv_trim(&h) > 88.0f && hv_trim(&h) < 92.5f);
}

/* То же с шумом монитора ±10 В: подстройка сходится, ложных срабатываний нет. */
static void test_trim_converges_noisy(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 1000.0f, 0);
    uint32_t now = 0; float out = 0;
    for (int i = 0; i < 9000; i++) {
        now += 10;
        uint16_t d = hv_tick(&h, mon_mv_of(out + noise(10.0f)), VDD, now);
        out = 0.916f * cw_of(d, VDD);
    }
    CHECK(hv_enabled(&h));
    CHECK(hv_trip_reason(&h) == NULL);
    CHECK_NEAR(out, 1000.0, 4.0);
    CHECK(hv_trim(&h) > 86.0f && hv_trim(&h) < 97.0f);
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

/* from → to; выход отстаёт от CW с τ (разряд через делитель / без нагрузки), коэффициент цепочки
   тот же 0.916. Добавка после масштабирования верна (1000 → 500: 0.916·(500 + 44.4) ≈ 498.7 —
   в мёртвой зоне), поэтому подстройка, пока монитор догоняет, не должна её стирать и ронять выход
   ниже уставки, а защита "over" — срабатывать на разряжающемся выходе. noise_amp — шум монитора, В. */
static void lowering_case(float from, float to, float tau_s, float noise_amp, int n_ticks)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, from, 0);
    uint32_t now = 0; float out = 0;
    for (int i = 0; i < 9000; i++) {
        now += 10;
        uint16_t d = hv_tick(&h, mon_mv_of(out + noise(noise_amp)), VDD, now);
        out = 0.916f * cw_of(d, VDD);
    }
    CHECK(hv_set(&h, to, now) == 0);
    float t_scaled = hv_trim(&h);
    CHECK(t_scaled > 0.07f * to);
    float trim_min = t_scaled, out_min = out;
    for (int i = 0; i < n_ticks; i++) {
        now += 10;
        uint16_t d = hv_tick(&h, mon_mv_of(out + noise(noise_amp)), VDD, now);
        float target = 0.916f * cw_of(d, VDD);
        out += (target - out) * (0.01f / tau_s);
        if (hv_trim(&h) < trim_min) trim_min = hv_trim(&h);
        if (out < out_min) out_min = out;
    }
    CHECK(hv_enabled(&h));
    CHECK(hv_trip_reason(&h) == NULL);
    CHECK(trim_min >= 0.9f * t_scaled);
    CHECK(out_min >= to - 5.0f);
    CHECK_NEAR(out, to, 3.5);
}

static void test_no_false_trip_after_lowering(void)
{
    lowering_case(1000.0f, 500.0f, 2.0f, 0.0f, 9000);     /* 90 с */
    lowering_case(1000.0f, 500.0f, 16.0f, 0.0f, 15000);   /* 150 с */
    /* большое снижение без нагрузки: к 30 с от команды выход ещё на сотни вольт выше уставки */
    lowering_case(1200.0f, 100.0f, 16.0f, 0.0f, 20000);   /* 200 с */
    lowering_case(1000.0f, 200.0f, 16.0f, 0.0f, 20000);
    lowering_case(1200.0f, 300.0f, 16.0f, 0.0f, 20000);
    /* то же с шумом монитора */
    lowering_case(1000.0f, 500.0f, 2.0f, 10.0f, 9000);
    lowering_case(1000.0f, 500.0f, 16.0f, 10.0f, 15000);
    lowering_case(1000.0f, 200.0f, 16.0f, 10.0f, 20000);
    lowering_case(1200.0f, 100.0f, 16.0f, 40.0f, 20000);
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

/* hv_set(0) при выключенном ВВ не включает ENABLE даже на один тик; квитирует аварию:
   trip и причина выключения сбрасываются, как при любой принятой уставке. */
static void test_set_zero_when_off(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    CHECK(hv_set(&h, 0.0f, 0) == 0);
    CHECK(!hv_enabled(&h));
    CHECK(hv_tick(&h, mon_mv_of(0), VDD, 10) == 0);
    CHECK(!hv_enabled(&h));
    hv_set(&h, 1200.0f, 20);
    uint32_t now = 20;
    for (int i = 0; i < 1000 && hv_enabled(&h); i++) { now += 10; hv_tick(&h, mon_mv_of(1301.0f), VDD, now); }
    CHECK(hv_trip_reason(&h) && strcmp(hv_trip_reason(&h), "abs") == 0);
    const char *why = hv_off_reason(&h);
    CHECK(why != NULL);
    CHECK(hv_set(&h, 0.0f, now) == 0);
    CHECK(!hv_enabled(&h));
    CHECK(hv_trip_reason(&h) == NULL);
    CHECK(hv_off_reason(&h) == NULL);
    CHECK(hv_tick(&h, mon_mv_of(0), VDD, now + 10) == 0);
    CHECK(!hv_enabled(&h));
    /* то же после обычного выключения командой */
    hv_set(&h, 100.0f, now);
    hv_off(&h, "cmd");
    CHECK(hv_off_reason(&h) && strcmp(hv_off_reason(&h), "cmd") == 0);
    CHECK(hv_set(&h, 0.0f, now) == 0);
    CHECK(hv_off_reason(&h) == NULL && hv_trip_reason(&h) == NULL && !hv_enabled(&h));
}

/* Подъём уставки с отмасштабированной добавкой: hv_trim() не выше потолка CW и во время рампы. */
static void test_trim_clamped_during_ramp(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 1000.0f, 0);
    uint32_t now = 0; float out = 0;
    run_model(&h, 997.0f / 1098.0f, &now, 9000, &out);   /* trim ≈ 98 */
    CHECK(hv_set(&h, 1200.0f, now) == 0);                 /* масштаб → ≈ 118 */
    for (int i = 0; i < 2500; i++) {
        now += 10;
        uint16_t d = hv_tick(&h, mon_mv_of(out), VDD, now);
        out = (997.0f / 1098.0f) * cw_of(d, VDD);
        CHECK(hv_ramp(&h) + hv_trim(&h) <= 1300.0f + 1e-3f);
    }
}

/* Опора монитора: вне 2000…3400 мВ отвергается, прежнее значение остаётся. */
static void test_set_vref(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    float mv4092 = 4092.0f * 2500.0f / 4095.0f;
    volatile float zero = 0.0f;
    CHECK(hv_set_vref(&h, 0.0f) == -1);
    CHECK(hv_set_vref(&h, -5.0f) == -1);
    CHECK(hv_set_vref(&h, 1999.0f) == -1);
    CHECK(hv_set_vref(&h, 3401.0f) == -1);
    CHECK(hv_set_vref(&h, zero / zero) == -1);                           /* NaN */
    CHECK_NEAR(hv_mon_volts(&h, mv4092), 0.0, 1e-9);                     /* 2500 осталось */
    CHECK_NEAR(hv_mon_volts(&h, mon_mv_of(500.0f)), 500.0, 1e-3);
    CHECK(hv_set_vref(&h, 3300.0f) == 0);
    CHECK_NEAR(hv_mon_volts(&h, mv4092), (ZERO - mv4092) / K, 1e-3);     /* уже не потолок */
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
    run_model(&h, 0.916f, &now, 4100, &out);      /* рампа 10 с + 31 с */
    CHECK(hv_enabled(&h));
    for (int i = 0; i < 1000 && hv_enabled(&h); i++) { now += 10; hv_tick(&h, mon_mv_of(1151.0f), VDD, now); }
    CHECK(!hv_enabled(&h));
    CHECK(hv_trip_reason(&h) && strcmp(hv_trip_reason(&h), "over") == 0);
}

/* Выдержка 30 с — от конца рампы, а не от команды: рампа 0 → 1000 идёт 10 с; перенапряжение
   с 15 с по 39 с (29 с после рампы) не срабатывает, к 41 с — срабатывает. */
static void test_over_hold_from_ramp_end(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, 1000.0f, 0);
    uint32_t now = 0; float out = 0;
    run_model(&h, 0.916f, &now, 1500, &out);      /* 15 с */
    while (now < 39000u) { now += 10; hv_tick(&h, mon_mv_of(1160.0f), VDD, now); }
    CHECK(hv_enabled(&h));
    while (now < 42000u && hv_enabled(&h)) { now += 10; hv_tick(&h, mon_mv_of(1160.0f), VDD, now); }
    CHECK(!hv_enabled(&h));
    CHECK(hv_trip_reason(&h) && strcmp(hv_trip_reason(&h), "over") == 0);
}

/* Переполнение millis (49.7 сут) посреди работы: подъём, выдержка и защита идут как обычно —
   ни преждевременного "over" на переходе через 0, ни потери защиты после него. */
static void test_millis_wrap(void)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    uint32_t now = 0xFFFFFFFFu - 15000u;          /* через 15 с — переход через 0 */
    hv_tick(&h, mon_mv_of(0.0f), VDD, now);
    CHECK(hv_set(&h, 1000.0f, now) == 0);
    float out = 0;
    for (int i = 0; i < 2000; i++) {              /* 20 с: рампа 10 с, выход 0.916·CW */
        now += 10;
        uint16_t d = hv_tick(&h, mon_mv_of(out), VDD, now);
        out = 0.916f * cw_of(d, VDD);
    }
    CHECK(now < 10000u);                          /* перешли через 0 */
    CHECK_NEAR(hv_ramp(&h), 1000.0, 1e-3);
    /* перенапряжение сразу после перехода: до 30 с с конца рампы — не срабатывает */
    for (int i = 0; i < 1500; i++) { now += 10; hv_tick(&h, mon_mv_of(1160.0f), VDD, now); }
    CHECK(hv_enabled(&h));
    for (int i = 0; i < 2000 && hv_enabled(&h); i++) { now += 10; hv_tick(&h, mon_mv_of(1160.0f), VDD, now); }
    CHECK(!hv_enabled(&h));
    CHECK(hv_trip_reason(&h) && strcmp(hv_trip_reason(&h), "over") == 0);
    /* рампа не ускоряется на переходе: подъём 0 → 300 через переход идёт 100 В/с */
    hv_init(&h, ZERO, K);
    now = 0xFFFFFFFFu - 995u;
    hv_tick(&h, mon_mv_of(0.0f), VDD, now);
    hv_set(&h, 300.0f, now);
    for (int i = 0; i < 100; i++) { now += 10; hv_tick(&h, mon_mv_of(0.0f), VDD, now); }
    CHECK_NEAR(hv_ramp(&h), 100.0, 1e-3);
}

/* Настоящее перенапряжение после большого снижения уставки: выход разрядился (τ 16 с, шум монитора),
   потом монитор поднимается до уставки + 151 и стоит — "over" срабатывает. */
static void over_after_lowering(float from, float to, float noise_amp)
{
    hv_ctl_t h; hv_init(&h, ZERO, K);
    hv_set(&h, from, 0);
    uint32_t now = 0; float out = 0;
    for (int i = 0; i < 9000; i++) {
        now += 10;
        uint16_t d = hv_tick(&h, mon_mv_of(out + noise(noise_amp)), VDD, now);
        out = 0.916f * cw_of(d, VDD);
    }
    hv_set(&h, to, now);
    for (int i = 0; i < 20000; i++) {             /* 200 с: разряд к уставке */
        now += 10;
        uint16_t d = hv_tick(&h, mon_mv_of(out + noise(noise_amp)), VDD, now);
        out += (0.916f * cw_of(d, VDD) - out) * (0.01f / 16.0f);
    }
    CHECK(hv_enabled(&h));
    CHECK(hv_trip_reason(&h) == NULL);
    /* авария: выход растёт к уставке + 151 (τ 2 с) и стоит */
    int t_trip = -1;
    for (int i = 0; i < 6000 && hv_enabled(&h); i++) {
        now += 10;
        out += (to + 151.0f - out) * (0.01f / 2.0f);
        hv_tick(&h, mon_mv_of(out + noise(noise_amp)), VDD, now);
        if (!hv_enabled(&h)) t_trip = i;
    }
    CHECK(!hv_enabled(&h));
    CHECK(hv_trip_reason(&h) && strcmp(hv_trip_reason(&h), "over") == 0);
    CHECK(t_trip >= 0 && t_trip < 3000);          /* за ≤ 30 с от начала аварии */
}

static void test_over_after_lowering(void)
{
    over_after_lowering(1200.0f, 100.0f, 0.0f);
    over_after_lowering(1000.0f, 200.0f, 0.0f);
    over_after_lowering(1000.0f, 200.0f, 10.0f);
    over_after_lowering(1200.0f, 100.0f, 40.0f);
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
    CHECK(hv_set_vref(&h, 2490.0f) == 0);
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
    test_trim_converges_noisy();
    test_deadband();
    test_trim_rescale();
    test_cw_ceiling();
    test_trip_abs();
    test_no_false_trip_after_lowering();
    test_trim_after_lowering_wrong_ratio();
    test_repeat_set_keeps_over_trip();
    test_set_zero_when_off();
    test_trim_clamped_during_ramp();
    test_set_vref();
    test_set_cal();
    test_trip_over();
    test_over_hold_from_ramp_end();
    test_millis_wrap();
    test_over_after_lowering();
    test_no_over_trip_before_30s();
    test_near_ceiling();
    return T_DONE();
}
