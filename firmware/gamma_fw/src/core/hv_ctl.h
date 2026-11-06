/*
 * hv_ctl.h — логика высокого напряжения (SPEC §5). Переносимый модуль, без железа.
 *
 * Железо (src/hw) само выставляет PB10/D102 по hv_enabled() и CCR1 ШИМ по hv_tick().
 * Знаки: все напряжения — модули (|V|), выход ВВ отрицательный.
 *
 *  - уставка выхода 0…1200 В, подъём/спуск 100 В/с;
 *  - уставка CW = ramp + trim ≤ 1300 В; |V_cw| = 454.5·V_CONTROL, V_CONTROL = скважность/4096·vdd;
 *  - подстройка trim по усреднённому монитору (τ ≈ 1 с), мёртвая зона ±3 В, τ ≈ 10 с,
 *    пределы −50…+150 В, масштаб пропорционально при смене уставки; только на стоящей уставке
 *    и после того, как выход «устоялся» (выдержка + монитор перестал идти к уставке — см. hv_ctl.c);
 *  - монитор |V| = (zero − V_mon)/k; у потолка АЦП (≥ 4090 кодов) — «≈0, не точно»;
 *  - защита по усреднённому монитору: > 1300 В → выкл ("abs", всегда); негодная амплитуда ШИМ
 *    → выкл ("vdd"); нечисловой монитор подряд → выкл ("mon");
 *    > уставки + 150 В → выкл ("over"), если с конца рампы (рампа дошла до уставки) прошло > 30 с,
 *    выход устоялся и усреднённый монитор не падает (выход, сам разряжающийся к сниженной
 *    уставке, — не авария; см. hv_ctl.c). Выдержки — по накопленному dt, переполнение millis
 *    (49.7 сут) на них не влияет.
 */
#ifndef HV_CTL_H
#define HV_CTL_H

#include <stdint.h>
#include <stdbool.h>

#ifndef HV_EXPERIMENT_1500
#define HV_MAX_SET_V     1200.0f  /* уставка выхода */
#define HV_MAX_CW_V      1300.0f  /* C210–C217 по 500 В */
#define HV_TRIP_ABS_V    1300.0f
#else
/* ⚠ ОПЫТНАЯ сборка (EXTRA=-DHV_EXPERIMENT_1500 OUT=gamma_fw_hv1500 ./build.sh): выход до 1350 В, CW до 1500 В
   (проектный предел умножителя; C210–C217 на 500 В — сознательный риск пробоя, решение пользователя 2026-11-06).
   ФЭУ R1306/R1307: абсолютный максимум 1500 В — авария "abs" по выходу 1420 В. В приборе не оставлять. */
#define HV_MAX_SET_V     1350.0f
#define HV_MAX_CW_V      1500.0f
#define HV_TRIP_ABS_V    1420.0f
#endif
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
/* Правдоподобная амплитуда ШИМ (+3.3V_D, AMS1117 ≈ 3295 мВ, ±2 % по даташиту + погрешность замера
   VBAT/3). Вне диапазона замер негоден: ВВ выключается ("vdd"). Скважность дополнительно ограничена
   так, чтобы при vdd = HV_VDD_MIN_MV CW не превышал HV_MAX_CW_V (hv_ctl.c, DUTY_MAX). */
#define HV_VDD_MIN_MV    3200.0f
#define HV_VDD_MAX_MV    3400.0f
#define HV_ZERO_DEF_MV   2460.0f  /* умолчания калибровки монитора (как settings_defaults) */
#define HV_K_DEF_MV_PER_V 1.640f

