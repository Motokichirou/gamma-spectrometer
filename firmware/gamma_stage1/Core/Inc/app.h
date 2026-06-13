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

/* === API для источника событий === */
#include <stdbool.h>
bool app_is_acquiring(void);            /* идёт ли набор (гейт для spectrum_add) */
void app_count_events(uint32_t n);      /* валидные импульсы (для cps) */
void app_count_invalid(uint32_t n);     /* отбракованные (поле invalid_pulses) */

/* Слабый хук источника: вызывается из app_poll(). Дефолтная реализация —
 * синтетический генератор этапа 1. Этап 2 (stage2_hw.c) переопределяет. */
void app_source_poll(uint32_t now_ms);

/* Отправить текстовый кадр (cmd=0x03) — для отчётов самотеста и т.п. */
void app_send_text(const char *s);

/* === порт: реализуется в main.c === */
void     app_port_uart_send(const uint8_t *data, size_t n);  /* блокирующая отправка */
uint32_t app_port_millis(void);                               /* HAL_GetTick() */
float    app_port_temperature_c(void);                        /* Pt1000 (этап 1: заглушка) */

#endif /* APP_H */
