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

void pulsegen_init(uint32_t mean_period_us, uint16_t dac_baseline);

/* Заполнить таблицу очередного импульса (масштабированная форма + baseline).
 * Возвращает амплитуду импульса в DAC-кодах (для отладки). */
uint16_t pulsegen_fill(uint32_t *table);  /* uint32: DAC DHR на AHB2 требует 32-битных DMA-записей */

/* Пуассоновский интервал до следующего импульса, МКС.
 * Минимум 60 мкс (импульс 48 мкс должен вернуться к baseline,
 * иначе DSP склеит соседние импульсы), максимум 60000 мкс. */
uint32_t pulsegen_next_delay_us(void);

#endif /* PULSEGEN_H */
