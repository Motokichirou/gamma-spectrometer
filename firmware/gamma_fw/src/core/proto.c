/* proto.c — команды и ответы по shproto (см. proto.h). Образец BecqMoni-части — firmware/core/app.c. */
#include <string.h>
#ifdef HV_EXPERIMENT_1500
#define HV_EXP_TAG " HVEXP1500"
#else
#define HV_EXP_TAG ""
#endif
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include "proto.h"
#include "shproto.h"
#include "becq_cal.h"
#include "spectrum.h"
#include "fmt.h"

#define PL_MAX   (2u + 4u * PROTO_CHUNK_CH)             /* самый длинный кадр — кусок спектра */
#define SPEC_FRAMES (SPECTRUM_CHANNELS / PROTO_CHUNK_CH) /* 16 кадров 0x01 */
#define OUT_IDLE    0xFFu                               /* выдача спектра не идёт */
/* проверки при сборке (MSVC без /std:c11 не знает _Static_assert) */
typedef char chk_frame_max[(PROTO_TX_FRAME_MAX == 2u * (PL_MAX + 3u) + 3u) ? 1 : -1];
typedef char chk_chunks[(SPECTRUM_CHANNELS % PROTO_CHUNK_CH == 0) ? 1 : -1];
#define TPL_ROWS 64u
#define TPL_COLS 7u
#define TPL_QMAX 16384L                                 /* |отсчёт эталона| ≤ 1.0 в Q14 */

enum { FL_NONE, FL_DEFAULTS, FL_OK, FL_PENDING, FL_ERR };

static const proto_port_t *P;
static settings_t *L;
static shproto_rx_t rx;
static bool     acquiring;
static bool     pending;          /* запись настроек отложена до -sto */
static uint8_t  flash_st;
static uint8_t  flash_st_prev;    /* состояние до отложенной записи — вернуть при её отмене */
static uint8_t  boot_idx;         /* сколько символов "!boot" уже совпало */
static float    temp_c;
static uint32_t cal_stage[BECQ_WORDS];
static uint16_t cal_mask;         /* бит i — слово i получено */
static int16_t  tpl_stage[TPL_ROWS][TPL_COLS];
static uint64_t tpl_mask;         /* бит i — строка i получена */
static uint64_t tl_mask;          /* бит 4·r + c — кусок -tlw r c получен (пишется прямо в L->tail) */
static uint8_t  tlm_mask;         /* бит c — кусок -tlm c получен */
static uint32_t rx_frames;       /* корректных кадров (CRC сошёлся) с пуска — сторож -baud */
static uint8_t  out_step;         /* 0…15 — кадр 0x01, 16 — статус 0x04, OUT_IDLE — нет выдачи */

static uint8_t txbuf[PROTO_TX_FRAME_MAX];
static uint8_t pl[PL_MAX];

/* ---- передача ---- */
static void send_frame(uint8_t cmd, const uint8_t *p, size_t n)
{
    size_t m = shproto_build(cmd, p, n, txbuf);
    if (P && P->tx) P->tx(txbuf, m);
}

void proto_send_frame(uint8_t cmd, const uint8_t *p, size_t n) { send_frame(cmd, p, n); }

void proto_send_text(const char *s)
{
    send_frame(SHPROTO_CMD_TEXT, (const uint8_t *)s, strlen(s));
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* fmt_fix / fmt_exp — числа без %f/%e (newlib-nano), fmt.c: k1/k2 в fmt_exp — 7 значащих
   цифр, читаются назад без потерь. */

/* ---- флеш: только при остановленном наборе (SPEC §7.9) ---- */
static bool commit_now(void)
{
    settings_seal(L);
    bool ok = P->settings_commit && P->settings_commit(L);
    flash_st = ok ? FL_OK : FL_ERR;
    pending = false;
    return ok;
}

/* 1 — записано, 0 — отложено до -sto, −1 — ошибка записи */
static int request_commit(void)
{
    settings_seal(L);
    if (acquiring) {
        if (!pending) flash_st_prev = flash_st;
        pending = true;
        flash_st = FL_PENDING;
        return 0;
    }
    return commit_now() ? 1 : -1;
}

static void changed(uint32_t what)
{
    settings_seal(L);
    if (P->apply) P->apply(what);
}

/* ---- разбор аргументов ---- */
/* Целые через пробел, ровно max штук не обязательно: возвращает число, −1 — мусор. */
static int parse_longs(const char *p, long *v, int max, int base)
{
    int n = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) return n;
        if (n >= max) return -1;
        char *e;
        v[n] = strtol(p, &e, base);
        if (e == p || (*e && *e != ' ' && *e != '\t')) return -1;
        n++; p = e;
    }
}

