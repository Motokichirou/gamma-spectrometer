/* harness_body.h — тело обвязки, включается из harness.c с P() (префикс), T() (det_/detref_),
 * M() (DET_/DETREF_). Без защиты от повторного включения — так задумано. */

CCM_BSS T(t) P(det);             /* как в main.c: det_t в CCM SRAM */
uint16_t P(ch)[CHMAX];               /* выданные каналы с последнего <p>_clear */
uint32_t P(nch);
uint16_t P(capbuf)[CAPN * CAPW];
T(cap_t) P(capinfo)[CAPN];

/* снимок состояния для сверки: статистика, база и σ побитно, порог, окна */
struct P(dump_s) {
    T(stats_t) st;
    float    base, sigma;
    uint32_t thr, measuring, capn;
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
}
