# -*- coding: utf-8 -*-
"""Генератор mc_csp_vp.py из оригинального v2/simulation/mc_csp.py (оригинал не трогается).

Запуск:  python v2/simulation/arena/d11/A/patch.py
Делает копию и вносит правки строковыми заменами — так судье видно, что ровно изменено.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, '..', '..', '..', 'mc_csp.py'))
DST = os.path.join(HERE, 'mc_csp_vp.py')
s = open(SRC, encoding='utf-8').read()


def rep(old, new):
    global s
    assert s.count(old) == 1, ('не найдено однозначно', old[:80])
    s = s.replace(old, new)


rep("ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))",
    "ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..', '..'))")
rep("NO_REFINE = '--no-refine' in sys.argv", """NO_REFINE = '--no-refine' in sys.argv


def _arg(name, default):
    for a in sys.argv:
        if a.startswith(name + '='):
            return a.split('=', 1)[1]
    return default


# none   — без уточнения
# lm     — оригинал D11 как есть
# lm_fix — тот же ЛМ, исправлен только знак шага
# vp     — variable projection + ЛМ по моментам (Kaufman), масштаб = отсчёт
# grid   — сетка+парабола по каждому событию на VP-остатке, затем vp
# oracle — истинные моменты обнаруженных событий, без уточнения (потолок)
METHOD = _arg('--method', 'none' if NO_REFINE else 'lm')
SEED = int(_arg('--seed', '20261009'))""")
rep("rng = np.random.default_rng(20261009)", "rng = np.random.default_rng(SEED)")

NEW_SOLVER = '''
def _vp_fit(seg, tl, tt_, ta):
    """Variable projection: амплитуды и постоянная исключены МНК аналитически (QR)."""
    H = np.empty((len(seg), len(tt_) + 1))
    for q, t0 in enumerate(tt_):
        H[:, q] = hstep(tl - t0, ta)
    H[:, -1] = 1.0
    Qm, Rm = np.linalg.qr(H)
    dg = np.abs(np.diag(Rm))
    if dg.min() < 1e-9 * dg.max():
        return None, None, np.inf
    th = np.linalg.solve(Rm, Qm.T @ seg)
    r = seg - H @ th
    return th, Qm, float(r @ r)


def _sep_ok(tt_, T):
    return len(tt_) < 2 or np.min(np.diff(np.sort(tt_))) >= 0.3 * T


def _vp_lm(seg, tl, tq, ta, T, th, Qm, rn, maxit=10):
    """ЛМ по моментам в редуцированной задаче, параметр = момент в отсчётах.

    r(p) = P_perp(p) y;  якобиан Kaufman: J_q = -P_perp (dH/dp_q) th_q
                                              = T th_q P_perp h'(t - t_q).
    r(p + dp) ~ r + J dp  =>  dp = -(J'J + lam D)^-1 J'r.
    Шаг принимается только при уменьшении нормы остатка.
    """
    lam = 1e-3
    for _ in range(maxit):
        r = seg - Qm @ (Qm.T @ seg)
        M = np.empty((len(seg), len(tq)))
        for q in range(len(tq)):
            M[:, q] = T * th[q] * dhstep(tl - tq[q], ta)
        J = M - Qm @ (Qm.T @ M)
        A = J.T @ J
        g = J.T @ r
        d = np.diag(A).copy()
        d[d <= 0] = 1.0
        accepted = False
        dp = np.zeros(len(tq))
        while lam < 1e4:
            try:
                dp = -np.linalg.solve(A + lam * np.diag(d), g)
            except np.linalg.LinAlgError:
                lam *= 10
                continue
            dp = np.clip(dp, -1.0, 1.0)             # не дальше отсчёта за шаг
            tq_try = tq + dp * T
            if not _sep_ok(tq_try, T):
                lam *= 10
                continue
            th2, Q2, rn2 = _vp_fit(seg, tl, tq_try, ta)
            if th2 is not None and rn2 < rn:
                tq, th, Qm, rn = tq_try, th2, Q2, rn2
                lam = max(lam / 3, 1e-7)
                accepted = True
                break
            lam *= 10
        if not accepted or np.max(np.abs(dp)) < 1e-3:
            break
    return th, tq, Qm, rn


def _grid_sweep(seg, tl, tq, ta, T, th, Qm, rn, sweeps=2, h=0.5):
    """По каждому событию: VP-норма остатка в t-h, t, t+h отсчёта -> вершина параболы."""
    tq = tq.copy()
    for _ in range(sweeps):
        for q in range(len(tq)):
            vals = {}
            for k in (-h, h):
                t2 = tq.copy()
                t2[q] += k * T
                vals[k] = _vp_fit(seg, tl, t2, ta)[2] if _sep_ok(t2, T) else np.inf
            fm, f0, fp = vals[-h], rn, vals[h]
            cand = []
            if np.isfinite(fm) and np.isfinite(fp):
                den = fm - 2 * f0 + fp
                if den > 0:
                    cand.append(float(np.clip(0.5 * h * (fm - fp) / den, -1.5, 1.5)))
            cand += [k for k in (-h, h) if vals[k] < rn]
            for x in cand:
                t2 = tq.copy()
                t2[q] += x * T
                if not _sep_ok(t2, T):
                    continue
                th2, Q2, rn2 = _vp_fit(seg, tl, t2, ta)
                if th2 is not None and rn2 < rn:
                    tq, th, Qm, rn = t2, th2, Q2, rn2
    return th, tq, Qm, rn


def solve_cluster(seg, tl, tq, ta, refine=True, maxit=4, method=None):
    method = method or ('lm' if refine else 'none')
    if method in ('vp', 'grid'):
        T = tl[1] - tl[0]
        th, Qm, rn = _vp_fit(seg, tl, tq, ta)
        if th is None:
            return None
        if method == 'grid':
            th, tq, Qm, rn = _grid_sweep(seg, tl, tq, ta, T, th, Qm, rn)
        th, tq, Qm, rn = _vp_lm(seg, tl, tq, ta, T, th, Qm, rn)
        return th[:-1], tq, rn
    return _solve_cluster_orig(seg, tl, tq, ta, refine=(method in ('lm', 'lm_fix')),
                               maxit=maxit, sign=(+1 if method == 'lm_fix' else -1))


def _solve_cluster_orig(seg, tl, tq, ta, refine=True, maxit=4, sign=-1):'''
rep("\ndef solve_cluster(seg, tl, tq, ta, refine=True, maxit=4):", NEW_SOLVER)
rep("        tq_try = tq - dt\n",
    "        tq_try = tq + sign * dt          # sign=-1 — как в оригинале D11\n")
rep("        res = solve_cluster(seg, tl, tg.copy(), ta, refine=refine)",
    """        tg = tg.copy()
        if METHOD == 'oracle':
            # истинные моменты тех обнаруженных событий, у которых нашлась пара
            k_ = np.clip(np.searchsorted(t_ev, tg), 1, max(1, n_ev - 1))
            pk = np.where(np.abs(t_ev[k_] - tg) < np.abs(t_ev[k_ - 1] - tg), k_, k_ - 1)
            okk = np.abs(t_ev[pk] - tg) < max(2.5 * T, 25e-9)
            tg[okk] = t_ev[pk][okk]
            if len(tg) > 1 and np.min(np.diff(np.sort(tg))) <= 0:
                continue
        res = solve_cluster(seg, tl, tg, ta,
                            method=('none' if METHOD == 'oracle' else METHOD))""")
rep("        err = ea[g] / amp[pick][g] - 1.0\n", """        err = ea[g] / amp[pick][g] - 1.0
        dtn = (et[g] - t_ev[pick][g]) / T
        if g.sum() >= 30:
            t1, t2_, t3 = np.percentile(dtn, [15.865, 50, 84.135])
            tstat = dict(dt_med=float(t2_), dt_sig=float((t3 - t1) / 2))
        else:
            tstat = {}
""")
rep("                        fwhm=(2.3548 * sgv * 100 if sgv else None))",
    "                        fwhm=(2.3548 * sgv * 100 if sgv else None), **tstat)")
rep("refine=refine)\n    return out", "refine=refine, method=METHOD, seed=SEED)\n    return out")
rep("    fs_list = [40e6, 65e6, 125e6]\n", """    fs_list = [40e6, 65e6, 125e6]
    if _arg('--fs', None):
        fs_list = [float(x) * 1e6 for x in _arg('--fs', '').split(',')]
""")
rep("    dur = 2e-3 if QUICK else 4e-3\n", """    dur = 2e-3 if QUICK else 4e-3
    if _arg('--rates', None):
        rates = [float(x) for x in _arg('--rates', '').split(',')]
""")
s = s.replace("os.path.join(OUT, 'mc_csp.json')",
    "os.path.join(OUT, 'mc_csp_%s_s%d%s.json' % (METHOD, SEED, '_q' if QUICK else ''))")
rep("""    print('спектр Cs-137 из pulsegen.c, доля фотопика %.3f; уточнение моментов: %s'
          % (f_pp, 'выключено' if NO_REFINE else 'ЛМ с откатом'))""",
    """    print('спектр Cs-137 из pulsegen.c, доля фотопика %.3f; метод: %s, seed %d'
          % (f_pp, METHOD, SEED))""")
rep("""            print('%7.0f %8.1f %9.0f | %-18s %-18s %-14s %5d (%.2f%%)' %
                  (fs / 1e6, f_a / 1e6, rate, fmt(r['fit']), fmt(r['solo']),
                   fmt(r['twoavg']), r['_']['resets'], r['_']['deadtime_pct']))""",
    """            print('%7.0f %8.1f %9.0f | %-18s %-18s %-14s %5d (%.2f%%)  dt[отсч] %+.3f ± %.3f' %
                  (fs / 1e6, f_a / 1e6, rate, fmt(r['fit']), fmt(r['solo']),
                   fmt(r['twoavg']), r['_']['resets'], r['_']['deadtime_pct'],
                   r['fit'].get('dt_med', np.nan), r['fit'].get('dt_sig', np.nan)))""")
# --- 'off' / 'off_vp': систематический сдвиг детектора, откалиброванный на БЕСШУМНОМ
# эталоне (без знания истинных моментов событий): та же цепочка sharpen + парабола
# на 16 фазах, среднее смещение пика относительно начала ступеньки.
rep("    t_det = (i0 + np.clip(sub, -1, 1)) * T\n", """    t_det = (i0 + np.clip(sub, -1, 1)) * T
    _b = []
    for ph in np.arange(16) / 16.0:
        v = STEP_662 * hstep((np.arange(L + 20) - 10 - ph) * T, ta)
        sv = sharpen(v)
        m_ = int(np.argmax(sv[2:-2])) + 2
        c0, c1, c2 = sv[m_ - 1], sv[m_], sv[m_ + 1]
        dd = c0 - 2 * c1 + c2
        _b.append(m_ + (0.5 * (c0 - c2) / dd if dd != 0 else 0.0) - (10 + ph))
    det_bias = float(np.mean(_b))                 # в отсчётах
    if METHOD in ('off', 'off_vp'):
        t_det = t_det - det_bias * T
""")
rep("            t_det = (i0 + np.clip(sub, -1, 1)) * T\n" if False else
    "                            method=('none' if METHOD == 'oracle' else METHOD))",
    "                            method={'oracle': 'none', 'off': 'none', 'off_vp': 'vp'}.get(METHOD, METHOD))")
rep("refine=refine, method=METHOD, seed=SEED)", "refine=refine, method=METHOD, seed=SEED, det_bias=det_bias)")

# Систематика (одинаково для всех методов): высвечивание в СИГНАЛЕ = TD*TDK,
# эталоны обработки остаются на TD. По умолчанию TDK=1 — стенд не меняется.
rep("        y[j:j + L] += amp[i] * hstep((off - frac[i]) * T, ta)\n",
    "        y[j:j + L] += amp[i] * hstep((off - frac[i]) * T, ta, td=TD * TDK)\n")
s = s.replace("SEED = int(", "TDK = float(_arg('--tdk', '1.0'))\nSEED = int(", 1)
open(DST, 'w', encoding='utf-8').write(s)
print('written', DST)
