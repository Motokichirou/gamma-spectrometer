/*
 * logger.h — автономный регистратор спектров во внешнюю флешку W25Q64 (SPEC_logger.md).
 * Переносимый модуль: флешка — через w25_port_t, приложение — через logger_port_t.
 *
 * Разметка флешки: 0x000000 и 0x001000 — две копии конфигурации (поколения), 0x010000…конец —
 * кольцевой журнал. Запись начинается с границы сектора 4 КБ: страница заголовка (logrec_hdr_t)
 * + нагрузка (накопленный спектр, logrec.h). Заголовок пишется ПОСЛЕДНИМ — недописанная запись
 * не проходит проверку. Запись не переходит через конец флешки: если до конца меньше места,
 * чем на худшую запись, она начинается с начала журнала.
 *
 * Всё неблокирующее (§2.1): logger_poll() делает не больше одной операции с флешкой за вызов
 * и ничего не ждёт; стирание/программирование идут в самой флешке, готовность — опросом.
 *
 * Сессия: запуск набора (acq_begin: -rst + -sta) → запись 0 (LOGF_SESSION_START) → каждые period_s
 * запись накопленного спектра и телеметрии за интервал. Автозапуск — если включён в конфигурации,
 * через delay_s после старта прибора и после поиска головы журнала.
 *
 * Команды (logger_handle, ответ одной строкой; list и read досылают своё сами):
 *   -log                        состояние: "-ok log on|off period P delay D active A session S rec R
 *                               records N used_kb U total_kb T wrap W id XXXXXX err E scan done|NN%"
 *   -log on <период_с> [выдержка_с]   автозапуск вкл (60…86400; 0…7200), в конфиг сразу
 *   -log off                    автозапуск выкл (журнал не трогать)
 *   -log start | -log stop      сессия вручную в этом пуске (stop пишет запись LOGF_SESSION_END)
 *   -log time <unix>            опорное время (метки на ПК)
 *   -log erase                  стереть журнал (только без активной сессии), прогресс — в -log
 *   -log list                   строки "-log rec <сектор> <seq> <сессия> <номер> <нагрузка> <флаги>
 *                               <t_end_ms>" по всем записям (порядок — по адресам), затем
 *                               "-ok log list <n>"
 *   -log read <сектор>          кадры SHPROTO_CMD_LOG: [сектор u16][смещение u32][всего u32][данные],
 *                               смещение — в записи (0…223 заголовок, с 256 — нагрузка), всего =
 *                               256 + нагрузка; затем "-ok log read <сектор>" или "-err log read"
 */
#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "w25.h"
#include "logrec.h"

#define SHPROTO_CMD_LOG     0x05u          /* устройство → ПК: кусок записи журнала */
#define LOG_CFG_A           0x000000u
#define LOG_CFG_B           0x001000u
#define LOG_START           0x010000u
#define LOG_END             W25_SIZE
#define LOG_SECTORS         ((LOG_END - LOG_START) / W25_SECTOR)
#define LOG_REC_MAX_SECTORS ((LOGREC_HDR_PAGE + LOGREC_PAYLOAD_MAX + W25_SECTOR - 1u) / W25_SECTOR)
#define LOG_PERIOD_MIN      60u
#define LOG_PERIOD_MAX      86400u
#define LOG_PERIOD_DEF      600u
#define LOG_DELAY_MAX       7200u
#define LOG_DELAY_DEF       120u
#define LOG_READ_CHUNK      512u           /* байт данных в кадре -log read */
#define LOG_SCAN_PER_POLL   2u             /* заголовков за вызов при поиске головы */

/* накопленное приложением на момент записи (счётчики — с пуска прибора, как в det_stats) */
typedef struct {
    uint64_t real_ms, live_ms;
    float    base, sigma, vref_mv, vdd_mv;
    uint32_t rej_shape, rej_width, rej_pile, rej_budget, overrun, lost;
    float    tc_t0, tc_k1, tc_k2, tc_gain;
    uint16_t hv_set_v;
    bool     acq_running;
} log_snap_t;

/* раз в секунду */
typedef struct {
    float    temp_c;     /* NaN — нет */
    float    hv_out_v;   /* |В| по монитору */
    bool     hv_on;
    uint16_t hv_set_v;
    bool     vref_ok;
    uint32_t cps_in, cps_acc;
} log_tel_t;

typedef struct {
    const w25_port_t *spi;
    uint32_t (*millis)(void);
    const uint32_t *(*spectrum)(void);
    void (*acq_begin)(void);                 /* начать набор с нуля: -rst + -sta */
    void (*snap)(log_snap_t *s);
    void (*send_text)(const char *s);
    void (*send_frame)(uint8_t cmd, const uint8_t *p, size_t n);
    size_t (*tx_free)(void);                 /* свободно в кольце UART; NULL — без ограничения */
    const char *fw_tag;
    const char *serial;
} logger_port_t;

void logger_init(const logger_port_t *port);    /* флешка: пробуждение, ID, конфигурация */
void logger_poll(uint32_t now_ms);              /* шаг автомата — на каждом проходе главного цикла */
void logger_tick_1s(const log_tel_t *t);
void logger_event(uint32_t flag);               /* LOGF_HOST_* — команды хоста за интервал */
bool logger_handle(const char *cmd, char *reply, size_t cap);   /* -log …; false — не наша */
bool logger_session_active(void);

#endif
