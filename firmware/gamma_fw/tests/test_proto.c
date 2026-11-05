/* Тесты proto: команды BecqMoni и сервисные, отложенная запись, !boot, пакеты 0x01/0x02/0x04. */
#define _CRT_SECURE_NO_WARNINGS
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "t.h"
#include "proto.h"
#include "shproto.h"
#include "settings.h"
#include "becq_cal.h"
#include "spectrum.h"

/* ---- порт-заглушки ---- */
static uint8_t  txb[200000];
static size_t   txn;
static uint32_t now_ms;
static int      n_commit, n_start, n_stop, n_clear, n_boot, n_service;
static uint32_t applied;
static settings_t committed, live;
static bool     commit_ok = true;
static char     last_service[300];
static proto_status_t stat_stub;

static void p_tx(const uint8_t *p, size_t n)
{
    if (txn + n <= sizeof txb) { memcpy(txb + txn, p, n); txn += n; }
}
static uint32_t p_millis(void) { return now_ms; }
static bool p_commit(const settings_t *s)
{
    n_commit++;
    committed = *s;
    return commit_ok;
}
static void p_start(void) { n_start++; }
static void p_stop(void) { n_stop++; }
static void p_clear(void) { n_clear++; }
static void p_boot(void) { n_boot++; }
static void p_service(const char *cmd, char *reply, size_t cap)
{
    n_service++;
    snprintf(last_service, sizeof last_service, "%s", cmd);
    if (strncmp(cmd, "-hv", 3) == 0) snprintf(reply, cap, "-ok hv");
    /* остальное — пустой ответ = не знаю */
}
static void p_status(proto_status_t *st) { *st = stat_stub; }
static void p_apply(uint32_t what) { applied |= what; }
static size_t tx_room;
static size_t p_txfree(void) { return tx_room; }

static const proto_port_t port = {
    p_tx, p_millis, p_commit, p_start, p_stop, p_clear, p_boot, p_service, p_status, p_apply, p_txfree
};

static void reset(void)
{
    txn = 0; now_ms = 0;
    n_commit = n_start = n_stop = n_clear = n_boot = n_service = 0;
    applied = 0; commit_ok = true; last_service[0] = 0;
    tx_room = 100000;
    memset(&committed, 0, sizeof committed);
    memset(&stat_stub, 0, sizeof stat_stub);
    settings_defaults(&live);
    spectrum_clear();
    proto_init(&port, &live);
}

/* ---- разбор вывода: свой декодер (кадры 0x01 длиннее SHPROTO_RX_MAX_PAYLOAD) ---- */
typedef struct { uint8_t cmd; size_t len; uint8_t pl[4200]; } frame_t;
static frame_t fr[64];
static int nfr;

static void decode(void)
{
    nfr = 0;
    size_t i = 0;
    while (i < txn && nfr < 64) {
        if (txb[i] != SHPROTO_START) { i++; continue; }
        uint8_t raw[4300]; size_t rn = 0; int esc = 0; bool done = false;
        for (i++; i < txn; i++) {
            uint8_t b = txb[i];
            if (b == SHPROTO_START) break;
            if (b == SHPROTO_FINISH) { done = true; i++; break; }
            if (b == SHPROTO_ESC) { esc = 1; continue; }
            if (esc) { b = (uint8_t)~b; esc = 0; }
            if (rn < sizeof raw) raw[rn++] = b;
        }
        if (!done || rn < 3) continue;
        uint16_t crc = (uint16_t)(raw[rn - 2] | (raw[rn - 1] << 8));
        if (shproto_crc16(raw, rn - 2) != crc) { printf("  bad crc\n"); continue; }
        fr[nfr].cmd = raw[0];
        fr[nfr].len = rn - 3;
        memcpy(fr[nfr].pl, raw + 1, rn - 3);
        fr[nfr].pl[rn - 3] = 0;
        nfr++;
    }
}

