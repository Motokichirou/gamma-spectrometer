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
#include <string.h>
#include <stdio.h>

/* ---- настройки ---------------------------------------------------------- */
#define SEND_PERIOD_MS    1000u   /* период отправки спектр+статус */
#define TEST_CPS          500u    /* синтетических событий в секунду */
#define CHUNK_CHANNELS    512u    /* каналов в одном кадре спектра */

/* ---- состояние ----------------------------------------------------------- */
static shproto_rx_t rx;
static bool         acquiring;
static uint32_t     t_start_ms;       /* момент -sta */
static uint32_t     t_last_send_ms;
static uint32_t     t_last_gen_ms;
static uint32_t     counts_last_sec;  /* для поля cps */

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

/* Статус: [elapsed_ms u32][cpu_load u16][cps u32][invalid_pulses u32] */
static void send_stat(void)
{
    uint8_t st[14];
    uint32_t elapsed = acquiring ? (app_port_millis() - t_start_ms) : 0u;
    put_u32le(&st[0], elapsed & 0x7FFFFFFu);
    put_u16le(&st[4], 100u);             /* cpu_load: заглушка ~10% (ед. 0.1%?) */
    put_u32le(&st[6], counts_last_sec);  /* cps */
    put_u32le(&st[10], 0u);              /* invalid_pulses */
    send_frame(SHPROTO_CMD_STAT, st, sizeof(st));
}

/* ---- команды ------------------------------------------------------------- */
static void handle_text(const uint8_t *pl, uint16_t len)
{
    char cmd[32];
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
        send_text("-ok");
    } else if (strncmp(cmd, "-inf", 4) == 0) {
        /* %f не поддерживается newlib-nano по умолчанию — выводим вручную */
        char info[64];
        float t = app_port_temperature_c();
        int t10 = (int)(t * 10.0f + (t >= 0.0f ? 0.5f : -0.5f));
        int frac = t10 % 10;
        if (frac < 0) frac = -frac;
        snprintf(info, sizeof(info), "GammaSpec v0.1 T1 %d.%d", t10 / 10, frac);
        send_text(info);
    }
    /* незнакомые команды молча игнорируем (этап 1) */
}

/* ---- публичный API -------------------------------------------------------- */
void app_init(void)
{
    shproto_rx_init(&rx);
    spectrum_clear();
    acquiring       = false;
    t_last_send_ms  = app_port_millis();
    t_last_gen_ms   = t_last_send_ms;
    counts_last_sec = 0;
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

    /* генерация тестовых событий — порциями каждые 100 мс */
    if (acquiring && (now - t_last_gen_ms) >= 100u) {
        t_last_gen_ms += 100u;
        spectrum_test_accumulate(TEST_CPS / 10u);
        counts_last_sec += TEST_CPS / 10u;
    }

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
