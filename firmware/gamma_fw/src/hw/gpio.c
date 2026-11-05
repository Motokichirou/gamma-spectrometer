/* gpio.c — см. gpio.h */
#include "stm32g474xx.h"
#include "gpio.h"

#define MODE_MASK(p)  (3u << (2u * (p)))
#define MODE_OUT(p)   (1u << (2u * (p)))

void gpio_hv_force_off(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
    (void)RCC->AHB2ENR;
    /* сначала уровень 0 в ODR, потом режим «выход» — без выброса вверх */
    GPIOB->BSRR = (1u << (10 + 16)) | (1u << (6 + 16));
    GPIOB->OTYPER &= ~((1u << 10) | (1u << 6));
    GPIOB->MODER = (GPIOB->MODER & ~(MODE_MASK(10) | MODE_MASK(6))) | MODE_OUT(10) | MODE_OUT(6);
    GPIOA->BSRR = 1u << (8 + 16);
    GPIOA->OTYPER &= ~(1u << 8);                    /* загрузчик мог оставить open-drain (I2C2_SDA) */
    GPIOA->PUPDR &= ~MODE_MASK(8);
    GPIOA->MODER = (GPIOA->MODER & ~MODE_MASK(8)) | MODE_OUT(8);
}

void gpio_safe_init(void)
{
    gpio_hv_force_off();                            /* ВВ — первой строкой */

    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOCEN;
    (void)RCC->AHB2ENR;

    /* GPIOA: все в аналог, кроме PA8 (выход 0) и PA13/PA14/PA15 (как по сбросу: AF, подтяжки) */
    const uint32_t keep_a = MODE_MASK(8) | MODE_MASK(13) | MODE_MASK(14) | MODE_MASK(15);
    GPIOA->MODER = (GPIOA->MODER & keep_a) | (0xFFFFFFFFu & ~keep_a);
    GPIOA->PUPDR &= MODE_MASK(13) | MODE_MASK(14) | MODE_MASK(15);
    GPIOA->AFR[1] &= 0xFFF00000u;                   /* PA8…PA12: AF0 (PA13–15 не трогаем) */
    GPIOA->AFR[0] = 0;

    /* GPIOB: все в аналог, кроме PB6/PB10 (выходы 0) и PB3/PB4 (как по сбросу) */
    const uint32_t keep_b = MODE_MASK(3) | MODE_MASK(4) | MODE_MASK(6) | MODE_MASK(10);
    GPIOB->MODER = (GPIOB->MODER & keep_b) | (0xFFFFFFFFu & ~keep_b);
    GPIOB->PUPDR &= MODE_MASK(3) | MODE_MASK(4);
    GPIOB->AFR[0] &= 0x000FF000u;                   /* PB3/PB4 — как были */
    GPIOB->AFR[1] = 0;

    /* GPIOC: всё в аналог (загрузчик держит USART3 на PC10/PC11, AN2606 Табл. 101) */
    GPIOC->MODER = 0xFFFFFFFFu;
    GPIOC->PUPDR = 0;
    GPIOC->AFR[0] = GPIOC->AFR[1] = 0;
}
