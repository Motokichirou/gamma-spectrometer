/* logger.c — автономный регистратор спектров (см. logger.h, SPEC_logger.md). */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "logger.h"

#define CFG_MAGIC  0x46434C47u          /* "GLCF" */
#define FRAME_HDR  10u                  /* [сектор u16][смещение u32][всего u32] */
#define TX_NEED_FRAME (2u * (FRAME_HDR + LOG_READ_CHUNK + 3u) + 3u)
#define TX_NEED_LINE  256u

typedef struct {
    uint32_t magic, gen, enabled, period_s, delay_s;
    uint32_t reserved[3];
    uint32_t crc;
} logcfg_t;

enum { W_IDLE, W_RUN, W_VERIFY_PAGE, W_VERIFY_HDR };
enum { C_IDLE, C_ERASE, C_PROG, C_VERIFY };

static const logger_port_t *P;

static struct {
    bool     present, awake, busy;
    uint8_t  id[3];
    uint32_t err;
    /* время */
    uint64_t up_ms;                     /* с logger_init */
    uint32_t last_millis;
    /* конфигурация */
    logcfg_t cfg;
    uint32_t cfg_addr;                  /* где лежит действующая копия; ~0 — нигде */
    bool     cfg_dirty;
    uint8_t  cst;
    uint32_t cfg_target;
    /* журнал */
    bool     scanned;
    uint32_t scan_s;
    uint32_t n_rec;
    bool     have_head, wrapped;
    uint32_t head_seq, head_sector, head_secs, max_session;
    uint32_t wr_sector, next_seq, next_session;
    /* сессия */
    bool     active, auto_done;
    uint32_t session, rec_n;
    uint64_t sess_up0;                  /* up_ms на старте сессии */
    uint64_t next_rec_ms;               /* от старта сессии */
    uint64_t int_start_ms;
    bool     time_set;
    uint64_t time_unix, time_at_up;
    uint64_t unix_base;
    bool     req, req_end;
    /* интервал */
    uint32_t flags;
    float    t_first, t_last, t_sum, t_min, t_max;
    uint32_t t_n, sec_n;
    double   hv_sum, cps_in_sum, cps_acc_sum;
    uint32_t hv_n;
    log_snap_t prev;
    /* запись */
    uint8_t  wst;
    bool     cur_end;
    uint32_t rec_sector, rec_base, wr_addr, erased_end, page_n, payload_len, payload_crc;
    logrec_enc_t enc;
    logrec_hdr_t hdr;
    uint8_t  page[W25_PAGE];
    /* стирание журнала */
    bool     erasing;
    uint32_t erase_addr;
    /* выгрузка */
    bool     listing, reading;
    uint32_t list_s, list_n;
    uint32_t rd_sector, rd_off, rd_total;
    uint8_t  rbuf[FRAME_HDR + LOG_READ_CHUNK];
} S;

/* ---------------- мелочи ---------------- */
static uint32_t sec_addr(uint32_t s) { return LOG_START + s * W25_SECTOR; }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void awake(void)
{
    if (!S.awake) { w25_wake(P->spi); S.awake = true; }
}

static void issued(void) { S.busy = true; }

static int16_t c100(float t)
{
    if (!isfinite(t) || t < -300.0f || t > 300.0f) return (int16_t)LOGREC_T_NONE;
    return (int16_t)lrintf(t * 100.0f);
}

static bool tx_ok(size_t need) { return !P->tx_free || P->tx_free() >= need; }

static bool read_hdr(uint32_t s, logrec_hdr_t *h)
{
    w25_read(P->spi, sec_addr(s), h, sizeof *h);
    if (!logrec_hdr_valid(h)) return false;
    return s + logrec_sectors(h->payload_len, W25_SECTOR) <= LOG_SECTORS;
}

/* ---------------- конфигурация ---------------- */
static uint32_t cfg_crc(const logcfg_t *c) { return logrec_crc32(0, c, offsetof(logcfg_t, crc)); }

