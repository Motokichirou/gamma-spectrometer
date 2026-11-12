/* uart.c — см. uart.h */
#include "stm32g474xx.h"
#include "uart.h"
#include "ring.h"
#include "hwcalc.h"
#include "clock.h"
#include "timebase.h"

#define RX_LEN  1024u
#define TX_LEN  8192u
#define TX_DMA_MAX 1024u                         /* байт на одну передачу DMA: 17 мс при 600 000 */
#define DMA_REQ_USART1_TX 25u                    /* RM0440 Табл. 91 (DMAMUX) */
#define TXCH    DMA1_Channel2                    /* ↔ DMAMUX1_Channel1; DMA1_Channel1 — под АЦП (Task 10) */
#define TXMUX   DMAMUX1_Channel1

static uint8_t uart_tx_ring[TX_LEN];             /* имя проверяет build.sh (бюджет ОЗУ) */
static uint8_t uart_rx_ring[RX_LEN];
static ring_t tx, rx;
static volatile uint32_t tx_inflight;            /* байт отдано DMA, ещё не снято с кольца */
static volatile uint32_t rx_overflow, rx_errors, tx_dropped;

/* запустить DMA с хвоста кольца, если он свободен; звать при запрещённых прерываниях или из ISR */
static void tx_kick(void)
{
    if (tx_inflight) return;
    const uint8_t *p;
    uint32_t n = ring_peek_contig(&tx, &p);
    if (!n) return;
    /* не больше TX_DMA_MAX за раз: место в кольце освобождается только по концу передачи, а 8 КБ
       одним куском идут 137 мс — дольше UART_TX_STALL_MS, и ждущий uart_tx бросал остаток кадра
       (терялся 8-й…9-й кадр "-osc raw", замер 2026-11-06) */
    if (n > TX_DMA_MAX) n = TX_DMA_MAX;
    TXCH->CCR &= ~DMA_CCR_EN;
    DMA1->IFCR = DMA_IFCR_CGIF2;
    TXCH->CMAR = (uint32_t)p;
    TXCH->CNDTR = n;
    tx_inflight = n;
    TXCH->CCR |= DMA_CCR_EN;
}

/* обслуживание конца/ошибки передачи; из ISR и из ожидания (тогда — при запрещённых прерываниях) */
static void tx_service(void)
{
    uint32_t isr = DMA1->ISR;
    if (isr & (DMA_ISR_TCIF2 | DMA_ISR_TEIF2)) {
        DMA1->IFCR = DMA_IFCR_CGIF2;
        TXCH->CCR &= ~DMA_CCR_EN;
        if (isr & DMA_ISR_TEIF2) tx_dropped += tx_inflight;   /* ошибка шины — кусок потерян, идём дальше */
        ring_drop(&tx, tx_inflight);
        tx_inflight = 0;
        tx_kick();
    }
}

void DMA1_Channel2_IRQHandler(void) { tx_service(); }

void USART1_IRQHandler(void)
{
    uint32_t isr = USART1->ISR;
    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) {
        USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
        rx_errors++;
    }
    /* FIFO 8 байт: забрать всё, но не крутиться бесконечно */
    for (int k = 0; k < 16 && (USART1->ISR & USART_ISR_RXNE_RXFNE); k++) {
        uint8_t b = (uint8_t)USART1->RDR;
        if (!ring_put1(&rx, b)) rx_overflow++;
    }
    if (isr & USART_ISR_IDLE) USART1->ICR = USART_ICR_IDLECF;
}

