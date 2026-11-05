/* hvpwm.c — см. hvpwm.h (как pwm_init прошивки запуска, проверено на плате 2026-11-05). */
#include "stm32g474xx.h"
#include "hvpwm.h"

void hvpwm_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
    (void)RCC->APB2ENR;
    TIM1->CR1 = 0;
    TIM1->PSC = 0;
    TIM1->ARR = 4095u;
    TIM1->CCR1 = 0;
    TIM1->CCMR1 = (6u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;   /* PWM1, предзагрузка */
    TIM1->CCER = TIM_CCER_CC1E;
    TIM1->BDTR = TIM_BDTR_MOE;
    TIM1->EGR = TIM_EGR_UG;
    TIM1->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
    /* PA8: выход 0 (gpio_safe_init) → AF6 при CCR1 = 0 */
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~15u) | 6u;
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 16)) | (2u << 16);
}

void hvpwm_set(uint16_t duty)
{
    TIM1->CCR1 = duty > 4095u ? 4095u : duty;
}

uint16_t hvpwm_get(void) { return (uint16_t)TIM1->CCR1; }

void hv_enable_pin(bool on)
{
    GPIOB->BSRR = on ? ((1u << 10) | (1u << 6)) : ((1u << (10 + 16)) | (1u << (6 + 16)));
}