/* отправить команду текстовым кадром, вернуть ответ (последний текстовый кадр) или NULL */
static const char *cmd(const char *s)
{
    uint8_t f[600];
    size_t n = shproto_build(SHPROTO_CMD_TEXT, (const uint8_t *)s, strlen(s), f);
    txn = 0;
    proto_rx(f, n);
    decode();
    for (int k = nfr - 1; k >= 0; k--)
        if (fr[k].cmd == SHPROTO_CMD_TEXT) return (const char *)fr[k].pl;
    return NULL;
}

static bool eq(const char *a, const char *b) { return a && strcmp(a, b) == 0; }

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* ---- тесты ---- */
static void test_sta_sto_rst(void)
{
    reset();
    CHECK(eq(cmd("-sta"), "-ok\r\n"));
    CHECK(n_start == 1 && proto_acquiring());
    CHECK(eq(cmd("-stt"), "-ok collecting\r\n"));
    CHECK(eq(cmd("-sta"), "-ok\r\n"));          /* повтор — набор не перезапускается */
    CHECK(n_start == 1);
    CHECK(eq(cmd("-rst"), "-ok\r\n"));
    CHECK(n_clear == 1);
    CHECK(eq(cmd("-sto"), "-ok\r\n"));
    CHECK(n_stop == 1 && !proto_acquiring());
    CHECK(eq(cmd("-stt"), "-ok\r\n"));
    CHECK(eq(cmd("-mode 0"), "-ok\r\n"));
    CHECK(eq(cmd("-sho"), "-ok collecting\r\n"));
    CHECK(eq(cmd("-sta\r\n"), "-ok\r\n"));      /* хвостовые \r\n не мешают */
    CHECK(n_start == 2);
    CHECK(n_commit == 0);
}

static void test_inf_both_parsers(void)
{
    reset();
    proto_set_temperature(23.46f);
    const char *r = cmd("-inf");
    CHECK(r != NULL);
    if (!r) return;
    char b[200]; snprintf(b, sizeof b, "%s", r);
    char *tok[32]; int nt = 0;
    for (char *t = strtok(b, " \r\n\t"); t && nt < 32; t = strtok(NULL, " \r\n\t")) tok[nt++] = t;
    CHECK(nt >= 12);
    if (nt < 12) return;
    /* старый BecqMoni: позиции 3, 5, 9 — числа */
    CHECK(atoi(tok[3]) == (int)PROTO_INF_RISE);
    CHECK(atoi(tok[5]) == (int)PROTO_INF_FALL);
    CHECK(strtoul(tok[9], NULL, 10) == PROTO_INF_FS);
    /* новый: пары по ключам */
    CHECK(strcmp(tok[2], "RISE") == 0 && strcmp(tok[4], "FALL") == 0 && strcmp(tok[8], "F") == 0);
    CHECK(strcmp(tok[6], "T1") == 0 && strcmp(tok[7], "23.5") == 0);
    CHECK(strcmp(tok[10], "VERSION") == 0 && atoi(tok[11]) == 100);
    proto_set_temperature(-0.26f);
    r = cmd("-inf");
    CHECK(r && strstr(r, "T1 -0.3 ") != NULL);
}

static int split_lines(char *buf, char **ln, int max)
{
    int n = 0; char *p = buf;
    while (*p && n < max) {
        ln[n++] = p;
        char *e = strstr(p, "\r\n");
        if (!e) break;
        *e = 0; p = e + 2;
    }
    return n;
}

static void test_cal_read_40_regs(void)
{
    reset();
    const char *r = cmd("-cal");
    CHECK(r != NULL);
    if (!r) return;
    char b[600]; snprintf(b, sizeof b, "%s", r);
    char *ln[64];
    CHECK(split_lines(b, ln, 64) == 40);
    CHECK(strcmp(ln[39], PROTO_SERIAL) == 0);
    CHECK(strcmp(ln[0], "00000000") == 0 && strcmp(ln[10], "00000000") == 0);

    const double c[5] = { 1.5, 0.3456, 1e-6, 0, 0 };
    becq_coef_to_words(2, c, live.becq_words);
    live.becq_valid = 1;
    r = cmd("-cal");
    snprintf(b, sizeof b, "%s", r ? r : "");
    CHECK(split_lines(b, ln, 64) == 40);
    CHECK(strtoul(ln[0], 0, 16) == live.becq_words[0]);
    CHECK(strtoul(ln[10], 0, 16) == live.becq_words[10]);
    char exp[16]; snprintf(exp, sizeof exp, "%08lX", (unsigned long)live.becq_words[2]);
    CHECK(eq(cmd("-cal 2"), exp));
    CHECK(eq(cmd("-cal 11 0"), "-err cal\r\n"));
}

