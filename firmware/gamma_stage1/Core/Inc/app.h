/*
 * app.h — прикладная логика спектрометра (этап 1: shproto + тестовый спектр)
 *
 * Переносимый модуль. Связь с железом — через три функции-порта,
 * которые реализует пользовательский main.c (HAL, Nucleo или боевая плата).
 */
#ifndef APP_H
#define APP_H

#include <stdint.h>
#include <stddef.h>

/* === вызывает пользовательский код === */
void app_init(void);
void app_rx_feed(const uint8_t *data, size_t n);  /* скормить принятые байты UART */
void app_poll(void);                              /* вызывать в главном цикле */

/* === порт: реализуется в main.c === */
void     app_port_uart_send(const uint8_t *data, size_t n);  /* блокирующая отправка */
uint32_t app_port_millis(void);                               /* HAL_GetTick() */
float    app_port_temperature_c(void);                        /* Pt1000 (этап 1: заглушка) */

#endif /* APP_H */