/* Состояние; поля закрыты, менять только функциями. */
typedef struct {
    float zero_mv, k_mv_per_v, vref_mv;
    float target, ramp, trim;     /* уставка выхода, текущая по рампе, добавка к CW */
    float mon_f;                  /* усреднённый монитор, |V| */
    float mon_slow;               /* медленный фильтр монитора — для оценки скорости */
    bool  mon_seeded, near_ceil, en;
    bool  at_target, settled;     /* рампа дошла до уставки; выход устоялся — можно подстраивать */
    bool  cal_ok;                 /* калибровка монитора годна — иначе ВВ не включается */
    uint8_t mon_bad;              /* нечисловых замеров монитора подряд */
    uint32_t last_ms;
    uint32_t at_ms;               /* мс с конца рампы (Σ dt, насыщается) — выдержка и защита "over" */
    uint32_t settle_ms;           /* сколько подряд выполняется условие «устоялся» */
    const char *trip, *off_why;
} hv_ctl_t;

/* Калибровка монитора проверяется: k > 0 и оба конечны. Иначе — умолчания (2460 мВ, 1.640 мВ/В)
   для показаний и hv_cal_ok() = false: hv_set(> 0) отвергается, пока hv_set_cal не задаст верные. */
void  hv_init(hv_ctl_t *h, float zero_mv, float k_mv_per_v);
bool  hv_cal_ok(const hv_ctl_t *h);
/* Опора АЦП монитора (для порога «потолка»); по умолчанию 2500 мВ.
   0 — ок, −1 — вне 2000…3400 мВ (значение не меняется). */
int   hv_set_vref(hv_ctl_t *h, float vref_mv);
/* Калибровка монитора (ноль, мВ/В) без сброса остального состояния. Только при выключенном ВВ:
   0 — ок, −1 — ВВ включено, k ≤ 0 или значения не конечны. Верная калибровка снимает запрет hv_cal_ok. */
int   hv_set_cal(hv_ctl_t *h, float zero_mv, float k_mv_per_v);
/* 0…1200 В; 0 — ок, −1 — вне диапазона или калибровка монитора негодна (hv_cal_ok).
   Включает ENABLE; 0 В — плавный спуск и выключение.
   Повтор той же уставки ничего не меняет (не откладывает защиту "over"). Повышение уставки не
   перезапускает выдержку "over" (30 с от конца рампы); понижение — перезапускает.
   Любая принятая уставка сбрасывает hv_trip_reason и hv_off_reason (в NULL). В том числе 0 В при
   выключенном ВВ: ENABLE не включается (ни на один тик), это квитирование аварии — trip и причина
   выключения сбрасываются, как при любой другой уставке. */
int   hv_set(hv_ctl_t *h, float volts, uint32_t now_ms);
void  hv_off(hv_ctl_t *h, const char *why);
/* Шаг 10 мс: монитор (мВ), амплитуда ШИМ (мВ, уже усреднённая) → скважность 0…4095.
   При включённом ВВ: vdd вне HV_VDD_MIN_MV…HV_VDD_MAX_MV (и NaN) → выкл ("vdd"); нечисловой
   монитор в фильтры не идёт, 3 тика подряд → выкл ("mon"). */
uint16_t hv_tick(hv_ctl_t *h, float mon_mv, float vdd_mv, uint32_t now_ms);
/* |V| по монитору; при V_mon ≥ 4090 кодов-эквивалент → 0 (флаг — hv_near_ceiling после hv_tick). */
float hv_mon_volts(const hv_ctl_t *h, float mon_mv);
bool  hv_near_ceiling(const hv_ctl_t *h);
bool  hv_enabled(const hv_ctl_t *h);              /* → PB10 и D102 */
const char *hv_trip_reason(const hv_ctl_t *h);    /* NULL, "abs", "over", "vdd", "mon" */
const char *hv_off_reason(const hv_ctl_t *h);     /* последний why из hv_off/защиты; NULL — не
                                                     выключали или после hv_set (квитировано) */
float hv_trim(const hv_ctl_t *h);
float hv_ramp(const hv_ctl_t *h);
float hv_setpoint(const hv_ctl_t *h);
float hv_mon_avg(const hv_ctl_t *h);              /* усреднённый монитор, |V| */

#endif
