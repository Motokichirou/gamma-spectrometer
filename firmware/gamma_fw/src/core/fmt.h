/* fmt.h — числа в текст без %f/%e (в newlib-nano их нет). Переносимый модуль. */
#ifndef FMT_H
#define FMT_H

#include <stddef.h>

/* Фиксированная точка: dec знаков после точки (0…6); "nan" для нечисловых, без "-0.0". */
void fmt_fix(char *out, size_t cap, float v, int dec);
/* Экспоненциальная форма "-1.234567e-5": 7 значащих цифр (столько держит float). */
void fmt_exp(char *out, size_t cap, float v);

#endif
