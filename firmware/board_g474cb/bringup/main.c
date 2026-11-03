/* Прошивка первого запуска боевой платы МК (STM32G474CBT6, LQFP-48).
 * Без HAL, на регистрах CMSIS. Прошивается по UART через системный загрузчик.
 *
 * Делает:
 *   - сразу прижимает ВВ: PB10 (HV_ENABLE) = 0, PA8 (HV_CONTROL) = 0;
 *   - USART1 PA9/PA10, 115200 8N1 (через FT231X на USB-плате);
 *   - печатает DEV_ID, объём флеш, корпус, FLASH_OPTR;
 *   - запускает HSE (кварц 16 МГц) и переводит на него SYSCLK —
 *     если после перехода текст читается, кварц на 16 МГц исправен;
 *   - раз в секунду: мигает D102 (PB6), меряет VREFINT -> VREF+ (ожидаем ~2.5 В от REF35250);
 *   - по строке "!boot" прыгает в системный загрузчик (для следующих прошивок без BOOT0).
 */
#include <stdint.h>
#include "stm32g4xx.h"

#define BAUD 115200u
#define VREFINT_CAL (*(const uint16_t *)0x1FFF75AAu) /* при VREF+ = 3.0 В, 30 °C */
#define FLASH_KB    (*(const uint16_t *)0x1FFF75E0u)
#define PKG_ID      (*(const uint16_t *)0x1FFF7500u)

static volatile uint32_t ms;
void SysTick_Handler(void) { ms++; }

void SystemInit(void)
{
    SCB->CPACR |= (3u << 20) | (3u << 22); /* FPU */
    /* Таблица векторов — наша. После команды Go загрузчика VTOR может смотреть в его ПЗУ,
     * и первый же SysTick улетит в чужой обработчик. */
    SCB->VTOR = 0x08000000u;
    __DSB();
}

static uint32_t sysclk = 16000000u;

static void uart_putc(char c)
{
    while (!(USART1->ISR & USART_ISR_TXE_TXFNF)) {}
    USART1->TDR = (uint8_t)c;
}
static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}
static void uart_hex(uint32_t v, int nd)
{
    static const char h[] = "0123456789ABCDEF";
    for (int i = nd - 1; i >= 0; i--) uart_putc(h[(v >> (4 * i)) & 15]);
}
static void uart_dec(uint32_t v)
{
    char b[11]; int i = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) uart_putc(b[--i]);
}
static void uart_flush(void) { while (!(USART1->ISR & USART_ISR_TC)) {} }

static void uart_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;
    USART1->CR1 = 0;
    USART1->BRR = (sysclk + BAUD / 2) / BAUD;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

static void gpio_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
    (void)RCC->AHB2ENR;

    /* ВВ выключено: PB10 = 0, PA8 = 0, оба выходы push-pull */
    GPIOB->BSRR = 1u << (10 + 16);
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 20)) | (1u << 20);
    GPIOA->BSRR = 1u << (8 + 16);
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 16)) | (1u << 16);

    /* LED D102: PB6 выход */
    GPIOB->BSRR = 1u << (6 + 16);
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 12)) | (1u << 12);

    /* USART1: PA9 TX, PA10 RX, AF7, подтяжка на RX */
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((15u << 4) | (15u << 8))) | (7u << 4) | (7u << 8);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3u << 20)) | (1u << 20);
    GPIOA->MODER = (GPIOA->MODER & ~((3u << 18) | (3u << 20))) | (2u << 18) | (2u << 20);
}

static int hse_start(void)
{
    RCC->CR |= RCC_CR_HSEON;
    uint32_t t = ms;
    while (!(RCC->CR & RCC_CR_HSERDY)) {
        if (ms - t > 200) { RCC->CR &= ~RCC_CR_HSEON; return 0; }
    }
    return 1;
}

static void sysclk_to_hse(void)
{
    uart_flush();
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | (2u << RCC_CFGR_SW_Pos);
    while (((RCC->CFGR & RCC_CFGR_SWS) >> RCC_CFGR_SWS_Pos) != 2u) {}
    /* HSE = 16 МГц, как HSI: делители UART и SysTick не меняются */
}

static void adc_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_ADC12EN;
    (void)RCC->AHB2ENR;
    ADC12_COMMON->CCR = (1u << ADC_CCR_CKMODE_Pos) | ADC_CCR_VREFEN; /* HCLK/1 */
    ADC1->CR &= ~ADC_CR_DEEPPWD;
    ADC1->CR |= ADC_CR_ADVREGEN;
    uint32_t t = ms; while (ms - t < 2) {}
    ADC1->CR |= ADC_CR_ADCAL;
    while (ADC1->CR & ADC_CR_ADCAL) {}
    t = ms; while (ms - t < 1) {}
    ADC1->ISR = ADC_ISR_ADRDY;
    ADC1->CR |= ADC_CR_ADEN;
    while (!(ADC1->ISR & ADC_ISR_ADRDY)) {}
    ADC1->SMPR2 = (ADC1->SMPR2 & ~(7u << 24)) | (7u << 24); /* канал 18: 640.5 такта */
    ADC1->SQR1 = (18u << ADC_SQR1_SQ1_Pos);                  /* L = 0, SQ1 = VREFINT */
}

