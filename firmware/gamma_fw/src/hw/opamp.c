/* opamp.c — см. opamp.h (как board_g474cb/bringup/main.c, проверено на плате 2026-11-03/06). */
#include "stm32g474xx.h"
#include "opamp.h"

void opamp_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;          /* регистры ОУ — в SYSCFG */
    (void)RCC->APB2ENR;
    opamp_shaper_set(0, 4);
    /* Pt1000: узел ~0.25 В → ×8 → ~2 В на ADC4; 1 LSB = 0.09 °C вместо 0.75 (SPEC §8) */
    OPAMP6->CSR = OPAMP_CSR_OPAMPxEN | (2u << OPAMP_CSR_VMSEL_Pos) | (2u << OPAMP_CSR_PGGAIN_Pos)
                | OPAMP_CSR_OPAMPINTEN;            /* VPSEL = 0: PB12 */
}

static unsigned pga_code(unsigned pga) { return pga == 2u ? 0u : pga == 8u ? 2u : pga == 16u ? 3u : 1u; }

void opamp_shaper_set(int hs, unsigned pga)
{
    uint32_t h = hs ? OPAMP_CSR_HIGHSPEEDEN : 0u;
    OPAMP1->CSR = 0; OPAMP2->CSR = 0; OPAMP3->CSR = 0;          /* OPAHSM — только при выключенном ОУ */
    OPAMP1->CSR = h;
    OPAMP2->CSR = h;
    OPAMP3->CSR = h | (2u << OPAMP_CSR_VMSEL_Pos) | (pga_code(pga) << OPAMP_CSR_PGGAIN_Pos);
    OPAMP1->CSR |= OPAMP_CSR_OPAMPxEN;
    OPAMP2->CSR |= OPAMP_CSR_OPAMPxEN;
    OPAMP3->CSR |= OPAMP_CSR_OPAMPxEN;
}

void opamp_shaper_get(int *hs, unsigned *pga)
{
    static const unsigned g[4] = { 2u, 4u, 8u, 16u };
    *hs = (OPAMP1->CSR & OPAMP_CSR_HIGHSPEEDEN) ? 1 : 0;
    *pga = g[(OPAMP3->CSR >> OPAMP_CSR_PGGAIN_Pos) & 3u];
}
