/* Тесты flashstore: загрузка из образа страницы и порядок записи на модели NOR-флеш G4
   (стирание → 0xFF; двойное слово пишется только в стёртое место и только по адресу, кратному 8,
   как PROGERR/PGAERR у FLASH G4). */
#include <string.h>
#include "t.h"
#include "flashstore.h"

#define PAGE 4096

typedef struct {
    uint8_t mem[PAGE];
    int n_erase, n_prog;
    bool fail_erase;
    int fail_prog_at;              /* номер записи (с 1), на которой отказ; 0 — без отказа */
    int stuck_byte;                /* байт, в котором бит 0 не программируется (1 → 0), −1 — нет */
} nor_t;

static bool m_erase(void *c)
{
    nor_t *f = c;
    f->n_erase++;
    if (f->fail_erase) return false;
    memset(f->mem, 0xFF, PAGE);
    return true;
}

static bool m_prog(void *c, uint32_t off, uint32_t lo, uint32_t hi)
{
    nor_t *f = c;
    f->n_prog++;
    if (f->fail_prog_at && f->n_prog == f->fail_prog_at) return false;
    if (off % 8u || off + 8u > PAGE) return false;                 /* PGAERR */
    for (int i = 0; i < 8; i++) if (f->mem[off + i] != 0xFF) return false;  /* PROGERR */
    uint8_t b[8];
    for (int i = 0; i < 4; i++) { b[i] = (uint8_t)(lo >> (8 * i)); b[4 + i] = (uint8_t)(hi >> (8 * i)); }
    for (int i = 0; i < 8; i++) {
        uint8_t v = b[i];
        if ((int)(off + i) == f->stuck_byte) v |= 1u;              /* бит 0 «залип» в 1 */
        f->mem[off + i] &= v;                                      /* NOR: только 1 → 0 */
    }
    return true;
}

static const uint8_t *m_page(void *c) { return ((nor_t *)c)->mem; }

static nor_t nor;
static fstore_ops_t ops = { m_erase, m_prog, m_page, &nor };

static void nor_blank(void)
{
    memset(&nor, 0, sizeof nor);
    memset(nor.mem, 0xFF, PAGE);
    nor.stuck_byte = -1;
}

static void sample(settings_t *s)
{
    settings_defaults(s);
    s->hv_mon_k_mv_per_v = 1.652f;
    s->pt_rpu_ohm = 10031.0f;
    s->becq_words[3] = 0xDEADBEEFu;
    s->becq_valid = 1;
    s->tpl[63][6] = -1234;
    settings_seal(s);
}

/* первая прошивка: страница стёрта — false, *out не тронут */
static void test_load_blank(void)
{
    nor_blank();
    settings_t out;
    memset(&out, 0x5A, sizeof out);
    CHECK(!fstore_load(nor.mem, &out));
    CHECK(((uint8_t *)&out)[0] == 0x5A && ((uint8_t *)&out)[sizeof out - 1] == 0x5A);
    CHECK(!fstore_load(NULL, &out));
}

static void test_save_load_roundtrip(void)
{
    nor_blank();
    settings_t s, out;
    sample(&s);
    CHECK(fstore_save(&s, &ops));
    CHECK(nor.n_erase == 1 && nor.n_prog == (int)(sizeof s / 8u));
    memset(&out, 0, sizeof out);
    CHECK(fstore_load(nor.mem, &out));
    CHECK(memcmp(&out, &s, sizeof s) == 0);
    CHECK_NEAR(out.hv_mon_k_mv_per_v, 1.652, 1e-6);
    CHECK(out.becq_words[3] == 0xDEADBEEFu && out.tpl[63][6] == -1234);
    /* за пределами структуры страница осталась стёртой */
    CHECK(nor.mem[sizeof s] == 0xFF && nor.mem[PAGE - 1] == 0xFF);
}

/* то же самое повторно — страница не стирается (нет остановки ЦП и износа) */
static void test_save_same_skips(void)
{
    nor_blank();
    settings_t s;
    sample(&s);
    CHECK(fstore_save(&s, &ops));
    int e = nor.n_erase, p = nor.n_prog;
    CHECK(fstore_save(&s, &ops));
    CHECK(nor.n_erase == e && nor.n_prog == p);
    s.pt_rpu_ohm = 10040.0f;
    settings_seal(&s);
    CHECK(fstore_save(&s, &ops));                   /* изменилось — стирание и запись заново */
    CHECK(nor.n_erase == e + 1);
    settings_t out;
    CHECK(fstore_load(nor.mem, &out) && out.pt_rpu_ohm == 10040.0f);
}

