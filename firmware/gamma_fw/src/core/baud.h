/*
 * baud.h — временная скорость UART для опытов (-baud), SPEC §9. Переносимый модуль: само
 * переключение USART1 — в приложении (port->set), здесь — разбор, порядок и сторож.
 *
 *   -baud                 → "-ok baud <текущая>"
 *   -baud <N>             → N ∈ {600000, 1000000, 1500000, 2000000, 3000000} и ошибка делителя
 *                           при clk_hz ≤ 1 % → "-ok baud N" (на СТАРОЙ скорости), переключение —
 *                           в baud_poll (после того как ответ отдан в кольцо UART); иначе "-err baud".
 * Только в ОЗУ: не в settings_t, после сброса — BAUD_DEFAULT.
 *
 * Сторож: скорость ≠ BAUD_DEFAULT и BAUD_IDLE_MS нет ни одного корректного кадра (счётчик
 * proto_rx_frames) — вернуть BAUD_DEFAULT (зависший хост не оставит прибор глухим для BecqMoni).
 * Отсчёт идёт и от момента переключения.
 *
 * Порядок в главном цикле: proto_rx(...) → baud_poll(millis(), proto_rx_frames()).
 * port->set(N): дождаться опустошения передачи (с таймаутом) и перенастроить делитель USART1,
 * не трогая кольца приёма/передачи.
 */
#ifndef BAUD_H
#define BAUD_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BAUD_DEFAULT   600000u
#define BAUD_IDLE_MS   5000u
#define BAUD_ERR_PPM   10000      /* 1 % */

typedef struct {
    void (*set)(uint32_t baud);          /* перенастроить USART1 */
    uint32_t (*clk_hz)(void);            /* такт USART1 (проверка делителя) */
} baud_port_t;

void     baud_init(const baud_port_t *port);
bool     baud_handle(const char *cmd, char *reply, size_t cap);  /* false — не наша команда */
void     baud_poll(uint32_t now_ms, uint32_t rx_frames);
uint32_t baud_current(void);
bool     baud_allowed(uint32_t baud, uint32_t clk_hz);

#endif
