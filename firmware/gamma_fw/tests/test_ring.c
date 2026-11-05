/* Тесты кольцевого буфера SPSC (UART: приём из прерывания, передача по DMA). */
#include "t.h"
#include "ring.h"
#include <string.h>

static void test_init(void)
{
    ring_t r; uint8_t b[16];
    CHECK(!ring_init(&r, b, 12));
    CHECK(ring_size(&r) == 0 && ring_free(&r) == 0);
    CHECK(ring_put1(&r, 1) == false);                     /* негодное кольцо ничего не берёт */
    CHECK(ring_init(&r, b, 16));
    CHECK(ring_size(&r) == 16 && ring_used(&r) == 0 && ring_free(&r) == 16);
}

static void test_fill_and_overflow(void)
{
    ring_t r; uint8_t b[8], src[20], dst[20];
    for (int i = 0; i < 20; i++) src[i] = (uint8_t)(i + 1);
    ring_init(&r, b, 8);
    CHECK(ring_put(&r, src, 20) == 8);                    /* влезает ровно size */
    CHECK(ring_free(&r) == 0);
    CHECK(!ring_put1(&r, 99));
    CHECK(ring_get(&r, dst, 3) == 3 && memcmp(dst, src, 3) == 0);
    CHECK(ring_put(&r, src + 8, 5) == 3);
    CHECK(ring_get(&r, dst, 20) == 8 && memcmp(dst, src + 3, 8) == 0);
    CHECK(ring_used(&r) == 0 && ring_get(&r, dst, 20) == 0);
}

static void test_contig_wrap(void)
{
    ring_t r; uint8_t b[8], src[8] = {1, 2, 3, 4, 5, 6, 7, 8}, tmp[8];
    const uint8_t *p;
    ring_init(&r, b, 8);
    ring_put(&r, src, 6);
    ring_get(&r, tmp, 5);                                 /* хвост на 5 */
    ring_put(&r, src, 6);                                 /* 1 старый + 6 новых, перенос через конец */
    CHECK(ring_used(&r) == 7);
    uint32_t n = ring_peek_contig(&r, &p);
    CHECK(n == 3 && p == b + 5 && p[0] == 6 && p[1] == 1 && p[2] == 2);
    ring_drop(&r, n);
    n = ring_peek_contig(&r, &p);
    CHECK(n == 4 && p == b && p[0] == 3 && p[3] == 6);
    ring_drop(&r, 100);                                   /* больше, чем есть, — не уходит в минус */
    CHECK(ring_used(&r) == 0);
    CHECK(ring_peek_contig(&r, &p) == 0);
}

static void test_index_wraparound(void)
{
    /* свободно бегущие индексы у переполнения uint32 */
    ring_t r; uint8_t b[4], v[3] = {7, 8, 9}, d[3];
    ring_init(&r, b, 4);
    r.head = r.tail = 0xFFFFFFFEu;
    CHECK(ring_put(&r, v, 3) == 3 && ring_used(&r) == 3 && ring_free(&r) == 1);
    CHECK(ring_get(&r, d, 3) == 3 && d[0] == 7 && d[2] == 9);
    CHECK(r.tail == 1u && ring_used(&r) == 0);
}

static void test_stream(void)
{
    /* долгий поток кусками разной длины — порядок и содержимое сохраняются */
    ring_t r; uint8_t b[64], chunk[37], out[64];
    uint32_t wr = 0, rd = 0; int bad = 0;
    ring_init(&r, b, 64);
    for (int it = 0; it < 5000; it++) {
        uint32_t k = (uint32_t)(it * 7 % 37);
        for (uint32_t i = 0; i < k; i++) chunk[i] = (uint8_t)(wr + i);
        wr += ring_put(&r, chunk, k);
        uint32_t m = ring_get(&r, out, (uint32_t)(it * 5 % 29));
        for (uint32_t i = 0; i < m; i++) if (out[i] != (uint8_t)(rd + i)) bad++;
        rd += m;
    }
    CHECK(bad == 0);
    CHECK(wr - rd == ring_used(&r));
}

int main(void)
{
    test_init();
    test_fill_and_overflow();
    test_contig_wrap();
    test_index_wraparound();
    test_stream();
    return T_DONE();
}
