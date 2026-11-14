/*
 * harness.c — обвязка detector.c для эмулятора тактов (tools/cyc/cyc.py). Только для замеров.
 *
 * Собирается тем же тулчейном и с теми же флагами, что build.sh. Варианты (-D):
 *   WITH_NEW — src/core/detector.c, функции new_*;
 *   WITH_REF — tests/detector_ref.c (детектор до ускорения), функции ref_*;
 *   оба сразу — для сверки бит в бит на ARM (cyc.py --equiv).
 * Отсчёты порции cyc.py пишет в h_buf, затем зовёт <p>_block(смещение, n).
 */
#include <stdint.h>
#include <string.h>
#include "tpl_3467k.h"
#include "tpl_dac.h"
#include "ccm.h"

#define HMAX   (16u * 8192u)
#define CHMAX  4096u
#define CAPN   64u
#define CAPW   (8u + 24u)

uint16_t h_buf[HMAX];

#if defined(WITH_NEW)
#include "detector.h"
#define P(x) new_##x
#define T(x) det_##x
#define M(x) DET_##x
#include "harness_body.h"
#undef P
#undef T
#undef M

/* cyc.py --raw: эталон с записанных данных (TPL_FILE — заголовок с tpl_pmt, gammapult tpl build) и
   таблица хвоста h_tail (cyc.py пишет её байты; new_settail(1) — det_set_tail, разбор вкл) */
det_tail_t h_tail;
void new_settail(uint32_t on) { det_set_tail(&new_det, on ? &h_tail : 0); }
#ifdef TPL_FILE
#include TPL_FILE
void new_init_pmt(uint32_t thr, uint32_t max_fits, uint32_t minw, uint32_t maxw)
{
    det_cfg_t c;
    memset(&c, 0, sizeof c);
    c.thr = (uint16_t)thr;
    c.min_w = (uint8_t)minw;
    c.max_w = (uint8_t)maxw;
    c.max_fits = (uint16_t)max_fits;
    det_init(&new_det, &c, tpl_pmt, new_emit);
    new_nch = 0;
}
#endif
#endif

#if defined(WITH_REF)
#include "detector_ref.h"
#define P(x) ref_##x
#define T(x) detref_##x
#define M(x) DETREF_##x
#include "harness_body.h"
#undef P
#undef T
#undef M
#endif
