/* replay.c — прогон детектора прошивки (src/core/detector.c) по сырым порциям на ПК.
 * Сборка: tools\replay\build.cmd. Запуск: replay raw.bin out.csv [thr] [gain]
 *   raw.bin — порции uint16 × 8192 подряд (gammapult tpl collect); эталон — /FI заголовок tpl_pmt.h.
 *   Между порциями — разрыв во времени: детектор в GUARD без взвода (ждёт спада ниже thr/2).
 * Итог: спектр out.csv (channel,count) и статистика детектора. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "detector.h"

#define BLK 8192u
static uint32_t hist[DET_CHANNELS];
static void emit(uint16_t ch) { hist[ch]++; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "replay raw.bin out.csv [thr] [gain]\n"); return 1; }
    unsigned thr = argc > 3 ? (unsigned)atoi(argv[3]) : 0u;
    float gain = argc > 4 ? (float)atof(argv[4]) : 1.0f;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "не открыть %s\n", argv[1]); return 2; }
    static det_t d;
    det_cfg_t cfg = { (uint16_t)thr, DET_MINW_BOARD, DET_MAXW_BOARD, 0.0f, DET_MAX_FITS_BOARD };
    det_init(&d, &cfg, tpl_pmt, emit);
    det_set_gain(&d, gain);
    static uint16_t b[BLK];
    unsigned nb = 0;
    while (fread(b, 2, BLK, f) == BLK) {
        if (nb) {
            det_add_lost(&d, 1u << 20);
            d.state = 3; d.guard_left = DET_GUARD; d.armed = 0;   /* DET_S_GUARD, без взвода */
        }
        det_process_block(&d, b, BLK);
        nb++;
    }
    fclose(f);
    det_stats_t st; det_get_stats(&d, &st);
    printf("порций %u, база %.2f σ %.2f порог %u\n", nb, det_baseline(&d), det_sigma(&d), det_thr(&d));
    unsigned long long trig = (unsigned long long)st.accepted + st.rej_pileup + st.rej_shape + st.rej_width + st.rej_budget;
    printf("принято %u, наложения %u, форма %u, ширина %u, бюджет %u (всего %llu, брак %.1f %%)\n",
           st.accepted, st.rej_pileup, st.rej_shape, st.rej_width, st.rej_budget, trig,
           trig ? 100.0 * (double)(trig - st.accepted) / (double)trig : 0.0);
    FILE *o = fopen(argv[2], "w");
    if (!o) return 3;
    fprintf(o, "channel,count\n");
    for (unsigned i = 0; i < DET_CHANNELS; i++) fprintf(o, "%u,%u\n", i, hist[i]);
    fclose(o);
    return 0;
}