static int parse_floats(const char *p, float *v, int max)
{
    int n = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) return n;
        if (n >= max) return -1;
        char *e;
        double d = strtod(p, &e);
        if (e == p || (*e && *e != ' ' && *e != '\t') || !isfinite(d)) return -1;
        v[n++] = (float)d; p = e;
    }
}

/* ---- команды BecqMoni ---- */
static void cmd_inf(void)
{
    char t[16], r[96];
    fmt_fix(t, sizeof t, temp_c, 1);
    snprintf(r, sizeof r, "GammaSpec v1 RISE %u FALL %u T1 %s F %lu VERSION 100" HV_EXP_TAG,
             (unsigned)PROTO_INF_RISE, (unsigned)PROTO_INF_FALL, t, (unsigned long)PROTO_INF_FS);
    proto_send_text(r);
}

static void cmd_cal(const char *args)
{
    while (*args == ' ') args++;
    if (!*args) {                                          /* 40 регистров */
        char r[480];
        if (becq_format_registers(L->becq_words, L->becq_valid != 0, PROTO_SERIAL, r, sizeof r))
            proto_send_text(r);
        else
            proto_send_text("-err cal\r\n");
        return;
    }
    char *e;
    long idx = strtol(args, &e, 10);
    if (e == args || idx < 0 || idx >= (long)BECQ_WORDS || (*e && *e != ' ' && *e != '\t')) {
        proto_send_text("-err cal\r\n");                   /* "-cal 1abc" — не слово 1 */
        return;
    }
    while (*e == ' ' || *e == '\t') e++;
    if (!*e) {                                             /* чтение одного слова */
        char r[16];
        snprintf(r, sizeof r, "%08lX", (unsigned long)(L->becq_valid ? L->becq_words[idx] : 0u));
        proto_send_text(r);
        return;
    }
    /* слово — ровно 1…8 hex-цифр без знака и "0x": strtoul молча даёт ULONG_MAX при
       переполнении и принимает "-1" */
    uint32_t w = 0;
    unsigned nd = 0;
    for (; isxdigit((unsigned char)*e); e++, nd++) {
        unsigned c = (unsigned char)*e;
        unsigned d = (c <= '9') ? c - '0' : (c | 0x20u) - 'a' + 10u;
        w = (w << 4) | d;
    }
    while (*e == ' ' || *e == '\t') e++;
    if (nd == 0 || nd > 8 || *e) { proto_send_text("-err cal\r\n"); return; }
    cal_stage[idx] = w;
    cal_mask |= (uint16_t)(1u << idx);
    if (cal_mask != (1u << BECQ_WORDS) - 1u) { proto_send_text("-ok\r\n"); return; }

    cal_mask = 0;                                          /* все 11 получены */
    if (becq_checksum(cal_stage) != cal_stage[10]) {       /* битую сумму не хранить: BecqMoni */
        proto_send_text("-err cal\r\n");                   /* падает на её чтении */
        return;
    }
    memcpy(L->becq_words, cal_stage, sizeof cal_stage);
    L->becq_valid = 1;
    proto_send_text(request_commit() < 0 ? "-err flash\r\n" : "-ok\r\n");
}

/* ---- сервисные (только настройки) ---- */
static void cmd_thr(const char *args)
{
    long v; char r[32];
    int n = parse_longs(args, &v, 1, 10);
    if (n < 0 || (n == 1 && (v < 0 || v > (long)PROTO_THR_MAX))) { proto_send_text("-err thr\r\n"); return; }
    if (n == 1) { L->thr_codes = (uint16_t)v; changed(PROTO_APPLY_THR); }
    snprintf(r, sizeof r, "-ok thr %u\r\n", (unsigned)L->thr_codes);
    proto_send_text(r);
}

static void cmd_tc(const char *args)
{
    float v[3]; char a[3][24], r[96];
    int n = parse_floats(args, v, 3);
    if (n != 0 && n != 3) { proto_send_text("-err tc\r\n"); return; }
    if (n == 3) { L->tc_t0_c = v[0]; L->tc_k1 = v[1]; L->tc_k2 = v[2]; changed(PROTO_APPLY_TC); }
    fmt_fix(a[0], sizeof a[0], L->tc_t0_c, 3);
    fmt_exp(a[1], sizeof a[1], L->tc_k1);                 /* k2 ~1e-6: fmt_fix(…, 6) его теряет */
    fmt_exp(a[2], sizeof a[2], L->tc_k2);
    snprintf(r, sizeof r, "-ok tc %s %s %s\r\n", a[0], a[1], a[2]);
    proto_send_text(r);
}

