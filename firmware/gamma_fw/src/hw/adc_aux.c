/* adc_aux.c — см. adc_aux.h. Последовательность включения/калибровки — RM0440 §21.4.6–21.4.9,
   как adc_on() прошивки запуска (проверено на плате), но со сбросом битов «rs» при записи CR
   и таймаутами: сбой АЦП не вешает главный цикл. */
#include "stm32g474xx.h"
#include "adc_aux.h"
#include "clock.h"
#include "timebase.h"

#define VREFINT_CAL_ADDR ((const uint16_t *)0x1FFF75AAu)
/* биты CR со свойством rs: при записи — только тот, что ставим (как HAL ADC_CR_BITS_PROPERTY_RS) */
#define CR_RS   (ADC_CR_ADCAL | ADC_CR_JADSTP | ADC_CR_ADSTP | ADC_CR_JADSTART | ADC_CR_ADSTART | \
                 ADC_CR_ADDIS | ADC_CR_ADEN)
#define OVS_256 (ADC_CFGR2_ROVSE | (7u << ADC_CFGR2_OVSR_Pos) | (4u << ADC_CFGR2_OVSS_Pos))  /* DR = Σ/16 */
#define OVS_16  (ADC_CFGR2_ROVSE | (3u << ADC_CFGR2_OVSR_Pos))                             /* DR = Σ    */
#define ISR_ALL (ADC_ISR_ADRDY | ADC_ISR_EOSMP | ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR)

#define CH_MON   1u     /* ADC2_IN1  = PA0 */
#define CH_VOP6  17u    /* ADC4_IN17 = VOPAMP6 */
#define CH_VREF  18u    /* ADC4_IN18 = VREFINT */
#define CH_VBAT  17u    /* ADC5_IN17 = VBAT/3 */

static bool ready;
static adc_aux_t last;
static uint8_t ch4 = CH_VOP6;

static uint32_t us_cycles(uint32_t us) { return (clock_hz() / 1000000u) * us; }

static bool wait_reg(volatile uint32_t *r, uint32_t m, uint32_t want, uint32_t us)
{
    uint32_t t0 = cycles(), lim = us_cycles(us);
    while ((*r & m) != want)
        if (cycles() - t0 > lim) return (*r & m) == want;
    return true;
}

static void cr_set(ADC_TypeDef *a, uint32_t bit) { a->CR = (a->CR & ~CR_RS) | bit; }

bool adc_hw_off(ADC_TypeDef *a)
{
    if (a->CR & ADC_CR_ADSTART) {
        cr_set(a, ADC_CR_ADSTP);
        if (!wait_reg(&a->CR, ADC_CR_ADSTART, 0u, 1000u)) return false;
    }
    if (a->CR & ADC_CR_ADEN) {
        cr_set(a, ADC_CR_ADDIS);
        if (!wait_reg(&a->CR, ADC_CR_ADEN, 0u, 1000u)) return false;
    }
    return true;
}

/* выкл → регулятор → калибровка (single-ended) → вкл; все каналы 640.5 такта */
bool adc_hw_cal_on(ADC_TypeDef *a)
{
    if (!adc_hw_off(a)) return false;
    a->CR = 0;                                   /* DEEPPWD = 0 */
    a->CR = ADC_CR_ADVREGEN;
    delay_cycles(us_cycles(25u));                /* tADCVREG_STUP 20 мкс */
    cr_set(a, ADC_CR_ADCAL);                     /* ADCALDIF = 0 */
    if (!wait_reg(&a->CR, ADC_CR_ADCAL, 0u, 10000u)) return false;
    delay_cycles(us_cycles(2u));                 /* ≥ 4 такта АЦП после калибровки */
    a->ISR = ADC_ISR_ADRDY;
    cr_set(a, ADC_CR_ADEN);
    if (!wait_reg(&a->ISR, ADC_ISR_ADRDY, ADC_ISR_ADRDY, 2000u)) return false;
    a->ISR = ADC_ISR_ADRDY;
    a->SMPR1 = 0x3FFFFFFFu;                      /* SMP = 7: 640.5 такта, каналы 0…9 */
    a->SMPR2 = 0x07FFFFFFu;                      /* каналы 10…18 */
    return true;
}

static void adc_go(ADC_TypeDef *a, uint32_t ch, bool cont, uint32_t ovs)
{
    a->CFGR = ADC_CFGR_JQDIS | ADC_CFGR_OVRMOD | (cont ? ADC_CFGR_CONT : 0u);   /* 12 бит, программный запуск */
    a->CFGR2 = ovs;
    a->SQR1 = ch << ADC_SQR1_SQ1_Pos;            /* L = 0: один канал */
    a->ISR = ISR_ALL;
    cr_set(a, ADC_CR_ADSTART);
}

static bool start_all(void)
{
    ADC_TypeDef *const all[4] = { ADC1, ADC2, ADC4, ADC5 };
    for (int i = 0; i < 4; i++)
        if (!adc_hw_cal_on(all[i])) return false;
    adc_go(ADC2, CH_MON, true, OVS_256);
    ch4 = CH_VOP6;
    adc_go(ADC4, ch4, false, OVS_256);
    adc_go(ADC5, CH_VBAT, true, OVS_256);
    return true;
}