static uint32_t adc_vrefint(void)
{
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        ADC1->ISR = ADC_ISR_EOC;
        ADC1->CR |= ADC_CR_ADSTART;
        while (!(ADC1->ISR & ADC_ISR_EOC)) {}
        sum += ADC1->DR;
    }
    return sum / 16;
}

static void jump_to_bootloader(void)
{
    GPIOB->BSRR = 1u << (10 + 16); /* ВВ выкл. */
    GPIOA->BSRR = 1u << (8 + 16);
    uart_puts("-> system bootloader\n");
    uart_flush();

    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->VAL = 0;
    for (int i = 0; i < 8; i++) { NVIC->ICER[i] = 0xFFFFFFFFu; NVIC->ICPR[i] = 0xFFFFFFFFu; }

    ADC1->CR |= ADC_CR_ADDIS;
    RCC->AHB2RSTR |= RCC_AHB2RSTR_ADC12RST;  RCC->AHB2RSTR &= ~RCC_AHB2RSTR_ADC12RST;
    RCC->APB2RSTR |= RCC_APB2RSTR_USART1RST; RCC->APB2RSTR &= ~RCC_APB2RSTR_USART1RST;
    RCC->APB2ENR &= ~RCC_APB2ENR_USART1EN;
    RCC->AHB2ENR &= ~RCC_AHB2ENR_ADC12EN;

    /* SYSCLK обратно на HSI, HSE выключить */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | (1u << RCC_CFGR_SW_Pos);
    while (((RCC->CFGR & RCC_CFGR_SWS) >> RCC_CFGR_SWS_Pos) != 1u) {}
    RCC->CR &= ~RCC_CR_HSEON;

    /* PA9/PA10 вернуть в аналоговый режим (как после сброса); PB10/PA8 остаются выходами 0 */
    GPIOA->MODER |= (3u << 18) | (3u << 20);

    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;
    SYSCFG->MEMRMP = 1u; /* system flash по адресу 0 */

    uint32_t sp = *(volatile uint32_t *)0x1FFF0000u;
    uint32_t pc = *(volatile uint32_t *)0x1FFF0004u;
    __set_MSP(sp);
    __DSB(); __ISB();
    __enable_irq();
    ((void (*)(void))pc)();
    for (;;) {}
}

int main(void)
{
    gpio_init();
    SysTick_Config(sysclk / 1000u);
    uart_init();

    uart_puts("\n\n=== gamma MCU board bring-up ===\n");
    uart_puts("DEV_ID 0x"); uart_hex(DBGMCU->IDCODE & 0xFFF, 3);
    uart_puts("  REV 0x");  uart_hex(DBGMCU->IDCODE >> 16, 4);
    uart_puts("\nFLASH "); uart_dec(FLASH_KB); uart_puts(" KB  PKG 0x"); uart_hex(PKG_ID & 0x1F, 2);
    uart_puts("  OPTR 0x"); uart_hex(FLASH->OPTR, 8); uart_puts("\n");

    int hse = hse_start();
    uart_puts(hse ? "HSE ready, switching SYSCLK to HSE...\n" : "HSE FAILED (no HSERDY in 200 ms), staying on HSI\n");
    if (hse) {
        sysclk_to_hse();
        uart_puts("SYSCLK = HSE 16 MHz (this line readable => crystal OK)\n");
    }

    adc_init();
    uart_puts("type !boot to jump to system bootloader\n");

    const char *key = "!boot";
    int kpos = 0;
    uint32_t next = ms, n = 0;
    for (;;) {
        if (USART1->ISR & USART_ISR_ORE) USART1->ICR = USART_ICR_ORECF;
        if (USART1->ISR & USART_ISR_RXNE_RXFNE) {
            char c = (char)USART1->RDR;
            kpos = (c == key[kpos]) ? kpos + 1 : (c == key[0] ? 1 : 0);
            if (key[kpos] == 0) jump_to_bootloader();
        }
        if ((int32_t)(ms - next) >= 0) {
            next += 1000;
            GPIOB->ODR ^= 1u << 6;
            uint32_t raw = adc_vrefint();
            uint32_t vref_mv = raw ? (3000u * VREFINT_CAL + raw / 2) / raw : 0;
            uart_puts("t="); uart_dec(n++);
            uart_puts(" clk="); uart_puts((RCC->CFGR & RCC_CFGR_SWS) == (2u << RCC_CFGR_SWS_Pos) ? "HSE" : "HSI");
            uart_puts(" VREFINT raw="); uart_dec(raw);
            uart_puts(" cal="); uart_dec(VREFINT_CAL);
            uart_puts(" -> VREF+="); uart_dec(vref_mv); uart_puts(" mV\n");
        }
    }
}
