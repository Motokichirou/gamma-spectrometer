/* spiflash.c — см. spiflash.h. W25Q64JV: Power-down 0xB9, tDP ≤ 3 мкс (даташит §8.2.36).
   Восемь бит программно: такт ≈ 1 МГц, без включения SPI1 и его тактирования. */
#include "stm32g474xx.h"
#include "spiflash.h"
#include "timebase.h"

#define CMD_POWER_DOWN 0xB9u

#define MODE_MASK(p)  (3u << (2u * (p)))
#define MODE_OUT(p)   (1u << (2u * (p)))

extern uint32_t SystemCoreClock;

static void dly_us(uint32_t us)
{
    uint32_t c = (SystemCoreClock / 1000000u) * us;
    delay_cycles(c ? c : 16u);
}

#define CS_HI()   (GPIOA->BSRR = 1u << 15)
#define CS_LO()   (GPIOA->BSRR = 1u << (15 + 16))
#define SCK_HI()  (GPIOB->BSRR = 1u << 3)
#define SCK_LO()  (GPIOB->BSRR = 1u << (3 + 16))
#define MOSI(b)   (GPIOB->BSRR = (b) ? (1u << 5) : (1u << (5 + 16)))

void spiflash_sleep(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
    (void)RCC->AHB2ENR;

    /* /CS = 1 до перевода в выход (по сбросу PA15 — JTDI с подтяжкой вверх, т. е. и так 1) */
    CS_HI();
    GPIOA->OTYPER &= ~(1u << 15);
    GPIOA->PUPDR &= ~MODE_MASK(15);
    GPIOA->MODER = (GPIOA->MODER & ~MODE_MASK(15)) | MODE_OUT(15);
    /* CLK = 0, MOSI = 0 — выходы; MISO не читаем */
    SCK_LO();
    MOSI(0);
    GPIOB->OTYPER &= ~((1u << 3) | (1u << 5));
    GPIOB->PUPDR &= ~(MODE_MASK(3) | MODE_MASK(4) | MODE_MASK(5));
    GPIOB->MODER = (GPIOB->MODER & ~(MODE_MASK(3) | MODE_MASK(5))) | MODE_OUT(3) | MODE_OUT(5);
    dly_us(1);

    CS_LO();
    dly_us(1);
    for (int i = 7; i >= 0; i--) {              /* режим 0: данные до фронта, защёлка по фронту */
        MOSI((CMD_POWER_DOWN >> i) & 1u);
        dly_us(1);
        SCK_HI();
        dly_us(1);
        SCK_LO();
    }
    dly_us(1);
    CS_HI();                                    /* команда исполняется по фронту /CS */
    dly_us(5);                                  /* tDP 3 мкс */

    /* CLK/MOSI/MISO — в аналог (/CS = 1 — входы флеш игнорирует); /CS остаётся выходом 1 */
    GPIOB->MODER |= MODE_MASK(3) | MODE_MASK(4) | MODE_MASK(5);
    GPIOB->AFR[0] &= ~((0xFu << 12) | (0xFu << 16) | (0xFu << 20));
}
