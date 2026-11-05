/* hvpwm.h — ШИМ HV_CONTROL и HV_ENABLE (SPEC §1, §5). */
#ifndef HVPWM_H
#define HVPWM_H
#include <stdint.h>
#include <stdbool.h>

/* TIM1_CH1 на PA8 (AF6), 12 бит: ARR 4095, 156 МГц / 4096 = 38.1 кГц, PWM1 с предзагрузкой.
   CCR1 = 0 до переключения PA8 из «выход 0» в AF — выброса нет. */
void hvpwm_init(void);
void hvpwm_set(uint16_t duty);       /* 0…4095 (больше — 4095) */
uint16_t hvpwm_get(void);
/* PB10 (HV_ENABLE, SHDN MAX1847) и PB6 (D102 — индикатор ВВ) вместе. */
void hv_enable_pin(bool on);

#endif
