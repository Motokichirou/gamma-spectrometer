/* Тесты hwcalc: делитель UART, PLL, частота выборки (SPEC §2). */
#include "t.h"
#include "hwcalc.h"
#include "proto.h"

int main(void)
{
    /* PLL: HSE 16 / 2 · 39 = 312 МГц; R=2 → 156, P=6 → 52 */
    CHECK(hw_pll_hz(HW_HSE_HZ, HW_PLL_M, HW_PLL_N, HW_PLL_R) == HW_SYSCLK_HZ);
    CHECK(hw_pll_hz(HW_HSE_HZ, HW_PLL_M, HW_PLL_N, HW_PLL_P) == HW_PLLP_HZ);
    CHECK(hw_pll_hz(HW_HSI_HZ, HW_PLL_M, HW_PLL_N, HW_PLL_R) == 156000000u);  /* запасной от HSI — та же частота */
    CHECK(hw_pll_hz(16000000u, 1u, 39u, 2u) == 0u);      /* VCO 624 — вне диапазона */
    CHECK(hw_pll_hz(16000000u, 8u, 39u, 2u) == 0u);      /* вход PLL 2 МГц — мало */
    CHECK(hw_pll_hz(16000000u, 2u, 39u, 1u) == 0u);      /* делитель 1 недопустим */
    CHECK(hw_pll_hz(16000000u, 4u, 75u, 2u) == 150000000u);  /* запасной вариант без boost */

    /* UART: 600 000 и 3 М — ровно, 115 200 — +0.012 % (SPEC §2) */
    CHECK(hw_uart_brr(HW_SYSCLK_HZ, HW_UART_BAUD) == 260u);
    CHECK(hw_uart_err_ppm(HW_SYSCLK_HZ, HW_UART_BAUD) == 0);
    CHECK(hw_uart_brr(HW_SYSCLK_HZ, 3000000u) == 52u);
    CHECK(hw_uart_brr(HW_SYSCLK_HZ, HW_BOOT_BAUD) == 1354u);
    CHECK_NEAR(hw_uart_err_ppm(HW_SYSCLK_HZ, HW_BOOT_BAUD), 123, 1);
    CHECK(hw_uart_brr(16000000u, 115200u) == 139u);       /* 16 МГц без PLL */
    CHECK(hw_uart_brr(16000000u, 600000u) == 27u);        /* 16 МГц: 592 593 бод, −1.2 % — ещё принимает */
    CHECK_NEAR(hw_uart_err_ppm(16000000u, 600000u), -12346, 2);
    CHECK(hw_uart_brr(16000000u, 2000000u) == 0u);        /* BRR < 16 */
    CHECK(hw_uart_brr(16000000u, 0u) == 0u);

    /* АЦП: 52 МГц, 2.5 + 12.5 такта → 3 466 667 = F в -inf */
    CHECK(hw_adc_fs(HW_PLLP_HZ, 5u) == 3466667u);
    CHECK(hw_adc_fs(HW_PLLP_HZ, 5u) == PROTO_INF_FS);
    CHECK(hw_adc_fs(HW_PLLP_HZ, 13u) == 2736842u);        /* 6.5 такта */
    return T_DONE();
}
