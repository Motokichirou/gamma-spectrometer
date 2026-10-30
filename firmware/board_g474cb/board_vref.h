/* board_vref.h — опора АЦП боевой платы МК (STM32G474CBT6): внешний REF35250 (U301)
 * или внутренний VREFBUF.
 *
 * Штатно впаян U301 (REF35250 получен 2026-10-30), VREFBUF выключен.
 * Вариант без U301 (отладка, опора сгорела/не впаяна): VREF+ (вывод 20)
 * питает внутренний VREFBUF: 2.500 В ±0.16 %, нагрузка до 6.5 мА при наших 0.643 мА
 * (делители R312/R313 и т.п. + R303 к Pt1000), C303 = 1 мкФ укладывается в требуемые
 * 0.5–1.5 мкФ (DS12288, табл. 78). ТКН 50 ppm/°C против 12 у REF35250.
 *
 * ⛔ Нельзя: VREFBUF включён И U301 впаян — два источника на одном узле.
 * Поэтому источник выбирается при сборке:
 *   по умолчанию          — U301 впаян, VREFBUF выключен и в высокоимпедансном состоянии;
 *   -DBOARD_VREF_INTERNAL — U301 НЕ впаян, VREF+ питает VREFBUF 2.5 В.
 * (До 2026-10-30 было наоборот, флаг -DBOARD_VREF_EXTERNAL; он теперь вызывает #error,
 * чтобы старая сборка не прошла молча.)
 * Дополнительно board_vref_init() меряет VREF+ через VREFINT и сверяет с ожиданием.
 *
 * ⛔ На Nucleo-G474RE этот модуль НЕ подключать: там VREF+ соединён с VDDA.
 */
#ifndef BOARD_VREF_H
#define BOARD_VREF_H

#include <stdint.h>

#if defined(BOARD_VREF_EXTERNAL)
#error "BOARD_VREF_EXTERNAL упразднён: внешняя опора теперь по умолчанию; без U301 — -DBOARD_VREF_INTERNAL"
#endif

typedef enum {
    BOARD_VREF_OK = 0,          /* опора включена/выбрана, замер в допуске */
    BOARD_VREF_ERR_TIMEOUT,     /* VREFBUF не выставил VRR за таймаут HAL */
    BOARD_VREF_ERR_RANGE        /* замер VREF+ вне 2.500 В ±2 % — проверить U301/C303 */
} board_vref_status_t;

/* Включить выбранную опору. Вызывать ДО калибровки и запуска АЦП
 * (после SystemClock_Config). */
board_vref_status_t board_vref_init(void);

/* Сверить VREF+ с 2.500 В ±2 % по 12-битному отсчёту канала VREFINT, снятому
 * уже после board_vref_init() и калибровки АЦП. 0 = замера нет (вернёт OK). */
board_vref_status_t board_vref_check(uint16_t vrefint_raw);

/* VREF+ в мВ по отсчёту VREFINT (12 бит): VREF+ = 3000 · VREFINT_CAL / raw. */
uint32_t board_vref_mv_from_vrefint(uint16_t vrefint_raw);

#endif /* BOARD_VREF_H */
