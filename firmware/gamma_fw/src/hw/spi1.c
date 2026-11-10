/* spi1.c — см. spi1.h. */
#include "stm32g474xx.h"
#include "spi1.h"
#include "timebase.h"

#define MODE_MASK(p)  (3u << (2u * (p)))
#define MODE_OUT(p)   (1u << (2u * (p)))
#define MODE_AF(p)    (2u << (2u * (p)))

extern uint32_t SystemCoreClock;

static void cs(bool on)
{
    if (on) GPIOA->BSRR = 1u << (15 + 16);
    else {
        while (SPI1->SR & SPI_SR_BSY) { }      /* последний байт ушёл — только тогда /CS вверх */
        GPIOA->BSRR = 1u << 15;
    }
}

static void xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    volatile uint8_t *dr = (volatile uint8_t *)&SPI1->DR;   /* 8-битный доступ: один байт в FIFO */
    for (size_t i = 0; i < n; i++) {
        while (!(SPI1->SR & SPI_SR_TXE)) { }
        *dr = tx ? tx[i] : 0xFFu;
        while (!(SPI1->SR & SPI_SR_RXNE)) { }
        uint8_t b = *dr;
        if (rx) rx[i] = b;
    }
}

static void dly_us(uint32_t us)
{
    uint32_t c = (SystemCoreClock / 1000000u) * us;
    delay_cycles(c ? c : 16u);
}

const w25_port_t spi1_w25 = { cs, xfer, dly_us };

void spi1_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;

    GPIOA->BSRR = 1u << 15;                    /* /CS = 1 */
    GPIOA->OTYPER &= ~(1u << 15);
    GPIOA->MODER = (GPIOA->MODER & ~MODE_MASK(15)) | MODE_OUT(15);

    /* PB3/4/5 — AF5, высокая скорость; MISO с подтяжкой вверх (флешка в Hi-Z при /CS = 1) */
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~((0xFu << 12) | (0xFu << 16) | (0xFu << 20)))
                  | (5u << 12) | (5u << 16) | (5u << 20);
    GPIOB->OTYPER &= ~((1u << 3) | (1u << 5));
    GPIOB->OSPEEDR |= (2u << 6) | (2u << 8) | (2u << 10);
    GPIOB->PUPDR = (GPIOB->PUPDR & ~(MODE_MASK(3) | MODE_MASK(4) | MODE_MASK(5))) | (1u << 8);
    GPIOB->MODER = (GPIOB->MODER & ~(MODE_MASK(3) | MODE_MASK(4) | MODE_MASK(5)))
                 | MODE_AF(3) | MODE_AF(4) | MODE_AF(5);

    SPI1->CR1 = 0;
    SPI1->CR2 = (7u << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH;                     /* 8 бит, RXNE на байт */
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (2u << SPI_CR1_BR_Pos);   /* /8, режим 0 */
    SPI1->CR1 |= SPI_CR1_SPE;
}
