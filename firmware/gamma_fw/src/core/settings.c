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

/* Раскладка settings_t версии 2 (прежняя, на приборах в поле): v3 дописала поля после setup,
   поэтому всё до конца setup совпадает байт в байт — v2 переносится одним memcpy. */
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
    setup_rec_t setup;
    uint8_t reserved[8];
    uint32_t crc32;
} settings_v2_t;

/* Раскладка settings_t версии 3 (прежняя, на приборах в поле): v4 дописала список DNL после pile_on, поэтому всё
   до pile_on включительно совпадает байт в байт — v3 переносится одним memcpy. */
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
    setup_rec_t setup;
    det_tail_t tail;
    uint8_t tail_valid;
    uint8_t pile_on;
    uint8_t reserved[6];
    uint32_t crc32;
} settings_v3_t;
typedef char chk_v3_size[(sizeof(settings_v3_t) == 3160u) ? 1 : -1];     /* как прошивки v3 в поле */
typedef char chk_v3_prefix[(offsetof(settings_v3_t, pile_on) == offsetof(settings_t, pile_on)) ? 1 : -1];
/* Любая перестановка полей v4 до pile_on (и v3/v2 внутри общего префикса) ломает сборку: перенос v3/v2 — memcpy префикса */
#define SAME_OFF(T, f) typedef char chk_off_##T##_##f[(offsetof(T, f) == offsetof(settings_t, f)) ? 1 : -1]
SAME_OFF(settings_v3_t, hv_mon_zero_mv);
SAME_OFF(settings_v3_t, hv_setpoint_v);
SAME_OFF(settings_v3_t, pt_rpu_ohm);
SAME_OFF(settings_v3_t, tc_t0_c);
SAME_OFF(settings_v3_t, thr_codes);
SAME_OFF(settings_v3_t, becq_words);
SAME_OFF(settings_v3_t, becq_valid);
SAME_OFF(settings_v3_t, tpl);
SAME_OFF(settings_v3_t, tpl_valid);
SAME_OFF(settings_v3_t, setup);
SAME_OFF(settings_v3_t, tail);
SAME_OFF(settings_v3_t, tail_valid);
SAME_OFF(settings_v3_t, pile_on);
/* v2: общий с v4 префикс — до конца setup включительно */
SAME_OFF(settings_v2_t, hv_mon_zero_mv);
SAME_OFF(settings_v2_t, hv_setpoint_v);
SAME_OFF(settings_v2_t, pt_rpu_ohm);
SAME_OFF(settings_v2_t, tc_t0_c);
SAME_OFF(settings_v2_t, thr_codes);
SAME_OFF(settings_v2_t, becq_words);
SAME_OFF(settings_v2_t, becq_valid);
SAME_OFF(settings_v2_t, tpl);
SAME_OFF(settings_v2_t, tpl_valid);
SAME_OFF(settings_v2_t, setup);

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
    s->tail_valid = 0;
    s->pile_on = 1;
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

    /* Без копии settings_t на стеке (≈ 3 КБ, стек 4 КБ): CRC проверяется прямо по образу,
       результат собирается сразу в *out (после проверки — при отказе *out не тронут). */
    if (ver == SETTINGS_VERSION && size == (uint32_t)sizeof(settings_t)) {
        if (rd32(img + offsetof(settings_t, crc32)) != crc32_zip(img, offsetof(settings_t, crc32)))
            return false;
        memcpy(out, img, sizeof *out);
        return true;
    }
    if (ver == 3u && size == (uint32_t)sizeof(settings_v3_t)) {
        if (rd32(img + offsetof(settings_v3_t, crc32)) != crc32_zip(img, offsetof(settings_v3_t, crc32)))
            return false;
        settings_defaults(out);                   /* dnl_n 0, список нули */
        memcpy(out, img, offsetof(settings_v3_t, pile_on) + 1u);
        out->version = SETTINGS_VERSION;          /* magic/size из образа v3 — перекрыть */
        out->size = (uint32_t)sizeof *out;
        settings_seal(out);
        return true;
    }
    if (ver == 2u && size == (uint32_t)sizeof(settings_v2_t)) {
        if (rd32(img + offsetof(settings_v2_t, crc32)) != crc32_zip(img, offsetof(settings_v2_t, crc32)))
            return false;
        settings_defaults(out);                   /* tail нули, tail_valid 0, pile_on 1, dnl_n 0 */
        memcpy(out, img, offsetof(settings_v2_t, setup) + sizeof out->setup);
        out->version = SETTINGS_VERSION;          /* magic/size из образа v2 — перекрыть */
        out->size = (uint32_t)sizeof *out;
        settings_seal(out);
        return true;
    }
    if (ver == 1u && size == (uint32_t)sizeof(settings_v1_t)) {
        if (rd32(img + offsetof(settings_v1_t, crc32)) != crc32_zip(img, offsetof(settings_v1_t, crc32)))
            return false;
        settings_v1_t v;                          /* ≈ 1.1 КБ, как и раньше */
        memcpy(&v, img, sizeof v);
        settings_defaults(out);
        out->hv_mon_zero_mv = v.hv_mon_zero_mv;
        out->hv_mon_k_mv_per_v = v.hv_mon_k_mv_per_v;
        out->hv_setpoint_v = v.hv_setpoint_v;
        out->reserved_boot = v.reserved_boot;
        out->pt_rpu_ohm = v.pt_rpu_ohm;
        out->tc_t0_c = v.tc_t0_c; out->tc_k1 = v.tc_k1; out->tc_k2 = v.tc_k2;
        out->thr_codes = v.thr_codes;
        memcpy(out->becq_words, v.becq_words, sizeof out->becq_words);
        out->becq_valid = v.becq_valid;
        memcpy(out->tpl, v.tpl, sizeof out->tpl);
        out->tpl_valid = v.tpl_valid;
        if (v.tpl_valid && v.becq_valid) out->setup.done_unix = SETUP_DONE_UNKNOWN;
        settings_seal(out);
        return true;
    }
    return false;
}

bool settings_same_as_image(const settings_t *live, const uint8_t *img)
{
    if (!live || !img) return false;
    /* без копии settings_t на стеке (≈ 1 КБ): тело и хвост — memcmp, crc — по live */
    const size_t o = offsetof(settings_t, crc32);
    uint32_t crc = calc_crc(live), icrc;
    memcpy(&icrc, img + o, sizeof icrc);
    if (crc != icrc) return false;
    if (memcmp(live, img, o) != 0) return false;
    return memcmp((const uint8_t *)live + o + sizeof crc, img + o + sizeof crc,
                  sizeof(settings_t) - o - sizeof crc) == 0;
}
