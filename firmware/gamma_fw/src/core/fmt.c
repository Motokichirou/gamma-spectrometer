/* fmt.c — см. fmt.h (вынесено из proto.c: нужно и сервису svc.c). */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "fmt.h"

void fmt_fix(char *out, size_t cap, float v, int dec)
{
    static const uint32_t P10[] = { 1, 10, 100, 1000, 10000, 100000, 1000000 };
    if (dec < 0) dec = 0;
    if (dec > 6) dec = 6;
    if (!isfinite(v)) { snprintf(out, cap, "nan"); return; }
    const char *sg = (v < 0.0f) ? "-" : "";
    double a = fabs((double)v) * P10[dec] + 0.5;
    if (a > 4.0e9) a = 4.0e9;
    uint32_t q = (uint32_t)a;
    uint32_t ip = q / P10[dec], fp = q % P10[dec];
    if (q == 0) sg = "";                                   /* без "-0.0" */
    if (dec == 0) snprintf(out, cap, "%s%lu", sg, (unsigned long)ip);
    else snprintf(out, cap, "%s%lu.%0*lu", sg, (unsigned long)ip, dec, (unsigned long)fp);
}

void fmt_exp(char *out, size_t cap, float v)
{
    if (!isfinite(v)) { snprintf(out, cap, "nan"); return; }
    if (v == 0.0f) { snprintf(out, cap, "0"); return; }
    double a = fabs((double)v);
    int e = (int)floor(log10(a));
    double m = a / pow(10.0, e);
    uint32_t q = (uint32_t)(m * 1000000.0 + 0.5);          /* мантисса × 10^6 */
    if (q >= 10000000u) { q = (q + 5u) / 10u; e++; }       /* 9.9999996 → 10.000000 */
    else if (q < 1000000u) { q *= 10u; e--; }              /* погрешность log10 вниз */
    snprintf(out, cap, "%s%lu.%06lue%d", v < 0.0f ? "-" : "",
             (unsigned long)(q / 1000000u), (unsigned long)(q % 1000000u), e);
}
