/* adc_sig.c — см. adc_sig.h. Включение и калибровка — общие с служебными АЦП (adc_hw_cal_on),
   DMA по кругу — RM0440 §21.4.27 (DMACFG = 1), §12 (DMA), §13 (DMAMUX). */
#include "stm32g474xx.h"
#include "adc_sig.h"
#include "adc_aux.h"
#include "clock.h"
#include "timebase.h"
#include "hwcalc.h"
#include "detector.h"

#define SIG_CH        1u                 /* ADC3_IN1 = PB1 (быстрый канал) */
#define DMA_REQ_ADC3  37u                /* RM0440 Табл. 91 (DMAMUX) */
#define DCH           DMA1_Channel1      /* ↔ DMAMUX1_Channel0; DMA1_Channel2 — передача UART */
#define DMUX          DMAMUX1_Channel0
#define BUF_LEN       (2u * DET_BLK_DEF)
#define CR_RS   (ADC_CR_ADCAL | ADC_CR_JADSTP | ADC_CR_ADSTP | ADC_CR_JADSTART | ADC_CR_ADSTART | \
                 ADC_CR_ADDIS | ADC_CR_ADEN)
#define ISR_ALL (ADC_ISR_ADRDY | ADC_ISR_EOSMP | ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR)

/* 2 порции по 8192 отсчёта = 32 КБ. Имя проверяет build.sh (бюджет ОЗУ). Только ОЗУ, не CCM. */
uint16_t adc_dma_buf[BUF_LEN];

static volatile uint32_t seq;            /* номер последней готовой половины (из прерывания) */
static volatile uint8_t  rdy_half;       /* 0 — первая, 1 — вторая */
static volatile uint32_t dma_err;
static uint32_t taken, lost;
static uint8_t  smp = ADC_SIG_SMP_2C5;
static bool     running;

void DMA1_Channel1_IRQHandler(void)
{
    uint32_t isr = DMA1->ISR;
    DMA1->IFCR = isr & (DMA_IFCR_CHTIF1 | DMA_IFCR_CTCIF1 | DMA_IFCR_CTEIF1 | DMA_IFCR_CGIF1);
    if (isr & DMA_ISR_TEIF1) { dma_err++; return; }      /* канал выключен аппаратно */
    /* HT и TC сразу — обработчик опоздал на полпорции: обе половины готовы, свежая — вторая */
    if (isr & DMA_ISR_HTIF1) { rdy_half = 0; seq++; }
    if (isr & DMA_ISR_TCIF1) { rdy_half = 1; seq++; }
}

static void cr_set(uint32_t bit) { ADC3->CR = (ADC3->CR & ~CR_RS) | bit; }

static bool wait_clear(volatile uint32_t *r, uint32_t m, uint32_t us)
{
    uint32_t t0 = cycles(), lim = (clock_hz() / 1000000u) * us;
    while (*r & m)
        if (cycles() - t0 > lim) return !(*r & m);
    return true;
}

static bool stop(void)
{
    bool ok = true;
    if (ADC3->CR & ADC_CR_ADSTART) {
        cr_set(ADC_CR_ADSTP);
        ok = wait_clear(&ADC3->CR, ADC_CR_ADSTART, 1000u);
    }
    DCH->CCR &= ~DMA_CCR_EN;
    DMA1->IFCR = DMA_IFCR_CGIF1;
    running = false;
    return ok;
}

/* DMA с начала буфера и запуск; ADC3 включён (ADEN), преобразования остановлены */
static bool start(void)
{
    DCH->CCR &= ~DMA_CCR_EN;
    DMA1->IFCR = DMA_IFCR_CGIF1;
    DCH->CPAR = (uint32_t)&ADC3->DR;
    DCH->CMAR = (uint32_t)adc_dma_buf;
    DCH->CNDTR = BUF_LEN;
    /* периферия → память, 16/16 бит, инкремент памяти, по кругу, HT/TC/TE, приоритет «очень высокий» */
    DCH->CCR = (3u << DMA_CCR_PL_Pos) | (1u << DMA_CCR_MSIZE_Pos) | (1u << DMA_CCR_PSIZE_Pos)
             | DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE;
    DCH->CCR |= DMA_CCR_EN;
    /* 12 бит, вправо, непрерывно, DMA по кругу, при переполнении — перезапись (не останавливаться) */
    ADC3->CFGR = ADC_CFGR_JQDIS | ADC_CFGR_OVRMOD | ADC_CFGR_CONT | ADC_CFGR_DMACFG | ADC_CFGR_DMAEN;
    ADC3->CFGR2 = 0;
    ADC3->SMPR1 = (uint32_t)smp << ADC_SMPR1_SMP1_Pos;
    ADC3->SMPR2 = 0;
    ADC3->SQR1 = SIG_CH << ADC_SQR1_SQ1_Pos;     /* L = 0: один канал */
    ADC3->ISR = ISR_ALL;
    __disable_irq();
    taken = seq;                                 /* пропуск на время остановки — не потеря */
    __enable_irq();
    dma_err = 0;
    cr_set(ADC_CR_ADSTART);
    running = true;
    return true;
}

static bool bring_up(void)
{
    stop();
    if (!adc_hw_cal_on(ADC3)) return false;      /* калибровка, ADEN */
    return start();
}

bool adc_sig_init(uint8_t smp_code)
{
    smp = smp_code ? ADC_SIG_SMP_6C5 : ADC_SIG_SMP_2C5;
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
    RCC->AHB2ENR |= RCC_AHB2ENR_ADC345EN;
    (void)RCC->AHB2ENR;
    NVIC_DisableIRQ(DMA1_Channel1_IRQn);
    DCH->CCR = 0;
    DMUX->CCR = DMA_REQ_ADC3;
    NVIC_SetPriority(DMA1_Channel1_IRQn, 2);     /* выше UART (4): отметка порции — короткая */
    NVIC_ClearPendingIRQ(DMA1_Channel1_IRQn);
    NVIC_EnableIRQ(DMA1_Channel1_IRQn);
    return bring_up();
}

bool adc_sig_running(void) { return running && !dma_err; }
uint8_t adc_sig_smp(void) { return smp; }

uint32_t adc_sig_fs(void)
{
    uint32_t adc_hz = clock_pll_ok() ? HW_PLLP_HZ : clock_hz();   /* без PLL — синхронно HCLK/1 */
    return hw_adc_fs(adc_hz, smp ? 13u : 5u);
}

const uint16_t *adc_sig_take(size_t *n)
{
    if (seq == taken) return NULL;
    __disable_irq();
    uint32_t s = seq;
    uint8_t h = rdy_half;
    __enable_irq();
    if (s - taken > 1u) lost += s - taken - 1u;
    taken = s;
    if (n) *n = DET_BLK_DEF;
    return adc_dma_buf + (size_t)h * DET_BLK_DEF;
}

uint32_t adc_sig_lost(void) { return lost; }

bool adc_sig_set_smp(uint8_t smp_code)
{
    if (!stop()) return false;
    smp = smp_code ? ADC_SIG_SMP_6C5 : ADC_SIG_SMP_2C5;
    return start();
}

bool adc_sig_hold(void) { return stop(); }
bool adc_sig_resume(void) { return start(); }
bool adc_sig_recalibrate(void) { return bring_up(); }
