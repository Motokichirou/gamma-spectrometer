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
#include "detector.h"   /* det_tail_t */

#define SETTINGS_MAGIC   0x47534631u   /* "GSF1" */
#define SETTINGS_VERSION 3u
#define SETUP_DONE_UNKNOWN 1u   /* done_unix: настроен до появления записи, дата неизвестна */

/* Запись о настройке прибора (мастер на PC): что и когда сделано; 0 в *_unix — не делалось. */
typedef struct {
    uint32_t chk_unix;  uint8_t chk_ok; uint8_t pad_[3];   /* «Проверка электроники» */
    uint32_t zero_unix;                                    /* «Ноль монитора ВВ» */
    uint32_t hv_unix;   float hv_v;   float hv_peak_ch;    /* последний подбор ВВ (шаги 4 и 6) */
    uint32_t tpl_unix;                                     /* эталон загружен мастером */
    uint32_t res_unix;  float res_fwhm_pct; float res_peak_ch;
    uint32_t done_unix;                                    /* «Сохранение и отчёт»; 1 = настроен до учёта */
} setup_rec_t;

typedef struct {
    uint32_t magic, version, size;           /* size = sizeof(settings_t) */
    float hv_mon_zero_mv, hv_mon_k_mv_per_v; /* 2460, 1.640 */
    float hv_setpoint_v;                     /* автозапуск ВВ после подачи питания, В (-hvboot); 0 — выкл */
    uint8_t reserved_boot;                   /* было hv_on_at_boot: не используется, всегда 0 */
    float pt_rpu_ohm;                        /* 10024.5 */
    float tc_t0_c, tc_k1, tc_k2;             /* усиление ФЭУ относительно t0 = 1 + k1·dT + k2·dT²;
                                                канал × 1/это (temp.h, tc_corr); 25, 0, 0 */
    uint16_t thr_codes;                      /* 0 = авто 5 сигма */
    uint32_t becq_words[11];                 /* слова -cal как прислал BecqMoni */
    uint8_t becq_valid;
    int16_t tpl[64][7];                      /* эталон с реального тракта (Q14) */
    uint8_t tpl_valid;
    setup_rec_t setup;                       /* запись о настройке */
    det_tail_t tail;                         /* таблица хвоста для разбора наложений (-tlw/-tlm) */
    uint8_t tail_valid;                      /* таблица принята (-tlok) */
    uint8_t pile_on;                         /* разбор наложений включён (-pile); по умолчанию 1 */
    uint8_t reserved[6];            /* до кратности 8: флеш пишется двойными словами */
    uint32_t crc32;                          /* по всем байтам до crc32; последнее поле */
} settings_t;
/* sizeof(settings_t) кратен 8 и ≤ страницы 4 КБ — проверяется при сборке в settings.c */

/* Значения по умолчанию (структура обнуляется целиком) + seal. */
void settings_defaults(settings_t *s);
/* magic, version, size, crc. */
bool settings_valid(const settings_t *s);
/* Пересчитать crc32. */
void settings_seal(settings_t *s);

/* Образ страницы флеш → настройки: v3 как есть (settings_valid), v2 и v1 — перенос всех полей (таблицы хвоста нет, pile_on = 1),
   setup пустой (done_unix = SETUP_DONE_UNKNOWN, если у v1 были эталон и калибровка BecqMoni).
   false — образ пустой/битый/чужой; *out не тронут. */
bool settings_from_image(const uint8_t *img, settings_t *out);
/* live (с пересчитанным crc) совпадает с образом байт в байт; live не меняется. */
bool settings_same_as_image(const settings_t *live, const uint8_t *img);

/* CRC-32 zip: poly 0x04C11DB7 reflected, init/xorout 0xFFFFFFFF. */
uint32_t crc32_zip(const uint8_t *p, size_t n);

#endif