static bool cfg_read(uint32_t addr, logcfg_t *c)
{
    w25_read(P->spi, addr, c, sizeof *c);
    return c->magic == CFG_MAGIC && c->crc == cfg_crc(c)
        && c->period_s >= LOG_PERIOD_MIN && c->period_s <= LOG_PERIOD_MAX && c->delay_s <= LOG_DELAY_MAX;
}

static void cfg_load(void)
{
    logcfg_t a, b;
    bool va = cfg_read(LOG_CFG_A, &a), vb = cfg_read(LOG_CFG_B, &b);
    if (va && (!vb || (int32_t)(a.gen - b.gen) >= 0)) { S.cfg = a; S.cfg_addr = LOG_CFG_A; }
    else if (vb) { S.cfg = b; S.cfg_addr = LOG_CFG_B; }
    else {
        memset(&S.cfg, 0, sizeof S.cfg);
        S.cfg.magic = CFG_MAGIC;
        S.cfg.period_s = LOG_PERIOD_DEF;
        S.cfg.delay_s = LOG_DELAY_DEF;
        S.cfg_addr = 0xFFFFFFFFu;
    }
}

static bool cfg_step(void)
{
    switch (S.cst) {
    case C_IDLE:
        if (!S.cfg_dirty) return false;
        S.cfg_dirty = false;
        S.cfg_target = (S.cfg_addr == LOG_CFG_A) ? LOG_CFG_B : LOG_CFG_A;
        S.cfg.gen++;
        S.cfg.magic = CFG_MAGIC;
        S.cfg.crc = cfg_crc(&S.cfg);
        w25_erase4k(P->spi, S.cfg_target);
        issued();
        S.cst = C_ERASE;
        return true;
    case C_ERASE:
        w25_program(P->spi, S.cfg_target, &S.cfg, sizeof S.cfg);
        issued();
        S.cst = C_PROG;
        return true;
    case C_PROG: {
        logcfg_t c;
        if (cfg_read(S.cfg_target, &c) && memcmp(&c, &S.cfg, sizeof c) == 0) S.cfg_addr = S.cfg_target;
        else S.err++;
        S.cst = C_IDLE;
        return true;
    }
    default:
        S.cst = C_IDLE;
        return false;
    }
}

/* ---------------- поиск головы журнала ---------------- */
static void scan_finish(void)
{
    S.scanned = true;
    S.wr_sector = S.have_head ? S.head_sector + S.head_secs : 0u;
    if (S.wr_sector >= LOG_SECTORS) S.wr_sector = 0;
    S.next_seq = S.have_head ? S.head_seq + 1u : 1u;
    S.next_session = S.max_session + 1u;
}

static void scan_step(void)
{
    for (unsigned k = 0; k < LOG_SCAN_PER_POLL; k++) {
        if (S.scan_s >= LOG_SECTORS) { scan_finish(); return; }
        logrec_hdr_t h;
        if (read_hdr(S.scan_s, &h)) {
            uint32_t secs = logrec_sectors(h.payload_len, W25_SECTOR);
            S.n_rec++;
            if (!S.have_head || (int32_t)(h.seq - S.head_seq) > 0) {
                S.have_head = true;
                S.head_seq = h.seq;
                S.head_sector = S.scan_s;
                S.head_secs = secs;
            }
            if (h.session > S.max_session) S.max_session = h.session;
            S.scan_s += secs;
        } else {
            S.scan_s++;
        }
    }
}

/* ---------------- интервал ---------------- */
static void interval_reset(void)
{
    S.flags = 0;
    S.t_first = S.t_last = NAN;
    S.t_sum = 0.0f;
    S.t_min = 1e9f;
    S.t_max = -1e9f;
    S.t_n = S.sec_n = S.hv_n = 0;
    S.hv_sum = S.cps_in_sum = S.cps_acc_sum = 0.0;
}

static uint64_t sess_ms(void) { return S.up_ms - S.sess_up0; }

