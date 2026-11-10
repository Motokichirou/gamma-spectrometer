/* Тесты регистратора: logrec (формат, кодек), w25 + logger на модели NOR-флешки W25Q64.
   Модель разбирает SPI-транзакции как микросхема: стирание → 0xFF, программирование только 1→0,
   заворот адреса внутри страницы 256 Б, BUSY на заданное число опросов, команды во время BUSY и
   в power-down — нарушение. «Обрыв питания» — операция с номером cut исполняется наполовину,
   дальше флешка мертва до «перезапуска». */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "t.h"
#include "logrec.h"
#include "w25.h"
#include "logger.h"

/* ---------------- модель флешки ---------------- */
static uint8_t *mem;
static struct {
    bool sel, pd, wel, dead;
    int  busy_left;
    int  busy_polls;          /* сколько опросов RDSR флешка занята после операции */
    uint8_t cmd;
    uint32_t n, addr;
    uint8_t buf[4 + 256 + 16];
    uint32_t violations;
    uint32_t ops, cut;        /* cut — номер операции (0 — без обрыва), на которой «пропадает питание» */
    uint32_t bytes_poll, bytes_poll_max, ops_poll, ops_poll_max;
} F;

static void f_reset_bus(void) { F.sel = false; F.n = 0; }

static void op_done(void)
{
    F.ops++;
    F.ops_poll++;
    F.busy_left = F.busy_polls;
}

static void f_select(bool on)
{
    if (on) { F.sel = true; F.n = 0; F.cmd = 0; return; }
    if (!F.sel) { F.violations++; return; }
    F.sel = false;
    if (F.dead || F.n == 0) return;
    if (F.cmd == 0x02 && F.wel && F.n > 4) {                 /* page program */
        uint32_t a = F.addr, cnt = F.n - 4;
        bool cut = F.cut && F.ops + 1 == F.cut;
        if (cut) cnt /= 2;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t p = (a & ~0xFFu) | ((a + i) & 0xFFu);   /* заворот внутри страницы */
            mem[p] &= F.buf[4 + (i % 256)];
        }
        F.wel = false;
        if (cut) { F.dead = true; return; }
        op_done();
    } else if ((F.cmd == 0x20 || F.cmd == 0xD8) && F.wel && F.n == 4) {
        uint32_t sz = F.cmd == 0x20 ? 4096u : 65536u;
        uint32_t a = F.addr & ~(sz - 1u);
        bool cut = F.cut && F.ops + 1 == F.cut;
        memset(mem + a, 0xFF, cut ? sz / 2 : sz);            /* оборванное стирание — наполовину */
        F.wel = false;
        if (cut) { F.dead = true; return; }
        op_done();
    } else if (F.cmd == 0x06) {
        F.wel = true;
    } else if (F.cmd == 0xB9) {
        F.pd = true;
    } else if (F.cmd == 0xAB) {
        F.pd = false;
    }
}

static void f_xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    if (!F.sel) { F.violations++; return; }
    F.bytes_poll += (uint32_t)n;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = tx ? tx[i] : 0xFF, out = 0xFF;
        if (F.dead) { if (rx) rx[i] = 0xFF; continue; }
        if (F.n == 0) {
            F.cmd = b;
            if (F.pd && b != 0xAB) F.violations++;
            if (F.busy_left > 0 && b != 0x05) F.violations++;   /* во время BUSY — только RDSR */
        } else {
            switch (F.cmd) {
            case 0x05:
                out = F.busy_left > 0 ? 1u : 0u;
                if (F.busy_left > 0) F.busy_left--;
                break;
            case 0x9F: { static const uint8_t id[3] = { 0xEF, 0x40, 0x17 }; out = F.n <= 3 ? id[F.n - 1] : 0xFF; break; }
            case 0x03:
                if (F.n <= 3) F.addr = (F.addr << 8) | b;
                else out = mem[(F.addr + F.n - 4) & (W25_SIZE - 1u)];
                break;
            case 0x02: case 0x20: case 0xD8:
                if (F.n <= 3) F.addr = (F.addr << 8) | b;
                else if (F.cmd == 0x02 && F.n - 4 < 256) F.buf[4 + (F.n - 4)] = b;
                break;
            default: break;
            }
            if (F.n == 1) { if (F.cmd == 0x03 || F.cmd == 0x02 || F.cmd == 0x20 || F.cmd == 0xD8) F.addr = b; }
        }
        if (rx) rx[i] = out;
        F.n++;
    }
}

