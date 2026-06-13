/*
 * pulsegen.h — генератор тестовых импульсов для DAC (этап 2)
 *
 * Переносимая логика: форма CR-RC²-подобного импульса, амплитуды
 * по распределению "Cs-137 + Am-241 + комптон + фон", пуассоновские
 * интервалы. Вывод в DAC — на стороне проекта (stage2_hw.c).
 */
#ifndef PULSEGEN_H
#define PULSEGEN_H

#include <stdint.h>
#include <stddef.h>

#define PULSEGEN_TABLE_LEN  48u   /* сэмплов DAC на импульс (48 мкс @ 1 Мвыб/с) */

typedef enum {
    PG_OFF      = 0,   /* тишина (baseline) */
    PG_SPECTRUM = 1,   /* эмуляция источника: реальное распределение, пуассон */
    PG_MONO     = 2,   /* самотест: фикс. амплитуда, фикс. период */
} pg_mode_t;

void pulsegen_init(uint32_t mean_period_us, uint16_t dac_baseline);
void pulsegen_set_mode(pg_mode_t mode, uint16_t mono_amp, uint32_t mono_period_us);
pg_mode_t pulsegen_mode(void);

/* Распределение амплитуд для PG_SPECTRUM: 0 = Cs-137 (черника), 1 = Ra-226
 * (радий.xml). По умолчанию 0. Указатель меняется атомарно (Cortex-M4). */
void pulsegen_set_spectrum(uint8_t which);

/* Заполнить таблицу очередного импульса (масштабированная форма + baseline).
 * Возвращает амплитуду в DAC-кодах; 0 = режим OFF, не стрелять. */
uint16_t pulsegen_fill(uint32_t *table);  /* uint32: DAC DHR на AHB2 требует 32-битных DMA-записей */

/* Пуассоновский интервал до следующего импульса, МКС.
 * Минимум 60 мкс (импульс 48 мкс должен вернуться к baseline,
 * иначе DSP склеит соседние импульсы), максимум 60000 мкс. */
uint32_t pulsegen_next_delay_us(void);

#endif /* PULSEGEN_H */
