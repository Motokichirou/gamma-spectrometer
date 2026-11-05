/* becq_cal.c — энергокалибровка BecqMoni (порт логики cal_store.c). */
#include <string.h>
#include <math.h>
#include "becq_cal.h"
#include "settings.h"

static const char HEXD[] = "0123456789ABCDEF";

static void put_hex8(char *p, uint32_t v)
{
    for (int i = 7; i >= 0; i--) { p[i] = HEXD[v & 15u]; v >>= 4; }
}

/* "%X": без ведущих нулей (0 → "0"); возвращает число цифр */
static unsigned put_hex_min(char *p, uint32_t v)
{
    unsigned n = 1;
    for (uint32_t t = v >> 4; t; t >>= 4) n++;
    for (unsigned i = n; i-- > 0; v >>= 4) p[i] = HEXD[v & 15u];
    return n;
}

uint32_t becq_checksum(const uint32_t w[10])
{
    char s[80];
    for (int k = 0; k < 10; k++) put_hex8(s + 8 * k, w[k]);
    return crc32_zip((const uint8_t *)s, sizeof s);
}

bool becq_words_to_coef(const uint32_t w[11], int *order, double coef[5])
{
    if (becq_checksum(w) != w[10]) return false;
    double c[5];
    int ord = 0;
    for (int k = 0; k < 5; k++) {
        uint64_t u = ((uint64_t)w[2 * k] << 32) | w[2 * k + 1];
        memcpy(&c[k], &u, sizeof u);
        if (!isfinite(c[k])) return false;
        if (c[k] != 0.0) ord = k;
    }
    if (ord < 1) ord = 1;
    if (order) *order = ord;
    if (coef) for (int k = 0; k < 5; k++) coef[k] = c[k];
    return true;
}

void becq_coef_to_words(int order, const double coef[5], uint32_t w[11])
{
    if (order < 1) order = 1;
    if (order > 4) order = 4;
    for (int k = 0; k < 5; k++) {
        double c = (k <= order) ? coef[k] : 0.0;
        uint64_t u;
        memcpy(&u, &c, sizeof u);
        w[2 * k]     = (uint32_t)(u >> 32);
        w[2 * k + 1] = (uint32_t)(u & 0xFFFFFFFFu);
    }
    w[10] = becq_checksum(w);
}

size_t becq_format_registers(const uint32_t w[11], bool valid,
                             const char *serial, char *out, size_t cap)
{
    size_t slen = serial ? strlen(serial) : 0;
    size_t need = 39u * 10u + slen + 2u + 1u;
    if (!out || cap < need) return 0;
    size_t n = 0;
    for (unsigned i = 0; i < 39u; i++) {
        if (valid && i == BECQ_WORDS - 1u) {
            n += put_hex_min(out + n, w[i]);        /* CRC — как BecqMoni: crc.ToString("X") */
        } else {
            put_hex8(out + n, (valid && i < BECQ_WORDS) ? w[i] : 0u);
            n += 8;
        }
        out[n++] = '\r'; out[n++] = '\n';
    }
    if (slen) memcpy(out + n, serial, slen);
    n += slen;
    out[n++] = '\r'; out[n++] = '\n';
    out[n] = 0;
    return n;
}
