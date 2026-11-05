/* hwcalc.c — см. hwcalc.h */
#include "hwcalc.h"

uint32_t hw_uart_brr(uint32_t clk_hz, uint32_t baud)
{
    if (!baud) return 0;
    uint32_t brr = (uint32_t)(((uint64_t)clk_hz + baud / 2u) / baud);
    return (brr < 16u || brr > 0xFFFFu) ? 0u : brr;
}

int32_t hw_uart_err_ppm(uint32_t clk_hz, uint32_t baud)
{
    uint32_t brr = hw_uart_brr(clk_hz, baud);
    if (!brr) return INT32_MIN;
    /* реальная скорость clk/brr против baud */
    int64_t num = (int64_t)clk_hz * 1000000 - (int64_t)baud * brr * 1000000;
    int64_t den = (int64_t)baud * brr;
    return (int32_t)((num + (num >= 0 ? den / 2 : -den / 2)) / den);
}

uint32_t hw_pll_hz(uint32_t in_hz, uint32_t m, uint32_t n, uint32_t div)
{
    if (m < 1u || m > 16u || n < 8u || n > 127u || div < 2u || div > 31u) return 0;
    uint32_t pin = in_hz / m;
    if (pin < 2660000u || pin > 16000000u) return 0;
    uint64_t vco = (uint64_t)pin * n;
    if (vco < 96000000u || vco > 344000000u) return 0;
    return (uint32_t)(vco / div);
}

uint32_t hw_adc_fs(uint32_t adc_hz, uint32_t smp_x2)
{
    uint32_t cyc_x2 = smp_x2 + 25u;               /* + 12.5 такта преобразования */
    return (uint32_t)(((uint64_t)adc_hz * 2u + cyc_x2 / 2u) / cyc_x2);
}
