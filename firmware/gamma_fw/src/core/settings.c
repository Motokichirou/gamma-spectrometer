/* settings.c — умолчания, CRC-32, проверка настроек. */
#include <string.h>
#include "settings.h"

/* проверки при сборке (MSVC без /std:c11 не знает _Static_assert): запись во флеш G4 —
   двойными словами (8 байт), одна страница 4 КБ */
typedef char chk_settings_x8[(sizeof(settings_t) % 8u == 0u) ? 1 : -1];
typedef char chk_settings_page[(sizeof(settings_t) <= 4096u) ? 1 : -1];

/* Раскладка settings_t версии 1 (прежняя, на приборах в поле) — для переноса при загрузке. */
typedef struct {
    uint32_t magic, version, size;
    float hv_mon_zero_mv, hv_mon_k_mv_per_v;
    float hv_setpoint_v;
    uint8_t reserved_boot;
    float pt_rpu_ohm;
    float tc_t0_c, tc_k1, tc_k2;
    uint16_t thr_codes;
    uint32_t becq_words[11];
    uint8_t becq_valid;
    int16_t tpl[64][7];
    uint8_t tpl_valid;
    uint8_t reserved[5];
    uint32_t crc32;
} settings_v1_t;

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
    s->hv_mon_zero_mv = 2461.8f;      /* калибровка 2026-11-06 (ADC2 ×256), прибор №1 */
    s->hv_mon_k_mv_per_v = 1.633f;
    s->hv_setpoint_v = 0.0f;
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

/* чтение из образа без выравнивания */
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool settings_from_image(const uint8_t *img, settings_t *out)
{
    if (!img || !out) return false;
    if (rd32(img + offsetof(settings_t, magic)) != SETTINGS_MAGIC) return false;
    const uint32_t ver = rd32(img + offsetof(settings_t, version));
    const uint32_t size = rd32(img + offsetof(settings_t, size));

    if (ver == SETTINGS_VERSION && size == (uint32_t)sizeof(settings_t)) {
        settings_t t;
        memcpy(&t, img, sizeof t);
        if (!settings_valid(&t)) return false;
        *out = t;
        return true;
    }
    if (ver == 1u && size == (uint32_t)sizeof(settings_v1_t)) {
        if (rd32(img + offsetof(settings_v1_t, crc32)) != crc32_zip(img, offsetof(settings_v1_t, crc32)))
            return false;
        settings_v1_t v;
        memcpy(&v, img, sizeof v);
        settings_t t;
        settings_defaults(&t);
        t.hv_mon_zero_mv = v.hv_mon_zero_mv;
        t.hv_mon_k_mv_per_v = v.hv_mon_k_mv_per_v;
        t.hv_setpoint_v = v.hv_setpoint_v;
        t.reserved_boot = v.reserved_boot;
        t.pt_rpu_ohm = v.pt_rpu_ohm;
        t.tc_t0_c = v.tc_t0_c; t.tc_k1 = v.tc_k1; t.tc_k2 = v.tc_k2;
        t.thr_codes = v.thr_codes;
        memcpy(t.becq_words, v.becq_words, sizeof t.becq_words);
        t.becq_valid = v.becq_valid;
        memcpy(t.tpl, v.tpl, sizeof t.tpl);
        t.tpl_valid = v.tpl_valid;
        if (v.tpl_valid && v.becq_valid) t.setup.done_unix = SETUP_DONE_UNKNOWN;
        settings_seal(&t);
        *out = t;
        return true;
    }
    return false;
}

bool settings_same_as_image(const settings_t *live, const uint8_t *img)
{
    if (!live || !img) return false;
    settings_t t = *live;
    settings_seal(&t);
    return memcmp(&t, img, sizeof t) == 0;
}