static void cmd_tplw(const char *args)
{
    long v[1 + TPL_COLS];
    if (parse_longs(args, v, 1 + TPL_COLS, 10) != 1 + (int)TPL_COLS || v[0] < 0 || v[0] >= (long)TPL_ROWS) {
        proto_send_text("-err tplw\r\n");
        return;
    }
    /* Q14: 16384 = пик импульса, отсчёт строки по модулю не больше пика */
    for (unsigned i = 1; i <= TPL_COLS; i++)
        if (v[i] < -TPL_QMAX || v[i] > TPL_QMAX) { proto_send_text("-err tplw\r\n"); return; }
    for (unsigned i = 0; i < TPL_COLS; i++) tpl_stage[v[0]][i] = (int16_t)v[1 + i];
    tpl_mask |= 1ull << v[0];
    proto_send_text("-ok\r\n");
}

static void cmd_tplok(void)
{
    if (tpl_mask != ~0ull) {
        unsigned got = 0; char r[32];
        for (unsigned i = 0; i < TPL_ROWS; i++) got += (unsigned)((tpl_mask >> i) & 1u);
        snprintf(r, sizeof r, "-err tpl %u/64\r\n", got);
        proto_send_text(r);
        return;
    }
    memcpy(L->tpl, tpl_stage, sizeof L->tpl);   /* между порциями: детектор сейчас не работает */
    L->tpl_valid = 1;
    tpl_mask = 0;
    L->tail_valid = 0; tl_mask = 0; tlm_mask = 0;   /* таблица хвоста строилась по прежнему эталону */
    changed(PROTO_APPLY_TPL | PROTO_APPLY_TAIL);
    proto_send_text("-ok tpl\r\n");
}

/* ---- таблица хвоста разбора наложений ----
 * Куски пишутся прямо в L->tail (отдельного буфера нет: ОЗУ). Пока не принят -tlok, tail_valid = 0 и
 * детектор таблицу не видит; принятая таблица при первом же новом куске снимается (иначе разбор шёл бы
 * по полузаписанной). */
static void tail_drop(void)
{
    if (L->tail_valid) { L->tail_valid = 0; changed(PROTO_APPLY_TAIL); }
}

static void cmd_tlw(const char *args)
{
    long v[2 + 16];
    if (parse_longs(args, v, 2 + 16, 10) != 2 + 16 || v[0] < 0 || v[0] >= (long)DET_TL_ROWS || v[1] < 0 || v[1] > 3) {
        proto_send_text("-err tlw\r\n");
        return;
    }
    for (int i = 2; i < 2 + 16; i++)
        if (v[i] < -32768 || v[i] > 32767) { proto_send_text("-err tlw\r\n"); return; }
    tail_drop();
    for (int i = 0; i < 16; i++) L->tail.s[v[0]][16 * v[1] + i] = (int16_t)v[2 + i];
    tl_mask |= 1ull << (4 * v[0] + v[1]);
    proto_send_text("-ok\r\n");
}

static void cmd_tlm(const char *args)
{
    long v[1 + 16];
    if (parse_longs(args, v, 1 + 16, 10) != 1 + 16 || v[0] < 0 || v[0] > 3) { proto_send_text("-err tlm\r\n"); return; }
    for (int i = 1; i < 1 + 16; i++)
        if (v[i] < 0 || v[i] >= (long)DET_TL_ROWS) { proto_send_text("-err tlm\r\n"); return; }
    tail_drop();
    for (int i = 0; i < 16; i++) L->tail.row[16 * v[0] + i] = (uint8_t)v[1 + i];
    tlm_mask |= (uint8_t)(1u << v[0]);
    proto_send_text("-ok\r\n");
}

static void cmd_tlok(void)
{
    if (tl_mask != ~0ull || tlm_mask != 0x0Fu) { proto_send_text("-err tl\r\n"); return; }
    if (!L->tpl_valid) { proto_send_text("-err tl\r\n"); return; }   /* таблица — к своему эталону, не к встроенному */
    tl_mask = 0; tlm_mask = 0;
    L->tail_valid = 1;
    changed(PROTO_APPLY_TAIL);
    proto_send_text("-ok tl\r\n");
}

static void cmd_pile(const char *args)
{
    while (*args == ' ' || *args == '\t') args++;
    size_t n = strcspn(args, " \t");
    const char *rest = args + n;
    while (*rest == ' ' || *rest == '\t') rest++;
    if (n) {
        if (*rest) { proto_send_text("-err pile\r\n"); return; }
        if (n == 2 && memcmp(args, "on", 2) == 0) L->pile_on = 1;
        else if (n == 3 && memcmp(args, "off", 3) == 0) L->pile_on = 0;
        else { proto_send_text("-err pile\r\n"); return; }
        changed(PROTO_APPLY_TAIL);
    }
    proto_send_text(L->pile_on ? "-ok pile on\r\n" : "-ok pile off\r\n");
}

