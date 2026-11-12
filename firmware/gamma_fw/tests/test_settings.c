/* Тесты settings: умолчания, CRC, версия, размер, crc32_zip. */
#include <string.h>
#include "t.h"
#include "settings.h"

static void test_defaults_valid(void)
{
    settings_t s; settings_defaults(&s); settings_seal(&s);
    CHECK(settings_valid(&s));
    CHECK(s.magic == SETTINGS_MAGIC && s.version == SETTINGS_VERSION);
    CHECK(s.size == sizeof(settings_t));
    CHECK_NEAR(s.hv_mon_zero_mv, 2461.8, 1e-4);
    CHECK_NEAR(s.hv_mon_k_mv_per_v, 1.633, 1e-6);
    CHECK_NEAR(s.hv_setpoint_v, 0.0, 1e-9);
    CHECK(s.reserved_boot == 0);
    CHECK_NEAR(s.pt_rpu_ohm, 10024.5, 1e-3);
    CHECK_NEAR(s.tc_t0_c, 25.0, 1e-6);
    CHECK_NEAR(s.tc_k1, 0.0, 1e-12);
    CHECK_NEAR(s.tc_k2, 0.0, 1e-12);
    CHECK(s.thr_codes == 0);
    CHECK(!s.becq_valid && !s.tpl_valid);
}

static void test_crc_corruption(void)
{
    settings_t s; settings_defaults(&s);
    ((uint8_t *)&s)[offsetof(settings_t, pt_rpu_ohm)] ^= 1;
    CHECK(!settings_valid(&s));
    settings_defaults(&s);
    s.crc32 ^= 1;
    CHECK(!settings_valid(&s));
}

static void test_changed_then_sealed(void)
{
    settings_t s; settings_defaults(&s);
    s.hv_setpoint_v = 800.0f;
    CHECK(!settings_valid(&s));
    settings_seal(&s);
    CHECK(settings_valid(&s));
}

static void test_version_mismatch(void)
{
    settings_t s; settings_defaults(&s);
    s.version = 4; settings_seal(&s);   /* CRC верен, версия чужая */
    CHECK(!settings_valid(&s));
}

static void test_blank_flash(void)
{
    settings_t s; memset(&s, 0xFF, sizeof s);
    CHECK(!settings_valid(&s));
    memset(&s, 0, sizeof s);
    CHECK(!settings_valid(&s));
}

static void test_size_fits_page(void)
{
    CHECK(sizeof(settings_t) <= 4096);
    CHECK(sizeof(settings_t) % 8 == 0);              /* флеш G4 пишется двойными словами */
    CHECK(offsetof(settings_t, crc32) + 4 == sizeof(settings_t));   /* crc — последним, без хвоста */
}

static void test_settings_size(void)
{
    CHECK(sizeof(settings_t) % 8 == 0 && sizeof(settings_t) <= 4096);
}

static void test_v2_layout(void)
{
    settings_t s; settings_defaults(&s);
    CHECK(sizeof(settings_t) % 8 == 0);
    CHECK(sizeof(settings_t) <= 4096);
    CHECK(SETTINGS_VERSION == 3u);
    CHECK(s.tail_valid == 0 && s.pile_on == 1);
    CHECK(DET_TL_ROWS == 16u && DET_TL_LEN == 64u);
    CHECK(s.setup.done_unix == 0);
    CHECK(settings_valid(&s));
}

static void test_crc32_zip(void)
{
    CHECK(crc32_zip((const uint8_t *)"123456789", 9) == 0xCBF43926u);
}

int main(void)
{
    test_defaults_valid();
    test_crc_corruption();
    test_changed_then_sealed();
    test_version_mismatch();
    test_blank_flash();
    test_size_fits_page();
    test_settings_size();
    test_v2_layout();
    test_crc32_zip();
    return T_DONE();
}
