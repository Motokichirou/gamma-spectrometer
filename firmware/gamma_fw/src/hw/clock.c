/* clock.c — см. clock.h */
#include "stm32g474xx.h"
#include "clock.h"
#include "timebase.h"
#include "hwcalc.h"

uint32_t SystemCoreClock = HW_HSI_HZ;   /* имя CMSIS (system_stm32g4xx.h) */
static bool hse_ok, pll_ok;

#define SW_HSI  1u
#define SW_HSE  2u
#define SW_PLL  3u

/* ждать (reg & mask) == want не дольше us микросекунд при текущей частоте; true — дождались */
static bool wait_bits(volatile uint32_t *reg, uint32_t mask, uint32_t want, uint32_t us)
{
    uint32_t t0 = cycles(), lim = (SystemCoreClock / 1000000u) * us;
    uint32_t spin = lim;                         /* страховка, если DWT стоит */
    while ((*reg & mask) != want) {
        if ((cycles() - t0) > lim || !spin--) return (*reg & mask) == want;
    }
    return true;
}

static bool switch_sysclk(uint32_t sw)
{
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | (sw << RCC_CFGR_SW_Pos);
    return wait_bits(&RCC->CFGR, RCC_CFGR_SWS, sw << RCC_CFGR_SWS_Pos, 5000u);
}

void clock_to_hsi(void)
{
    RCC->CR |= RCC_CR_HSION;
    wait_bits(&RCC->CR, RCC_CR_HSIRDY, RCC_CR_HSIRDY, 1000u);
    /* вниз по частоте делить AHB не нужно; HPRE/PPRE — в 1 */
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_SW | RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2 |
                               RCC_CFGR_MCOSEL | RCC_CFGR_MCOPRE)) | (SW_HSI << RCC_CFGR_SW_Pos);
    SystemCoreClock = HW_HSI_HZ;                 /* до ожидания: таймаут считается в тактах */
    wait_bits(&RCC->CFGR, RCC_CFGR_SWS, SW_HSI << RCC_CFGR_SWS_Pos, 5000u);
    RCC->CR &= ~RCC_CR_PLLON;
    wait_bits(&RCC->CR, RCC_CR_PLLRDY, 0u, 1000u);
    RCC->CR &= ~RCC_CR_HSEON;
    RCC->PLLCFGR = 0x00001000u;                  /* значение по сбросу */
    RCC->CCIPR = 0;
    /* boost → normal: только на частоте ≤ 150 МГц, т. е. теперь (RM0440 §6.1.5) */
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void)RCC->APB1ENR1;
    PWR->CR5 |= PWR_CR5_R1MODE;
    pll_ok = false;
}

bool clock_init(void)
{
    timebase_dwt_init();
    clock_to_hsi();
    hse_ok = pll_ok = false;

    /* HSE: кварц 16 МГц, ждать HSERDY ≤ 200 мс */
    RCC->CR |= RCC_CR_HSEON;
    hse_ok = wait_bits(&RCC->CR, RCC_CR_HSERDY, RCC_CR_HSERDY, 200000u);
    if (!hse_ok) RCC->CR &= ~RCC_CR_HSEON;

    /* PLL: вход 16/2 = 8 МГц, VCO ×39 = 312, R /2 = 156 (SYSCLK), P /6 = 52 (АЦП) */
    RCC->PLLCFGR = (hse_ok ? 3u : 2u) << RCC_PLLCFGR_PLLSRC_Pos
                 | (HW_PLL_M - 1u) << RCC_PLLCFGR_PLLM_Pos
                 | HW_PLL_N << RCC_PLLCFGR_PLLN_Pos
                 | ((HW_PLL_R / 2u) - 1u) << RCC_PLLCFGR_PLLR_Pos
                 | RCC_PLLCFGR_PLLREN
                 | HW_PLL_P << RCC_PLLCFGR_PLLPDIV_Pos
                 | RCC_PLLCFGR_PLLPEN;
    RCC->CR |= RCC_CR_PLLON;
    if (!wait_bits(&RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY, 5000u)) {
        /* PLL не захватился: остаёмся на 16 МГц, от кварца, если он есть */
        RCC->CR &= ~RCC_CR_PLLON;
        if (hse_ok && switch_sysclk(SW_HSE)) SystemCoreClock = HW_HSE_HZ;
        return hse_ok;
    }

    /* Range 1 boost (> 150 МГц), RM0440 §6.1.5: AHB /2 → R1MODE = 0 → латентность → PLL → ≥ 1 мкс → AHB /1 */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_HPRE) | RCC_CFGR_HPRE_DIV2;
    PWR->CR5 &= ~PWR_CR5_R1MODE;
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_4WS
               | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
    wait_bits(&FLASH->ACR, FLASH_ACR_LATENCY, FLASH_ACR_LATENCY_4WS, 100u);
    if ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_4WS || !switch_sysclk(SW_PLL)) {
        RCC->CFGR &= ~RCC_CFGR_HPRE;
        RCC->CR &= ~RCC_CR_PLLON;
        PWR->CR5 |= PWR_CR5_R1MODE;
        if (hse_ok && switch_sysclk(SW_HSE)) SystemCoreClock = HW_HSE_HZ;
        return hse_ok;
    }
    SystemCoreClock = HW_SYSCLK_HZ / 2u;         /* ядро пока на 78 МГц (AHB /2) */
    delay_cycles(200u);                          /* ≥ 1 мкс на промежуточной частоте */
    RCC->CFGR &= ~RCC_CFGR_HPRE;
    SystemCoreClock = HW_SYSCLK_HZ;
    pll_ok = true;

    /* АЦП: ADC12SEL = ADC345SEL = 01 (PLLP 52 МГц); USART1SEL = 00 (PCLK2) */
    RCC->CCIPR = (RCC->CCIPR & ~(RCC_CCIPR_ADC12SEL_Msk | RCC_CCIPR_ADC345SEL_Msk | RCC_CCIPR_USART1SEL_Msk))
               | RCC_CCIPR_ADC12SEL_0 | RCC_CCIPR_ADC345SEL_0;
    return hse_ok;
}

uint32_t clock_hz(void)     { return SystemCoreClock; }
bool     clock_hse_ok(void) { return hse_ok; }
bool     clock_pll_ok(void) { return pll_ok; }