/* заголовок по накопленному за интервал (нагрузка и CRC — потом) */
static void hdr_fill(void)
{
    logrec_hdr_t *h = &S.hdr;
    log_snap_t s;
    memset(&s, 0, sizeof s);
    if (P->snap) P->snap(&s);
    memset(h, 0, sizeof *h);
    h->seq = S.next_seq;
    h->session = S.session;
    h->rec_in_session = S.rec_n;
    h->flags = S.flags;
    if (S.rec_n == 0) h->flags |= LOGF_SESSION_START;
    if (S.cur_end) h->flags |= LOGF_SESSION_END;
    if (!s.acq_running) h->flags |= LOGF_ACQ_STOPPED;
    if (s.tc_k1 != 0.0f || s.tc_k2 != 0.0f) h->flags |= LOGF_TC_ON;
    h->t_start_ms = S.int_start_ms;
    h->t_end_ms = sess_ms();
    h->real_ms = s.real_ms;
    h->live_ms = s.live_ms;
    h->unix_base = S.unix_base;
    h->t_start_c100 = c100(S.t_first);
    h->t_end_c100 = c100(S.t_last);
    h->t_avg_c100 = S.t_n ? c100(S.t_sum / (float)S.t_n) : (int16_t)LOGREC_T_NONE;
    h->t_min_c100 = S.t_n ? c100(S.t_min) : (int16_t)LOGREC_T_NONE;
    h->t_max_c100 = S.t_n ? c100(S.t_max) : (int16_t)LOGREC_T_NONE;
    h->hv_set_v = s.hv_set_v;
    h->hv_avg_v = S.hv_n ? (float)(S.hv_sum / S.hv_n) : 0.0f;
    h->vref_mv = s.vref_mv;
    h->vdd_mv = s.vdd_mv;
    h->base = s.base;
    h->sigma = s.sigma;
    h->cps_in = S.sec_n ? (float)(S.cps_in_sum / S.sec_n) : 0.0f;
    h->cps_acc = S.sec_n ? (float)(S.cps_acc_sum / S.sec_n) : 0.0f;
    h->rej_shape = s.rej_shape - S.prev.rej_shape;
    h->rej_width = s.rej_width - S.prev.rej_width;
    h->rej_pile = s.rej_pile - S.prev.rej_pile;
    h->rej_budget = s.rej_budget - S.prev.rej_budget;
    h->overrun = s.overrun - S.prev.overrun;
    h->lost = s.lost - S.prev.lost;
    h->tc_t0 = s.tc_t0;
    h->tc_k1 = s.tc_k1;
    h->tc_k2 = s.tc_k2;
    h->tc_gain = s.tc_gain;
    if (P->fw_tag) strncpy(h->fw_tag, P->fw_tag, sizeof h->fw_tag - 1);
    if (P->serial) strncpy(h->serial, P->serial, sizeof h->serial - 1);
    h->sec_count = S.sec_n;
    S.prev = s;
    S.int_start_ms = h->t_end_ms;
    interval_reset();
}

/* ---------------- запись ---------------- */
static void rec_begin(void)
{
    S.cur_end = S.req_end;
    S.req = false;
    hdr_fill();
    if (S.wr_sector + LOG_REC_MAX_SECTORS > LOG_SECTORS) S.wr_sector = 0;
    S.rec_sector = S.wr_sector;
    S.rec_base = sec_addr(S.wr_sector);
    S.wr_addr = S.rec_base + LOGREC_HDR_PAGE;
    S.erased_end = S.rec_base;
    S.payload_len = 0;
    S.payload_crc = 0;
    logrec_enc_init(&S.enc);
    S.wst = W_RUN;
}

static void rec_abort(void)
{
    S.err++;
    S.wr_sector = (S.erased_end - LOG_START) / W25_SECTOR;   /* дальше — со свежего места */
    if (S.wr_sector >= LOG_SECTORS) S.wr_sector = 0;
    S.wst = W_IDLE;
    if (S.cur_end) { S.active = false; S.req_end = false; }
}

static void rec_commit(void)
{
    S.n_rec++;
    S.have_head = true;
    S.head_seq = S.hdr.seq;
    S.head_sector = S.rec_sector;
    S.head_secs = logrec_sectors(S.payload_len, W25_SECTOR);
    if (S.hdr.session > S.max_session) S.max_session = S.hdr.session;
    S.next_seq++;
    S.wr_sector = S.rec_sector + S.head_secs;
    if (S.wr_sector >= LOG_SECTORS) S.wr_sector = 0;
    S.rec_n++;
    S.wst = W_IDLE;
    if (S.cur_end) { S.active = false; S.req_end = false; }
}

