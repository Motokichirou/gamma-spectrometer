/*
 * dactest.h — тестовые импульсы ЦАП (самотест тракта без ФЭУ, SPEC §7.7). Переносимый модуль.
 *
 * Цепь: PA4 = DAC1_OUT1 → R307 10 к + C318 100 пФ → вход CR (узел C315/R309). Ступенька ЦАП ΔU
 * вносит заряд ΔU·100 пФ — импульс по форме как от ФЭУ. Ступенька ВВЕРХ даёт импульс вверх
 * (его ловит детектор), ступенька ВНИЗ — импульс вниз (детектор его не видит: ищет только
 * превышение базы). Поэтому один «импульс» = ступенька вверх, полпериода, ступенька вниз.
 *
 * Здесь — логика без железа: разбор команды и машина состояний таймера (dact_tick зовётся из
 * прерывания таймера, на ПК — из теста). Железо — src/hw/dac_test.c (TIM6 156 МГц без деления,
 * DAC1 без триггера: запись DHR → выход через такт).
 *
 * Последовательность: ЦАП включается на DACT_BASE (выход из Hi-Z — сам по себе ступенька,
 * поэтому первая пауза DACT_SETTLE_US), затем n раз: вверх на step, через half тактов вниз,
 * через half + случайная добавка 0…jit−1 тактов — следующий. После последнего спада ещё half —
 * и конец: ЦАП выкл, PA4 — аналоговый вход (Hi-Z), как требует SPEC §7.7 для набора.
 *
 * Случайная добавка: шаг таймера 1 такт ЦП, отсчёт АЦП = jit тактов (45 при 2.5 такта
 * выборки, 57 при 6.5). Без добавки период импульсов кратен такту и фаза ступеньки относительно
 * отсчётов АЦП перебирает лишь несколько значений (100 мкс = 15 600 тактов ≡ 30 mod 45 ⇒ 3 фазы);
 * с равномерной добавкой 0…jit−1 фаза равномерна — проверяется вся таблица эталона.
 *
 * Команды (dact_handle; ответ одной строкой, proto допишет \r\n):
 *   -dac <код> <n> [период_мкс] → "-ok dac step=<код> n=<n> period=<мкс>"; код 1…DACT_STEP_MAX,
 *                                 n 1…65535, период DACT_PER_MIN…DACT_PER_MAX (по умолчанию
 *                                 DACT_PER_DEF). Занято — "-err dac busy". По окончании
 *                                 приложение шлёт текст "-ok dac done <n>".
 *   -dac                        → "-ok dac idle" / "-ok dac busy done=<k> n=<n>"
 *   -dac off                    → остановить: "-ok dac off done=<k>"
 *   Разрешено и в наборе (для того и нужно: пик в спектре), и при включённом ВВ.
 */
#ifndef DACTEST_H
#define DACTEST_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DACT_BASE       400u     /* код покоя ≈ 0.24 В: буфер ЦАП линеен от 0.2 В (DS) */
#define DACT_STEP_MAX   (4095u - DACT_BASE)
#define DACT_PER_MIN    20u      /* мкс: импульс шейпера ≈ 5 мкс + отрицательный от спада */
#define DACT_PER_MAX    800u     /* мкс: полпериода + добавка ≤ 65535 тактов при 156 МГц */
#define DACT_PER_DEF    100u
#define DACT_SETTLE_US  400u     /* пауза после включения ЦАП до первой ступеньки (таймер 16 бит) */

typedef enum { DACT_IDLE = 0, DACT_SETTLE, DACT_HIGH, DACT_LOW, DACT_TAIL } dact_phase_t;

typedef struct {
    uint16_t step, n, period_us;
    uint32_t half;               /* тактов таймера на полпериода */
    uint32_t settle;             /* тактов первой паузы */
    uint16_t jit;                /* добавка 0…jit−1 тактов к паузе после спада */
    uint32_t rnd;                /* xorshift32 */
    volatile uint16_t done;      /* ступенек вверх выдано (читает главный цикл) */
    volatile uint8_t  phase;     /* dact_phase_t */
} dact_t;

/* Подготовить серию. tick_hz — такт таймера, jit — тактов таймера на отсчёт АЦП (≥ 1).
   false — параметры вне пределов (dact_t не тронут). После true: железо пишет DACT_BASE в ЦАП,
   включает его и запускает таймер с первым интервалом d->settle. */
bool dact_start(dact_t *d, uint16_t step, uint16_t n, uint16_t period_us, uint32_t tick_hz, uint16_t jit);

/* Шаг машины (из прерывания таймера по окончании интервала). true — записать *code в ЦАП и
   следующий интервал *ticks (≥ 1); false — серия окончена: ЦАП и таймер выключить. */
bool dact_tick(dact_t *d, uint16_t *code, uint32_t *ticks);

static inline bool dact_busy(const dact_t *d) { return d->phase != DACT_IDLE; }
/* Принудительный конец (команда -dac off); железо выключает таймер ДО вызова. */
void dact_abort(dact_t *d);

/* приложение для команд */
typedef struct {
    bool     (*start)(uint16_t step, uint16_t n, uint16_t period_us);   /* false — занято/сбой */
    void     (*stop)(void);
    bool     (*busy)(void);
    uint16_t (*done)(void);
    uint16_t (*total)(void);
} dact_ctx_t;

/* true — команда -dac (ответ в reply), false — не наша. */
bool dact_handle(const dact_ctx_t *c, const char *cmd, char *reply, size_t cap);

#endif
