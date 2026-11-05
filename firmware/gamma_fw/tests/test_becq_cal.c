/* Тесты becq_cal: слова, сумма, 40 регистров. */
#include <string.h>
#include <stdlib.h>
#include "t.h"
#include "becq_cal.h"
#include "settings.h"

static void test_becq_roundtrip(void)
{
    const double c[5] = { 1.5, 0.3456, 1e-6, 0, 0 };
    uint32_t w[11]; double r[5]; int ord = 0;
    becq_coef_to_words(2, c, w);
    CHECK(w[10] == becq_checksum(w));
    CHECK(becq_words_to_coef(w, &ord, r));
    CHECK(ord == 2);
    for (int k = 0; k < 5; k++) CHECK(memcmp(&r[k], &c[k], sizeof(double)) == 0);
    w[3] ^= 1;
    CHECK(!becq_words_to_coef(w, &ord, r));   /* битая сумма */
}

static void test_becq_known_words(void)
{
    /* c0 = 10.0 (0x4024000000000000), c1 = 0.5 (0x3FE0000000000000) */
    const double c[5] = { 10.0, 0.5, 0, 0, 0 };
    uint32_t w[11]; becq_coef_to_words(1, c, w);
    CHECK(w[0] == 0x40240000u && w[1] == 0 && w[2] == 0x3FE00000u && w[3] == 0);
}

static void test_becq_order_clamp(void)
{
    const double c[5] = { 1, 2, 3, 4, 5 };
    uint32_t w[11]; double r[5]; int ord;
    becq_coef_to_words(9, c, w);          /* >4 -> 4 */
    CHECK(becq_words_to_coef(w, &ord, r) && ord == 4);
    const double z[5] = { 0, 0, 0, 0, 0 };
    becq_coef_to_words(1, z, w);          /* все нули: степень не ниже 1 */
    CHECK(becq_words_to_coef(w, &ord, r) && ord == 1);
}

static int split_lines(char *buf, char **ln, int max)
{
    int n = 0; char *p = buf;
    while (*p && n < max) {
        ln[n++] = p;
        char *e = strstr(p, "\r\n");
        if (!e) break;
        *e = 0; p = e + 2;
    }
    return n;
}

static void test_becq_registers(void)
{
    const double c[5] = { 1.5, 0.3456, 1e-6, 0, 0 };
    uint32_t w[11]; becq_coef_to_words(2, c, w);
    char buf[512]; char *ln[64];
    size_t n = becq_format_registers(w, true, "GS474-0001", buf, sizeof buf);
    CHECK(n > 0 && n == strlen(buf));
    int cnt = split_lines(buf, ln, 64);
    CHECK(cnt == 40);
    CHECK(strcmp(ln[39], "GS474-0001") == 0);
    CHECK(strtoul(ln[0], 0, 16) == w[0]);
    CHECK(strtoul(ln[10], 0, 16) == w[10]);
    for (int i = 11; i < 39; i++) CHECK(strcmp(ln[i], "00000000") == 0);

    n = becq_format_registers(w, false, "GS474-0001", buf, sizeof buf);
    CHECK(n > 0);
    cnt = split_lines(buf, ln, 64);
    CHECK(cnt == 40);
    for (int i = 0; i <= 10; i++) CHECK(strcmp(ln[i], "00000000") == 0);
    CHECK(strcmp(ln[39], "GS474-0001") == 0);

    char tiny[16];
    CHECK(becq_format_registers(w, true, "GS474-0001", tiny, sizeof tiny) == 0);
}

int main(void)
{
    test_becq_roundtrip();
    test_becq_known_words();
    test_becq_order_clamp();
    test_becq_registers();
    return T_DONE();
}