static const w25_port_t spi = { f_select, f_xfer, NULL };

/* ---------------- приложение ---------------- */
static uint32_t now_ms;
static uint32_t spec[LOGREC_CHANNELS];
static int acq_begins;
static bool acq_run;
static char texts[1 << 16];
static size_t texts_n;
/* сборка записи из кадров -log read */
static uint8_t rd_buf[LOGREC_HDR_PAGE + LOGREC_PAYLOAD_MAX];
static uint32_t rd_total, rd_got, rd_sector;
static uint32_t frames;

static uint32_t t_millis(void) { return now_ms; }
static const uint32_t *t_spec(void) { return spec; }
static void t_acq_begin(void) { acq_begins++; acq_run = true; memset(spec, 0, sizeof spec); }
static void t_snap(log_snap_t *s)
{
    memset(s, 0, sizeof *s);
    s->real_ms = now_ms; s->live_ms = now_ms; s->base = 640.0f; s->sigma = 3.0f;
    s->vref_mv = 2512.0f; s->vdd_mv = 3295.0f; s->tc_t0 = 25.0f; s->tc_gain = 1.0f; s->hv_set_v = 565;
    s->acq_running = acq_run;
}
static void t_text(const char *s)
{
    size_t n = strlen(s);
    if (texts_n + n < sizeof texts) { memcpy(texts + texts_n, s, n); texts_n += n; texts[texts_n] = 0; }
}
static void t_frame(uint8_t cmd, const uint8_t *p, size_t n)
{
    if (cmd != SHPROTO_CMD_LOG || n < 10) return;
    frames++;
    uint32_t sec = p[0] | (p[1] << 8);
    uint32_t off = p[2] | (p[3] << 8) | (p[4] << 16) | ((uint32_t)p[5] << 24);
    uint32_t tot = p[6] | (p[7] << 8) | (p[8] << 16) | ((uint32_t)p[9] << 24);
    rd_sector = sec;
    rd_total = tot;
    if (off + (n - 10) <= sizeof rd_buf) memcpy(rd_buf + off, p + 10, n - 10);
    rd_got += (uint32_t)(n - 10);
}
static const logger_port_t port = { &spi, t_millis, t_spec, t_acq_begin, t_snap, t_text, t_frame, NULL,
                                    "testtag", "GS474-TEST" };

static char rep[512];
static const char *cmd(const char *c)
{
    rep[0] = 0;
    if (!logger_handle(c, rep, sizeof rep)) strcpy(rep, "<not log>");
    return rep;
}
static bool starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

/* n проходов главного цикла; время идёт по dt_ms за проход */
static void run(int n, uint32_t dt_ms)
{
    for (int i = 0; i < n; i++) {
        F.bytes_poll = 0; F.ops_poll = 0;
        logger_poll(now_ms);
        if (F.bytes_poll > F.bytes_poll_max) F.bytes_poll_max = F.bytes_poll;
        if (F.ops_poll > F.ops_poll_max) F.ops_poll_max = F.ops_poll;
        now_ms += dt_ms;
    }
}

/* до конца текущей работы (запись/стирание/скан), но не больше limit проходов */
static void settle(int limit)
{
    for (int i = 0; i < limit; i++) {
        run(1, 1);
        if (strstr(cmd("-log"), "scan done") && !strstr(rep, "erasing")) {
            /* грубо: ещё немного, чтобы дописалась начатая запись */
        }
    }
}

static void tel_seconds(int n, float temp)
{
    log_tel_t t;
    memset(&t, 0, sizeof t);
    t.temp_c = temp; t.hv_out_v = 564.8f; t.hv_on = true; t.hv_set_v = 565; t.vref_ok = true;
    t.cps_in = 5000; t.cps_acc = 4800;
    for (int i = 0; i < n; i++) {
        logger_tick_1s(&t);
        run(40, 25);                           /* секунда = 40 проходов по 25 мс */
    }
}

static void boot(void)
{
    f_reset_bus();
    F.dead = false;
    F.pd = true;                               /* как после spiflash_sleep() */
    F.wel = false;
    F.busy_left = 0;
    acq_run = false;
    texts_n = 0; texts[0] = 0;
    logger_init(&port);
}

