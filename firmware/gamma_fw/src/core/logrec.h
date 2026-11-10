/*
 * logrec.h — формат записи журнала регистратора (SPEC_logger.md §2.4). Переносимый C, компилируется
 * и прошивкой, и пультом gammapult (как C++), поэтому формат один на обе стороны.
 *
 * Запись = страница заголовка (LOGREC_HDR_PAGE байт, сам заголовок — logrec_hdr_t, остаток 0xFF)
 * + полезная нагрузка с этого смещения. Нагрузка — накопленный спектр с начала сессии:
 * 8192 канала, d[i] = c[i] − c[i−1] (c[−1] = 0), zigzag, varint LEB128.
 * Все числа little-endian; раскладка проверяется при сборке (logrec.c).
 */
#ifndef LOGREC_H
#define LOGREC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOGREC_MAGIC      0x474C4F47u   /* "GOLG" в памяти: 'G','O','L','G' */
#define LOGREC_FMT        1u
#define LOGREC_HDR_PAGE   256u          /* заголовок занимает первую страницу записи */
#define LOGREC_CHANNELS   8192u
#define LOGREC_PAYLOAD_MAX (LOGREC_CHANNELS * 5u)   /* худший случай varint */
#define LOGREC_T_NONE     0x7FFF        /* температура неизвестна */

/* flags */
#define LOGF_SESSION_START 0x0001u  /* первая запись сессии (спектр пуст или почти пуст) */
#define LOGF_SESSION_END   0x0002u  /* последняя запись сессии (-log stop) */
#define LOGF_HOST_STA      0x0004u  /* за интервал хост дал -sta */
#define LOGF_HOST_STO      0x0008u  /* за интервал хост дал -sto (набор мог стоять) */
#define LOGF_HOST_RST      0x0010u  /* за интервал хост дал -rst: спектр начат заново, база для разностей — эта запись */
#define LOGF_HV_TRIP       0x0020u  /* за интервал ВВ выключено защитой / было выключено */
#define LOGF_VREF_BAD      0x0040u  /* за интервал опора была вне нормы */
#define LOGF_SATURATED     0x0080u  /* канал спектра упёрся в 2^32−1 */
#define LOGF_ACQ_STOPPED   0x0100u  /* на конец интервала набор не идёт */
#define LOGF_TC_ON         0x0200u  /* термокомпенсация прибора включена (k1 или k2 ≠ 0) */

typedef struct {
    uint32_t magic;            /* LOGREC_MAGIC */
    uint16_t fmt;              /* LOGREC_FMT */
    uint16_t hdr_len;          /* sizeof(logrec_hdr_t) */
    uint32_t payload_len;      /* байт нагрузки */
    uint32_t seq;              /* сквозной номер записи (голова журнала — наибольший) */
    uint32_t session;          /* номер сессии */
    uint32_t rec_in_session;   /* номер записи в сессии с 0 */
    uint32_t flags;            /* LOGF_* */
    uint32_t channels;         /* LOGREC_CHANNELS */
    uint64_t t_start_ms;       /* начало интервала, мс от старта сессии */
    uint64_t t_end_ms;         /* конец интервала */
    uint64_t real_ms;          /* реальное время набора (накопленное), мс */
    uint64_t live_ms;          /* живое время набора, мс */
    uint64_t unix_base;        /* unix-время старта сессии, если задано -log time, иначе 0 */
    uint64_t total;            /* сумма каналов спектра на конец интервала */
    /* интервал */
    int16_t  t_start_c100;     /* Pt1000, °C ×100, LOGREC_T_NONE — нет */
    int16_t  t_end_c100;
    int16_t  t_avg_c100;
    int16_t  t_min_c100;
    int16_t  t_max_c100;
    uint16_t hv_set_v;         /* уставка ВВ, В */
    float    hv_avg_v;         /* среднее по монитору, |В| */
    float    vref_mv, vdd_mv;
    float    base, sigma;      /* база и σ шума, коды АЦП, на конец интервала */
    float    cps_in, cps_acc;  /* средние за интервал, имп/с */
    uint32_t rej_shape, rej_width, rej_pile, rej_budget;   /* отбраковка за интервал */
    uint32_t overrun, lost;    /* затёртые и потерянные порции за интервал */
    float    tc_t0, tc_k1, tc_k2;   /* действующая термокомпенсация */
    float    tc_gain;          /* множитель усиления прибора на конец интервала (1 — без поправки) */
    char     fw_tag[16];       /* хэш прошивки */
    char     serial[16];       /* серийник */
    uint32_t sec_count;        /* секунд телеметрии в интервале */
    uint32_t reserved[5];
    uint32_t payload_crc;      /* CRC32 нагрузки */
    uint32_t hdr_crc;          /* CRC32 заголовка от начала до этого поля (не включая) */
} logrec_hdr_t;

uint32_t logrec_crc32(uint32_t crc, const void *p, size_t n);   /* старт crc = 0, как zlib */
void     logrec_hdr_seal(logrec_hdr_t *h);                       /* hdr_len, magic, fmt, hdr_crc */
bool     logrec_hdr_valid(const logrec_hdr_t *h);                /* magic, fmt, длины, CRC */
uint32_t logrec_sectors(uint32_t payload_len, uint32_t sector);  /* сколько секторов займёт запись */

/* потоковый кодировщик: каналы по одному, байты — в буфер вызывающего */
typedef struct {
    uint32_t ch;               /* следующий канал */
    uint32_t prev;
    uint8_t  pend[5];          /* недописанный varint, если буфер кончился посреди */
    uint8_t  npend, ipend;
    uint64_t sum;              /* сумма закодированных каналов */
    bool     sat;              /* встретился канал 2^32−1 (насыщение) */
} logrec_enc_t;
void   logrec_enc_init(logrec_enc_t *e);
/* закодировать следующие каналы из spec в out[0..cap); возврат — записано байт; *done — все каналы */
size_t logrec_enc_step(logrec_enc_t *e, const uint32_t *spec, uint8_t *out, size_t cap, bool *done);

/* декодирование целой нагрузки; false — испорчена (не хватило/лишние байты, переполнение) */
bool logrec_decode(const uint8_t *p, size_t n, uint32_t *spec, uint32_t channels);

#ifdef __cplusplus
}
#endif
#endif
