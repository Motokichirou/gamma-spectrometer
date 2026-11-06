/* opamp.h — встроенные ОУ тракта и Pt1000 (SPEC §6, §8). */
#ifndef OPAMP_H
#define OPAMP_H

/* OPAMP1, OPAMP2 — внешняя обвязка (VINP0/VINM0, выход на вывод): CR- и RC-звенья шейпера.
   OPAMP3 — PGA ×4 от PB0 (VMSEL = 2, PGGAIN = 1), выход на PB1 (= ADC3_IN1, сигнал).
   OPAMP6 — PGA ×8 от PB12 (Pt1000; VMSEL = 2, PGGAIN = 2), выход внутрь → ADC4_IN17.
   Заводская подстройка смещения (USERTRIM = 0). Выводы — аналог (gpio_safe_init). */
void opamp_init(void);

/* Сервис (команда -opa): быстрый режим OPAMP1–3 (OPAHSM, бит 7 CSR) и усиление PGA OPAMP3.
   pga — 2|4|8|16. По RM0440 стр. 790 OPAHSM меняется только при выключенном ОУ:
   ОУ выключаются, биты пишутся, ОУ включаются. После смены ждать установления базы (сотни мкс). */
void opamp_shaper_set(int hs, unsigned pga);
void opamp_shaper_get(int *hs, unsigned *pga);

#endif
