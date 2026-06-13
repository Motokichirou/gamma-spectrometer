/* app.c — прикладная логика этапа 1 (см. app.h)
 *
 * Поведение (совместимо с BecqMoni):
 *  - принимает текстовые команды (cmd=0x03): -sta / -sto / -rst / -inf
 *  - на -sta/-sto/-rst отвечает "-ok"
 *  - на -inf отвечает строкой с "T1 <температура>"
 *  - при наборе: раз в секунду шлёт спектр (cmd=0x01, кусками) и статус (cmd=0x04)
 *  - BecqMoni перерисовывает экран только по приходу cmd=0x04!
 */

#include "app.h"
#include "shproto.h"
#include "spectrum.h"
#include "selftest.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- настройки ---------------------------------------------------------- */
#define SEND_PERIOD_MS    1000u   /* период отправки спектр+статус */
#define TEST_CPS          500u    /* синтетических событий в секунду */
#define CHUNK_CHANNELS    512u    /* каналов в одном кадре спектра */
#define APP_SERIAL        "GS474-0001"   /* серийник для BecqMoni (-cal) */

/* ---- состояние ----------------------------------------------------------- */
static shproto_rx_t rx;
static bool         acquiring;
static uint32_t     t_start_ms;       /* момент -sta */
static uint32_t     t_last_send_ms;
static uint32_t     t_last_gen_ms;    /* для дефолтного (синтетического) источника */
static uint32_t     counts_last_sec;  /* для поля cps */
static uint32_t     invalid_total;    /* отбраковка PUR (поле invalid_pulses) */

/* буфер сборки кадра: худший случай — все байты стаффятся */
static uint8_t txbuf[2u * (CHUNK_CHANNELS * 4u + 2u + 3u) + 3u];
static uint8_t payload[CHUNK_CHANNELS * 4u + 2u];

/* ---- передача ------------------------------------------------------------ */
static void send_frame(uint8_t cmd, const uint8_t *pl, size_t len)
{
    size_t n = shproto_build(cmd, pl, len, txbuf);
    app_port_uart_send(txbuf, n);
}

static void send_text(const char *s)
{
    send_frame(SHPROTO_CMD_TEXT, (const uint8_t *)s, strlen(s));
}

void app_send_text(const char *s)
{
    send_text(s);
}

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Спектр кусками: [offset u16 LE][канал u32 LE × CHUNK] */
static void send_spectrum(void)
{
    const uint32_t *spec = spectrum_data();
    for (uint16_t off = 0; off < SPECTRUM_CHANNELS; off += CHUNK_CHANNELS) {
        put_u16le(&payload[0], off);
        for (uint16_t i = 0; i < CHUNK_CHANNELS; i++)
            put_u32le(&payload[2 + 4u * i], spec[off + i] & SPECTRUM_MASK);
        send_frame(SHPROTO_CMD_HISTOGRAM, payload, 2u + 4u * CHUNK_CHANNELS);
    }
}

/* Статус: [elapsed u32 СЕКУНДЫ][cpu_load u16][cps u32][invalid_pulses u32]
 * ⚠ BecqMoni: TimeSpan.FromSeconds(ElapsedTime) — единица именно секунды! */
static void send_stat(void)
{
    uint8_t st[14];
    uint32_t elapsed = (acquiring ? (app_port_millis() - t_start_ms) : 0u) / 1000u;
    put_u32le(&st[0], elapsed & 0x7FFFFFFu);
    put_u16le(&st[4], 100u);             /* cpu_load: заглушка ~10% (ед. 0.1%?) */
    put_u32le(&st[6], counts_last_sec);  /* cps */
    put_u32le(&st[10], invalid_total);   /* invalid_pulses (PUR) */
    send_frame(SHPROTO_CMD_STAT, st, sizeof(st));
}

