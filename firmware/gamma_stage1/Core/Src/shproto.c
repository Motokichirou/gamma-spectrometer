/* shproto.c — реализация протокола shproto (см. shproto.h) */

#include "shproto.h"

enum { SH_IDLE = 0, SH_CMD = 1, SH_DATA = 2 };

uint16_t shproto_crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int b = 0; b < 8; b++)
        crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
    return crc;
}

uint16_t shproto_crc16(const uint8_t *data, size_t n)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < n; i++)
        crc = shproto_crc16_update(crc, data[i]);
    return crc;
}

void shproto_rx_init(shproto_rx_t *rx)
{
    rx->state = SH_IDLE;
    rx->esc   = false;
    rx->len   = 0;
}

bool shproto_rx_byte(shproto_rx_t *rx, uint8_t b)
{
    /* START всегда сбрасывает парсер (0xFE не может встретиться внутри кадра —
     * внутри он передаётся стаффленным) */
    if (b == SHPROTO_START) {
        rx->state = SH_CMD;
        rx->esc   = false;
        rx->len   = 0;
        return false;
    }

    if (rx->state == SH_IDLE)
        return false;  /* в т.ч. игнорируем keepalive 0xFF между кадрами */

    if (b == SHPROTO_FINISH && !rx->esc) {
        bool ok = false;
        if (rx->state == SH_DATA && rx->len >= 2) {
            uint16_t rx_crc = (uint16_t)rx->buf[rx->len - 2] |
                              ((uint16_t)rx->buf[rx->len - 1] << 8);
            uint16_t crc = shproto_crc16_update(0xFFFFu, rx->cmd);
            for (uint16_t i = 0; i < (uint16_t)(rx->len - 2u); i++)
                crc = shproto_crc16_update(crc, rx->buf[i]);
            if (crc == rx_crc) {
                rx->len -= 2;   /* отрезали CRC, остался чистый payload */
                ok = true;
            }
        }
        rx->state = SH_IDLE;
        return ok;
    }

    if (b == SHPROTO_ESC && !rx->esc) {
        rx->esc = true;
        return false;
    }

    if (rx->esc) {
        b = (uint8_t)~b;
        rx->esc = false;
    }

    if (rx->state == SH_CMD) {
        rx->cmd   = b;
        rx->state = SH_DATA;
        rx->len   = 0;
        return false;
    }

    if (rx->len < sizeof(rx->buf))
        rx->buf[rx->len++] = b;
    else
        rx->state = SH_IDLE;   /* переполнение — кадр битый, бросаем */

    return false;
}

static size_t stuff_byte(uint8_t *p, uint8_t b)
{
    if (b == SHPROTO_START || b == SHPROTO_ESC || b == SHPROTO_FINISH) {
        p[0] = SHPROTO_ESC;
        p[1] = (uint8_t)~b;
        return 2;
    }
    p[0] = b;
    return 1;
}

size_t shproto_build(uint8_t cmd, const uint8_t *payload, size_t len, uint8_t *out)
{
    size_t o = 0;

    out[o++] = SHPROTO_KEEPALIVE;  /* префикс как у эталонных устройств */
    out[o++] = SHPROTO_START;

    uint16_t crc = shproto_crc16_update(0xFFFFu, cmd);
    for (size_t i = 0; i < len; i++)
        crc = shproto_crc16_update(crc, payload[i]);

    o += stuff_byte(&out[o], cmd);
    for (size_t i = 0; i < len; i++)
        o += stuff_byte(&out[o], payload[i]);
    o += stuff_byte(&out[o], (uint8_t)(crc & 0xFFu));   /* CRC LSB first */
    o += stuff_byte(&out[o], (uint8_t)(crc >> 8));

    out[o++] = SHPROTO_FINISH;
    return o;
}