static bool writer_step(void)
{
    switch (S.wst) {
    case W_IDLE:
        if (!S.req || !S.scanned || S.erasing) return false;
        rec_begin();
        return true;
    case W_RUN: {
        uint32_t need = S.enc.ch < LOGREC_CHANNELS || S.enc.ipend < S.enc.npend ? S.wr_addr : S.rec_base;
        if (need >= S.erased_end) {            /* сектор под запись ещё не стёрт */
            uint32_t m = 0;
            w25_read(P->spi, S.erased_end, &m, sizeof m);
            if (m == LOGREC_MAGIC && S.n_rec) { S.n_rec--; S.wrapped = true; }   /* затираем старую */
            w25_erase4k(P->spi, S.erased_end);
            S.erased_end += W25_SECTOR;
            issued();
            return true;
        }
        bool done = false;
        size_t n = 0;
        if (need != S.rec_base) n = logrec_enc_step(&S.enc, P->spectrum(), S.page, W25_PAGE, &done);
        else done = true;
        if (n) {
            w25_program(P->spi, S.wr_addr, S.page, n);
            S.payload_crc = logrec_crc32(S.payload_crc, S.page, n);
            S.page_n = (uint32_t)n;
            issued();
            S.wst = W_VERIFY_PAGE;
            return true;
        }
        /* нагрузка записана — заголовок последним */
        S.hdr.payload_len = S.payload_len;
        S.hdr.payload_crc = S.payload_crc;
        S.hdr.total = S.enc.sum;
        if (S.enc.sat) S.hdr.flags |= LOGF_SATURATED;
        logrec_hdr_seal(&S.hdr);
        w25_program(P->spi, S.rec_base, &S.hdr, sizeof S.hdr);
        issued();
        S.wst = W_VERIFY_HDR;
        return true;
    }
    case W_VERIFY_PAGE: {
        uint8_t *v = S.rbuf;
        w25_read(P->spi, S.wr_addr, v, S.page_n);
        if (memcmp(v, S.page, S.page_n) != 0) { rec_abort(); return true; }
        S.wr_addr += W25_PAGE;
        S.payload_len += S.page_n;
        S.wst = W_RUN;
        return true;
    }
    case W_VERIFY_HDR: {
        logrec_hdr_t h;
        w25_read(P->spi, S.rec_base, &h, sizeof h);
        if (memcmp(&h, &S.hdr, sizeof h) != 0) { rec_abort(); return true; }
        rec_commit();
        return true;
    }
    default:
        S.wst = W_IDLE;
        return false;
    }
}

/* ---------------- стирание журнала ---------------- */
static bool erase_step(void)
{
    if (!S.erasing) return false;
    if (S.erase_addr >= LOG_END) {
        S.erasing = false;
        S.n_rec = 0;
        S.have_head = S.wrapped = false;
        S.wr_sector = 0;
        return false;
    }
    w25_erase64k(P->spi, S.erase_addr);
    S.erase_addr += W25_BLOCK;
    issued();
    return true;
}

/* ---------------- выгрузка ---------------- */
static bool list_step(void)
{
    if (!S.listing) return false;
    if (!tx_ok(TX_NEED_LINE)) return true;
    for (unsigned k = 0; k < LOG_SCAN_PER_POLL; k++) {
        if (S.list_s >= LOG_SECTORS) {
            char r[48];
            snprintf(r, sizeof r, "-ok log list %lu\r\n", (unsigned long)S.list_n);
            P->send_text(r);
            S.listing = false;
            return true;
        }
        logrec_hdr_t h;
        if (read_hdr(S.list_s, &h)) {
            char r[128];
            snprintf(r, sizeof r, "-log rec %lu %lu %lu %lu %lu %lu %llu\r\n", (unsigned long)S.list_s,
                     (unsigned long)h.seq, (unsigned long)h.session, (unsigned long)h.rec_in_session,
                     (unsigned long)h.payload_len, (unsigned long)h.flags, (unsigned long long)h.t_end_ms);
            P->send_text(r);
            S.list_n++;
            S.list_s += logrec_sectors(h.payload_len, W25_SECTOR);
            return true;                       /* одна строка за проход */
        }
        S.list_s++;
    }
    return true;
}

