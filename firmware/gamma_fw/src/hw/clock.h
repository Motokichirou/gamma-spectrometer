/* clock.h — тактирование (SPEC §2). */
#ifndef CLOCK_H
#define CLOCK_H
#include <stdint.h>
#include <stdbool.h>

/* SYSCLK 156 МГц: PLL M=2 N=39 R=2 (VCO 312) от HSE 16 МГц (ожидание HSERDY ≤ 200 мс),
   при отказе HSE — от HSI 16 МГц (та же частота, точность ±1 %). Range 1 boost, FLASH 4WS +
   предвыборка и кэши, переход через AHB/2 (RM0440 §6.1.5, §7.2.7). ADC12/ADC345 тактируются
   от PLLP = 52 МГц (CCIPR); USART1 — от PCLK2 = 156 МГц.
   Если PLL не захватился — SYSCLK остаётся 16 МГц (HSE или HSI), clock_pll_ok() = false;
   UART всё равно работает: делитель считается от clock_hz().
   Перед настройкой возвращает SYSCLK на HSI и гасит PLL — на случай старта по Go из загрузчика.
   Возврат: true — от HSE, false — от HSI. */
bool     clock_init(void);
uint32_t clock_hz(void);       /* текущий SYSCLK = HCLK = PCLK2 */
bool     clock_hse_ok(void);
bool     clock_pll_ok(void);
/* SYSCLK → HSI 16 МГц, PLL и HSE выкл, регулятор Range 1 normal, CCIPR по сбросу.
   Латентность флеш остаётся (с запасом). Для перехода в системный загрузчик. */
void     clock_to_hsi(void);

#endif
