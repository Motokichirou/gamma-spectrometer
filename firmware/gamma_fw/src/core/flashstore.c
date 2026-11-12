/* flashstore.c — см. flashstore.h */
#include <string.h>
#include "flashstore.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Проверка прямо по образу, побайтно (без копии settings_t на стеке ~3 КБ: запись зовётся
   из глубины proto_rx, стек 4 КБ) — то же, что settings_valid (только текущая версия). */
static bool img_valid(const uint8_t *img)
{
    if (rd32(img + offsetof(settings_t, magic)) != SETTINGS_MAGIC) return false;
    if (rd32(img + offsetof(settings_t, version)) != SETTINGS_VERSION) return false;
    if (rd32(img + offsetof(settings_t, size)) != (uint32_t)sizeof(settings_t)) return false;
    return rd32(img + offsetof(settings_t, crc32)) == crc32_zip(img, offsetof(settings_t, crc32));
}

bool fstore_load(const uint8_t *img, settings_t *out)
{
    if (!img) return false;
    return settings_from_image(img, out);      /* v3 как есть, v2 и v1 с переносом полей */
}

bool fstore_save(const settings_t *s, const fstore_ops_t *ops)
{
    if (!s || !ops || !ops->erase || !ops->prog64 || !ops->page) return false;
    if (!settings_valid(s)) return false;      /* незапечатанное не пишем: при старте не загрузится */

    const uint8_t *src = (const uint8_t *)s;
    const size_t n = sizeof *s;                /* кратен 8 — проверяется в settings.c */
    const uint8_t *pg = ops->page(ops->ctx);
    if (pg && memcmp(pg, src, n) == 0) return true;   /* уже записано — страницу не трогать */

    if (!ops->erase(ops->ctx)) return false;
    for (size_t off = 0; off < n; off += 8u)
        if (!ops->prog64(ops->ctx, (uint32_t)off, rd32(src + off), rd32(src + off + 4u))) return false;

    pg = ops->page(ops->ctx);
    return pg && memcmp(pg, src, n) == 0 && img_valid(pg);   /* сверка чтением */
}