static void cmd_save(void)
{
    int r = request_commit();
    proto_send_text(r > 0 ? "-ok saved\r\n" : r == 0 ? "-ok deferred\r\n" : "-err flash\r\n");
}

/* ---- разбор текстового кадра ---- */
static bool tok_is(const char *line, size_t tl, const char *name)
{
    return strlen(name) == tl && memcmp(line, name, tl) == 0;
}

static void handle_text(const uint8_t *p, size_t n)
{
    char line[SHPROTO_RX_MAX_PAYLOAD + 1];
    if (n > SHPROTO_RX_MAX_PAYLOAD) n = SHPROTO_RX_MAX_PAYLOAD;
    memcpy(line, p, n);
    line[n] = 0;
    for (size_t i = 0; i < n; i++) if (line[i] == 0) { n = i; break; }
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ' || line[n - 1] == '\t'))
        line[--n] = 0;
    char *s = line;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s != '-') return;                                 /* не команда — тишина */

    size_t tl = strcspn(s, " \t");
    const char *args = s + tl;

    /* ⚠ ack-и BecqMoni — строго "-ok\r\n": AtomSpectra сверяет ответ точно. */
    if (tok_is(s, tl, "-sta")) {
        if (!acquiring) { acquiring = true; if (P->acq_start) P->acq_start(); }
        proto_send_text("-ok\r\n");
    } else if (tok_is(s, tl, "-sto")) {
        if (P->acq_stop) P->acq_stop();
        proto_acq_stopped();                               /* отложенная запись — ровно один раз */
        proto_send_text("-ok\r\n");
    } else if (tok_is(s, tl, "-rst")) {
        if (P->acq_clear) P->acq_clear();
        proto_send_text("-ok\r\n");
    } else if (tok_is(s, tl, "-stt")) {
        proto_send_text(acquiring ? "-ok collecting\r\n" : "-ok\r\n");
    } else if (tok_is(s, tl, "-mode")) {
        proto_send_text("-ok\r\n");
    } else if (tok_is(s, tl, "-sho")) {
        proto_send_text("-ok collecting\r\n");
    } else if (tok_is(s, tl, "-inf")) {
        cmd_inf();
    } else if (tok_is(s, tl, "-cal")) {
        cmd_cal(args);
    } else if (tok_is(s, tl, "-save")) {
        cmd_save();
    } else if (tok_is(s, tl, "-defaults")) {
        if (P->hv_on && P->hv_on()) { proto_send_text("-err hv_on\r\n"); return; }
        settings_defaults(L);
        cal_mask = 0; tpl_mask = 0; tl_mask = 0; tlm_mask = 0;
        /* умолчания — только в ОЗУ: отложенная запись (-cal/-save во время набора) отменяется,
           иначе -sto записал бы во флеш умолчания вместо отложенного */
        if (pending) { pending = false; flash_st = flash_st_prev; }
        changed(PROTO_APPLY_ALL);
        proto_send_text("-ok defaults\r\n");
    } else if (tok_is(s, tl, "-thr")) {
        cmd_thr(args);
    } else if (tok_is(s, tl, "-tc")) {
        cmd_tc(args);
    } else if (tok_is(s, tl, "-tplw")) {
        cmd_tplw(args);
    } else if (tok_is(s, tl, "-tplok")) {
        cmd_tplok();
    } else if (tok_is(s, tl, "-tlw")) {
        cmd_tlw(args);
    } else if (tok_is(s, tl, "-tlm")) {
        cmd_tlm(args);
    } else if (tok_is(s, tl, "-tlok")) {
        cmd_tlok();
    } else if (tok_is(s, tl, "-pile")) {
        cmd_pile(args);
    } else if (tok_is(s, tl, "-tplclr")) {
        L->tpl_valid = 0;
        tpl_mask = 0;
        L->tail_valid = 0; tl_mask = 0; tlm_mask = 0;
        changed(PROTO_APPLY_TPL | PROTO_APPLY_TAIL);
        proto_send_text("-ok tpl builtin\r\n");
    } else {
        char r[512];
        r[0] = 0;
        if (P->service) P->service(s, r, sizeof r - 2);
        r[sizeof r - 3] = 0;
        size_t rl = strlen(r);
        if (!rl) { proto_send_text("-err unknown\r\n"); return; }
        if (rl < 2 || r[rl - 2] != '\r' || r[rl - 1] != '\n') { r[rl] = '\r'; r[rl + 1] = '\n'; r[rl + 2] = 0; }
        proto_send_text(r);
    }
}