static void test_cal_parse_strict(void)
{
    reset();
    CHECK(eq(cmd("-cal 1abc"), "-err cal\r\n"));         /* не слово 1 со значением ABC */
    CHECK(eq(cmd("-cal 1x"), "-err cal\r\n"));
    CHECK(eq(cmd("-cal 1 1FFFFFFFF"), "-err cal\r\n"));   /* 9 цифр — переполнение */
    CHECK(eq(cmd("-cal 1 -1"), "-err cal\r\n"));
    CHECK(eq(cmd("-cal 1 0x12"), "-err cal\r\n"));
    CHECK(eq(cmd("-cal 1 12 34"), "-err cal\r\n"));
    CHECK(eq(cmd("-cal 1 12g"), "-err cal\r\n"));
    /* ни одно из них не засчитано: после 11 верных слов — ровно одна запись */
    const double c[5] = { 0.25, 0.5, 0, 0, 0 };
    uint32_t w[11]; becq_coef_to_words(1, c, w);
    char s[40];
    for (int i = 0; i < 11; i++) {
        snprintf(s, sizeof s, "-cal\t%d\tffffffff", i);   /* табуляции, нижний регистр, 8 цифр */
        CHECK(eq(cmd(s), "-ok\r\n") || i == 10);
    }
    CHECK(live.becq_valid == 0);                         /* сумма не сошлась */
    for (int i = 0; i < 11; i++) {
        snprintf(s, sizeof s, "-cal %d %lx", i, (unsigned long)w[i]);
        CHECK(eq(cmd(s), "-ok\r\n"));
    }
    CHECK(live.becq_valid == 1 && memcmp(live.becq_words, w, sizeof w) == 0 && n_commit == 1);
}

static void test_poll_stepwise(void)
{
    reset();
    spectrum_add(3); spectrum_add(1500);
    cmd("-sta");
    stat_stub.elapsed_s = 7;
    txn = 0;
    proto_poll();                                /* без тика — тишина */
    CHECK(txn == 0);
    proto_tick_1s();
    CHECK(txn == 0);                             /* тик сам ничего не шлёт */

    tx_room = PROTO_TX_FRAME_MAX - 1;            /* места мало — ждём */
    proto_poll();
    CHECK(txn == 0);

    tx_room = 5000;
    proto_poll();
    decode();
    CHECK(nfr == 1 && fr[0].cmd == SHPROTO_CMD_HISTOGRAM && rd16(fr[0].pl) == 0);
    CHECK(txn <= PROTO_TX_FRAME_MAX);

    proto_tick_1s();                             /* выдача идёт — повторный тик не перезапускает */
    for (int k = 1; k < 17; k++) proto_poll();
    decode();
    CHECK(nfr == 17);
    for (int k = 0; k < 16 && k < nfr; k++)
        CHECK(fr[k].cmd == SHPROTO_CMD_HISTOGRAM && rd16(fr[k].pl) == k * PROTO_CHUNK_CH);
    CHECK(nfr == 17 && fr[16].cmd == SHPROTO_CMD_STAT && rd32(fr[16].pl) == 7);
    CHECK(rd32(fr[0].pl + 2 + 4 * 3) == 1 && rd32(fr[2].pl + 2 + 4 * (1500 - 1024)) == 1);

    size_t was = txn;                            /* выдача закончена: дальше тишина */
    for (int k = 0; k < 5; k++) proto_poll();
    CHECK(txn == was);

    /* почти худший кадр (3 из 4 байт каналов — со стаффингом; старший байт ≤ 0x07 по маске)
       влезает в PROTO_TX_FRAME_MAX: 2 + 3 + 2050 + 1536 = 3591 байт */
    for (uint32_t i = 0; i < SPECTRUM_CHANNELS; i++)
        ((uint32_t *)spectrum_data())[i] = 0x05FEFEFEu;  /* все байты кроме старшего — FE */
    txn = 0;
    proto_tick_1s();
    proto_poll();
    CHECK(txn >= 3591 && txn <= PROTO_TX_FRAME_MAX);

    /* без tx_free (NULL) — тоже по кадру за вызов */
    static proto_port_t port2;
    port2 = port; port2.tx_free = NULL;
    settings_defaults(&live); spectrum_clear();
    proto_init(&port2, &live);
    cmd("-sta");
    txn = 0;
    proto_tick_1s();
    proto_poll(); proto_poll();
    decode();
    CHECK(nfr == 2);
}

