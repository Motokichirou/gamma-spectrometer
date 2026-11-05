/* adc_aux.h — служебные АЦП параллельно с сигнальным ADC3 (SPEC §7.8).
 *
 * Такт: ADC12 = PLLP 52 МГц с PRESC /2 = 26 МГц, ADC345 = PLLP 52 МГц (CKMODE = 00, CCIPR из
 * clock_init). Если PLL не захватился — синхронно HCLK/1 (16 МГц). Все каналы — 640.5 такта,
 * аппаратное усреднение ×256 (сдвиг 4: в DR — сумма/16, дробная часть кода сохраняется).
 *   ADC2_IN1  (PA0)  монитор ВВ, непрерывно: 24.6 мкс выборка, замер (×256) каждые 6.4 мс;
 *   ADC4_IN17        VOPAMP6 (Pt1000) и ADC4_IN18 VREFINT — поочерёдно, по 3.2 мс на замер;
 *   ADC5_IN17        VBAT/3 (+3.3V_D — амплитуда ШИМ ВВ), непрерывно, 3.2 мс на замер.
 *   ADC1             только для -diag (узлы тракта), разово.
 * ADC345_COMMON->CCR (VREFEN, VBATSEL, такт) пишется здесь, пока ADC3/4/5 выключены:
 * сигнальный ADC3 (Task 10) включать ПОСЛЕ adc_aux_init и CCR не трогать. Мост VBAT/3
 * включён постоянно — нагрузка на +3.3V_D не переключается (SPEC §5).
 * Калибровка АЦП — не раньше 1.5 с после сброса (SPEC §3.3): adc_aux_init звать после.
 */
#ifndef ADC_AUX_H
#define ADC_AUX_H
#include <stdint.h>
#include <stdbool.h>
#include "svc.h"
#include "stm32g474xx.h"

typedef struct {
    float mon_raw;     bool mon_new;      /* коды 0…4095 (среднее ×256) */
    float pt_raw;      bool pt_new;
    float vrefint_raw; bool vrefint_new;
    float vbat_raw;    bool vbat_new;
} adc_aux_t;

/* Включить ADC1/2/4/5: калибровка, настройка, запуск. false — АЦП не ответил (таймаут). */
bool  adc_aux_init(void);
bool  adc_aux_ready(void);
/* Неблокирующий опрос: значения — последние замеры, *_new — пришёл новый с прошлого опроса. */
void  adc_aux_poll(adc_aux_t *out);
/* Перекалибровать ADC1/2/4/5 и перезапустить (≈ 1 мс; монитор на это время без новых замеров). */
bool  adc_aux_recalibrate(void);
/* Узлы -diag в порядке svc_nodes (коды, среднее ×16), разово; ADC2 на ≈ 1.5 мс уходит
   с монитора. false — АЦП не запущены или таймаут. */
bool  adc_aux_diag(float raw[SVC_DIAG_N]);
float adc_aux_vrefint_cal(void);      /* VREFINT_CAL @ 0x1FFF75AA (при VREF+ = 3.0 В) */
/* Общие шаги для всех АЦП (и сигнального ADC3, adc_sig.c), с таймаутами:
   adc_hw_off — остановить и выключить; adc_hw_cal_on — выкл → регулятор → калибровка
   (single-ended) → вкл, все каналы 640.5 такта. false — АЦП не ответил. */
bool  adc_hw_off(ADC_TypeDef *a);
bool  adc_hw_cal_on(ADC_TypeDef *a);

#endif
