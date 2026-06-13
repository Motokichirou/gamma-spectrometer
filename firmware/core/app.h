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

/* Слабый хук установки порога DSP, в КАНАЛАХ (этап 2 переопределяет).
 * channels<=0 — только запрос текущего. Возвращает эффективный порог в каналах,
 * -1 = не поддерживается. */
int app_set_threshold_ch(int channels);

/* Слабые хуки энергокалибровки (канал→кэВ), хранение во flash (этап 2/боевая
 * плата переопределяют). order = степень полинома, coef[0..order]: E=Σcᵢ·chⁱ.
 * write: 1=успех; read: 1=есть валидная калибровка, заполняет order/coef. */
int app_cal_write(int order, const double *coef);
int app_cal_read(int *order, double *coef);

/* Протокол энергокалибровки BecqMoni: 11 «сырых» 32-битных слов.
 * stage_word — приём одного слова `-cal <i> <hex>` (копится, шьётся по приходу
 * всех 11). read_words — отдать сохранённые 11 слов (1=есть валидные). */
int app_cal_stage_word(int idx, uint32_t word);
int app_cal_read_words(uint32_t w[11]);
int app_cal_clear(void);      /* стереть сохранённую калибровку (1=успех) */

/* Отправить текстовый кадр (cmd=0x03) — для отчётов самотеста и т.п. */
void app_send_text(const char *s);

/* === порт: реализуется в main.c === */
void     app_port_uart_send(const uint8_t *data, size_t n);  /* блокирующая отправка */
uint32_t app_port_millis(void);                               /* HAL_GetTick() */
float    app_port_temperature_c(void);                        /* Pt1000 (этап 1: заглушка) */

#endif /* APP_H */