void uart_init(uint32_t baud)
{
    ring_init(&tx, uart_tx_ring, TX_LEN);
    ring_init(&rx, uart_rx_ring, RX_LEN);
    tx_inflight = 0;

    NVIC_DisableIRQ(USART1_IRQn);
    NVIC_DisableIRQ(DMA1_Channel2_IRQn);

    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    (void)RCC->APB2ENR;
    RCC->APB2RSTR |= RCC_APB2RSTR_USART1RST;      /* чистое состояние, в т. ч. после загрузчика */
    RCC->APB2RSTR &= ~RCC_APB2RSTR_USART1RST;

    /* PA9 TX, PA10 RX (подтяжка вверх), AF7, средняя скорость фронтов */
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((15u << 4) | (15u << 8))) | (7u << 4) | (7u << 8);
    GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~(3u << 18)) | (1u << 18);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~((3u << 18) | (3u << 20))) | (1u << 20);
    GPIOA->MODER = (GPIOA->MODER & ~((3u << 18) | (3u << 20))) | (2u << 18) | (2u << 20);

    /* DMA: память → TDR, байты, инкремент памяти, прерывания TC и TE */
    TXCH->CCR = 0;
    DMA1->IFCR = DMA_IFCR_CGIF2;
    TXMUX->CCR = DMA_REQ_USART1_TX;
    TXCH->CPAR = (uint32_t)&USART1->TDR;
    TXCH->CCR = DMA_CCR_MINC | DMA_CCR_DIR | DMA_CCR_TCIE | DMA_CCR_TEIE;

    uint32_t brr = hw_uart_brr(clock_hz(), baud);
    if (!brr) brr = hw_uart_brr(clock_hz(), HW_BOOT_BAUD);   /* не бывает при 16/156 МГц; лишь бы не 0 */
    USART1->BRR = brr;
    USART1->CR3 = USART_CR3_DMAT;
    USART1->CR1 = USART_CR1_FIFOEN | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE | USART_CR1_IDLEIE;
    USART1->CR1 |= USART_CR1_UE;

    NVIC_SetPriority(USART1_IRQn, 1);             /* приём — выше всего: FIFO 8 байт = 133 мкс на 600 000 */
    NVIC_SetPriority(DMA1_Channel2_IRQn, 4);
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_ClearPendingIRQ(DMA1_Channel2_IRQn);
    NVIC_EnableIRQ(USART1_IRQn);
    NVIC_EnableIRQ(DMA1_Channel2_IRQn);
}

bool uart_set_baud(uint32_t baud)
{
    uint32_t brr = hw_uart_brr(clock_hz(), baud);
    if (!brr) return false;
    /* BRR пишется только при UE = 0 (RM0440 §37.8.1). Кольца и DMA не трогаем: CR1/CR3 сохраняются,
       при UE = 0 теряется лишь содержимое FIFO; незаконченная передача DMA (flush не дождался)
       продолжится уже на новой скорости по запросу TXE */
    NVIC_DisableIRQ(USART1_IRQn);
    USART1->CR1 &= ~USART_CR1_UE;
    USART1->BRR = brr;
    USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF | USART_ICR_IDLECF;
    USART1->CR1 |= USART_CR1_UE;
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_EnableIRQ(USART1_IRQn);
    return true;
}

void uart_tx(const uint8_t *p, size_t n)
{
    const uint32_t lim = (clock_hz() / 1000u) * UART_TX_STALL_MS;
    uint32_t t0 = cycles(), spin = lim;
    while (n) {
        uint32_t k = ring_put(&tx, p, (uint32_t)n);
        p += k; n -= k;
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        if (!k) tx_service();                     /* ждём место: обслужить DMA и без прерываний */
        tx_kick();
        __set_PRIMASK(pm);
        if (k) { t0 = cycles(); spin = lim; }
        else if ((cycles() - t0) > lim || !spin--) { tx_dropped += (uint32_t)n; return; }
    }
}

size_t uart_tx_free(void) { return ring_free(&tx); }

size_t uart_rx(uint8_t *dst, size_t cap)
{
    return ring_get(&rx, dst, (uint32_t)cap);
}

bool uart_flush(uint32_t timeout_ms)
{
    const uint32_t lim = (clock_hz() / 1000u) * timeout_ms;
    uint32_t t0 = cycles(), spin = lim;
    for (;;) {
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        tx_service();
        tx_kick();
        bool empty = !tx_inflight && ring_used(&tx) == 0;
        __set_PRIMASK(pm);
        if (empty && (USART1->ISR & USART_ISR_TC)) return true;
        if ((cycles() - t0) > lim || !spin--) return false;
    }
}

void uart_get_stats(uart_stats_t *st)
{
    st->rx_overflow = rx_overflow;
    st->rx_errors = rx_errors;
    st->tx_dropped = tx_dropped;
}