static void test_acq_stopped_commits(void)
{
    reset();
    CHECK(eq(cmd("-sta"), "-ok\r\n"));
    CHECK(eq(cmd("-thr 33"), "-ok thr 33\r\n"));
    CHECK(eq(cmd("-save"), "-ok deferred\r\n"));
    CHECK(n_commit == 0);
    proto_acq_stopped();                         /* набор остановлен не по -sto (сервис, авария) */
    CHECK(!proto_acquiring());
    CHECK(n_commit == 1 && committed.thr_codes == 33);
    CHECK(strcmp(proto_flash_status(), "ok") == 0);
    proto_acq_stopped();                         /* повтор безопасен */
    CHECK(n_commit == 1);
    CHECK(eq(cmd("-save"), "-ok saved\r\n"));    /* не "deferred" */
    CHECK(n_commit == 2);
    proto_tick_1s();                             /* не набираем — выдачи нет */
    txn = 0; proto_poll();
    CHECK(txn == 0);
}

static void send_cal_words(const uint32_t w[11])
{
    char s[40];
    for (int i = 0; i < 11; i++) {
        snprintf(s, sizeof s, "-cal %d %08lX", i, (unsigned long)w[i]);
        CHECK(eq(cmd(s), "-ok\r\n"));
    }
}

static void test_cal_write_sequence(void)
{
    reset();
    const double c[5] = { -3.25, 0.51234, 2e-6, -1e-10, 0 };
    uint32_t w[11]; becq_coef_to_words(3, c, w);
    send_cal_words(w);
    CHECK(live.becq_valid == 1);
    CHECK(memcmp(live.becq_words, w, sizeof w) == 0);
    CHECK(n_commit == 1);
    CHECK(settings_valid(&committed));
    CHECK(committed.becq_valid == 1 && memcmp(committed.becq_words, w, sizeof w) == 0);
    CHECK(strcmp(proto_flash_status(), "ok") == 0);

    /* битая сумма: 11-е слово → -err, в настройки не попадает */
    reset();
    w[10] ^= 1;
    char s[40];
    for (int i = 0; i < 10; i++) {
        snprintf(s, sizeof s, "-cal %d %08lX", i, (unsigned long)w[i]);
        CHECK(eq(cmd(s), "-ok\r\n"));
    }
    snprintf(s, sizeof s, "-cal 10 %08lX", (unsigned long)w[10]);
    CHECK(eq(cmd(s), "-err cal\r\n"));
    CHECK(live.becq_valid == 0 && n_commit == 0);
}

