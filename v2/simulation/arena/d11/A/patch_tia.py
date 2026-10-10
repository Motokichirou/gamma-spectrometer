# -*- coding: utf-8 -*-
"""Генератор mc_pileup_vp.py из v2/simulation/mc_pileup.py (оригинал не трогается).

Запуск:  python v2/simulation/arena/d11/A/patch_tia.py
Добавляет --method=none|vp|off|oracle, --seed, --fs, --rates, --ratios, --dur.
Генерация сигнала, шум, АЦП, детектор, блоки и метрика — без изменений.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, '..', '..', '..', 'mc_pileup.py'))
DST = os.path.join(HERE, 'mc_pileup_vp.py')
s = open(SRC, encoding='utf-8').read()


def rep(old, new, count=1):
    global s
    assert s.count(old) == count, ('не найдено однозначно', old[:80], s.count(old))
    s = s.replace(old, new)


rep("ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))",
    "ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..', '..'))")
rep("QUICK = '--quick' in sys.argv", """QUICK = '--quick' in sys.argv


def _arg(name, default):
    for a in sys.argv:
        if a.startswith(name + '='):
            return a.split('=', 1)[1]
    return default


METHOD = _arg('--method', 'none')      # none | vp | off | off_vp | oracle
SEED = int(_arg('--seed', '20261008'))""")
rep("rng = np.random.default_rng(20261008)", "rng = np.random.default_rng(SEED)")

rep("\ndef e_out_density():", '''
def _vp_block(seg, tl, tq, tt, T, maxit=10):
    """VP + ЛМ по моментам всего блока. Амплитуды и постоянная исключены (QR).
    Якобиан Kaufman: J_q = T th_q P_perp h'(t - t_q). Шаг — только при падении остатка."""
    def fit(tt_):
        H = np.empty((len(seg), len(tt_) + 1))
        for q, t0 in enumerate(tt_):
            H[:, q] = pulse(tl - t0, tt)
        H[:, -1] = 1.0
        Qm, Rm = np.linalg.qr(H)
        dg = np.abs(np.diag(Rm))
        if dg.min() < 1e-9 * dg.max():
            return None, None, np.inf
        th = np.linalg.solve(Rm, Qm.T @ seg)
        r = seg - H @ th
        return th, Qm, float(r @ r)

    def sep_ok(tt_):
        return len(tt_) < 2 or np.min(np.diff(np.sort(tt_))) >= 0.3 * T

    th, Qm, rn = fit(tq)
    if th is None:
        return None, tq
    lam = 1e-3
    for _ in range(maxit):
        r = seg - Qm @ (Qm.T @ seg)
        Mx = np.empty((len(seg), len(tq)))
        for q in range(len(tq)):
            Mx[:, q] = T * th[q] * dpulse(tl - tq[q], tt)
        J = Mx - Qm @ (Qm.T @ Mx)
        A = J.T @ J
        g = J.T @ r
        d = np.diag(A).copy()
        d[d <= 0] = 1.0
        acc = False
        dp = np.zeros(len(tq))
        while lam < 1e4:
            try:
                dp = -np.linalg.solve(A + lam * np.diag(d), g)
            except np.linalg.LinAlgError:
                lam *= 10
                continue
            dp = np.clip(dp, -1.0, 1.0)
            tq_try = tq + dp * T
            if not sep_ok(tq_try):
                lam *= 10
                continue
            th2, Q2, rn2 = fit(tq_try)
            if th2 is not None and rn2 < rn:
                tq, th, Qm, rn = tq_try, th2, Q2, rn2
                lam = max(lam / 3, 1e-7)
                acc = True
                break
            lam *= 10
        if not acc or np.max(np.abs(dp)) < 1e-3:
            break
    return th, tq


