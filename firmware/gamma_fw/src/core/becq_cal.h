/*
 * becq_cal.h — энергокалибровка BecqMoni (команды -cal): 11 слов.
 * Слова 0..9 — пять коэффициентов double (старшее слово, младшее слово),
 * слово 10 — CRC-32 по ASCII "%08X" x10 (docs/ru/protocol.md).
 * Без HAL и без malloc, тестируется на ПК.
 */
#ifndef BECQ_CAL_H
#define BECQ_CAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BECQ_WORDS 11u
#define BECQ_REGS  40u

/* Сумма для слова 10 по словам 0..9. */
uint32_t becq_checksum(const uint32_t w[10]);

/* Слова -> степень (1..4) и коэффициенты. false, если сумма не сходится
 * или коэффициент не конечен. */
bool becq_words_to_coef(const uint32_t w[11], int *order, double coef[5]);

/* Коэффициенты -> слова (с настоящей суммой). Коэффициенты выше order = 0. */
void becq_coef_to_words(int order, const double coef[5], uint32_t w[11]);

/* Ответ на голый -cal: 40 строк "XXXXXXXX\r\n", последняя (индекс 39) —
 * серийник. При valid=false слова 0..10 нулевые. Возвращает длину без
 * завершающего нуля; 0, если не влезло в cap. */
size_t becq_format_registers(const uint32_t w[11], bool valid,
                             const char *serial, char *out, size_t cap);

#endif
