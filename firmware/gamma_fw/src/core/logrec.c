/* logrec.c — см. logrec.h. Переносимый C (собирается и как C++ в gammapult). */
#include <string.h>
#include "logrec.h"

/* раскладка заголовка: одинакова в прошивке (arm-gcc) и в пульте (MSVC x64) */
typedef char logrec_chk_size[(sizeof(logrec_hdr_t) == 224u) ? 1 : -1];
typedef char logrec_chk_page[(sizeof(logrec_hdr_t) <= LOGREC_HDR_PAGE) ? 1 : -1];
typedef char logrec_chk_crc[(offsetof(logrec_hdr_t, hdr_crc) == 220u) ? 1 : -1];
typedef char logrec_chk_t64[(offsetof(logrec_hdr_t, t_start_ms) == 32u) ? 1 : -1];

uint32_t logrec_crc32(uint32_t crc, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    crc = ~crc;
    while (n--) {
        crc ^= *b++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void logrec_hdr_seal(logrec_hdr_t *h)
{
    h->magic = LOGREC_MAGIC;
    h->fmt = LOGREC_FMT;
    h->hdr_len = (uint16_t)sizeof *h;
    h->channels = LOGREC_CHANNELS;
    h->hdr_crc = logrec_crc32(0, h, offsetof(logrec_hdr_t, hdr_crc));
}

bool logrec_hdr_valid(const logrec_hdr_t *h)
{
    if (h->magic != LOGREC_MAGIC || h->fmt != LOGREC_FMT || h->hdr_len != sizeof *h) return false;
    if (h->channels != LOGREC_CHANNELS || h->payload_len > LOGREC_PAYLOAD_MAX) return false;
    return h->hdr_crc == logrec_crc32(0, h, offsetof(logrec_hdr_t, hdr_crc));
}

uint32_t logrec_sectors(uint32_t payload_len, uint32_t sector)
{
    uint32_t bytes = LOGREC_HDR_PAGE + payload_len;
    return (bytes + sector - 1u) / sector;
}

void logrec_enc_init(logrec_enc_t *e)
{
    memset(e, 0, sizeof *e);
}

size_t logrec_enc_step(logrec_enc_t *e, const uint32_t *spec, uint8_t *out, size_t cap, bool *done)
{
    size_t w = 0;
    for (;;) {
        while (e->ipend < e->npend) {          /* дописать начатый varint */
            if (w >= cap) { *done = false; return w; }
            out[w++] = e->pend[e->ipend++];
        }
        if (e->ch >= LOGREC_CHANNELS) { *done = true; return w; }
        uint32_t c = spec[e->ch++];
        e->sum += c;
        if (c == 0xFFFFFFFFu) e->sat = true;
        int32_t d = (int32_t)(c - e->prev);    /* разность по модулю 2^32 — обратима точно */
        e->prev = c;
        uint32_t z = ((uint32_t)d << 1) ^ (uint32_t)(d >> 31);
        e->npend = 0;
        e->ipend = 0;
        do {
            uint8_t b = (uint8_t)(z & 0x7Fu);
            z >>= 7;
            if (z) b |= 0x80u;
            e->pend[e->npend++] = b;
        } while (z);
    }
}

bool logrec_decode(const uint8_t *p, size_t n, uint32_t *spec, uint32_t channels)
{
    size_t i = 0;
    uint32_t prev = 0;
    for (uint32_t ch = 0; ch < channels; ch++) {
        uint32_t z = 0;
        int shift = 0;
        for (;;) {
            if (i >= n || shift > 28) return false;
            uint8_t b = p[i++];
            z |= (uint32_t)(b & 0x7Fu) << shift;
            shift += 7;
            if (!(b & 0x80u)) break;
        }
        int32_t d = (int32_t)((z >> 1) ^ (0u - (z & 1u)));
        prev += (uint32_t)d;
        spec[ch] = prev;
    }
    return i == n;
}