/* битые/чужие образы: CRC, версия, размер — не загружаются */
static void test_load_rejects(void)
{
    settings_t s, out;
    uint8_t img[PAGE];
    sample(&s);

    memset(img, 0xFF, sizeof img);
    memcpy(img, &s, sizeof s);
    CHECK(fstore_load(img, &out));
    img[offsetof(settings_t, pt_rpu_ohm) + 1] ^= 0x10;          /* 1 бит — CRC не сходится */
    CHECK(!fstore_load(img, &out));

    settings_t v = s;                                            /* старая/новая версия структуры */
    v.version = 5u;                                              /* ни v1, ни v2, ни v3, ни v4 */
    settings_seal(&v);
    memcpy(img, &v, sizeof v);
    CHECK(!fstore_load(img, &out));

    settings_t z = s;                                            /* другой размер структуры */
    z.size = (uint32_t)sizeof z - 8u;
    settings_seal(&z);
    memcpy(img, &z, sizeof z);
    CHECK(!fstore_load(img, &out));

    memset(img, 0, sizeof img);                                  /* обнулённая страница */
    CHECK(!fstore_load(img, &out));
}

/* незапечатанное не пишется: иначе при старте не загрузится, а страница уже стёрта */
static void test_save_unsealed_refused(void)
{
    nor_blank();
    settings_t s;
    sample(&s);
    CHECK(fstore_save(&s, &ops));
    s.pt_rpu_ohm = 1.0f;                                         /* без settings_seal */
    CHECK(!fstore_save(&s, &ops));
    CHECK(nor.n_erase == 1);                                     /* прежняя запись цела */
    settings_t out;
    CHECK(fstore_load(nor.mem, &out) && out.pt_rpu_ohm == 10031.0f);
}

static void test_save_failures(void)
{
    settings_t s, out;
    sample(&s);

    nor_blank();
    nor.fail_erase = true;
    CHECK(!fstore_save(&s, &ops));
    CHECK(nor.n_prog == 0);

    nor_blank();
    nor.fail_prog_at = 5;                                        /* отказ посреди записи */
    CHECK(!fstore_save(&s, &ops));
    CHECK(nor.n_prog == 5);
    CHECK(!fstore_load(nor.mem, &out));                          /* недописанное не загрузится */

    nor_blank();
    /* запись «прошла», а байт не тот; байт с битом 0 = 0 (2461.8f = cd dc 19 45: берём dc) */
    nor.stuck_byte = (int)offsetof(settings_t, hv_mon_zero_mv) + 1;
    CHECK(!fstore_save(&s, &ops));
    CHECK(!fstore_load(nor.mem, &out));

    CHECK(!fstore_save(NULL, &ops));
    fstore_ops_t bad = ops;
    bad.erase = NULL;
    CHECK(!fstore_save(&s, &bad));
}

/* замороженная копия раскладки settings_t версии 1 (как на приборе до v2) */
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
} v1_t;

static void mk_v1(v1_t *v, int tpl_valid)
{
    memset(v, 0, sizeof *v);
    v->magic = SETTINGS_MAGIC; v->version = 1; v->size = (uint32_t)sizeof *v;
    v->hv_mon_zero_mv = 2469.7f; v->hv_mon_k_mv_per_v = 1.633f; v->hv_setpoint_v = 565.0f;
    v->pt_rpu_ohm = 10024.5f; v->tc_t0_c = 25.0f; v->tc_k1 = 0.001f; v->tc_k2 = 0.0f;
    v->thr_codes = 7; v->becq_valid = 1; v->becq_words[0] = 0x12345678u;
    v->tpl_valid = (uint8_t)tpl_valid; v->tpl[3][2] = -1234;
    v->crc32 = crc32_zip((const uint8_t *)v, offsetof(v1_t, crc32));
}