static unsigned long long kv_u(const char *line, const char *key)
{
    const char *p = strstr(line, key);
    if (!p) return 0xFFFFFFFFFFFFFFFFull;
    return strtoull(p + strlen(key), NULL, 10);
}

/* ---------------- кодек ---------------- */
static uint32_t rnd_s = 12345;
static uint32_t rnd(void) { rnd_s = rnd_s * 1664525u + 1013904223u; return rnd_s; }

static void test_codec(void)
{
    static uint32_t a[LOGREC_CHANNELS], b[LOGREC_CHANNELS];
    static uint8_t buf[LOGREC_PAYLOAD_MAX];
    for (int pass = 0; pass < 5; pass++) {
        for (uint32_t i = 0; i < LOGREC_CHANNELS; i++) {
            switch (pass) {
            case 0: a[i] = 0; break;
            case 1: a[i] = 0xFFFFFFFFu; break;
            case 2: a[i] = rnd(); break;
            case 3: a[i] = (uint32_t)(10000.0 * exp(-(double)(i - 1190) * (i - 1190) / (2.0 * 35 * 35)) + 300.0 * exp(-i / 800.0)); break;
            default: a[i] = (i & 1) ? 0xFFFFFFFFu : 0u; break;
            }
        }
        logrec_enc_t e;
        logrec_enc_init(&e);
        size_t n = 0;
        bool done = false;
        while (!done) n += logrec_enc_step(&e, a, buf + n, (pass & 1) ? 7 : 256, &done);   /* мелкие куски — разрыв varint */
        CHECK(n <= LOGREC_PAYLOAD_MAX);
        memset(b, 0xAA, sizeof b);
        CHECK(logrec_decode(buf, n, b, LOGREC_CHANNELS));
        CHECK(memcmp(a, b, sizeof a) == 0);
        uint64_t s = 0; for (uint32_t i = 0; i < LOGREC_CHANNELS; i++) s += a[i];
        CHECK(e.sum == s);
        CHECK(e.sat == (pass == 1 || pass == 4));
        if (pass == 0) CHECK(n == LOGREC_CHANNELS);                 /* нули — по байту */
        if (pass == 3) CHECK(n < 2u * LOGREC_CHANNELS);             /* гладкий спектр сжимается */
        CHECK(!logrec_decode(buf, n - 1, b, LOGREC_CHANNELS));      /* обрезанная — ошибка */
    }
    logrec_hdr_t h;
    memset(&h, 0, sizeof h);
    h.payload_len = 1234;
    logrec_hdr_seal(&h);
    CHECK(logrec_hdr_valid(&h));
    h.seq ^= 1;
    CHECK(!logrec_hdr_valid(&h));
    CHECK(logrec_crc32(0, "123456789", 9) == 0xCBF43926u);         /* эталон CRC-32 */
    CHECK(logrec_sectors(0, 4096) == 1 && logrec_sectors(4096 - 256, 4096) == 1 && logrec_sectors(4096 - 255, 4096) == 2);
}

/* ---------------- журнал ---------------- */
static void fill_spec(uint32_t k)
{
    for (uint32_t i = 0; i < LOGREC_CHANNELS; i++)
        spec[i] += (uint32_t)(k * 10.0 * exp(-(double)(i - 1190) * (i - 1190) / (2.0 * 35 * 35))) + (i < 3000 ? k : 0);
}

/* выгрузить запись по сектору, проверить CRC, раскодировать в out */
static bool read_record(uint32_t sector, logrec_hdr_t *h, uint32_t *out)
{
    char c[32];
    snprintf(c, sizeof c, "-log read %lu", (unsigned long)sector);
    rd_total = rd_got = 0;
    memset(rd_buf, 0, sizeof rd_buf);
    texts_n = 0; texts[0] = 0;
    if (!starts(cmd(c), "-ok log read begin")) return false;
    for (int i = 0; i < 2000 && !strstr(texts, "log read"); i++) run(1, 1);
    if (!strstr(texts, "-ok log read")) return false;
    memcpy(h, rd_buf, sizeof *h);
    if (!logrec_hdr_valid(h)) return false;
    if (rd_total != LOGREC_HDR_PAGE + h->payload_len) return false;
    const uint8_t *pl = rd_buf + LOGREC_HDR_PAGE;
    if (logrec_crc32(0, pl, h->payload_len) != h->payload_crc) return false;
    return logrec_decode(pl, h->payload_len, out, LOGREC_CHANNELS);
}

