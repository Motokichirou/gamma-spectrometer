/* boot.c — см. boot.h */
#include "stm32g474xx.h"
#include "boot.h"
#include "gpio.h"
#include "clock.h"
#include "uart.h"
#include "timebase.h"

#define SYSMEM_BASE 0x1FFF0000u

static void hv_off_hw(void)
{
    gpio_hv_force_off();                        /* PB10 = 0, PB6 = 0, PA8 → выход 0 */
    if (RCC->APB2ENR & RCC_APB2ENR_TIM1EN) { TIM1->CR1 = 0; TIM1->BDTR = 0; }
    RCC->APB2RSTR |= RCC_APB2RSTR_TIM1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_TIM1RST;
    RCC->APB2ENR &= ~RCC_APB2ENR_TIM1EN;
}

void boot_enter_system(void)
{
    hv_off_hw();
    uart_flush(50u);                            /* дослать ответ хосту */

    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
    for (unsigned i = 0; i < sizeof NVIC->ICER / sizeof NVIC->ICER[0]; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }

    /* ОУ (регистры в SYSCFG) — выкл */
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;
    OPAMP1->CSR = 0; OPAMP2->CSR = 0; OPAMP3->CSR = 0;
    OPAMP4->CSR = 0; OPAMP5->CSR = 0; OPAMP6->CSR = 0;

    /* используемая периферия — сброс и такт выкл (GPIO, PWR, SYSCFG, FLASH остаются) */
    const uint32_t ahb1 = RCC_AHB1RSTR_DMA1RST | RCC_AHB1RSTR_DMA2RST | RCC_AHB1RSTR_DMAMUX1RST;
    const uint32_t ahb2 = RCC_AHB2RSTR_ADC12RST | RCC_AHB2RSTR_ADC345RST | RCC_AHB2RSTR_DAC1RST;
    const uint32_t apb2 = RCC_APB2RSTR_USART1RST | RCC_APB2RSTR_SPI1RST | RCC_APB2RSTR_TIM1RST;
    RCC->AHB1RSTR |= ahb1; RCC->AHB1RSTR &= ~ahb1;
    RCC->AHB2RSTR |= ahb2; RCC->AHB2RSTR &= ~ahb2;
    RCC->APB2RSTR |= apb2; RCC->APB2RSTR &= ~apb2;
    RCC->AHB1ENR &= ~(RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMA2EN | RCC_AHB1ENR_DMAMUX1EN);
    RCC->AHB2ENR &= ~(RCC_AHB2ENR_ADC12EN | RCC_AHB2ENR_ADC345EN | RCC_AHB2ENR_DAC1EN);
    RCC->APB2ENR &= ~(RCC_APB2ENR_USART1EN | RCC_APB2ENR_SPI1EN | RCC_APB2ENR_TIM1EN);
    RCC->APB1RSTR1 |= RCC_APB1RSTR1_TIM6RST; RCC->APB1RSTR1 &= ~RCC_APB1RSTR1_TIM6RST;   /* ЦАП-тест */
    RCC->APB1ENR1 &= ~RCC_APB1ENR1_TIM6EN;

    clock_to_hsi();                             /* HSI 16 МГц, PLL/HSE выкл, Range 1 normal */

    /* PA9/PA10 — в аналог без подтяжек, как после сброса; PB10/PB6/PA8 остаются выходами 0
       (загрузчик PB10 не трогает, R203 и так держит ВВ выключенным) */
    GPIOA->PUPDR &= ~((3u << 18) | (3u << 20));
    GPIOA->MODER |= (3u << 18) | (3u << 20);

    SYSCFG->MEMRMP = 1u;                        /* system flash по адресу 0 */

    uint32_t sp = *(volatile uint32_t *)SYSMEM_BASE;
    uint32_t pc = *(volatile uint32_t *)(SYSMEM_BASE + 4u);
    __set_CONTROL(0);                           /* MSP, привилегированный, без FPCA */
    __ISB();
    __set_MSP(sp);
    __DSB(); __ISB();
    __enable_irq();
    ((void (*)(void))pc)();
    for (;;) { }
}

void Fault_Handler(void)
{
    __disable_irq();
    hv_off_hw();
    /* ≈1 с по DWT (на 16 МГц — 1 с по 16 МГц, на 156 — по 156), затем сброс */
    uint32_t f = clock_hz();
    delay_cycles(f ? f : 16000000u);
    NVIC_SystemReset();
}

void HardFault_Handler(void)  __attribute__((noreturn, alias("Fault_Handler")));
void MemManage_Handler(void)  __attribute__((noreturn, alias("Fault_Handler")));
void BusFault_Handler(void)   __attribute__((noreturn, alias("Fault_Handler")));
void UsageFault_Handler(void) __attribute__((noreturn, alias("Fault_Handler")));
void NMI_Handler(void)        __attribute__((noreturn, alias("Fault_Handler")));