static void test_migrate_v1(void)
{
    uint8_t img[PAGE];
    v1_t v; settings_t out;

    mk_v1(&v, 1);
    memset(img, 0xFF, sizeof img); memcpy(img, &v, sizeof v);
    memset(&out, 0, sizeof out);
    CHECK(fstore_load(img, &out));
    CHECK_NEAR(out.hv_mon_zero_mv, 2469.7, 1e-3);
    CHECK_NEAR(out.hv_mon_k_mv_per_v, 1.633, 1e-6);
    CHECK_NEAR(out.hv_setpoint_v, 565.0, 1e-6);
    CHECK_NEAR(out.pt_rpu_ohm, 10024.5, 1e-3);
    CHECK_NEAR(out.tc_k1, 0.001, 1e-9);
    CHECK_NEAR(out.tc_t0_c, 25.0, 1e-6);
    CHECK(out.thr_codes == 7);
    CHECK(out.becq_valid == 1 && out.becq_words[0] == 0x12345678u);
    CHECK(out.tpl_valid == 1 && out.tpl[3][2] == -1234);
    CHECK(out.version == SETTINGS_VERSION && settings_valid(&out));
    CHECK(out.tail_valid == 0 && out.pile_on == 1 && out.dnl_n == 0);
    CHECK(out.setup.done_unix == SETUP_DONE_UNKNOWN);
    CHECK(out.setup.chk_unix == 0 && out.setup.hv_unix == 0 && out.setup.res_unix == 0);

    mk_v1(&v, 0);                                   /* эталона нет — «не настроен» */
    memset(img, 0xFF, sizeof img); memcpy(img, &v, sizeof v);
    CHECK(fstore_load(img, &out));
    CHECK(out.setup.done_unix == 0 && out.thr_codes == 7 && out.tpl_valid == 0);

    mk_v1(&v, 1);                                   /* битый CRC v1 */
    v.crc32 ^= 1u;
    memset(img, 0xFF, sizeof img); memcpy(img, &v, sizeof v);
    CHECK(!fstore_load(img, &out));
}

/* замороженная копия раскладки settings_t версии 2 (как на приборе до v3) */
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
} v2_t;

static void mk_v2(v2_t *v)
{
    memset(v, 0, sizeof *v);
    v->magic = SETTINGS_MAGIC; v->version = 2; v->size = (uint32_t)sizeof *v;
    v->hv_mon_zero_mv = 2461.8f; v->hv_mon_k_mv_per_v = 1.640f; v->hv_setpoint_v = 755.0f;
    v->reserved_boot = 0; v->pt_rpu_ohm = 10031.0f; v->tc_t0_c = 25.0f; v->tc_k1 = 0.002f; v->tc_k2 = 1e-5f;
    v->thr_codes = 11; v->becq_valid = 1; v->becq_words[10] = 0xCAFEBABEu;
    v->tpl_valid = 1; v->tpl[63][6] = -4321; v->tpl[0][0] = 77;
    v->setup.chk_unix = 111; v->setup.chk_ok = 1; v->setup.zero_unix = 222;
    v->setup.hv_unix = 333; v->setup.hv_v = 755.5f; v->setup.hv_peak_ch = 1234.5f;
    v->setup.tpl_unix = 444; v->setup.res_unix = 555; v->setup.res_fwhm_pct = 7.1f;
    v->setup.res_peak_ch = 999.0f; v->setup.done_unix = 666;
    v->crc32 = crc32_zip((const uint8_t *)v, offsetof(v2_t, crc32));
}

static void test_settings_v2_to_v3(void)
{
    uint8_t img[PAGE];
    v2_t v; settings_t out;

    mk_v2(&v);
    memset(img, 0xFF, sizeof img); memcpy(img, &v, sizeof v);
    memset(&out, 0xAA, sizeof out);
    CHECK(fstore_load(img, &out));
    CHECK(out.version == SETTINGS_VERSION && out.size == (uint32_t)sizeof(settings_t) && settings_valid(&out));
    CHECK_NEAR(out.hv_mon_zero_mv, 2461.8, 1e-3);
    CHECK_NEAR(out.hv_mon_k_mv_per_v, 1.640, 1e-6);
    CHECK_NEAR(out.hv_setpoint_v, 755.0, 1e-6);
    CHECK_NEAR(out.pt_rpu_ohm, 10031.0, 1e-3);
    CHECK_NEAR(out.tc_k1, 0.002, 1e-9);
    CHECK_NEAR(out.tc_k2, 1e-5, 1e-10);
    CHECK(out.thr_codes == 11);
    CHECK(out.becq_valid == 1 && out.becq_words[10] == 0xCAFEBABEu);
    CHECK(out.tpl_valid == 1 && out.tpl[63][6] == -4321 && out.tpl[0][0] == 77);
    CHECK(out.setup.chk_unix == 111 && out.setup.chk_ok == 1 && out.setup.zero_unix == 222);
    CHECK(out.setup.hv_unix == 333 && out.setup.tpl_unix == 444 && out.setup.res_unix == 555);
    CHECK_NEAR(out.setup.hv_v, 755.5, 1e-3);
    CHECK_NEAR(out.setup.res_fwhm_pct, 7.1, 1e-4);
    CHECK(out.setup.done_unix == 666);
    CHECK(out.tail_valid == 0 && out.pile_on == 1);
    CHECK(out.dnl_n == 0 && out.pad_dnl == 0);
    {   /* таблица хвоста пуста */
        const uint8_t *t = (const uint8_t *)&out.tail;
        int nz = 0;
        for (size_t i = 0; i < sizeof out.tail; i++) nz |= t[i];
        CHECK(nz == 0);
    }

    mk_v2(&v);                                      /* битый CRC v2 */
    v.crc32 ^= 1u;
    memset(img, 0xFF, sizeof img); memcpy(img, &v, sizeof v);
    CHECK(!fstore_load(img, &out));
}

