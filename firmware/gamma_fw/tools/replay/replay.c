/* replay.c — прогон детектора прошивки (src/core/detector.c) по сырым порциям на ПК.
 * Сборка: tools\replay\build.cmd. Запуск: replay raw.bin out.csv [thr] [gain] [tail.bin|-] [ev.csv|-] [dnl.txt]
 *   raw.bin — порции uint16 × 8192 подряд (gammapult tpl collect); эталон — /FI заголовок tpl_pmt.h.
 *   thr 0 (или не задан) — авто-порог, gain — 1.
 *   tail.bin — таблица хвоста (gammapult tpl build … --tail-out, 2112 Б: s[16][64] int16 LE, row[64]):
 *   задана — det_set_tail, разбор хвостовых наложений включён; нет (или «-») — прежний детектор.
 *   Между порциями — разрыв во времени: det_add_lost (хвосты сбрасываются), детектор в GUARD без
 *   взвода (ждёт спада ниже thr/2).
 * Итог: спектр out.csv (channel,count), с разбором ещё out.csv → *_rec.csv (только восстановленные —
 * принятые с вычетом хвоста предыдущего) и *_com.csv (остальные принятые); статистика детектора,
 * живое время (отсчёты минус мёртвые, без искусственных разрывов между порциями).
 * ev.csv (только с tail.bin) — принятые события: порция, отсчёт максимума в порции (из списка хвостов
 * детектора, −1 — неизвестен), канал, восстановлен ли (для синтетических пар tools/pileup/synpairs.py);
 *   «-» — не писать.
 * dnl.txt — таблица поправки DNL АЦП (gammapult adc dnl … --out dnl.txt): строки «код сдвиг» (сдвиг центра кода в
 *   1/16 LSB), # — комментарий; задана — det_set_dnl, как -dnl в приборе; нет (или «-») — без поправки. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "detector.h"

#ifndef REPLAY_FITS_PILE              /* бюджет порции при разборе; /DREPLAY_FITS_PILE=95 — без среза для опытов */
#define REPLAY_FITS_PILE DET_MAX_FITS_BOARD_PILE
#endif
#define BLK 8192u
#define GAP (1u << 20)
static uint32_t hist[DET_CHANNELS], hist_rec[DET_CHANNELS];
static det_t d;
static uint32_t rec_seen, blk_start, blk_no;
static FILE *evf;
static void emit(uint16_t ch)
{
    hist[ch]++;
    const int rec = d.st.recovered != rec_seen;      /* recovered++ перед emit */
    if (rec) { rec_seen = d.st.recovered; hist_rec[ch]++; }
    if (evf) {   /* с разбором выдача — из front_accept: максимум импульса — последний хвост списка */
        const long pos = d.tl_n ? (long)(d.tl_t[d.tl_n - 1u] - blk_start) : -1L;
        fprintf(evf, "%u,%ld,%u,%d\n", blk_no, pos, ch, rec);
    }
}

static int write_csv(const char *path, const uint32_t *h, const uint32_t *sub)
{
    FILE *o = fopen(path, "w");
    if (!o) { fprintf(stderr, "не записать %s\n", path); return 0; }
    fprintf(o, "channel,count\n");
    for (unsigned i = 0; i < DET_CHANNELS; i++) fprintf(o, "%u,%u\n", i, h[i] - (sub ? sub[i] : 0u));
    fclose(o);
    return 1;
}

/* таблица DNL: «код сдвиг» построчно, # — комментарий; 0 — ошибка (сообщение уже выведено) */
static int load_dnl(const char *path, det_dnl_t *t, unsigned *n)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "не открыть %s\n", path); return 0; }
    char ln[256];
    unsigned m = 0, no = 0;
    while (fgets(ln, sizeof ln, f)) {
        no++;
        char *h = strchr(ln, '#');
        if (h) *h = 0;
        long c, o;
        char rest[8];
        int k = sscanf(ln, "%ld %ld %7s", &c, &o, rest);
        if (k <= 0) continue;                        /* пустая строка */
        if (k != 2 || c < 0 || c >= (long)DET_DNL_CODES || o < -DET_DNL_OFF_MAX || o > DET_DNL_OFF_MAX || o == 0
            || m >= DET_DNL_MAX) {
            fprintf(stderr, "%s:%u: нужно «код 0…4095 сдвиг ±1…%d», не больше %u строк\n", path, no,
                    DET_DNL_OFF_MAX, DET_DNL_MAX);
            fclose(f);
            return 0;
        }
        t[m].code = (uint16_t)c; t[m].off = (int16_t)o; m++;
    }
    fclose(f);
    *n = m;
    return 1;
}

