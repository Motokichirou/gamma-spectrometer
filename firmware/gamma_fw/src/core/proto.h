/*
 * proto.h — команды и ответы по shproto (SPEC §9). Переносимый модуль, без железа.
 *
 * Хост → прибор: текстовые кадры 0x03. Ответ — один текстовый кадр 0x03.
 *  - Команды BecqMoni — строго как ждёт upstream (docs/ru/protocol.md):
 *      -sta / -sto / -rst      → "-ok\r\n"
 *      -stt                    → "-ok collecting\r\n" (идёт набор) / "-ok\r\n"
 *      -mode …                 → "-ok\r\n"
 *      -sho                    → "-ok collecting\r\n"
 *      -inf                    → "GammaSpec v1 RISE <r> FALL <f> T1 <t> F <fs> VERSION 100"
 *                                 (токены [3], [5], [9] = r, f, fs — старый позиционный разбор
 *                                 BecqMoni; пары RISE/FALL/F — новый DeadTimeFromInfo)
 *      -cal                    → 40 регистров (becq_format_registers), [39] = серийник
 *      -cal i                  → "XXXXXXXX" (слово i, как этап 1)
 *      -cal i hex              → "-ok\r\n"; по приходу всех 11 слов с верной суммой —
 *                                 в live-настройки и запись во флеш (при наборе — отложенная)
 *  - Сервисные, обрабатываемые здесь (только настройки):
 *      -save                   → "-ok saved" / "-ok deferred" (идёт набор) / "-err flash"
 *      -defaults               → настройки по умолчанию в ОЗУ (не записываются до -save);
 *                                 при включённом ВВ (port->hv_on) — "-err hv_on", ничего не
 *                                 меняется: калибровку монитора (zero, k) при включённом ВВ
 *                                 не меняет и hv_set_cal
 *      -thr [кодов|0]          → "-ok thr N" (0 — авто 5σ); без аргумента — запрос
 *      -tc [t0 k1 k2]          → "-ok tc t0 k1 k2"; без аргументов — запрос
 *      -tplw <фаза> <7 чисел>  → строка эталона в ОТДЕЛЬНЫЙ буфер (набор её не видит)
 *      -tplok                  → все 64 строки получены → копия в live->tpl, tpl_valid = 1,
 *                                 apply(PROTO_APPLY_TPL) (приложение зовёт det_set_template)
 *      -tplclr                 → tpl_valid = 0 (встроенный эталон), apply(PROTO_APPLY_TPL)
 *  - Прочие "-…" (-baud, -hv, -hvzero, -hvk, -ptcal, -gst, -diag, -osc, -dac, …) — в port->service;
 *    пустой ответ сервиса (или нет сервиса) → "-err unknown\r\n".
 *  - Текст не с '-' и нетекстовые кадры — тишина (как этап 1).
 *
 * "!boot" ищется подстрокой в сыром входном потоке (в кадре и вне его, в том числе разорванный
 * между вызовами proto_rx) → port->enter_bootloader() (там же ВВ выкл, SPEC §4).
 *
 * Флеш (SPEC §7.9): port->settings_commit вызывается ТОЛЬКО при остановленном наборе.
 * Запись во время набора откладывается до остановки набора и выполняется ровно один раз:
 * по -sto или по proto_acq_stopped(), которую приложение ОБЯЗАНО звать при любой иной
 * остановке набора (сервисная команда, авария ВВ, логика приложения).
 *
 * Прибор → хост раз в секунду при наборе: 16 кадров 0x01 по 512 каналов
 * ([offset u16][канал u32 & 0x7FFFFFF] LE), затем статус 0x04 (14 байт:
 * [elapsed u32 СЕКУНДЫ][cpu u16, 0.1 %][cps u32][invalid u32] LE) — по нему BecqMoni
 * перерисовывает спектр. Выдача пошаговая (SPEC §7.9 — не блокировать обработку):
 * proto_tick_1s() только взводит её, proto_poll() на каждом проходе главного цикла шлёт
 * не больше одного кадра и только если port->tx_free() ≥ PROTO_TX_FRAME_MAX (≈4.1 КБ,
 * худший кадр со стаффингом). Всего ≈34 КБ — при 600 000 бод ≈0.57 с. Если к следующему
 * тику прошлая выдача не закончилась, тик пропускается (не копится).
 * tx_free == NULL — порт без кольца (tx блокирует); выдача всё равно по кадру за proto_poll.
 *
 * Осциллограф (proto_send_pulse), кадр 0x02 — формат gamma_fw (BecqMoni его не знает):
 *   [index u16][total u16][отсчёт u16 × k] LE, k ≤ PROTO_PULSE_CHUNK; запись длиннее
 *   режется на кадры, index — номер первого отсчёта кадра в записи, total — длина записи.
 *
 *   ⚠ proto_send_pulse шлёт всю запись сразу (до 0xFFFF отсчётов) — только вне набора
 *   (осциллограф, сервис), иначе остановит главный цикл на время передачи.
 *
 * Контекст: proto_rx/proto_tick_1s/proto_poll вызывать из главного цикла — того же, где идёт
 * det_process_block (поэтому -tplok меняет таблицу между порциями).
 * port->tx обязан забрать данные до возврата (скопировать в свой буфер/DMA-кольцо или
 * дождаться отправки): буфер кадра в proto один и переиспользуется. Кольцо UART должно
 * вмещать не меньше PROTO_TX_FRAME_MAX, иначе proto_poll не отправит ни одного кадра 0x01.
 * Текстовые ответы (≤ ≈1 КБ на линии) идут сразу, без проверки tx_free.
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "settings.h"

#define PROTO_SERIAL        "04740001"   /* GS474-0001: 40-е слово -cal, AtomSpectra требует hex */
#define PROTO_CHUNK_CH      512u           /* каналов в кадре 0x01 */
#define PROTO_PULSE_CHUNK   512u           /* отсчётов в кадре 0x02 */
#define PROTO_THR_MAX       1000u          /* потолок -thr, коды АЦП */
/* худший размер кадра на линии: кусок спектра (2 + 4·512 байт) + cmd + CRC, всё со стаффингом ×2,
   плюс START/FINISH — столько должно быть свободно в кольце UART для одного шага proto_poll */