def e_out_density():''')

rep("    t_det = (i0 + np.clip(sub, -1, 1)) * T\n", """    t_det = (i0 + np.clip(sub, -1, 1)) * T
    # систематический сдвиг детектора по бесшумному эталону (16 фаз), в отсчётах
    _b = []
    for ph in np.arange(16) / 16.0:
        v = V_PEAK_CS * pulse((np.arange(L + 20) - 10 - ph) * T, tt)
        sv = np.empty(len(v))
        sv[0] = v[0]
        sv[1:] = v[1:] - d_pz * v[:-1]
        if NAV > 1:
            sv = np.convolve(sv, np.ones(NAV) / NAV, mode='same')
        m_ = int(np.argmax(sv[2:-2])) + 2
        c0, c1, c2 = sv[m_ - 1], sv[m_], sv[m_ + 1]
        dd = c0 - 2 * c1 + c2
        _b.append(m_ + (0.5 * (c0 - c2) / dd if dd != 0 else 0.0) - (10 + ph))
    det_bias = float(np.mean(_b))
    if METHOD in ('off', 'off_vp'):
        t_det = t_det - det_bias * T
    if METHOD == 'oracle':
        k_ = np.clip(np.searchsorted(t_ev, t_det), 1, max(1, n_ev - 1))
        pk = np.where(np.abs(t_ev[k_] - t_det) < np.abs(t_ev[k_ - 1] - t_det), k_, k_ - 1)
        okk = np.abs(t_ev[pk] - t_det) < max(2.5 * T, 25e-9)
        t_det = np.where(okk, t_ev[pk], t_det)
        t_det = np.sort(t_det)
""")
rep("        t_det_loc = t_det[sel]\n", """        t_det_loc = t_det[sel]
        if METHOD in ('vp', 'off_vp'):
            tl_ = np.arange(s_lo, s_hi) * T
            # Вклад событий, которые не уточняются, фиксируется по исходному решению блока.
            # и события левее начала блока: их хвосты — чистые экспоненты e^(-t/td),
            # коллинеарны друг другу. Уточняются только события с фронтом внутри блока.
            act = (t_det_loc >= tl_[0]) & (t_det_loc < tl_[-1] - 2 * T)
            seg_v = y[s_lo:s_hi] - H[:, :-1][:, ~act] @ th[:-1][~act]
            th_v, t_v = _vp_block(seg_v, tl_, t_det_loc[act].copy(), tt, T)
            if th_v is not None:
                th = th.copy()
                th[:-1][act] = th_v[:-1]
                th[-1] = th_v[-1]
                t_det_loc = t_det_loc.copy()
                t_det_loc[act] = t_v
""")
rep("        err = ea[ok] / amp[pick][ok] - 1.0\n", """        err = ea[ok] / amp[pick][ok] - 1.0
        dtn = (et[ok] - t_ev[pick][ok]) / T
        tstat = {}
        if ok.sum() >= 30:
            t1, t2_, t3 = np.percentile(dtn, [15.865, 50, 84.135])
            tstat = dict(dt_med=float(t2_), dt_sig=float((t3 - t1) / 2))
""")
rep("                        sigma=sg, fwhm=(2.3548 * sg * 100 if sg else None))",
    "                        sigma=sg, fwhm=(2.3548 * sg * 100 if sg else None), **tstat)")
rep("                    lsb_uV=lsb * 1e6, e_out_nV=e_out * 1e9,",
    "                    lsb_uV=lsb * 1e6, e_out_nV=e_out * 1e9, method=METHOD, seed=SEED, det_bias=det_bias,")
rep("    dur = 2e-3 if QUICK else 8e-3\n", """    dur = 2e-3 if QUICK else 8e-3
    if _arg('--fs', None):
        fs_list = [float(x) * 1e6 for x in _arg('--fs', '').split(',')]
    if _arg('--ratios', None):
        ratios = [float(x) for x in _arg('--ratios', '').split(',')]
    if _arg('--rates', None):
        rates = [float(x) for x in _arg('--rates', '').split(',')]
    dur = float(_arg('--dur', dur))
""")
rep("os.path.join(OUT, 'mc_pileup.json')",
    "os.path.join(OUT, 'mc_pileup_%s_s%d.json' % (METHOD, SEED))", count=2)
rep("""                       ('%6.2f%%' % r['trap']['fwhm']) if r['trap']['fwhm'] else 'n/a'))""",
    """                       ('%6.2f%%' % r['trap']['fwhm']) if r['trap']['fwhm'] else 'n/a'),
                      ' dt[отсч] %+.3f ± %.3f' % (r['fit2'].get('dt_med', np.nan),
                                                 r['fit2'].get('dt_sig', np.nan)))""")

# Систематика (одинаково для всех методов): высвечивание в СИГНАЛЕ = TD*TDK,
# эталоны обработки остаются на TD. По умолчанию TDK=1 — стенд не меняется.
rep("        y[j:j + L] += amp[i] * pulse((off - frac[i]) * T, tt)\n",
    "        y[j:j + L] += amp[i] * pulse((off - frac[i]) * T, tt, td=TD * TDK)\n")
s = s.replace("SEED = int(", "TDK = float(_arg('--tdk', '1.0'))\nSEED = int(", 1)
open(DST, 'w', encoding='utf-8').write(s)
print('written', DST)
