/*
 * flashstore.h — настройки на одной странице флеш: загрузка из образа и порядок записи.
 * Переносимый модуль: само железо (стирание, запись двойного слова, адрес страницы) —
 * за указателями fstore_ops_t (на плате — src/hw/flash_int.c, на ПК — модель NOR-флеш в тесте).
 *
 * Запись (SPEC §7.9, §9): только при остановленном наборе — это обеспечивает proto
 * (settings_commit). Порядок: если страница уже равна настройкам — ничего не делать
 * (нет стирания — нет остановки ЦП на ~25 мс и лишнего износа); иначе стереть страницу,
 * записать двойными словами (8 байт, младшее слово первым), сверить чтением байт в байт
 * и прогнать settings_valid по прочитанному.
 */
#ifndef FLASHSTORE_H
#define FLASHSTORE_H

#include <stdint.h>
#include <stdbool.h>
#include "settings.h"

typedef struct {
    bool (*erase)(void *ctx);                                 /* стереть страницу (всё 0xFF) */
    bool (*prog64)(void *ctx, uint32_t off, uint32_t lo, uint32_t hi); /* off кратен 8 */
    const uint8_t *(*page)(void *ctx);                        /* содержимое страницы для чтения */
    void *ctx;
} fstore_ops_t;

/* Образ страницы → настройки. true — образ прошёл settings_valid (magic, version, size, CRC),
   *out заполнен; false — пусто (0xFF после стирания), битый CRC, другая версия структуры —
   *out не тронут, приложение берёт settings_defaults. */
bool fstore_load(const uint8_t *img, settings_t *out);

/* Записать настройки (уже запечатанные settings_seal). false — настройки невалидны (не пишем),
   сбой стирания/записи или прочитанное не совпало. */
bool fstore_save(const settings_t *s, const fstore_ops_t *ops);

#endif