/* замороженная копия раскладки settings_t версии 3 (как на приборе до v4) */
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
} v3_t;

static void mk_v3(v3_t *v)
{
    v2_t v2;
    mk_v2(&v2);
    memset(v, 0, sizeof *v);
    memcpy(v, &v2, offsetof(v2_t, setup) + sizeof v2.setup);   /* до конца setup раскладки совпадают */
    v->version = 3; v->size = (uint32_t)sizeof *v;
    v->tail.s[0][0] = 32767; v->tail.s[15][63] = -123; v->tail.row[63] = 15;
    v->tail_valid = 1; v->pile_on = 0;
    v->crc32 = crc32_zip((const uint8_t *)v, offsetof(v3_t, crc32));
}

static void test_settings_v3_to_v4(void)
{
    uint8_t img[PAGE];
    v3_t v; settings_t out;
    CHECK(sizeof(v3_t) == 3160u);                   /* как прошивки v3 в поле */

    mk_v3(&v);
    memset(img, 0xFF, sizeof img); memcpy(img, &v, sizeof v);
    memset(&out, 0xAA, sizeof out);
    CHECK(fstore_load(img, &out));
    CHECK(out.version == 4u && out.size == (uint32_t)sizeof(settings_t) && settings_valid(&out));
    CHECK_NEAR(out.hv_setpoint_v, 755.0, 1e-6);
    CHECK_NEAR(out.tc_k2, 1e-5, 1e-10);
    CHECK(out.thr_codes == 11 && out.becq_words[10] == 0xCAFEBABEu);
    CHECK(out.tpl_valid == 1 && out.tpl[63][6] == -4321);
    CHECK(out.setup.done_unix == 666 && out.setup.tpl_unix == 444);
    CHECK(out.tail.s[0][0] == 32767 && out.tail.s[15][63] == -123 && out.tail.row[63] == 15);
    CHECK(out.tail_valid == 1 && out.pile_on == 0);
    CHECK(out.dnl_n == 0 && out.pad_dnl == 0);
    {   /* список DNL и резерв — нули */
        int nz = 0;
        for (unsigned k = 0; k < DET_DNL_MAX; k++) nz |= out.dnl[k].code | (uint16_t)out.dnl[k].off;
        for (unsigned k = 0; k < sizeof out.reserved; k++) nz |= out.reserved[k];
        CHECK(nz == 0);
    }

    mk_v3(&v);                                      /* битый CRC v3 */
    v.crc32 ^= 1u;
    memset(img, 0xFF, sizeof img); memcpy(img, &v, sizeof v);
    CHECK(!fstore_load(img, &out));

    /* v4 со списком: туда и обратно */
    settings_t s;
    settings_defaults(&s);
    s.dnl_n = 2; s.dnl[0].code = 2047; s.dnl[0].off = 112; s.dnl[1].code = 767; s.dnl[1].off = -9;
    settings_seal(&s);
    memset(img, 0xFF, sizeof img); memcpy(img, &s, sizeof s);
    CHECK(fstore_load(img, &out));
    CHECK(out.dnl_n == 2 && out.dnl[0].code == 2047 && out.dnl[0].off == 112 && out.dnl[1].off == -9);
}

static void test_same_as_image(void)
{
    nor_blank();
    settings_t live;
    sample(&live);
    CHECK(fstore_save(&live, &ops));
    CHECK(settings_same_as_image(&live, nor.mem));
    live.setup.res_fwhm_pct = 7.1f;                 /* без settings_seal */
    CHECK(!settings_same_as_image(&live, nor.mem));
    settings_seal(&live);                           /* запечатан, но в флеш не записан */
    CHECK(!settings_same_as_image(&live, nor.mem));
}

int main(void)
{
    test_load_blank();
    test_save_load_roundtrip();
    test_save_same_skips();
    test_load_rejects();
    test_save_unsealed_refused();
    test_save_failures();
    test_migrate_v1();
    test_settings_v2_to_v3();
    test_settings_v3_to_v4();
    test_same_as_image();
    return T_DONE();
}
