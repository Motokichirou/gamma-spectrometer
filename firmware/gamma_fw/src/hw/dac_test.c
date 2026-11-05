/* dac_test.c — см. dac_test.h */
#include "stm32g474xx.h"
#include "dac_test.h"
#include "dactest.h"
#include "clock.h"

static dact_t dt;
static volatile bool finished;

/* ЦАП и таймер выкл, PA4 остаётся в аналоговом режиме (gpio_safe_init) — при EN1 = 0 это Hi-Z */
static void hw_off(void)
{
    NVIC_DisableIRQ(TIM6_DAC_IRQn);
    if (RCC->APB1ENR1 & RCC_APB1ENR1_TIM6EN) {
        TIM6->CR1 = 0;
        TIM6->DIER = 0;
        TIM6->SR = 0;
    }
    if (RCC->AHB2ENR & RCC_AHB2ENR_DAC1EN) DAC1->CR &= ~DAC_CR_EN1;
    NVIC_ClearPendingIRQ(TIM6_DAC_IRQn);
}

bool dac_test_pulses(uint16_t step_code, uint16_t count, uint16_t period_us, uint16_t jit_cyc)
{
    if (dact_busy(&dt)) return false;
    if (!dact_start(&dt, step_code, count, period_us, clock_hz(), jit_cyc)) return false;
    finished = false;

    RCC->AHB2ENR |= RCC_AHB2ENR_DAC1EN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_TIM6EN;
    (void)RCC->APB1ENR1;

    /* ЦАП: MCR пишется только при EN1 = 0. MODE1 = 000 — нормальный режим, буфер вкл, выход на
       вывод; HFSEL = 01 — такт AHB > 80 МГц (156). Подстройка смещения — заводская (OTRIM1). */
    DAC1->CR &= ~DAC_CR_EN1;
    DAC1->MCR = (DAC1->MCR & ~(DAC_MCR_MODE1 | DAC_MCR_HFSEL)) | DAC_MCR_HFSEL_0;
    DAC1->DHR12R1 = DACT_BASE;                  /* без триггера: DOR = DHR через такт */
    DAC1->CR = (DAC1->CR & ~(DAC_CR_TEN1 | DAC_CR_WAVE1 | DAC_CR_DMAEN1 | DAC_CR_CEN1)) | DAC_CR_EN1;
    /* готовность (tWAKEUP ≈ 7.5 мкс); не дождались — всё выключить */
    for (uint32_t i = 0; !(DAC1->SR & DAC_SR_DAC1RDY); i++)
        if (i > 200000u) { hw_off(); dact_abort(&dt); return false; }

    /* TIM6: один такт ЦП на шаг; первый интервал — DACT_SETTLE_US (выход ЦАП из Hi-Z на базу —
       тоже ступенька, пусть отзвучит). ARR без предзагрузки: новый пишется в прерывании до
       следующего переполнения (интервал ≥ 3120 тактов). */
    TIM6->CR1 = 0;
    TIM6->PSC = 0;
    TIM6->ARR = dt.settle - 1u;
    TIM6->CNT = 0;
    TIM6->EGR = TIM_EGR_UG;                     /* PSC в силу */
    TIM6->SR = 0;
    TIM6->DIER = TIM_DIER_UIE;
    NVIC_ClearPendingIRQ(TIM6_DAC_IRQn);
    NVIC_SetPriority(TIM6_DAC_IRQn, 3);
    NVIC_EnableIRQ(TIM6_DAC_IRQn);
    TIM6->CR1 = TIM_CR1_CEN;
    return true;
}

void TIM6_DAC_IRQHandler(void)
{
    if (!(TIM6->SR & TIM_SR_UIF)) return;
    TIM6->SR = 0;
    uint16_t code;
    uint32_t ticks;
    if (dact_tick(&dt, &code, &ticks)) {
        DAC1->DHR12R1 = code;
        TIM6->ARR = ticks - 1u;
    } else {
        DAC1->DHR12R1 = DACT_BASE;
        hw_off();
        finished = true;
    }
}

void dac_test_stop(void)
{
    hw_off();                                   /* прерывание выкл до правки состояния */
    dact_abort(&dt);
}

bool dac_test_busy(void) { return dact_busy(&dt); }
uint16_t dac_test_done(void) { return dt.done; }
uint16_t dac_test_total(void) { return dt.n; }

bool dac_test_finished(void)
{
    if (!finished) return false;
    finished = false;
    return true;
}