bool adc_aux_init(void)
{
    ready = false;
    RCC->AHB2ENR |= RCC_AHB2ENR_ADC12EN | RCC_AHB2ENR_ADC345EN;
    (void)RCC->AHB2ENR;
    /* CCR — только при выключенных АЦП группы (ADC3 ещё не включён) */
    ADC_TypeDef *const all[5] = { ADC1, ADC2, ADC3, ADC4, ADC5 };
    for (int i = 0; i < 5; i++)
        if (!adc_hw_off(all[i])) return false;
    uint32_t ck12, ck345;
    if (clock_pll_ok()) {
        ck12 = 1u << ADC_CCR_PRESC_Pos;          /* асинхронно PLLP 52 / 2 = 26 МГц */
        ck345 = 0u;                              /* асинхронно PLLP 52 МГц */
    } else {
        ck12 = ck345 = 1u << ADC_CCR_CKMODE_Pos; /* PLL нет: синхронно HCLK/1 = 16 МГц */
    }
    ADC12_COMMON->CCR = ck12;
    ADC345_COMMON->CCR = ck345 | ADC_CCR_VREFEN | ADC_CCR_VBATSEL;
    last = (adc_aux_t){ 0 };
    ready = start_all();
    return ready;
}

bool adc_aux_ready(void) { return ready; }

float adc_aux_vrefint_cal(void) { return (float)*VREFINT_CAL_ADDR; }

void adc_aux_poll(adc_aux_t *out)
{
    if (ready) {
        if (ADC2->ISR & ADC_ISR_EOC) { last.mon_raw = (float)ADC2->DR / 16.0f; last.mon_new = true; }
        if (ADC5->ISR & ADC_ISR_EOC) { last.vbat_raw = (float)ADC5->DR / 16.0f; last.vbat_new = true; }
        if (ADC4->ISR & ADC_ISR_EOC) {
            float v = (float)ADC4->DR / 16.0f;
            if (ch4 == CH_VOP6) { last.pt_raw = v; last.pt_new = true; ch4 = CH_VREF; }
            else { last.vrefint_raw = v; last.vrefint_new = true; ch4 = CH_VOP6; }
        }
        /* одиночное преобразование закончено (или сбилось) — следующий канал */
        if (!(ADC4->CR & ADC_CR_ADSTART) && !(ADC4->ISR & ADC_ISR_EOC)) adc_go(ADC4, ch4, false, OVS_256);
    }
    *out = last;
    last.mon_new = last.pt_new = last.vrefint_new = last.vbat_new = false;
}

bool adc_aux_recalibrate(void)
{
    ready = false;
    ready = start_all();
    return ready;
}

/* разовый замер канала (ADC остановлен, настроен на одиночные с OVS_16): код, среднее ×16 */
static bool conv_once(ADC_TypeDef *a, uint32_t ch, float *out)
{
    a->SQR1 = ch << ADC_SQR1_SQ1_Pos;
    a->ISR = ISR_ALL;
    cr_set(a, ADC_CR_ADSTART);
    if (!wait_reg(&a->ISR, ADC_ISR_EOC, ADC_ISR_EOC, 5000u)) return false;   /* 16 × 25 мкс */
    *out = (float)a->DR / 16.0f;
    return true;
}

bool adc_aux_diag(float raw[SVC_DIAG_N])
{
    /* порядок — svc_nodes: va oa1n oa1 va2 oa2n oa2 vb base mon (SPEC §1, прошивка запуска) */
    static const struct { uint8_t adc2, ch; } t[SVC_DIAG_N] = {
        [SVC_N_VA]   = { 0, 2 },  [SVC_N_OA1N] = { 0, 4 },  [SVC_N_OA1] = { 0, 3 },
        [SVC_N_VA2]  = { 1, 4 },  [SVC_N_OA2N] = { 1, 13 }, [SVC_N_OA2] = { 1, 3 },
        [SVC_N_VB]   = { 0, 15 }, [SVC_N_BASE] = { 0, 12 }, [SVC_N_MON] = { 0, 1 },
    };
    if (!ready) return false;
    bool ok = true;
    /* ADC1 свободен; ADC2 — на время замера с монитора, потом обратно */
    if (!(ADC1->CR & ADC_CR_ADEN) || (ADC1->CR & ADC_CR_ADSTART)) return false;
    ADC1->CFGR = ADC_CFGR_JQDIS | ADC_CFGR_OVRMOD;
    ADC1->CFGR2 = OVS_16;
    if (ADC2->CR & ADC_CR_ADSTART) {
        cr_set(ADC2, ADC_CR_ADSTP);
        ok = wait_reg(&ADC2->CR, ADC_CR_ADSTART, 0u, 1000u);
    }
    ADC2->CFGR = ADC_CFGR_JQDIS | ADC_CFGR_OVRMOD;
    ADC2->CFGR2 = OVS_16;
    for (int i = 0; i < SVC_DIAG_N && ok; i++)
        ok = conv_once(t[i].adc2 ? ADC2 : ADC1, t[i].ch, &raw[i]);
    adc_go(ADC2, CH_MON, true, OVS_256);         /* монитор — снова непрерывно */
    return ok;
}