#define PROTO_TX_FRAME_MAX  (2u * (2u + 4u * PROTO_CHUNK_CH + 3u) + 3u)
/* -inf: мёртвое время для BecqMoni = (RISE + FALL + 1) / F. В отсчётах АЦП 3.4667 Мвыб/с, шейпер V2
   (2026-11-07): фронт 10 %→пик 5–6, спад до thr/2 ≈ 25…30 ⇒ (6 + 30 + 1)/F ≈ 10.7 мкс. Константы
   не следят за -smp. Живое время прибор и так считает сам (статус -gst). */
#define PROTO_INF_RISE      6u
#define PROTO_INF_FALL      30u
#define PROTO_INF_FS        3466667u

/* флаги для port->apply: что поменялось в live-настройках */
#define PROTO_APPLY_THR     0x01u
#define PROTO_APPLY_TC      0x02u
#define PROTO_APPLY_TPL     0x04u
#define PROTO_APPLY_ALL     0xFFFFFFFFu    /* -defaults */

typedef struct { uint32_t elapsed_s; uint16_t cpu_x10; uint32_t cps; uint32_t invalid; } proto_status_t;

typedef struct {                       /* железо/приложение, на ПК — заглушки */
    void (*tx)(const uint8_t *p, size_t n);
    uint32_t (*millis)(void);
    bool (*settings_commit)(const settings_t *s); /* запись во флеш; вызывается только при остановленном наборе */
    void (*acq_start)(void); void (*acq_stop)(void); void (*acq_clear)(void);
    void (*enter_bootloader)(void);
    void (*service)(const char *cmd, char *reply, size_t cap); /* сервисные команды, см. выше */
    void (*fill_status)(proto_status_t *st);       /* время, cpu (0.1 %), cps, invalid */
    void (*apply)(uint32_t what);  /* live-настройки изменены (PROTO_APPLY_*); может быть NULL */
    size_t (*tx_free)(void);       /* свободно байт в кольце UART; NULL — без ограничения */
    bool (*hv_on)(void);           /* ВВ включено (hv_enabled); NULL — считать выключенным */
} proto_port_t;

void proto_init(const proto_port_t *port, settings_t *live);
void proto_rx(const uint8_t *p, size_t n);      /* сюда же ищется "!boot" */
void proto_tick_1s(void);                       /* при наборе: взвести выдачу спектра (не шлёт сам) */
void proto_poll(void);                          /* каждый проход цикла: ≤ 1 кадр выдачи спектра/статуса */
/* Набор остановлен не по -sto (сервис, авария ВВ, приложение): сбросить признак набора и
   выполнить отложенную запись настроек. Безопасно звать повторно. */
void proto_acq_stopped(void);
void proto_send_pulse(const uint16_t *s, size_t n); /* 0x02 — осциллограф для gammapult */
void proto_send_text(const char *s);            /* произвольный текстовый кадр 0x03 */

/* Температура для -inf (T1), °C; приложение обновляет раз в секунду. */
void proto_set_temperature(float t_c);
bool proto_acquiring(void);
/* корректных входных кадров (любой cmd, CRC сошёлся) с proto_init — для сторожа -baud (baud.h) */
uint32_t proto_rx_frames(void);
/* набор начат приложением, а не хостом (регистратор): выдача спектра и отложенная запись настроек — как при -sta */
void proto_acq_started(void);
/* произвольный кадр (регистратор: SHPROTO_CMD_LOG) — та же оговорка, что для proto_send_pulse */
void proto_send_frame(uint8_t cmd, const uint8_t *p, size_t n);
/* Состояние настроек во флеш для -gst:
 *   "none"     — приложение не сообщило, что во флеш (после proto_init);
 *   "defaults" — во флеш пусто или битые настройки, работаем на умолчаниях (proto_flash_defaults);
 *   "ok"       — настройки при старте загружены из флеш (proto_flash_loaded) или последняя запись прошла;
 *   "pending" / "err" — запись отложена до остановки набора / сбой записи.
 * -defaults состояние не меняет (в ОЗУ; флеш не тронута до -save). */
const char *proto_flash_status(void);
/* Приложение при старте: настройки во флеш не прошли settings_valid, взяты умолчания.
   Звать после proto_init; сбрасывается первой удачной записью. */
void proto_flash_defaults(void);
/* Приложение при старте: настройки загружены из флеш (settings_valid прошёл) → "ok". */
void proto_flash_loaded(void);

#endif
