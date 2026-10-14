/* board_vref.c — опора АЦП боевой платы: VREFBUF 2.5 В или внешний REF35250.
 * Описание и ограничения — в board_vref.h.
 */
#include "stm32g4xx_hal.h" /* HAL: HAL_SYSCFG_*; не main.h — тот от Nucleo */
#include "board_vref.h"

#define VREF_NOMINAL_MV   2500u
#define VREF_TOL_MV       50u                       /* ±2 %: ловит грубые ошибки, а не точность */
#define VREFINT_CAL_RAW   (*(volatile const uint16_t *)0x1FFF75AAUL)   /* при VREF+ = 3.000 В, 30 °C */
#define VREFINT_CAL_MV    3000u

board_vref_status_t board_vref_init(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
#if defined(BOARD_VREF_EXTERNAL)
    /* U301 впаян: выход буфера выключен и отключён от вывода VREF+ */
    HAL_SYSCFG_DisableVREFBUF();
    HAL_SYSCFG_VREFBUF_HighImpedanceConfig(SYSCFG_VREFBUF_HIGH_IMPEDANCE_ENABLE);
    return BOARD_VREF_OK;
#else
    /* U301 не впаян: буфер 2.5 В на вывод VREF+ */
    HAL_SYSCFG_VREFBUF_VoltageScalingConfig(SYSCFG_VREFBUF_VOLTAGE_SCALE1);   /* 2.5 В */
    HAL_SYSCFG_VREFBUF_HighImpedanceConfig(SYSCFG_VREFBUF_HIGH_IMPEDANCE_DISABLE);
    /* HAL ждёт VRR (готовность) с таймаутом; C303 = 1 мкФ заряжается за единицы мс */
    if (HAL_SYSCFG_EnableVREFBUF() != HAL_OK)
        return BOARD_VREF_ERR_TIMEOUT;
    return BOARD_VREF_OK;
#endif
}

uint32_t board_vref_mv_from_vrefint(uint16_t vrefint_raw)
{
    if (vrefint_raw == 0u) return 0u;
    return (VREFINT_CAL_MV * (uint32_t)VREFINT_CAL_RAW + vrefint_raw / 2u) / vrefint_raw;
}

board_vref_status_t board_vref_check(uint16_t vrefint_raw)
{
    if (vrefint_raw == 0u) return BOARD_VREF_OK;
    uint32_t mv = board_vref_mv_from_vrefint(vrefint_raw);
    if (mv + VREF_TOL_MV < VREF_NOMINAL_MV || mv > VREF_NOMINAL_MV + VREF_TOL_MV)
        return BOARD_VREF_ERR_RANGE;
    return BOARD_VREF_OK;
}
