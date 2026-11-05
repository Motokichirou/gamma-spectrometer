/* timebase.h — SysTick 1 мс и счётчик тактов DWT. */
#ifndef TIMEBASE_H
#define TIMEBASE_H
#include <stdint.h>

void     timebase_dwt_init(void);          /* включить DWT->CYCCNT (зовёт clock_init первым делом) */
void     timebase_init(uint32_t sysclk_hz);/* SysTick 1 мс, низший приоритет */
uint32_t millis(void);                     /* мс с timebase_init, переполнение через 49 сут — сравнивать разностью */
uint32_t cycles(void);                     /* такты ядра (DWT), переполнение через 27 с на 156 МГц */
/* ожидание по DWT без прерываний (до 27 с на 156 МГц); годится и до SysTick, и в обработчике сбоя */
void     delay_cycles(uint32_t n);

#endif
