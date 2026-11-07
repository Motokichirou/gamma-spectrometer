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
    v.version = SETTINGS_VERSION + 1u;
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

int main(void)
{
    test_load_blank();
    test_save_load_roundtrip();
    test_save_same_skips();
    test_load_rejects();
    test_save_unsealed_refused();
    test_save_failures();
    return T_DONE();
}