/* ---- команды ------------------------------------------------------------- */
static void handle_text(const uint8_t *pl, uint16_t len)
{
    char cmd[160];   /* вмещает -wcal с 5 коэффициентами */
    if (len >= sizeof(cmd)) len = sizeof(cmd) - 1;
    memcpy(cmd, pl, len);
    cmd[len] = '\0';

    if (strncmp(cmd, "-sta", 4) == 0) {
        acquiring     = true;
        t_start_ms    = app_port_millis();
        t_last_gen_ms = t_start_ms;   /* иначе генератор "догонит" всё время простоя */
        send_text("-ok");
    } else if (strncmp(cmd, "-sto", 4) == 0) {
        acquiring = false;
        send_text("-ok");
    } else if (strncmp(cmd, "-rst", 4) == 0) {
        spectrum_clear();
        invalid_total = 0;
        send_text("-ok");
    } else if (strncmp(cmd, "-inf", 4) == 0) {
        /* Формат под парсер BecqMoni (deadTimeBtn_Click): split(' '),
         * [3]=rise int, [5]=fall int, [9]=частота. %f нет в newlib-nano. */
        char info[80];
        float t = app_port_temperature_c();
        int t10 = (int)(t * 10.0f + (t >= 0.0f ? 0.5f : -0.5f));
        int frac = t10 % 10;
        if (frac < 0) frac = -frac;
        snprintf(info, sizeof(info), "GammaSpec v0.1 rise 8 fall 8 T1 %d.%d freq 8000000",
                 t10 / 10, frac);
        send_text(info);
    } else if (strncmp(cmd, "-tst", 4) == 0) {
        selftest_command(cmd + 4);
    } else if (strncmp(cmd, "-thr", 4) == 0) {
        int eff = app_set_threshold_ch(atoi(cmd + 4));   /* каналы; <=0 — запрос */
        char r[32];
        if (eff < 0) snprintf(r, sizeof(r), "-err thr");
        else         snprintf(r, sizeof(r), "-ok thr %d", eff);
        send_text(r);
    } else if (strncmp(cmd, "-wcal", 5) == 0) {
        /* запись энергокалибровки: -wcal <order> <c0> <c1> ... <cN> */
        const char *p = cmd + 5;
        char *end;
        int order = (int)strtol(p, &end, 10); p = end;
        double c[5] = { 0, 0, 0, 0, 0 };
        int n = 0;
        while (n < 5) { double v = strtod(p, &end); if (end == p) break; c[n++] = v; p = end; }
        int ok = (order >= 1 && order <= 4 && n >= order + 1) ? app_cal_write(order, c) : 0;
        send_text(ok ? "-ok cal" : "-err cal");
    } else if (strncmp(cmd, "-rcal", 5) == 0) {
        /* Читаемый формат для НАШЕГО сервисного тула (gammapult), не для BecqMoni:
         * "CAL: <order> c0 c1 ..\r\n<серийник>\r\n" либо "CAL: none\r\n..". */
        char r[160];
        int order; double c[5];
        if (app_cal_read(&order, c)) {
            int o = snprintf(r, sizeof r, "CAL: %d", order);
            for (int i = 0; i <= order && i < 5; i++)
                o += snprintf(r + o, sizeof(r) - (size_t)o, " %.9g", c[i]);
            snprintf(r + o, sizeof(r) - (size_t)o, "\r\n" APP_SERIAL "\r\n");
        } else {
            snprintf(r, sizeof r, "CAL: none\r\n" APP_SERIAL "\r\n");
        }
        send_text(r);
    } else if (strncmp(cmd, "-calclr", 7) == 0) {
        send_text(app_cal_clear() ? "-ok cal cleared" : "-err cal");
    } else if (strncmp(cmd, "-cal", 4) == 0) {
        /* Протокол энергокалибровки BecqMoni:
         *  - голый "-cal"     : проверка связи И «Читать с устройства» (одна команда).
         *                       split("\r\n").Length>2, предпоследняя строка = серийник.
         *                       Если калибровка есть — отдаём 11 слов (по строке) + серийник.
         *  - "-cal <i> <hex>" : запись слова i (0..10), ответ "-ok".
         *  - "-cal <i>"       : чтение одного слова i в hex. */
        const char *p = cmd + 4;
        while (*p == ' ') p++;
        if (*p == '\0') {
            char r[200];
            uint32_t w[11];
            if (app_cal_read_words(w)) {
                int o = 0;
                for (int i = 0; i < 11; i++)
                    o += snprintf(r + o, sizeof(r) - (size_t)o, "%08lX\r\n",
                                  (unsigned long)w[i]);
                snprintf(r + o, sizeof(r) - (size_t)o, APP_SERIAL "\r\n");
            } else {
                snprintf(r, sizeof r, "CAL: none\r\n" APP_SERIAL "\r\n");
            }
            send_text(r);
        } else {
            char *end;
            long idx = strtol(p, &end, 10);
            while (*end == ' ') end++;
            if (idx < 0 || idx > 10) {
                send_text("-err cal");
            } else if (*end != '\0') {
                uint32_t word = (uint32_t)strtoul(end, NULL, 16);
                app_cal_stage_word((int)idx, word);
                send_text("-ok");
            } else {
                uint32_t w[11];
                char r[16];
                snprintf(r, sizeof r, "%08lX",
                         app_cal_read_words(w) ? (unsigned long)w[idx] : 0UL);
                send_text(r);
            }
        }
    }
    /* незнакомые команды молча игнорируем (этап 1) */
}