/* список записей: сектора по порядку; возвращает их число */
static int list_records(uint32_t *sectors, uint32_t *seqs, int cap)
{
    texts_n = 0; texts[0] = 0;
    if (!starts(cmd("-log list"), "-ok log list begin")) return -1;
    for (int i = 0; i < 20000 && !strstr(texts, "-ok log list"); i++) run(1, 1);
    int n = 0;
    for (const char *p = texts; (p = strstr(p, "-log rec ")) != NULL; p += 9) {
        unsigned long s, q;
        if (sscanf(p, "-log rec %lu %lu", &s, &q) == 2 && n < cap) { sectors[n] = (uint32_t)s; seqs[n] = (uint32_t)q; n++; }
    }
    return n;
}

static void test_basic_session(void)
{
    memset(mem, 0xFF, W25_SIZE);
    memset(&F, 0, sizeof F);
    F.busy_polls = 3;
    now_ms = 1000;
    boot();
    CHECK(starts(cmd("-log"), "-ok log off period 600 delay 120"));
    CHECK(strstr(rep, "id EF4017") != NULL);
    run(2000, 1);
    CHECK(strstr(cmd("-log"), "scan done") && kv_u(rep, "records ") == 0);
    CHECK(starts(cmd("-log start"), "-ok log start 1"));
    CHECK(acq_begins == 1);
    CHECK(starts(cmd("-log start"), "-err log active"));
    run(3000, 1);                              /* запись 0 */
    CHECK(kv_u(cmd("-log"), "records ") == 1);
    /* три интервала по 600 с с телеметрией: температура растёт */
    static uint32_t snap1[LOGREC_CHANNELS];
    for (int k = 1; k <= 3; k++) {
        fill_spec((uint32_t)k);
        logger_event(k == 2 ? LOGF_HOST_STO : 0);
        tel_seconds(600, 10.0f + k);
        run(3000, 1);
        if (k == 1) memcpy(snap1, spec, sizeof spec);
    }
    CHECK(kv_u(cmd("-log"), "records ") == 4);
    CHECK(starts(cmd("-log stop"), "-ok log stop 1"));
    run(3000, 1);
    CHECK(strstr(cmd("-log"), "active 0") != NULL);
    CHECK(kv_u(rep, "records ") == 5);

    uint32_t secs[16], seqs[16];
    int n = list_records(secs, seqs, 16);
    CHECK(n == 5);
    CHECK(strstr(texts, "-ok log list 5") != NULL);
    for (int i = 0; i < n; i++) CHECK(seqs[i] == (uint32_t)(i + 1));
    static uint32_t out[LOGREC_CHANNELS];
    logrec_hdr_t h;
    CHECK(read_record(secs[0], &h, out));
    CHECK((h.flags & LOGF_SESSION_START) && h.rec_in_session == 0 && h.total == 0);
    CHECK(read_record(secs[1], &h, out));
    CHECK(memcmp(out, snap1, sizeof out) == 0);              /* накопленный спектр как был */
    CHECK(h.rec_in_session == 1 && h.session == 1);
    CHECK(h.t_start_ms < 10 && h.t_end_ms >= 600000u && h.t_end_ms < 610000u);
    CHECK(h.t_avg_c100 == 1100 && h.t_start_c100 == 1100 && h.t_end_c100 == 1100);
    CHECK(h.sec_count >= 590 && h.sec_count <= 600);   /* граница периода — внутри секунд телеметрии */
    CHECK_NEAR(h.cps_in, 5000.0, 1e-3);
    CHECK_NEAR(h.hv_avg_v, 564.8, 1e-3);
    CHECK(h.hv_set_v == 565 && strcmp(h.fw_tag, "testtag") == 0 && strcmp(h.serial, "GS474-TEST") == 0);
    CHECK(read_record(secs[2], &h, out));
    CHECK(h.flags & LOGF_HOST_STO);
    CHECK(read_record(secs[4], &h, out));
    CHECK(h.flags & LOGF_SESSION_END);
    CHECK(memcmp(out, spec, sizeof out) == 0);
    CHECK(F.violations == 0);
    CHECK(F.ops_poll_max <= 1);                 /* не больше одной операции флешки за проход */
    CHECK(F.bytes_poll_max <= 1200);            /* ≤ ~0.5 мс SPI при 19.5 МГц */
    printf("  байт SPI за проход, макс.: %u\n", (unsigned)F.bytes_poll_max);

    /* после перезапуска голова находится, новая сессия — 2, номера продолжаются */
    now_ms += 5000;
    boot();
    run(3000, 1);
    CHECK(kv_u(cmd("-log"), "records ") == 5);
    CHECK(starts(cmd("-log start"), "-ok log start 2"));
    run(3000, 1);
    n = list_records(secs, seqs, 16);
    CHECK(n == 6 && seqs[5] == 6);
    CHECK(F.violations == 0);
}

