/*
 * main_glue.c — ШАБЛОН интеграции core/ в CubeMX-проект для NUCLEO-G474RE.
 * НЕ компилировать этот файл как есть! Куски кода переносятся в Core/Src/main.c,
 * сгенерированный CubeMX, в секции USER CODE (чтобы переживали регенерацию).
 *
 * Предполагается конфигурация CubeMX (см. firmware/README.md):
 *   - LPUART1: Asynchronous, 600000 8N1 (на Nucleo-G474RE это VCP ST-LINK, PA2/PA3)
 *   - DMA: LPUART1_RX, Circular, Byte
 *   - Clock: 170 МГц (HSI16 → PLL: M=4, N=85, R=2) + Boost mode
 */

/* ============ 1. В main.c, секция USER CODE BEGIN Includes ============ */
#include "app.h"
#include <string.h>

/* ============ 2. USER CODE BEGIN PV (приватные переменные) ============ */
#define UART_RX_RING_SIZE 256
static uint8_t  uart_rx_ring[UART_RX_RING_SIZE];
static uint32_t uart_rx_tail = 0;

extern UART_HandleTypeDef hlpuart1;   /* сгенерирует CubeMX */
extern DMA_HandleTypeDef  hdma_lpuart1_rx;

/* ============ 3. USER CODE BEGIN 0 (функции-порты для app.c) ============ */
void app_port_uart_send(const uint8_t *data, size_t n)
{
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)data, (uint16_t)n, 2000);
}

uint32_t app_port_millis(void)
{
    return HAL_GetTick();
}

float app_port_temperature_c(void)
{
    return 25.0f;   /* этап 1: заглушка; на боевой плате — Pt1000 через ADC4 */
}

/* вычитка кольцевого DMA-буфера: сколько байт пришло с прошлого вызова */
static void uart_rx_poll(void)
{
    uint32_t head = UART_RX_RING_SIZE - __HAL_DMA_GET_COUNTER(&hdma_lpuart1_rx);
    while (uart_rx_tail != head) {
        uint32_t chunk_end = (head > uart_rx_tail) ? head : UART_RX_RING_SIZE;
        app_rx_feed(&uart_rx_ring[uart_rx_tail], chunk_end - uart_rx_tail);
        uart_rx_tail = (chunk_end == UART_RX_RING_SIZE) ? 0 : chunk_end;
        head = UART_RX_RING_SIZE - __HAL_DMA_GET_COUNTER(&hdma_lpuart1_rx);
    }
}

/* ============ 4. В main(), USER CODE BEGIN 2 (после MX_..._Init) ============ */
/*
    HAL_UART_Receive_DMA(&hlpuart1, uart_rx_ring, UART_RX_RING_SIZE);
    app_init();
*/

/* ============ 5. В main(), USER CODE BEGIN WHILE (главный цикл) ============ */
/*
    while (1) {
        uart_rx_poll();
        app_poll();
    }
*/
