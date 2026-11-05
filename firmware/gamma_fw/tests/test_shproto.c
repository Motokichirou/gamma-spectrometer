/* Тесты shproto: CRC, стаффинг, ресинхронизация, переполнение. */
#include <string.h>
#include "t.h"
#include "shproto.h"

/* Скормить буфер парсеру; вернуть число принятых кадров, последний — в rx. */
static int feed(shproto_rx_t *rx, const uint8_t *d, size_t n)
{
    int frames = 0;
    for (size_t i = 0; i < n; i++)
        if (shproto_rx_byte(rx, d[i])) frames++;
    return frames;
}

static void test_crc_modbus(void)
{
    CHECK(shproto_crc16((const uint8_t *)"123456789", 9) == 0x4B37);
}

static void test_roundtrip_stuffing(void)
{
    const uint8_t pl[] = { 0xFE, 0xFD, 0xA5, 0x00, 0xFF };
    uint8_t out[64];
    size_t n = shproto_build(SHPROTO_CMD_TEXT, pl, sizeof pl, out);
    shproto_rx_t rx; shproto_rx_init(&rx);
    int frames = 0;
    for (size_t i = 0; i < n; i++)
        if (shproto_rx_byte(&rx, out[i])) {
            frames++;
            CHECK(i == n - 1);
        }
    CHECK(frames == 1);
    CHECK(rx.cmd == SHPROTO_CMD_TEXT);
    CHECK(rx.len == sizeof pl);
    CHECK(memcmp(rx.buf, pl, sizeof pl) == 0);
}

static void test_bad_crc_dropped(void)
{
    const uint8_t pl[] = { 1, 2, 3, 4 };
    uint8_t out[64];
    size_t n = shproto_build(SHPROTO_CMD_TEXT, pl, sizeof pl, out);
    out[4] ^= 0x01;   /* байт payload (индекс 2 — CMD, 3.. — данные) */
    shproto_rx_t rx; shproto_rx_init(&rx);
    CHECK(feed(&rx, out, n) == 0);
}

static void test_resync(void)
{
    const uint8_t pl[] = { 'a', 'b' };
    uint8_t good[64], bad[64];
    size_t ng = shproto_build(SHPROTO_CMD_TEXT, pl, sizeof pl, good);
    size_t nb = shproto_build(SHPROTO_CMD_TEXT, pl, sizeof pl, bad);
    shproto_rx_t rx; shproto_rx_init(&rx);
    /* обрывок: без FINISH */
    CHECK(feed(&rx, bad, nb - 1) == 0);
    CHECK(feed(&rx, good, ng) == 1);
    CHECK(rx.len == 2 && rx.buf[0] == 'a' && rx.buf[1] == 'b');
}

static void test_overlong_dropped(void)
{
    uint8_t pl[300];
    for (size_t i = 0; i < sizeof pl; i++) pl[i] = (uint8_t)(i * 7 + 1);
    uint8_t big[2 * (300 + 3) + 3];
    size_t nbig = shproto_build(SHPROTO_CMD_TEXT, pl, sizeof pl, big);
    const uint8_t ok_pl[] = { 'o', 'k' };
    uint8_t good[32];
    size_t ng = shproto_build(SHPROTO_CMD_TEXT, ok_pl, sizeof ok_pl, good);
    shproto_rx_t rx; shproto_rx_init(&rx);
    CHECK(feed(&rx, big, nbig) == 0);
    CHECK(feed(&rx, good, ng) == 1);
    CHECK(rx.len == 2 && rx.buf[0] == 'o');

    /* ровно максимальный payload принимается */
    uint8_t mx[2 * (SHPROTO_RX_MAX_PAYLOAD + 3) + 3];
    size_t nm = shproto_build(SHPROTO_CMD_TEXT, pl, SHPROTO_RX_MAX_PAYLOAD, mx);
    CHECK(feed(&rx, mx, nm) == 1);
    CHECK(rx.len == SHPROTO_RX_MAX_PAYLOAD);
    CHECK(memcmp(rx.buf, pl, SHPROTO_RX_MAX_PAYLOAD) == 0);
}

static void test_esc_at_end(void)
{
    /* кадр, у которого перед FINISH стоит ESC: A5 становится данными,
       кадр не завершается, но следующий START всё чинит */
    const uint8_t junk[] = { 0xFF, 0xFE, 0x03, 'x', 0xFD, 0xA5 };
    const uint8_t pl[] = { 'o', 'k' };
    uint8_t good[32];
    size_t ng = shproto_build(SHPROTO_CMD_TEXT, pl, sizeof pl, good);
    shproto_rx_t rx; shproto_rx_init(&rx);
    CHECK(feed(&rx, junk, sizeof junk) == 0);
    CHECK(feed(&rx, good, ng) == 1);
    CHECK(rx.len == 2 && rx.buf[0] == 'o');
}

static void test_keepalive_and_garbage_between(void)
{
    const uint8_t pl[] = { 'z' };
    uint8_t f[32];
    size_t n = shproto_build(SHPROTO_CMD_STAT, pl, sizeof pl, f);
    shproto_rx_t rx; shproto_rx_init(&rx);
    const uint8_t gar[] = { 0x00, 0x55, 0xFF, 0xFF, 0xA5, 0x12 };
    CHECK(feed(&rx, gar, sizeof gar) == 0);
    CHECK(feed(&rx, f, n) == 1);
    CHECK(rx.cmd == SHPROTO_CMD_STAT);
    CHECK(feed(&rx, f, n) == 1);   /* два кадра подряд */
}

int main(void)
{
    test_crc_modbus();
    test_roundtrip_stuffing();
    test_bad_crc_dropped();
    test_resync();
    test_overlong_dropped();
    test_esc_at_end();
    test_keepalive_and_garbage_between();
    return T_DONE();
}