static bool read_step(void)
{
    if (!S.reading) return false;
    if (!tx_ok(TX_NEED_FRAME)) return true;
    uint8_t *f = S.rbuf;
    if (S.rd_off == 0) {
        logrec_hdr_t h;
        if (S.rd_sector >= LOG_SECTORS || !read_hdr(S.rd_sector, &h)) {
            P->send_text("-err log read\r\n");
            S.reading = false;
            return true;
        }
        S.rd_total = LOGREC_HDR_PAGE + h.payload_len;
        put16(f, (uint16_t)S.rd_sector);
        put32(f + 2, 0);
        put32(f + 6, S.rd_total);
        memcpy(f + FRAME_HDR, &h, sizeof h);
        P->send_frame(SHPROTO_CMD_LOG, f, FRAME_HDR + sizeof h);
        S.rd_off = LOGREC_HDR_PAGE;
        return true;
    }
    if (S.rd_off >= S.rd_total) {
        char r[40];
        snprintf(r, sizeof r, "-ok log read %lu\r\n", (unsigned long)S.rd_sector);
        P->send_text(r);
        S.reading = false;
        return true;
    }
    uint32_t n = S.rd_total - S.rd_off;
    if (n > LOG_READ_CHUNK) n = LOG_READ_CHUNK;
    w25_read(P->spi, sec_addr(S.rd_sector) + S.rd_off, f + FRAME_HDR, n);
    put16(f, (uint16_t)S.rd_sector);
    put32(f + 2, S.rd_off);
    put32(f + 6, S.rd_total);
    P->send_frame(SHPROTO_CMD_LOG, f, FRAME_HDR + n);
    S.rd_off += n;
    return true;
}

/* ---------------- сессия ---------------- */
static void session_start(void)
{
    if (P->acq_begin) P->acq_begin();
    S.active = true;
    S.session = S.next_session++;
    S.rec_n = 0;
    S.sess_up0 = S.up_ms;
    S.int_start_ms = 0;
    S.next_rec_ms = (uint64_t)S.cfg.period_s * 1000u;
    S.unix_base = S.time_set ? S.time_unix + (S.up_ms - S.time_at_up) / 1000u : 0u;
    memset(&S.prev, 0, sizeof S.prev);
    if (P->snap) P->snap(&S.prev);
    interval_reset();
    S.req = true;                              /* запись 0 — сразу */
    S.req_end = false;
}

static void session_tick(void)
{
    if (!S.scanned) return;
    if (!S.active && S.cfg.enabled && !S.auto_done && S.up_ms >= (uint64_t)S.cfg.delay_s * 1000u) {
        S.auto_done = true;
        if (!S.erasing) session_start();
    }
    if (S.active && !S.req_end) {
        uint64_t t = sess_ms();
        if (t >= S.next_rec_ms) {
            S.req = true;
            S.next_rec_ms += (uint64_t)S.cfg.period_s * 1000u;
            if (S.next_rec_ms <= t) S.next_rec_ms = t + (uint64_t)S.cfg.period_s * 1000u;   /* отстали — не копить */
        }
    }
}

/* ---------------- публичное ---------------- */
void logger_init(const logger_port_t *port)
{
    memset(&S, 0, sizeof S);
    P = port;
    S.last_millis = P->millis ? P->millis() : 0u;
    w25_wake(P->spi);
    S.awake = true;
    w25_jedec(P->spi, S.id);
    S.present = S.id[0] == W25_JEDEC_MFR && S.id[1] == W25_JEDEC_TYPE && S.id[2] == W25_JEDEC_CAP;
    if (!S.present) { w25_sleep(P->spi); S.awake = false; return; }
    cfg_load();
    interval_reset();
}

