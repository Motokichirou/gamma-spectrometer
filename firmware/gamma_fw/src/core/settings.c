/* settings.c — умолчания, CRC-32, проверка настроек. */
#include <string.h>
#include "settings.h"

uint32_t crc32_zip(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Сумма по всем байтам структуры до поля crc32, включая выравнивание —
 * поэтому структура всегда обнуляется целиком перед заполнением. */
static uint32_t calc_crc(const settings_t *s)
{
    return crc32_zip((const uint8_t *)s, offsetof(settings_t, crc32));
}

void settings_seal(settings_t *s)
{
    s->crc32 = calc_crc(s);
}

void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->magic = SETTINGS_MAGIC;
    s->version = SETTINGS_VERSION;
    s->size = (uint32_t)sizeof *s;
    s->hv_mon_zero_mv = 2460.0f;
    s->hv_mon_k_mv_per_v = 1.640f;
    s->hv_setpoint_v = 0.0f;
    s->hv_on_at_boot = 0;
    s->pt_rpu_ohm = 10024.5f;
    s->tc_t0_c = 25.0f;
    s->tc_k1 = 0.0f;
    s->tc_k2 = 0.0f;
    s->thr_codes = 0;
    s->becq_valid = 0;
    s->tpl_valid = 0;
    settings_seal(s);
}

bool settings_valid(const settings_t *s)
{
    if (s->magic != SETTINGS_MAGIC) return false;
    if (s->version != SETTINGS_VERSION) return false;
    if (s->size != sizeof *s) return false;
    return s->crc32 == calc_crc(s);
}
