/* timebase.c — SysTick 1 мс, DWT. */
#include "stm32g474xx.h"
#include "timebase.h"

static volatile uint32_t ms;

void SysTick_Handler(void) { ms++; }

void timebase_dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void timebase_init(uint32_t sysclk_hz)
{
    SysTick->CTRL = 0;
    SysTick->LOAD = sysclk_hz / 1000u - 1u;
    SysTick->VAL = 0;
    NVIC_SetPriority(SysTick_IRQn, (1u << __NVIC_PRIO_BITS) - 1u);
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

uint32_t millis(void) { return ms; }
uint32_t cycles(void) { return DWT->CYCCNT; }

void delay_cycles(uint32_t n)
{
    uint32_t t0 = DWT->CYCCNT;
    /* страховка, если DWT вдруг не идёт: счётчик итераций (каждая ≥ 4 тактов) */
    for (uint32_t spin = n / 4u + 1u; (DWT->CYCCNT - t0) < n && spin; spin--) { __NOP(); }
}