void logger_poll(uint32_t now_ms)
{
    if (!P || !S.present) return;
    S.up_ms += (uint32_t)(now_ms - S.last_millis);
    S.last_millis = now_ms;
    session_tick();

    bool want = S.wst != W_IDLE || S.req || S.cst != C_IDLE || S.cfg_dirty || S.erasing || !S.scanned
             || S.listing || S.reading;
    if (!want) {
        /* без работы и без регистратора — флешка спит (шум, SPEC_logger.md §2.2) */
        if (S.awake && !S.busy && !S.active && !S.cfg.enabled) { w25_sleep(P->spi); S.awake = false; }
        return;
    }
    awake();
    if (S.busy) {
        if (w25_busy(P->spi)) return;
        S.busy = false;
    }
    if (writer_step()) return;
    if (cfg_step()) return;
    if (erase_step()) return;
    if (!S.scanned) { scan_step(); return; }
    if (read_step()) return;
    (void)list_step();
}

void logger_tick_1s(const log_tel_t *t)
{
    if (!S.active) return;
    S.sec_n++;
    S.cps_in_sum += t->cps_in;
    S.cps_acc_sum += t->cps_acc;
    if (isfinite(t->temp_c)) {
        if (!S.t_n) S.t_first = t->temp_c;
        S.t_last = t->temp_c;
        S.t_sum += t->temp_c;
        if (t->temp_c < S.t_min) S.t_min = t->temp_c;
        if (t->temp_c > S.t_max) S.t_max = t->temp_c;
        S.t_n++;
    }
    if (t->hv_on) { S.hv_sum += t->hv_out_v; S.hv_n++; }
    if (t->hv_set_v > 0 && !t->hv_on) S.flags |= LOGF_HV_TRIP;
    if (!t->vref_ok) S.flags |= LOGF_VREF_BAD;
}

void logger_event(uint32_t flag) { if (S.active) S.flags |= flag; }

bool logger_session_active(void) { return S.active; }

static const char *args_of(const char *cmd, const char *name)
{
    size_t n = strlen(name);
    if (strncmp(cmd, name, n) != 0) return NULL;
    if (cmd[n] != 0 && cmd[n] != ' ' && cmd[n] != '\t') return NULL;
    const char *a = cmd + n;
    while (*a == ' ' || *a == '\t') a++;
    return a;
}

static bool parse_u(const char *s, unsigned long long *v, const char **end)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    if (*s < '0' || *s > '9') return false;
    *v = strtoull(s, &e, 10);
    *end = e;
    return true;
}

static bool rest_empty(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return *s == 0;
}

