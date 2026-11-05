/*
 * svc.h — служебные замеры и сервисные команды gammapult (SPEC §5, §6, §8). Переносимый модуль:
 * железо (src/hw/adc_aux) отдаёт сырые коды, здесь — пересчёт, сглаживание, проверки и ответы.
 *
 * Служебные замеры (svc_aux_*):
 *  - VREF+ по VREFINT (ADC4_IN18): VREF+ = 3000·VREFINT_CAL/raw. Норма 2500 ± 15 мВ (SPEC §3.4).
 *    Для пересчёта кодов в мВ — сглаженное (τ 10 с) по замерам в норме; до первого такого — 2500.
 *    Монитор ВВ откалиброван (k = 1.640, ноль 2460) в мВ «по VREFINT» (прошивка запуска считала
 *    так же), поэтому пересчёт идёт по измеренной опоре, а не по номиналу.
 *    Вне нормы: ВВ не включается; ≥ 3 замера подряд при включённом ВВ → hv_off("vref");
 *    при выключенном ВВ — запрос перекалибровки АЦП (не больше SVC_RECAL_MAX подряд):
 *    калибровка на недостоявшейся REF35250 даёт сдвиг −64 кода (SPEC §3.3).
 *  - +3.3V_D = амплитуда ШИМ ВВ: VBAT/3 (ADC5_IN17) × 3, сглаживание τ 60 с (SPEC §5).
 *  - Pt1000 (ADC4_IN17, OPAMP6 ×8): сырой код, сглаживание τ 10 с (SPEC §8); смена состояния
 *    датчика (обрыв/замыкание/норма) — фильтр заново.
 *  - Монитор ВВ (ADC2_IN1): мВ для hv_tick; замер старше SVC_MON_STALE_MS → NaN (hv_ctl после
 *    3 таких тиков выключит ВВ, "mon": застрявший АЦП не ослепит защиту). Плюс сглаженный τ 1 с
 *    для -hvzero.
 *
 * Сервисные команды (svc_handle; ответ одной строкой, proto допишет \r\n):
 *   -hv <0…1200> | -hv off | -hv        уставка выхода, В (целое) / выкл сразу / запрос
 *   -hvzero                             ноль монитора = текущий сглаженный монитор; только при
 *                                       выкл. ВВ, ≥ 120 с после выключения, 2300 мВ…потолок АЦП
 *   -hvk [мВ/В]                         коэффициент монитора 0.5…5 (только при выкл. ВВ) / запрос
 *   -ptcal <°C>                         R_pu по эталонной температуре 0…60 °C (R_pu 9…11 кОм)
 *   -gst                                состояние одной строкой key=value (svc.c, svc_gst)
 *   -diag                               напряжения узлов тракта, мВ, и вердикт по нормам SPEC §6
 * Калибровки меняют live-настройки (в ОЗУ); во флеш — командой -save (proto).
 * Все напряжения ВВ — модули, В (выход отрицательный).
 */
#ifndef SVC_H
#define SVC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hv_ctl.h"
#include "settings.h"

#define SVC_VREF_NOM_MV    2500.0f
#define SVC_VREF_TOL_MV    15.0f
#define SVC_VREF_TAU_S     10.0f
#define SVC_VDD_TAU_S      60.0f
#define SVC_PT_TAU_S       10.0f
#define SVC_MON_TAU_S      1.0f
#define SVC_MON_STALE_MS   50u
#define SVC_VREF_TRIP_N    3u       /* замеров (с) подряд вне нормы при включённом ВВ → выкл */
#define SVC_RECAL_MAX      10u
#define SVC_HVZERO_WAIT_MS 120000u  /* разряд выхода без нагрузки τ ≈ 16 с: 7.5 τ */
#define SVC_HVZERO_MIN_MV  2300.0f
#define SVC_HVK_MIN        0.5f
#define SVC_HVK_MAX        5.0f
#define SVC_PTCAL_TMIN     0.0f
#define SVC_PTCAL_TMAX     60.0f
#define SVC_RPU_MIN        9000.0f
#define SVC_RPU_MAX        11000.0f