/* ---- публичное ---- */
void proto_init(const proto_port_t *port, settings_t *live)
{
    P = port;
    L = live;
    shproto_rx_init(&rx);
    acquiring = false;
    pending = false;
    flash_st = flash_st_prev = FL_NONE;
    boot_idx = 0;
    temp_c = 0.0f;
    cal_mask = 0;
    tpl_mask = 0; tl_mask = 0; tlm_mask = 0;
    out_step = OUT_IDLE;
    rx_frames = 0;
}

void proto_acq_started(void) { acquiring = true; }

void proto_acq_stopped(void)
{
    acquiring = false;
    if (pending) commit_now();
}

void proto_rx(const uint8_t *p, size_t n)
{
    static const char BOOT[] = "!boot";
    for (size_t i = 0; i < n; i++) {
        uint8_t b = p[i];
        /* "!boot" — подстрокой в сыром потоке; у шаблона нет самоперекрытия, кроме '!' */
        if (b == (uint8_t)BOOT[boot_idx]) {
            if (++boot_idx == 5u) {
                boot_idx = 0;
                if (P->enter_bootloader) P->enter_bootloader();
            }
        } else {
            boot_idx = (b == '!') ? 1u : 0u;
        }
        if (shproto_rx_byte(&rx, b)) {
            rx_frames++;
            if (rx.cmd == SHPROTO_CMD_TEXT) handle_text(rx.buf, rx.len);
        }
    }
}

void proto_tick_1s(void)
{
    /* только взводит выдачу; прошлая не закончилась (линия занята) — тик пропускаем, не копим */
    if (acquiring && out_step == OUT_IDLE) out_step = 0;
}

void proto_poll(void)
{
    if (out_step == OUT_IDLE) return;
    if (P->tx_free && P->tx_free() < PROTO_TX_FRAME_MAX) return;   /* ждём место в кольце */
    if (out_step < SPEC_FRAMES) {
        /* по кадру за проход: куски снимаются в разные моменты (за ≤ 0.6 с) — BecqMoni
           всё равно перерисовывает спектр только по статусу */
        uint32_t off = (uint32_t)out_step * PROTO_CHUNK_CH;
        const uint32_t *spec = spectrum_data();
        put16(pl, (uint16_t)off);
        for (uint32_t i = 0; i < PROTO_CHUNK_CH; i++)
            put32(pl + 2 + 4 * i, spec[off + i] > SPECTRUM_MASK ? SPECTRUM_MASK : spec[off + i]);   /* насыщение 27 бит */
        send_frame(SHPROTO_CMD_HISTOGRAM, pl, 2u + 4u * PROTO_CHUNK_CH);
        out_step++;
        return;
    }
    /* статус — последним: по нему BecqMoni перерисовывает спектр */
    proto_status_t st;
    memset(&st, 0, sizeof st);
    if (P->fill_status) P->fill_status(&st);
    put32(pl, st.elapsed_s);
    put16(pl + 4, st.cpu_x10);
    put32(pl + 6, st.cps);
    put32(pl + 10, st.invalid);
    send_frame(SHPROTO_CMD_STAT, pl, 14);
    out_step = OUT_IDLE;
}

void proto_send_pulse(const uint16_t *s, size_t n)
{
    if (n > 0xFFFFu) n = 0xFFFFu;
    for (size_t i = 0; i < n; i += PROTO_PULSE_CHUNK) {
        size_t k = n - i < PROTO_PULSE_CHUNK ? n - i : PROTO_PULSE_CHUNK;
        put16(pl, (uint16_t)i);
        put16(pl + 2, (uint16_t)n);
        for (size_t j = 0; j < k; j++) put16(pl + 4 + 2 * j, s[i + j]);
        send_frame(SHPROTO_CMD_PULSE, pl, 4u + 2u * k);
    }
}

void proto_flash_defaults(void) { flash_st = FL_DEFAULTS; }
void proto_flash_loaded(void) { flash_st = FL_OK; }
void proto_set_temperature(float t_c) { temp_c = t_c; }
bool proto_acquiring(void) { return acquiring; }
uint32_t proto_rx_frames(void) { return rx_frames; }

const char *proto_flash_status(void)
{
    switch (flash_st) {
    case FL_DEFAULTS: return "defaults";
    case FL_OK:      return "ok";
    case FL_PENDING: return "pending";
    case FL_ERR:     return "err";
    default:         return "none";
    }
}