static void test_config_autostart(void)
{
    memset(mem, 0xFF, W25_SIZE);
    memset(&F, 0, sizeof F);
    F.busy_polls = 2;
    now_ms = 0;
    boot();
    run(3000, 1);
    CHECK(starts(cmd("-log on 120 30"), "-ok log on 120 30"));
    CHECK(starts(cmd("-log on 10"), "-err log on"));          /* период < 60 */
    CHECK(starts(cmd("-log on 120 99999"), "-err log on"));
    run(100, 1);
    /* перезапуск: конфиг из флешки, автозапуск через 30 с */
    acq_begins = 0;
    now_ms = 0;
    boot();
    CHECK(starts(cmd("-log"), "-ok log on period 120 delay 30"));
    run(1000, 20);                             /* 20 с */
    CHECK(acq_begins == 0 && strstr(cmd("-log"), "active 0"));
    run(1000, 20);                             /* 40 с */
    CHECK(acq_begins == 1 && strstr(cmd("-log"), "active 1"));
    /* порча действующей копии — откат к предыдущему поколению (enabled 0 → «off») */
    CHECK(starts(cmd("-log off"), "-ok log off"));
    run(200, 1);
    boot();
    CHECK(starts(cmd("-log"), "-ok log off"));
    /* испортить копию с наибольшим поколением: она в A или B — ищем ту, где enabled == 0 */
    for (uint32_t a = LOG_CFG_A; a <= LOG_CFG_B; a += LOG_CFG_B) {
        uint32_t en; memcpy(&en, mem + a + 8, 4);
        if (en == 0 && mem[a] != 0xFF) mem[a + 12] ^= 0x01;    /* period_s — CRC не сойдётся */
    }
    boot();
    CHECK(starts(cmd("-log"), "-ok log on period 120 delay 30"));
    CHECK(F.violations == 0);
}

static void test_ring(void)
{
    memset(mem, 0xFF, W25_SIZE);
    memset(&F, 0, sizeof F);
    F.busy_polls = 1;
    now_ms = 0;
    boot();
    run(3000, 1);
    CHECK(starts(cmd("-log start"), "-ok log start"));
    run(3000, 1);                              /* запись 0 */
    /* «шумный» спектр: ~5 байт на канал ⇒ ~11 секторов на запись ⇒ заворот после ~185 записей */
    int recs = 0;
    for (int k = 0; k < 260; k++) {
        for (uint32_t i = 0; i < LOGREC_CHANNELS; i++) spec[i] = rnd() | 0x80000000u;
        now_ms += 600000;
        run(6000, 0);
        recs++;
    }
    cmd("-log");
    CHECK(strstr(rep, "wrap 1") != NULL);
    unsigned long long nrec = kv_u(rep, "records ");
    printf("  кольцо: записей %llu (%s)\n", nrec, rep);
    CHECK(nrec > 170 && nrec < 230);           /* 2032 сектора по ~10 на запись */
    /* перезапуск: голова — последняя запись, номера подряд */
    boot();
    run(4000, 1);
    CHECK(kv_u(cmd("-log"), "records ") == nrec);
    static uint32_t secs[400], seqs[400];
    int n = list_records(secs, seqs, 400);
    CHECK(n == (int)nrec);
    uint32_t mx = 0, mn = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) { if (seqs[i] > mx) mx = seqs[i]; if (seqs[i] < mn) mn = seqs[i]; }
    printf("  кольцо: seq %u…%u, n %d, записано %d\n", (unsigned)mn, (unsigned)mx, n, recs + 1);
    CHECK(mx == (uint32_t)recs + 1u);          /* +1 — запись 0 сессии */
    CHECK(mx - mn + 1u == (uint32_t)n);        /* без дыр */
    static uint32_t out[LOGREC_CHANNELS];
    logrec_hdr_t h;
    int last = 0;
    for (int i = 0; i < n; i++) if (seqs[i] == mx) last = i;
    CHECK(read_record(secs[last], &h, out));
    CHECK(memcmp(out, spec, sizeof out) == 0);
    CHECK(F.violations == 0);
}