typedef struct {
    float vrefint_cal;
    bool  ready;            /* пришёл хотя бы один замер VREFINT */
    bool  vref_ok;          /* последний замер в норме */
    bool  vref_seeded, vdd_seeded, pt_seeded, mon_seeded, mon_ever;
    uint8_t vref_bad, recal_n;
    float vref_now_mv;      /* последний замер */
    float vref_mv;          /* сглаженный по замерам в норме */
    float vdd_mv;           /* +3.3V_D, сглаженный; NaN — ещё не мерили */
    float pt_raw;           /* сглаженный; NaN — ещё не мерили */
    float mon_mv;           /* последний свежий замер монитора */
    float mon_mv_f;         /* сглаженный τ 1 с */
    uint32_t mon_fresh_ms, last_ms, hv_off_since_ms;
} svc_aux_t;

void  svc_aux_init(svc_aux_t *a, float vrefint_cal);
/* VREF+ по коду VREFINT (заводская VREFINT_CAL при 3.0 В); NaN при raw ≤ 0 или не числе. */
float svc_vref_from_vrefint(float raw, float cal);
/* Опора для пересчёта кодов в мВ. */
float svc_vref_mv(const svc_aux_t *a);
/* Шаг 10 мс. mon_raw — код монитора (среднее), fresh — это новый замер. hv_on — ВВ включено
   (отсчёт «выключено с» для -hvzero). Возврат — монитор, мВ, для hv_tick; NaN — нет/устарел. */
float svc_aux_10ms(svc_aux_t *a, float mon_raw, bool fresh, bool hv_on, uint32_t now_ms);
/* Шаг 1 с. NaN в аргументе — замер не пришёл. Может выключить ВВ (hv_off "vref").
   true — перекалибровать АЦП (VREF+ вне нормы при выключенном ВВ). */
bool  svc_aux_1s(svc_aux_t *a, hv_ctl_t *hv, float vrefint_raw, float vbat_raw, float pt_raw);
/* Можно включать ВВ: замеры есть, VREF+ в норме, амплитуда ШИМ измерена. */
bool  svc_hv_allowed(const svc_aux_t *a);

/* узлы -diag (SPEC §6) */
enum { SVC_N_VA, SVC_N_OA1N, SVC_N_OA1, SVC_N_VA2, SVC_N_OA2N, SVC_N_OA2, SVC_N_VB, SVC_N_BASE,
       SVC_N_MON, SVC_DIAG_N };
typedef struct { const char *key, *pin; float nom_mv, tol_mv; } svc_node_t;  /* tol 0 — без нормы */
extern const svc_node_t svc_nodes[SVC_DIAG_N];
typedef struct { float mv[SVC_DIAG_N]; } svc_diag_t;
/* Вне нормы: число узлов, имена через запятую в bad (может быть NULL). */
int   svc_diag_check(const svc_diag_t *d, char *bad, size_t cap);

/* -gst: то, что знает приложение (детектор, набор, флеш) */
typedef struct {
    float base, sigma;          /* коды */
    uint32_t cps_in, cps_acc, rej, lost;
    uint32_t overrun;           /* порций, перезаписанных DMA во время обработки */
    uint16_t thr;               /* действующий порог детектора, коды */
    uint32_t rej_shape, rej_width, rej_pile, rej_budget;   /* отбраковка по причинам с пуска */
    float live_s, real_s, cpu_pct;
    uint16_t pwm;               /* текущая скважность ШИМ ВВ */
    const char *flash;          /* proto_flash_status() */
} svc_gst_t;

typedef struct {
    hv_ctl_t   *hv;
    settings_t *live;
    svc_aux_t  *aux;
    uint32_t (*millis)(void);
    void (*fill_gst)(svc_gst_t *g);      /* может быть NULL */
    bool (*diag)(svc_diag_t *d);         /* замер узлов, мВ; false — АЦП не готовы; может быть NULL */
} svc_ctx_t;

void svc_init(const svc_ctx_t *ctx);
/* true — команда сервиса (ответ в reply), false — не наша (reply не тронут). */
bool svc_handle(const char *cmd, char *reply, size_t cap);

#endif