static void test_cal_write_during_acq_deferred(void)
{
    reset();
    const double c[5] = { 0.5, 0.4, 0, 0, 0 };
    uint32_t w[11]; becq_coef_to_words(1, c, w);
    CHECK(eq(cmd("-sta"), "-ok\r\n"));
    send_cal_words(w);
    CHECK(n_commit == 0);
    CHECK(live.becq_valid == 1);                 /* в ОЗУ — сразу (-cal читает новое) */
    CHECK(strcmp(proto_flash_status(), "pending") == 0);
    send_cal_words(w);                           /* повтор во время набора — тоже без записи */
    CHECK(n_commit == 0);
    CHECK(n_stop == 0);                          /* набор не прерван */
    CHECK(eq(cmd("-sto"), "-ok\r\n"));
    CHECK(n_commit == 1);
    CHECK(settings_valid(&committed) && memcmp(committed.becq_words, w, sizeof w) == 0);
    CHECK(eq(cmd("-sto"), "-ok\r\n"));
    CHECK(n_commit == 1);                        /* ровно один раз */
    CHECK(strcmp(proto_flash_status(), "ok") == 0);
}

static void test_save_during_acq(void)
{
    reset();
    CHECK(eq(cmd("-save"), "-ok saved\r\n"));
    CHECK(n_commit == 1 && settings_valid(&committed));
    commit_ok = false;
    CHECK(eq(cmd("-save"), "-err flash\r\n"));
    CHECK(strcmp(proto_flash_status(), "err") == 0);
    commit_ok = true;

    reset();
    CHECK(eq(cmd("-sta"), "-ok\r\n"));
    CHECK(eq(cmd("-thr 12"), "-ok thr 12\r\n"));
    CHECK(eq(cmd("-save"), "-ok deferred\r\n"));
    CHECK(eq(cmd("-save"), "-ok deferred\r\n"));
    CHECK(n_commit == 0 && n_stop == 0);
    CHECK(eq(cmd("-sto"), "-ok\r\n"));
    CHECK(n_commit == 1 && committed.thr_codes == 12 && settings_valid(&committed));
}

static void test_boot_substring(void)
{
    reset();
    const uint8_t a[] = "xx!boot";
    proto_rx(a, 7);
    CHECK(n_boot == 1);

    reset();
    proto_rx((const uint8_t *)"!bo", 3);
    CHECK(n_boot == 0);
    proto_rx((const uint8_t *)"ot", 2);
    CHECK(n_boot == 1);

    reset();                                     /* !!boot, мусор, 0xFF вокруг */
    const uint8_t b[] = { 0xFF, '!', '!', 'b', 'o', 'o', 't', 0xFE, 0xA5 };
    proto_rx(b, sizeof b);
    CHECK(n_boot == 1);

    reset();                                     /* внутри текстового кадра — тоже */
    cmd("!boot");
    CHECK(n_boot == 1);

    reset();                                     /* без !boot — не вызывается */
    const char *junk = "!bo!ot boot !boo t -sta -inf";
    proto_rx((const uint8_t *)junk, strlen(junk));
    cmd("-sta"); cmd("-inf"); cmd("-cal");
    CHECK(n_boot == 0);
}

static void test_status_payload(void)
{
    reset();
    proto_tick_1s();                             /* не набираем — тишина */
    for (int k = 0; k < 20; k++) proto_poll();
    CHECK(txn == 0);
    cmd("-sta");
    stat_stub.elapsed_s = 125; stat_stub.cpu_x10 = 371; stat_stub.cps = 29876; stat_stub.invalid = 44;
    txn = 0;
    proto_tick_1s();
    for (int k = 0; k < 17; k++) proto_poll();
    decode();
    CHECK(nfr == 17);
    if (nfr < 1) return;
    frame_t *f = &fr[nfr - 1];                   /* статус — последним */
    CHECK(f->cmd == SHPROTO_CMD_STAT);
    CHECK(f->len == 14);
    CHECK(rd32(f->pl) == 125);                   /* секунды */
    CHECK(rd16(f->pl + 4) == 371);
    CHECK(rd32(f->pl + 6) == 29876);
    CHECK(rd32(f->pl + 10) == 44);
}

