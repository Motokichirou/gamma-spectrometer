/* opamp.c — см. opamp.h (как board_g474cb/bringup/main.c, проверено на плате 2026-11-03/06). */
#include "stm32g474xx.h"
#include "opamp.h"

void opamp_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;          /* регистры ОУ — в SYSCFG */
    (void)RCC->APB2ENR;
    OPAMP1->CSR = OPAMP_CSR_OPAMPxEN;
    OPAMP2->CSR = OPAMP_CSR_OPAMPxEN;
    OPAMP3->CSR = OPAMP_CSR_OPAMPxEN | (2u << OPAMP_CSR_VMSEL_Pos) | (1u << OPAMP_CSR_PGGAIN_Pos);
    /* Pt1000: узел ~0.25 В → ×8 → ~2 В на ADC4; 1 LSB = 0.09 °C вместо 0.75 (SPEC §8) */
    OPAMP6->CSR = OPAMP_CSR_OPAMPxEN | (2u << OPAMP_CSR_VMSEL_Pos) | (2u << OPAMP_CSR_PGGAIN_Pos)
                | OPAMP_CSR_OPAMPINTEN;            /* VPSEL = 0: PB12 */
}
