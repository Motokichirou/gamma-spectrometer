/* opamp.h — встроенные ОУ тракта и Pt1000 (SPEC §6, §8). */
#ifndef OPAMP_H
#define OPAMP_H

/* OPAMP1, OPAMP2 — внешняя обвязка (VINP0/VINM0, выход на вывод): CR- и RC-звенья шейпера.
   OPAMP3 — PGA ×4 от PB0 (VMSEL = 2, PGGAIN = 1), выход на PB1 (= ADC3_IN1, сигнал).
   OPAMP6 — PGA ×8 от PB12 (Pt1000; VMSEL = 2, PGGAIN = 2), выход внутрь → ADC4_IN17.
   Заводская подстройка смещения (USERTRIM = 0). Выводы — аналог (gpio_safe_init). */
void opamp_init(void);

#endif