static void test_spectrum_chunks(void)
{
    reset();
    spectrum_add(0); spectrum_add(0); spectrum_add(513); spectrum_add(8191);
    ((uint32_t *)spectrum_data())[7000] = 0xFFFFFFFFu;   /* проверка маски 27 бит */
    cmd("-sta");
    txn = 0;
    proto_tick_1s();
    for (int k = 0; k < 17; k++) proto_poll();
    decode();
    CHECK(nfr == 17);
    int nh = 0;
    for (int k = 0; k < nfr && k < 16; k++) {
        frame_t *f = &fr[k];
        CHECK(f->cmd == SHPROTO_CMD_HISTOGRAM);
        CHECK(f->len == 2 + 4 * PROTO_CHUNK_CH);
        uint16_t off = rd16(f->pl);
        CHECK(off == k * PROTO_CHUNK_CH);
        for (unsigned i = 0; i < PROTO_CHUNK_CH; i++) {
            uint32_t v = rd32(f->pl + 2 + 4 * i);
            CHECK(v == (spectrum_get((uint16_t)(off + i)) & 0x7FFFFFFu));
            if (v > 0x7FFFFFFu) CHECK(0);
        }
        nh++;
    }
    CHECK(nh == 16);
    CHECK(rd32(fr[0].pl + 2) == 2);
    CHECK(rd32(fr[1].pl + 2 + 4) == 1);
    CHECK(rd32(fr[15].pl + 2 + 4 * 511) == 1);
    CHECK(rd32(fr[13].pl + 2 + 4 * (7000 - 13 * 512)) == 0x7FFFFFFu);
}

static void test_unknown_cmd(void)
{
    reset();
    CHECK(cmd("hello") == NULL);                 /* не команда — тишина */
    CHECK(txn == 0);
    uint8_t f[32];                               /* нетекстовый кадр — тишина */
    size_t n = shproto_build(0x05, (const uint8_t *)"-sta", 4, f);
    txn = 0; proto_rx(f, n);
    CHECK(txn == 0 && n_start == 0);
    CHECK(eq(cmd("-xyz"), "-err unknown\r\n"));
    CHECK(n_service == 1 && strcmp(last_service, "-xyz") == 0);
    CHECK(eq(cmd("-hv 500"), "-ok hv\r\n"));     /* сервисная — в port->service, \r\n дописан */
    CHECK(strcmp(last_service, "-hv 500") == 0);
    CHECK(eq(cmd("-cals"), "-err unknown\r\n")); /* токен целиком, не префикс */
}

static void test_thr_tc(void)
{
    reset();
    CHECK(eq(cmd("-thr"), "-ok thr 0\r\n"));
    CHECK(eq(cmd("-thr 9"), "-ok thr 9\r\n"));
    CHECK(live.thr_codes == 9 && (applied & PROTO_APPLY_THR));
    CHECK(eq(cmd("-thr 5000"), "-err thr\r\n"));
    CHECK(eq(cmd("-thr abc"), "-err thr\r\n"));
    CHECK(live.thr_codes == 9);
    applied = 0;
    CHECK(eq(cmd("-tc 25 -0.002 0.00001"), "-ok tc 25.000 -2.000000e-3 1.000000e-5\r\n"));
    CHECK_NEAR(live.tc_k1, -0.002, 1e-9);
    CHECK_NEAR(live.tc_k2, 0.00001, 1e-10);
    CHECK(applied & PROTO_APPLY_TC);
    CHECK(eq(cmd("-tc"), "-ok tc 25.000 -2.000000e-3 1.000000e-5\r\n"));
    CHECK(eq(cmd("-tc 25 1"), "-err tc\r\n"));
    /* малые k2 читаются назад без потерь (раньше 6 знаков после точки давали 0) */
    CHECK(eq(cmd("-tc -5 0 3.21e-7"), "-ok tc -5.000 0 3.210000e-7\r\n"));
    CHECK(eq(cmd("-tc 0 123.4567 -9.9999999e-12"), "-ok tc 0.000 1.234567e2 -1.000000e-11\r\n"));
    const char *r = cmd("-tc 20 1.234567e-6 -7.654321e-9");
    float back[3] = { 0, 0, 0 };
    CHECK(r && sscanf(r, "-ok tc %f %f %f", &back[0], &back[1], &back[2]) == 3);
    CHECK(back[1] == live.tc_k1 && back[2] == live.tc_k2);  /* 7 значащих цифр — float точно */
    CHECK(n_commit == 0);                        /* без -save во флеш не пишется */
}

