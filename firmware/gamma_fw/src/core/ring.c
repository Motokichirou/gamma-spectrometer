/* ring.c — кольцевой буфер SPSC (см. ring.h) */
#include "ring.h"

bool ring_init(ring_t *r, uint8_t *buf, uint32_t size)
{
    r->buf = buf;
    r->head = r->tail = 0;
    if (size < 2u || (size & (size - 1u))) { r->mask = 0; return false; }
    r->mask = size - 1u;
    return true;
}

uint32_t ring_size(const ring_t *r) { return r->mask ? r->mask + 1u : 0u; }
uint32_t ring_used(const ring_t *r) { return r->head - r->tail; }
uint32_t ring_free(const ring_t *r) { return ring_size(r) - ring_used(r); }

uint32_t ring_put(ring_t *r, const uint8_t *p, uint32_t n)
{
    uint32_t h = r->head, fr = ring_size(r) - (h - r->tail);
    if (n > fr) n = fr;
    for (uint32_t i = 0; i < n; i++) r->buf[(h + i) & r->mask] = p[i];
    r->head = h + n;               /* после данных */
    return n;
}

bool ring_put1(ring_t *r, uint8_t b)
{
    uint32_t h = r->head;
    if (h - r->tail >= ring_size(r)) return false;
    r->buf[h & r->mask] = b;
    r->head = h + 1u;
    return true;
}

uint32_t ring_get(ring_t *r, uint8_t *dst, uint32_t cap)
{
    uint32_t t = r->tail, n = r->head - t;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++) dst[i] = r->buf[(t + i) & r->mask];
    r->tail = t + n;               /* после чтения данных */
    return n;
}

uint32_t ring_peek_contig(const ring_t *r, const uint8_t **p)
{
    uint32_t t = r->tail, n = r->head - t, off = t & r->mask, to_end = ring_size(r) - off;
    *p = (const uint8_t *)r->buf + off;
    return n < to_end ? n : to_end;
}

void ring_drop(ring_t *r, uint32_t n)
{
    uint32_t used = r->head - r->tail;
    if (n > used) n = used;
    r->tail += n;
}