/* ---- API источника событий ------------------------------------------------ */
bool app_is_acquiring(void)
{
    return acquiring;
}

void app_count_events(uint32_t n)
{
    counts_last_sec += n;
}

void app_count_invalid(uint32_t n)
{
    invalid_total += n;
}

/* Дефолтный источник — синтетический генератор этапа 1.
 * Этап 2 переопределяет (сильное определение в stage2_hw.c). */
__attribute__((weak)) void app_source_poll(uint32_t now)
{
    if (acquiring && (now - t_last_gen_ms) >= 100u) {
        t_last_gen_ms += 100u;
        spectrum_test_accumulate(TEST_CPS / 10u);
        app_count_events(TEST_CPS / 10u);
    }
}

/* Дефолт: порог не настраивается (этап 1). Этап 2 (stage2_hw.c) переопределяет. */
__attribute__((weak)) int app_set_threshold_ch(int channels)
{
    (void)channels;
    return -1;
}

/* Дефолт: калибровка во flash не поддерживается (этап 1). */
__attribute__((weak)) int app_cal_write(int order, const double *coef)
{
    (void)order; (void)coef; return 0;
}
__attribute__((weak)) int app_cal_read(int *order, double *coef)
{
    (void)order; (void)coef; return 0;
}
__attribute__((weak)) int app_cal_stage_word(int idx, uint32_t word)
{
    (void)idx; (void)word; return 0;
}
__attribute__((weak)) int app_cal_read_words(uint32_t w[11])
{
    (void)w; return 0;
}
__attribute__((weak)) int app_cal_clear(void)
{
    return 0;
}

/* ---- публичный API -------------------------------------------------------- */
void app_init(void)
{
    shproto_rx_init(&rx);
    spectrum_clear();
    selftest_init();
    acquiring       = false;
    t_last_send_ms  = app_port_millis();
    t_last_gen_ms   = t_last_send_ms;
    counts_last_sec = 0;
    invalid_total   = 0;
}

void app_rx_feed(const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (shproto_rx_byte(&rx, data[i])) {
            if (rx.cmd == SHPROTO_CMD_TEXT)
                handle_text(rx.buf, rx.len);
            /* остальные команды от ПК на этапе 1 не ожидаются */
        }
    }
}

void app_poll(void)
{
    uint32_t now = app_port_millis();

    /* источник событий (синтетика по умолчанию / DAC+ADC+DSP на этапе 2) */
    app_source_poll(now);
    selftest_poll(now);

    /* раз в секунду: полный спектр + статус (статус — триггер отрисовки) */
    if ((now - t_last_send_ms) >= SEND_PERIOD_MS) {
        t_last_send_ms = now;
        if (acquiring) {
            send_spectrum();
            send_stat();
            counts_last_sec = 0;
        }
    }
}