static void test_power_loss(void)
{
    /* запись прерывается на каждой из первых N операций очередной записи; после «перезапуска»
       предыдущие записи целы, недописанная отброшена, следующая пишется */
    int ok_cases = 0;
    for (uint32_t cut = 1; cut <= 40; cut += 3) {
        memset(mem, 0xFF, W25_SIZE);
        memset(&F, 0, sizeof F);
        F.busy_polls = 1;
        now_ms = 0;
        boot();
        run(3000, 1);
        cmd("-log start");
        run(3000, 1);                          /* запись 0 */
        fill_spec(5);
        now_ms += 600000;
        run(3000, 1);                          /* запись 1 */
        CHECK(kv_u(cmd("-log"), "records ") == 2);
        fill_spec(7);
        F.cut = F.ops + cut;
        now_ms += 600000;
        run(3000, 1);                          /* запись 2 — обрыв */
        if (!F.dead) break;                    /* обрыв позже конца записи — случаи кончились */
        F.cut = 0;
        boot();
        run(4000, 1);
        unsigned long long r = kv_u(cmd("-log"), "records ");
        CHECK(r == 2);
        static uint32_t secs[8], seqs[8];
        int n = list_records(secs, seqs, 8);
        CHECK(n == 2);
        static uint32_t out[LOGREC_CHANNELS];
        logrec_hdr_t h;
        bool good = n == 2 && read_record(secs[1], &h, out) && h.seq == 2;
        CHECK(good);
        cmd("-log start");
        run(4000, 1);
        n = list_records(secs, seqs, 8);
        CHECK(n == 3);
        if (good && n == 3) ok_cases++;
        CHECK(F.violations == 0);
    }
    printf("  обрывов питания проверено: %d\n", ok_cases);
}

static void test_erase(void)
{
    memset(mem, 0xFF, W25_SIZE);
    memset(&F, 0, sizeof F);
    F.busy_polls = 2;
    now_ms = 0;
    boot();
    run(3000, 1);
    cmd("-log start");
    run(3000, 1);
    CHECK(starts(cmd("-log erase"), "-err log active"));
    cmd("-log stop");
    run(3000, 1);
    CHECK(kv_u(cmd("-log"), "records ") == 2);
    CHECK(starts(cmd("-log erase"), "-ok log erase"));
    run(2000, 1);
    CHECK(kv_u(cmd("-log"), "records ") == 0);
    boot();
    run(4000, 1);
    CHECK(kv_u(cmd("-log"), "records ") == 0);
    CHECK(starts(cmd("-log read 0"), "-ok log read begin"));
    texts_n = 0; texts[0] = 0;
    run(100, 1);
    CHECK(strstr(texts, "-err log read") != NULL);
    CHECK(starts(cmd("-log bogus"), "-err log"));
    CHECK(starts(cmd("-logx"), "<not log>"));
    CHECK(F.violations == 0);
}

static void test_sleep(void)
{
    memset(mem, 0xFF, W25_SIZE);
    memset(&F, 0, sizeof F);
    now_ms = 0;
    boot();
    run(3000, 1);
    CHECK(F.pd);                               /* регистратор выключен — флешка спит */
    cmd("-log");
    CHECK(strstr(rep, "records 0") != NULL);
    CHECK(starts(cmd("-log on 60 0"), "-ok log on"));
    run(500, 1);
    CHECK(!F.pd);
    CHECK(F.violations == 0);
}

int main(void)
{
    mem = (uint8_t *)malloc(W25_SIZE);
    if (!mem) return 1;
    test_codec();
    test_basic_session();
    test_config_autostart();
    test_ring();
    test_power_loss();
    test_erase();
    test_sleep();
    free(mem);
    (void)settle;
    return T_DONE();
}