bool logger_handle(const char *cmd, char *reply, size_t cap)
{
    const char *a = args_of(cmd, "-log");
    if (!a) return false;
    if (!P || !S.present) {
        snprintf(reply, cap, "-err log noflash id %02X%02X%02X", S.id[0], S.id[1], S.id[2]);
        return true;
    }
    const char *sub;
    if (!*a) {
        uint32_t used = 0;
        if (S.have_head) {
            uint32_t end = S.head_sector + S.head_secs;
            used = S.wrapped ? LOG_SECTORS : end;     /* грубо: занято до головы, после заворота — всё */
        }
        char scan[12];
        if (S.scanned) snprintf(scan, sizeof scan, "done");
        else snprintf(scan, sizeof scan, "%lu%%", (unsigned long)(S.scan_s * 100u / LOG_SECTORS));
        snprintf(reply, cap, "-ok log %s period %lu delay %lu active %d session %lu rec %lu records %lu "
                 "used_kb %lu total_kb %lu wrap %d id %02X%02X%02X err %lu scan %s%s",
                 S.cfg.enabled ? "on" : "off", (unsigned long)S.cfg.period_s, (unsigned long)S.cfg.delay_s,
                 S.active ? 1 : 0, (unsigned long)(S.active ? S.session : S.max_session),
                 (unsigned long)S.rec_n, (unsigned long)S.n_rec, (unsigned long)(used * 4u),
                 (unsigned long)(LOG_SECTORS * 4u), S.wrapped ? 1 : 0, S.id[0], S.id[1], S.id[2],
                 (unsigned long)S.err, scan,
                 S.erasing ? " erasing" : "");
        return true;
    }
    if ((sub = args_of(a, "on")) != NULL) {
        unsigned long long p = LOG_PERIOD_DEF, d = S.cfg.delay_s;
        const char *e = sub;
        if (*sub) {
            if (!parse_u(sub, &p, &e)) { snprintf(reply, cap, "-err log on"); return true; }
            if (!rest_empty(e) && !parse_u(e, &d, &e)) { snprintf(reply, cap, "-err log on"); return true; }
        }
        if (!rest_empty(e) || p < LOG_PERIOD_MIN || p > LOG_PERIOD_MAX || d > LOG_DELAY_MAX) {
            snprintf(reply, cap, "-err log on");
            return true;
        }
        S.cfg.enabled = 1;
        S.cfg.period_s = (uint32_t)p;
        S.cfg.delay_s = (uint32_t)d;
        S.cfg_dirty = true;
        snprintf(reply, cap, "-ok log on %lu %lu", (unsigned long)p, (unsigned long)d);
        return true;
    }
    if ((sub = args_of(a, "off")) != NULL) {
        S.cfg.enabled = 0;
        S.cfg_dirty = true;
        snprintf(reply, cap, "-ok log off");
        return true;
    }
    if ((sub = args_of(a, "start")) != NULL) {
        if (S.active) { snprintf(reply, cap, "-err log active"); return true; }
        if (!S.scanned || S.erasing) { snprintf(reply, cap, "-err log notready"); return true; }
        session_start();
        S.auto_done = true;
        snprintf(reply, cap, "-ok log start %lu", (unsigned long)S.session);
        return true;
    }
    if ((sub = args_of(a, "stop")) != NULL) {
        if (!S.active) { snprintf(reply, cap, "-err log inactive"); return true; }
        S.req = true;
        S.req_end = true;
        snprintf(reply, cap, "-ok log stop %lu", (unsigned long)S.session);
        return true;
    }
    if ((sub = args_of(a, "time")) != NULL) {
        unsigned long long t;
        const char *e;
        if (!parse_u(sub, &t, &e) || !rest_empty(e)) { snprintf(reply, cap, "-err log time"); return true; }
        S.time_set = true;
        S.time_unix = t;
        S.time_at_up = S.up_ms;
        if (S.active) S.unix_base = t - sess_ms() / 1000u;
        snprintf(reply, cap, "-ok log time %llu", t);
        return true;
    }
    if ((sub = args_of(a, "erase")) != NULL) {
        if (S.active || S.wst != W_IDLE) { snprintf(reply, cap, "-err log active"); return true; }
        if (S.listing || S.reading) { snprintf(reply, cap, "-err log busy"); return true; }
        S.erasing = true;
        S.erase_addr = LOG_START;
        snprintf(reply, cap, "-ok log erase");
        return true;
    }
    if ((sub = args_of(a, "list")) != NULL) {
        if (!S.scanned || S.erasing) { snprintf(reply, cap, "-err log notready"); return true; }
        if (S.listing || S.reading) { snprintf(reply, cap, "-err log busy"); return true; }
        S.listing = true;
        S.list_s = 0;
        S.list_n = 0;
        reply[0] = 0;                          /* ответ — строками из logger_poll */
        snprintf(reply, cap, "-ok log list begin");
        return true;
    }
    if ((sub = args_of(a, "read")) != NULL) {
        unsigned long long s;
        const char *e;
        if (!parse_u(sub, &s, &e) || !rest_empty(e) || s >= LOG_SECTORS) { snprintf(reply, cap, "-err log read"); return true; }
        if (!S.scanned || S.erasing) { snprintf(reply, cap, "-err log notready"); return true; }
        if (S.listing || S.reading) { snprintf(reply, cap, "-err log busy"); return true; }
        S.reading = true;
        S.rd_sector = (uint32_t)s;
        S.rd_off = 0;
        snprintf(reply, cap, "-ok log read begin %lu", (unsigned long)s);
        return true;
    }
    snprintf(reply, cap, "-err log");
    return true;
}
