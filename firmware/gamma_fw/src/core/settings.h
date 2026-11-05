/*
 * settings.h — настройки прибора (одна страница флеш, CRC-32).
 * Переносимый модуль: без HAL. Повреждённые/пустые настройки
 * (settings_valid == false) => работать на settings_defaults, ВВ выключено.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SETTINGS_MAGIC   0x47534631u   /* "GSF1" */
#define SETTINGS_VERSION 1u

typedef struct {
    uint32_t magic, version, size;           /* size = sizeof(settings_t) */
    float hv_mon_zero_mv, hv_mon_k_mv_per_v; /* 2460, 1.640 */
    float hv_setpoint_v;                     /* 0 */
    uint8_t reserved_boot;                   /* было hv_on_at_boot: не используется, всегда 0 —
                                                ВВ при старте не включается никогда */
    float pt_rpu_ohm;                        /* 10024.5 */
    float tc_t0_c, tc_k1, tc_k2;             /* усиление ФЭУ относительно t0 = 1 + k1·dT + k2·dT²;
                                                канал × 1/это (temp.h, tc_corr); 25, 0, 0 */
    uint16_t thr_codes;                      /* 0 = авто 5 сигма */
    uint32_t becq_words[11];                 /* слова -cal как прислал BecqMoni */
    uint8_t becq_valid;
    int16_t tpl[64][7];                      /* эталон с реального тракта (Q14) */
    uint8_t tpl_valid;
    uint8_t reserved[5];                     /* до кратности 8: флеш пишется двойными словами */
    uint32_t crc32;                          /* по всем байтам до crc32; последнее поле */
} settings_t;
/* sizeof(settings_t) кратен 8 и ≤ страницы 4 КБ — проверяется при сборке в settings.c */

/* Значения по умолчанию (структура обнуляется целиком) + seal. */
void settings_defaults(settings_t *s);
/* magic, version, size, crc. */
bool settings_valid(const settings_t *s);
/* Пересчитать crc32. */
void settings_seal(settings_t *s);

/* CRC-32 zip: poly 0x04C11DB7 reflected, init/xorout 0xFFFFFFFF. */
uint32_t crc32_zip(const uint8_t *p, size_t n);

#endif
