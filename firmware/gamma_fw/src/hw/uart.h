/* uart.h — USART1 (PA9 TX / PA10 RX, AF7) → FT231X. 8N1, FIFO (SPEC §9).
 * Приём: прерывание RXFNE/IDLE → кольцо 1 КБ; ошибки (ORE/FE/NE/PE) сбрасываются и считаются,
 *        переполнение кольца — байт теряется и считается, ничего не виснет.
 * Передача: кольцо 8 КБ (uart_tx_ring, ≥ PROTO_TX_FRAME_MAX) + DMA1 канал 2 (DMAMUX USART1_TX).
 *        uart_tx не блокирует, пока есть место; при полном кольце ждёт освобождения, но не
 *        дольше UART_TX_STALL_MS без продвижения — остаток отбрасывается и считается.
 * Работает и при запрещённых прерываниях: ожидание само обслуживает DMA по флагам. */
#ifndef UART_H
#define UART_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define UART_TX_STALL_MS 100u

void   uart_init(uint32_t baud);              /* после clock_init: делитель от clock_hz() */
void   uart_tx(const uint8_t *p, size_t n);
size_t uart_tx_free(void);
size_t uart_rx(uint8_t *dst, size_t cap);
bool   uart_flush(uint32_t timeout_ms);       /* кольцо и сдвиговый регистр пусты; false — таймаут */

typedef struct { uint32_t rx_overflow, rx_errors, tx_dropped; } uart_stats_t;
void   uart_get_stats(uart_stats_t *st);

#endif