static void test_template_staging(void)
{
    reset();
    char s[120];
    CHECK(eq(cmd("-tplok"), "-err tpl 0/64\r\n"));
    for (int ph = 0; ph < 64; ph++) {
        snprintf(s, sizeof s, "-tplw %d %d %d %d %d %d %d %d", ph,
                 ph, 16384, -ph, 3, -4, 5, -32768);
        CHECK(eq(cmd(s), "-ok\r\n"));
        CHECK(live.tpl_valid == 0 && live.tpl[ph][1] == 0);   /* живая таблица не тронута */
    }
    CHECK(eq(cmd("-tplw 64 1 2 3 4 5 6 7"), "-err tplw\r\n"));
    CHECK(eq(cmd("-tplw 1 1 2 3"), "-err tplw\r\n"));
    CHECK(eq(cmd("-tplw 1 1 2 3 4 5 6 40000"), "-err tplw\r\n"));
    CHECK(applied == 0);
    CHECK(eq(cmd("-tplok"), "-ok tpl\r\n"));
    CHECK(live.tpl_valid == 1 && (applied & PROTO_APPLY_TPL));
    CHECK(live.tpl[10][0] == 10 && live.tpl[10][1] == 16384 && live.tpl[10][2] == -10);
    CHECK(live.tpl[63][6] == -32768);
    CHECK(eq(cmd("-tplok"), "-err tpl 0/64\r\n"));            /* буфер принят и очищен */
    applied = 0;
    CHECK(eq(cmd("-tplclr"), "-ok tpl builtin\r\n"));
    CHECK(live.tpl_valid == 0 && (applied & PROTO_APPLY_TPL));
}

static void test_defaults(void)
{
    reset();
    cmd("-thr 20");
    live.becq_valid = 1;
    applied = 0;
    CHECK(eq(cmd("-defaults"), "-ok defaults\r\n"));
    CHECK(live.thr_codes == 0 && live.becq_valid == 0 && settings_valid(&live));
    CHECK(applied == PROTO_APPLY_ALL);
    CHECK(n_commit == 0);
}

static void test_pulse_frames(void)
{
    reset();
    uint16_t s[1100];
    for (int i = 0; i < 1100; i++) s[i] = (uint16_t)(i * 3 + 0xA5FE);
    proto_send_pulse(s, 32);
    decode();
    CHECK(nfr == 1 && fr[0].cmd == SHPROTO_CMD_PULSE && fr[0].len == 4 + 64);
    CHECK(rd16(fr[0].pl) == 0 && rd16(fr[0].pl + 2) == 32 && rd16(fr[0].pl + 4 + 62) == s[31]);
    txn = 0;
    proto_send_pulse(s, 1100);
    decode();
    CHECK(nfr == 3);
    CHECK(rd16(fr[1].pl) == 512 && rd16(fr[1].pl + 2) == 1100 && fr[1].len == 4 + 1024);
    CHECK(rd16(fr[2].pl) == 1024 && fr[2].len == 4 + 2 * 76);
    CHECK(rd16(fr[2].pl + 4 + 2 * 75) == s[1099]);
}

int main(void)
{
    test_sta_sto_rst();
    test_inf_both_parsers();
    test_cal_read_40_regs();
    test_cal_parse_strict();
    test_poll_stepwise();
    test_acq_stopped_commits();
    test_cal_write_sequence();
    test_cal_write_during_acq_deferred();
    test_save_during_acq();
    test_boot_substring();
    test_status_payload();
    test_spectrum_chunks();
    test_unknown_cmd();
    test_thr_tc();
    test_template_staging();
    test_defaults();
    test_pulse_frames();
    return T_DONE();
}
