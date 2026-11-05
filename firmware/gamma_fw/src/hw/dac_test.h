/* dac_test.h — тестовые импульсы DAC1_OUT1 на PA4 (SPEC §7.7). Логика серии — core/dactest.
 *
 * TIM6 (такт APB1 = SYSCLK 156 МГц, без деления) отсчитывает интервалы; в его прерывании
 * (TIM6_DAC_IRQn, приоритет 3: ниже приёма UART и DMA АЦП) пишется DHR12R1 — ЦАП без триггера,
 * выход меняется через такт. Опора ЦАП — VREF+ (2.5 В): 1 код = 0.61 мВ.
 * Вне серии ЦАП выключен (EN1 = 0) и PA4 — аналоговый вход Hi-Z (как после gpio_safe_init).
 */
#ifndef DAC_TEST_H
#define DAC_TEST_H
#include <stdint.h>
#include <stdbool.h>

/* Серия count импульсов «ступенька вверх на step_code — полпериода — вниз» с периодом
   period_us (+ случайная добавка 0…jit_cyc−1 тактов, jit_cyc — тактов ЦП на отсчёт АЦП).
   Неблокирующая. false — занято или параметры вне пределов (dactest.h). */
bool dac_test_pulses(uint16_t step_code, uint16_t count, uint16_t period_us, uint16_t jit_cyc);
void dac_test_stop(void);              /* прервать: таймер стоп, ЦАП выкл, PA4 Hi-Z */
bool dac_test_busy(void);
uint16_t dac_test_done(void);          /* выдано импульсов в текущей/последней серии */
uint16_t dac_test_total(void);         /* заказано в текущей/последней серии */
/* Серия закончилась сама с прошлого вызова (для сообщения "-ok dac done"); сбрасывает признак. */
bool dac_test_finished(void);

#endif