static void suffixed(char *dst, size_t n, const char *path, const char *suf)
{
    const char *dot = strrchr(path, '.');
    size_t stem = dot ? (size_t)(dot - path) : strlen(path);
    snprintf(dst, n, "%.*s%s%s", (int)stem, path, suf, dot ? dot : "");
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "replay raw.bin out.csv [thr] [gain] [tail.bin|-] [ev.csv|-] [dnl.txt]\n"); return 1; }
    unsigned thr = argc > 3 ? (unsigned)atoi(argv[3]) : 0u;
    float gain = argc > 4 ? (float)atof(argv[4]) : 1.0f;
    const char *tail_path = (argc > 5 && strcmp(argv[5], "-") != 0) ? argv[5] : NULL;
    static det_tail_t tl;
    if (tail_path) {
        FILE *t = fopen(tail_path, "rb");
        if (!t) { fprintf(stderr, "не открыть %s\n", tail_path); return 2; }
        uint8_t raw[sizeof tl.s + sizeof tl.row];
        size_t got = fread(raw, 1, sizeof raw, t);
        int extra = fgetc(t) != EOF;
        fclose(t);
        if (got != sizeof raw || extra) { fprintf(stderr, "%s: нужно %u байт\n", tail_path, (unsigned)sizeof raw); return 2; }
        for (unsigned r = 0; r < DET_TL_ROWS; r++)
            for (unsigned j = 0; j < DET_TL_LEN; j++) {
                const uint8_t *p = raw + 2u * (r * DET_TL_LEN + j);
                tl.s[r][j] = (int16_t)(uint16_t)(p[0] | (p[1] << 8));
            }
        memcpy(tl.row, raw + sizeof tl.s, sizeof tl.row);
        for (unsigned p = 0; p < DET_NPH; p++)
            if (tl.row[p] >= DET_TL_ROWS) { fprintf(stderr, "%s: row[%u] = %u\n", tail_path, p, tl.row[p]); return 2; }
    }
    static det_dnl_t dnl[DET_DNL_MAX];
    static uint32_t dnl_map[DET_DNL_WORDS];
    unsigned dnl_n = 0;
    const char *dnl_path = (argc > 7 && strcmp(argv[7], "-") != 0) ? argv[7] : NULL;
    if (dnl_path && !load_dnl(dnl_path, dnl, &dnl_n)) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "не открыть %s\n", argv[1]); return 2; }
    det_cfg_t cfg = { (uint16_t)thr, DET_MINW_BOARD, DET_MAXW_BOARD, 0.0f, DET_MAX_FITS_BOARD,
                      REPLAY_FITS_PILE };   /* как main.c */
    det_init(&d, &cfg, tpl_pmt, emit);
    det_set_gain(&d, gain);
    if (tail_path) det_set_tail(&d, &tl);
    if (dnl_n) det_set_dnl(&d, dnl, dnl_n, dnl_map);
    if (tail_path && argc > 6 && strcmp(argv[6], "-") != 0) {
        evf = fopen(argv[6], "w");
        if (!evf) { fprintf(stderr, "не записать %s\n", argv[6]); return 3; }
        fprintf(evf, "blk,pos,channel,rec\n");
    }
    static uint16_t b[BLK];
    unsigned nb = 0;
    while (fread(b, 2, BLK, f) == BLK) {
        if (nb) {
            det_add_lost(&d, GAP);
            d.state = 3; d.guard_left = DET_GUARD; d.armed = 0;   /* DET_S_GUARD, без взвода */
        }
        blk_start = d.pos0; blk_no = nb;
        det_process_block(&d, b, BLK);
        nb++;
    }
    fclose(f);
    if (evf) fclose(evf);
    det_stats_t st; det_get_stats(&d, &st);
    printf("порций %u, база %.2f σ %.2f порог %u, разбор %s, DNL %u пар\n", nb, det_baseline(&d), det_sigma(&d),
           det_thr(&d), det_pile_active(&d) ? "вкл" : "выкл", dnl_n);
    unsigned long long trig = (unsigned long long)st.accepted + st.rej_pileup + st.rej_shape + st.rej_width + st.rej_budget;
    printf("принято %u, наложения %u (фронт %u), форма %u, ширина %u, бюджет %u (всего %llu, брак %.2f %%)\n",
           st.accepted, st.rej_pileup, st.rej_front, st.rej_shape, st.rej_width, st.rej_budget, trig,
           trig ? 100.0 * (double)(trig - st.accepted) / (double)trig : 0.0);
    const unsigned long long total = (unsigned long long)nb * BLK;
    const unsigned long long dead = st.dead_samples - (unsigned long long)(nb ? nb - 1u : 0u) * GAP;
    const double live_s = (double)(total - dead) / (double)DET_FS_HZ;
    printf("восстановлено %u, мёртвых отсчётов %llu из %llu (%.2f %%), живое %.4f с из %.4f, принято/живое %.1f имп/с\n",
           st.recovered, dead, total, total ? 100.0 * (double)dead / (double)total : 0.0,
           live_s, (double)total / (double)DET_FS_HZ, live_s > 0 ? (double)st.accepted / live_s : 0.0);
    printf("STAT acc=%u pile=%u front=%u shape=%u width=%u budget=%u rec=%u dead=%llu total=%llu live_s=%.6f thr=%u\n",
           st.accepted, st.rej_pileup, st.rej_front, st.rej_shape, st.rej_width, st.rej_budget, st.recovered,
           dead, total, live_s, det_thr(&d));
    if (!write_csv(argv[2], hist, NULL)) return 3;
    if (tail_path) {
        char p[1024];
        suffixed(p, sizeof p, argv[2], "_rec");
        if (!write_csv(p, hist_rec, NULL)) return 3;
        suffixed(p, sizeof p, argv[2], "_com");
        if (!write_csv(p, hist, hist_rec)) return 3;
    }
    return 0;
}
