/* harness_body.h — тело обвязки, включается из harness.c с P() (префикс), T() (det_/detref_),
 * M() (DET_/DETREF_). Без защиты от повторного включения — так задумано. */

#ifdef DET_IN_RAM
T(t) P(det);                     /* --det-ram: det_t в ОЗУ */
#else
CCM_BSS T(t) P(det);             /* как в main.c: det_t в CCM SRAM */
#endif
uint16_t P(ch)[CHMAX];               /* выданные каналы с последнего <p>_clear */
uint32_t P(nch);
uint16_t P(capbuf)[CAPN * CAPW];
T(cap_t) P(capinfo)[CAPN];

/* снимок состояния для сверки: статистика, база и σ побитно, порог, окна, внутреннее состояние */
struct P(dump_s) {
    T(stats_t) st;
    float    base, sigma, var;
    uint32_t thr, measuring, capn;
    int32_t  bint;                   /* внутреннее — сверка бит в бит (как tests/test_det_equiv.c) */
    uint32_t qn;
    uint64_t qsum, qsq;
    uint32_t stk_n;
    int32_t  stk_s1;
    int64_t  stk_s2;
    uint32_t state, pos0, fits_left, cap_on;
} P(out);

static void P(emit)(uint16_t c)
{
    if (P(nch) < CHMAX) P(ch)[P(nch)] = c;
    P(nch)++;
}

void P(init)(uint32_t thr, uint32_t max_fits, uint32_t tpl_dac_sel, uint32_t minw, uint32_t maxw)
{
    T(cfg_t) c;
    memset(&c, 0, sizeof c);
    c.thr = (uint16_t)thr;
    c.min_w = (uint8_t)(minw ? minw : M(MINW_DEF));
    c.max_w = (uint8_t)(maxw ? maxw : M(MAXW_DEF));
    c.chi2_max = 0.0f;
    c.max_fits = (uint16_t)max_fits;
    T(init)(&P(det), &c, tpl_dac_sel ? tpl_dac : tpl_3467k, P(emit));
    P(nch) = 0;
}

void P(clear)(void) { P(nch) = 0; }

void P(block)(uint32_t off, uint32_t n) { T(process_block)(&P(det), h_buf + off, n); }

void P(capture)(uint32_t on)
{
    if (on) T(set_capture)(&P(det), P(capbuf), P(capinfo), 8u, 24u, CAPN);
    else    T(set_capture)(&P(det), 0, 0, 0u, 0u, 0u);
}

void P(hold)(uint32_t on)  { T(set_base_hold)(&P(det), (int)on); }
void P(thr)(uint32_t thr)  { T(set_thr)(&P(det), (uint16_t)thr); }
void P(width)(uint32_t a, uint32_t b) { T(set_width)(&P(det), (uint8_t)a, (uint8_t)b); }
void P(lost)(uint32_t n)   { T(add_lost)(&P(det), n); }
void P(tpl)(uint32_t dac)  { T(set_template)(&P(det), dac ? tpl_dac : tpl_3467k); }
void P(gain)(uint32_t milli) { T(set_gain)(&P(det), (float)milli * 0.001f); }

void P(dump)(void)
{
    T(get_stats)(&P(det), &P(out).st);
    P(out).base = T(baseline)(&P(det));
    P(out).sigma = T(sigma)(&P(det));
    P(out).thr = T(thr)(&P(det));
    P(out).measuring = (uint32_t)T(thr_measuring)(&P(det));
    P(out).capn = T(capture_count)(&P(det));
    P(out).var = P(det).var;
    P(out).bint = P(det).bint;
    P(out).qn = P(det).qn;
    P(out).qsum = P(det).qsum;
    P(out).qsq = P(det).qsq;
    P(out).stk_n = P(det).stk_n;
    P(out).stk_s1 = P(det).stk_s1;
    P(out).stk_s2 = P(det).stk_s2;
    P(out).state = P(det).state;
    P(out).pos0 = P(det).pos0;
    P(out).fits_left = P(det).fits_left;
    P(out).cap_on = P(det).cap_on;
}
