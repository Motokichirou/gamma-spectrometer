/*
 * hwcalc.h — арифметика настроек железа без железа (тестируется на ПК):
 * делитель UART, частоты PLL, частота выборки АЦП.
 *
 * Тактирование (SPEC §2): HSE 16 МГц, PLL M=2 N=39 → VCO 312 МГц; R=2 → SYSCLK 156 МГц;
 * P=6 → 52 МГц (ADC345 без делителя, ADC12 с PRESC /2 → 26 МГц).
 * Сигнальный АЦП: 2.5 + 12.5 такта на 52 МГц → 3 466 667 выб/с (= PROTO_INF_FS).
 */
#ifndef HWCALC_H
#define HWCALC_H

#include <stdint.h>

#define HW_HSE_HZ      16000000u
#define HW_HSI_HZ      16000000u
#define HW_PLL_M       2u
#define HW_PLL_N       39u
#define HW_PLL_R       2u
#define HW_PLL_P       6u
#define HW_SYSCLK_HZ   156000000u
#define HW_PLLP_HZ     52000000u
#define HW_UART_BAUD   600000u
#define HW_BOOT_BAUD   115200u

/* BRR USART при OVER8 = 0: округление до ближайшего; 0 — недопустимо (BRR < 16) */
uint32_t hw_uart_brr(uint32_t clk_hz, uint32_t baud);
/* ошибка скорости при этом BRR, миллионные доли (знак: + — реальная скорость выше) */
int32_t  hw_uart_err_ppm(uint32_t clk_hz, uint32_t baud);
/* выход PLL: in / m · n / div; 0 при недопустимых делителях или VCO вне 96…344 МГц (DS12288),
   или входе PLL вне 2.66…16 МГц */
uint32_t hw_pll_hz(uint32_t in_hz, uint32_t m, uint32_t n, uint32_t div);
/* частота выборки АЦП (Гц, округлённо): такт / (выборка + 12.5); smp_x2 — выборка ×2 (2.5 → 5) */
uint32_t hw_adc_fs(uint32_t adc_hz, uint32_t smp_x2);

#endif
