# -*- coding: utf-8 -*-
"""Арена D8, сторона A (ЗЧУ). Копия v2/simulation/mc_csp.py с ОПЦИЯМИ.

Без опций (solver='cluster', sys='none') физика и алгоритм совпадают с mc_csp.py
построчно; отличие только в том, что ГСЧ задаётся зерном на прогон (seed), а не
одной общей последовательностью на всю таблицу, — чтобы обе ветки и все варианты
шли на одинаковых зёрнах.

Добавлено:
  solver='block'  — МНК блоками с полями по всей записи (как в ветке TIA, mc_pileup.py),
                    колонки = ступеньки всех найденных событий + ступеньки сбросов +
                    постоянная. Плечи до/после ступеньки сами растягиваются до соседей —
                    это оптимальная оценка ступенек в белом шуме при известных моментах.
                    В исходном solve_cluster плечо «до» было 2 отсчёта.
  систематика (общая с tia.py, модуль common_sys.py):
    td реального события ≠ td эталона/полюс-нуля (температура, разброс кристаллов),
    медленная компонента высвечивания (баллистический дефицит).
"""
import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..', '..'))
sys.path.insert(0, HERE)
from common_sys import draw_td, slow_frac, unique_match  # noqa: E402
from fitcore import vp_refine, build, Refiner  # noqa: E402

TD = 230e-9
CF = 100e-12
C_IN = 6e-12
Q_662 = 7.2e-12
STEP_662 = Q_662 / CF
V_FS = 4.4
ADC_BITS = 16
EN = 4e-9
V_RAMP = 4.0
T_BLANK = 1e-6

rng = None


def cs_spectrum():
    src = open(os.path.join(ROOT, 'firmware', 'core', 'pulsegen.c'), encoding='utf-8').read()
    body = re.search(r'invcdf\[INVCDF_LEN\]\s*=\s*\{(.*?)\};', src, re.S).group(1)
    body = re.sub(r'/\*.*?\*/|//[^\n]*', '', body, flags=re.S)
    vals = np.array([int(x) for x in re.findall(r'\d+', body)], float)
    assert len(vals) == 1024, len(vals)
    lo = 0.5 * np.percentile(vals, 95)
    edges = np.arange(lo, vals.max() + 10, 10)
    hist, _ = np.histogram(vals, edges)
    j = int(np.argmax(hist))
    sel = vals[(vals >= edges[j] - 20) & (vals < edges[j + 1] + 20)]
    P = float(np.median(sel))
    rel = vals / P
    return float(np.mean(np.abs(rel - 1) < 0.1)), rel[rel < 0.85]


def draw_amplitudes(n, f_pp, cont):
    a = np.ones(n)
    k = rng.random(n) >= f_pp
    a[k] = np.interp(rng.random(int(k.sum())) * (len(cont) - 1), np.arange(len(cont)), cont)
    return a, ~k


def hstep(t, ta, td=TD):
    t = np.asarray(t, dtype=float)
    out = np.zeros(np.shape(t))
    m = t > 0
    out[m] = 1.0 - (td * np.exp(-t[m] / td) - ta * np.exp(-t[m] / ta)) / (td - ta)
    return out


def hstep2(t, ta, td, fslow, tslow):
    """Ступенька с медленной компонентой: доля fslow с постоянной tslow."""
    if fslow <= 0:
        return hstep(t, ta, td)
    return (1 - fslow) * hstep(t, ta, td) + fslow * hstep(t, ta, tslow)


def hstep_b(t, ta, td):
    """hstep с бродкастом по td (для фита td каждого события)."""
    tp = np.maximum(t, 0.0)
    v = 1.0 - (td * np.exp(-tp / td) - ta * np.exp(-tp / ta)) / (td - ta)
    return np.where(t > 0, v, 0.0)


def dhstep(t, ta, td=TD):
    t = np.asarray(t, dtype=float)
    out = np.zeros(np.shape(t))
    m = t > 0
    out[m] = (np.exp(-t[m] / td) - np.exp(-t[m] / ta)) / (td - ta)
    return out


def solve_cluster(seg, tl, tq, ta, refine=True, maxit=4, sign=-1.0):
    """Исходный решатель mc_csp.py без изменений."""
    def build(tt_):
        H = np.empty((len(seg), len(tt_) + 1))
        for q, t0 in enumerate(tt_):
            H[:, q] = hstep(tl - t0, ta)
        H[:, -1] = 1.0
        return H

    def fit(tt_):
        H = build(tt_)
        G = H.T @ H
        try:
            th = np.linalg.solve(G + 1e-12 * np.trace(G) / len(G) * np.eye(len(G)), H.T @ seg)
        except np.linalg.LinAlgError:
            return None, None, np.inf
        r = seg - H @ th
        return th, H, float(r @ r)

    th, H, rn = fit(tq)
    if th is None:
        return None
    if not refine:
        return th[:-1], tq, rn
    lam = 1e-3
    T = tl[1] - tl[0]
    for _ in range(maxit):
        r = seg - H @ th
        J = np.empty((len(seg), len(tq)))
        for q in range(len(tq)):
            J[:, q] = -th[q] * dhstep(tl - tq[q], ta)
        A = J.T @ J
        d = np.diag(A).copy()
        d[d <= 0] = 1.0
        try:
            dt = np.linalg.solve(A + lam * np.diag(d), J.T @ r)
        except np.linalg.LinAlgError:
            break
        dt = np.clip(dt, -0.5 * T, 0.5 * T)
        tq_try = tq + sign * dt   # исходник: tq - dt (sign=-1). J = dm/dt, шаг Гаусса-Ньютона = +dt
        if np.any(np.diff(np.sort(tq_try)) < 0.3 * T):
            lam *= 10
            continue
        th2, H2, rn2 = fit(tq_try)
        if th2 is not None and rn2 < rn:
            tq, th, H, rn = tq_try, th2, H2, rn2
            lam = max(lam / 3, 1e-6)
        else:
            lam *= 10
            if lam > 1e3:
                break
    return th[:-1], tq, rn


def robust_fwhm(err):
    q1, q3 = np.percentile(err, [15.865, 84.135])
    return 2.3548 * float((q3 - q1) / 2) * 100, float(np.median(err)) * 100


def run(seed, fs=125e6, rate=1e6, dur=4e-3, solver='cluster', sysmode='none',
        refine=True, L_arm=64, slow=0.0, detector='group',
        cf=CF, en_scale=1.0, nav=3, adc_bits=ADC_BITS, oracle=False, lm_fix=False, redetect=0, debug=False,
        k_slow=16, slow_k=5.0, slow_floor=0.01, near_min=4, fit_td=False):
    global rng
    rng = np.random.default_rng(seed)
    STEP_662 = Q_662 / cf
    f_pp, cont = cs_spectrum()
    T = 1.0 / fs
    f_a = fs / 2.5
    ta = 1.0 / (2 * np.pi * f_a)
    N = int(dur / T)
    fslow, tslow = slow_frac(slow)
    tmax = max(TD, tslow if fslow > 0 else TD) * 1.1
    L = int(8 * tmax / T) + 2

    n_ev = int(rng.poisson(rate * dur))
    t_ev = np.sort(rng.random(n_ev) * dur)
    amp, is_pp = draw_amplitudes(n_ev, f_pp, cont)
    td_ev = draw_td(seed, n_ev, t_ev, dur, sysmode, TD)

    y = np.zeros(N + L + 4)
    idx = (t_ev / T).astype(int)
    frac = t_ev / T - idx
    off = np.arange(L)
    for i in range(n_ev):
        j = idx[i]
        if j + L >= len(y):
            continue
        y[j:j + L] += amp[i] * hstep2((off - frac[i]) * T, ta, td_ev[i], fslow, tslow)
        y[j + L:] += amp[i]
    y = y[:N] * STEP_662

    blank = np.zeros(N, bool)
    nb = int(T_BLANK / T) + 1
    lvl_arr = np.empty(N)
    lvl = 0.0
    resets = []
    for i in range(N):
        if y[i] - lvl > V_RAMP:
            resets.append(i)
            lvl = y[i]
            blank[i:i + nb] = True
        lvl_arr[i] = lvl
    y = y - lvl_arr

    e_out = en_scale * EN * (1.0 + C_IN / cf)
    w = rng.standard_normal(N) * e_out * np.sqrt(fs / 2.0)
    f = np.fft.rfftfreq(N, T)
    nz = np.fft.irfft(np.fft.rfft(w) / (1.0 + 1j * f / f_a), N)
    y = y + nz

    lsb = V_FS / (1 << adc_bits)
    y = np.round(y / lsb) * lsb

    # --- обнаружение (как в mc_csp.py, эталонный td = TD)
    NAV = nav
    d_pz = np.exp(-T / TD)

    def sharpen(v):
        a = np.empty(len(v))
        a[0] = 0.0
        a[1:] = v[1:] - v[:-1]
        b = np.empty(len(a))
        b[0] = a[0]
        b[1:] = a[1:] - d_pz * a[:-1]
        return np.convolve(b, np.ones(NAV) / NAV, mode='same')

    s = sharpen(y)
    ps = sharpen(STEP_662 * hstep(np.arange(L) * T, ta))
    s = s / (ps.max() / STEP_662)
    s[blank] = 0.0
    seg0 = s[:min(N, 1 << 15)]
    sg = 1.4826 * np.median(np.abs(seg0 - np.median(seg0)))
    thr = max(5.0 * sg, 0.03 * STEP_662)
    HOLD = max(2, int(2.0 * ta / T) + 1)
    if detector == 'group':
        # исходный детектор mc_csp.py: непрерывный участок выше порога = одно событие
        above = np.where(s > thr)[0]
        above = above[(above > 0) & (above < N - 1)]
        brk = np.where(np.diff(above) > HOLD)[0]
        groups = np.split(above, brk + 1)
        i0 = np.array([g[int(np.argmax(s[g]))] for g in groups])
    else:
        # локальные максимумы (как в ветке TIA); максимумы ближе HOLD отсчётов —
        # ничьи квантования, из них берётся больший
        i0 = np.where((s[1:-1] > thr) & (s[1:-1] >= s[:-2]) & (s[1:-1] > s[2:]))[0] + 1
        if len(i0) > 1:
            keep = np.ones(len(i0), bool)
            for q in range(1, len(i0)):
                if i0[q] - i0[q - 1] <= HOLD:
                    if s[i0[q]] > s[i0[q - 1]]:
                        keep[q - 1] = False
                    else:
                        keep[q] = False
            i0 = i0[keep]
    i0 = i0[(i0 > 0) & (i0 < N - 1)]
    a0, a1, a2 = s[i0 - 1], s[i0], s[i0 + 1]
    den = a0 - 2 * a1 + a2
    sub = np.where(np.abs(den) > 1e-18, 0.5 * (a0 - a2) / np.where(den == 0, 1, den), 0.0)
    t_det = (i0 + np.clip(sub, -1, 1)) * T
    if oracle:   # ДИАГНОСТИКА: истинные моменты вместо детектора
        t_det = t_ev[~blank[np.minimum((t_ev / T).astype(int), N - 1)]].copy()

    gap_min = 2.0 * ta
    est_t, est_a, est_m = [], [], []
    if solver == 'cluster':
        W_n = int(4 * TD / T) + 3
        MARG = int(5 * TD / T) + 2
        K_MAX = 8
        nd = len(t_det)
        i = 0
        while i < nd:
            j = i
            while j + 1 < nd and (t_det[j + 1] - t_det[j]) < W_n * T:
                j += 1
            grp = list(range(i, j + 1))
            i = j + 1
            if len(grp) > K_MAX:
                continue
            tg = t_det[grp]
            if len(tg) > 1 and np.min(np.diff(tg)) < gap_min:
                continue
            n_lo = max(0, int(tg[0] / T) - 2)
            n_hi = min(N, int(tg[-1] / T) + MARG)
            if n_hi - n_lo < 8 + len(grp) or blank[n_lo:n_hi].any():
                continue
            res = solve_cluster(y[n_lo:n_hi], np.arange(n_lo, n_hi) * T, tg.copy(), ta,
                                refine=refine, sign=(1.0 if lm_fix else -1.0))
            if res is None:
                continue
            aa, tt_, _ = res
            for q, g in enumerate(grp):
                est_t.append(tt_[q])
                est_a.append(aa[q] / STEP_662)
                est_m.append(len(grp))
    else:
        # --- блочный МНК по всей записи
        Lf = int(8 * TD / T) + 2           # длина фронта эталона
        MARG = Lf
        CORE = max(4 * MARG, int(20e-6 / T))
        d_n = t_det / T
        too_close = np.zeros(len(t_det), bool)
        if len(t_det) > 1:
            dd = np.diff(t_det) < gap_min
            too_close[:-1] |= dd
            too_close[1:] |= dd
        res_arr = np.array(resets, int)
        K_SLOW = k_slow
        NEAR_MIN = near_min

        def slow_filter(v):
            c = np.concatenate([[0.0], np.cumsum(v)])
            n = len(v)
            idx = np.arange(n)
            a_hi = np.minimum(idx + K_SLOW, n)
            b_lo = np.maximum(idx - K_SLOW, 0)
            after = (c[a_hi] - c[idx]) / np.maximum(a_hi - idx, 1)
            before = (c[idx] - c[b_lo]) / np.maximum(idx - b_lo, 1)
            return (after - before) / (SLOW_PK * STEP_662)

        tmpl = hstep((np.arange(-4 * K_SLOW, 4 * K_SLOW + Lf) ) * T, ta)
        SLOW_PK = 1.0
        rsp = slow_filter(tmpl * STEP_662)
        SLOW_PK = float(rsp.max())
        SLOW_OFF = int(np.argmax(rsp)) - 4 * K_SLOW
        W_n = int(4 * TD / T) + 3
        for n_lo in range(0, N, CORE):
            n_hi = min(N, n_lo + CORE)
            s_lo, s_hi = max(0, n_lo - MARG), min(N, n_hi + MARG)
            sel = np.where((d_n > s_lo - Lf) & (d_n < s_hi))[0]
            if len(sel) == 0:
                continue
            rows = np.arange(s_lo, s_hi)
            keep = ~blank[s_lo:s_hi]
            tl = rows * T
            rr = res_arr[(res_arr > s_lo) & (res_arr < s_hi)]
            H = np.empty((len(rows), len(sel) + len(rr) + 1))
            for q, e in enumerate(sel):
                H[:, q] = hstep(tl - t_det[e], ta)
            for q, r0 in enumerate(rr):
                H[:, len(sel) + q] = (rows >= r0)
            H[:, -1] = 1.0
            H = H[keep]
            yy = y[s_lo:s_hi][keep]
            if solver == 'block':
                G = H.T @ H
                try:
                    th = np.linalg.solve(G + 1e-12 * np.trace(G) / len(G) * np.eye(len(G)), H.T @ yy)
                except np.linalg.LinAlgError:
                    continue
                for q, e in enumerate(sel):
                    if not (n_lo <= d_n[e] < n_hi) or too_close[e]:
                        continue
                    nbh = int(np.sum(np.abs(t_det - t_det[e]) < W_n * T)) - 1
                    est_t.append(t_det[e])
                    est_a.append(th[q] / STEP_662)
                    est_m.append(1 + nbh)
                continue
            # --- solver == 'vp': совместное уточнение моментов + повторный поиск в остатке
            X = H[:, len(sel):]
            tl_k = tl[keep]
            fH = lambda t: hstep(t, ta)
            dH = lambda t: dhstep(t, ta)
            tq = t_det[sel].copy()
            fixed = too_close[sel].copy()
            gH = lambda t, td: hstep_b(t, ta, td)
            R = Refiner(fH, dH, gH, TD, fit_td)
            th, tq, rn = R(yy, tl_k, tq, X, T)
            for _pass in range(redetect):
                # Повторный поиск в ОСТАТКЕ медленным детектором: разность двух средних
                # по K_SLOW отсчётов. Для ступеньки это почти согласованный фильтр,
                # двойного дифференцирования нет => порог в разы ниже, чем у быстрого.
                Xa = np.empty((len(rows), X.shape[1]))
                for q, r0 in enumerate(rr):
                    Xa[:, q] = (rows >= r0)
                Xa[:, -1] = 1.0
                th_all = np.linalg.lstsq(R.build(tl_k, X), yy, rcond=None)[0]
                res_b = y[s_lo:s_hi] - R.build(tl, Xa) @ th_all
                D = slow_filter(res_b)
                bl = blank[s_lo:s_hi]
                if bl.any():
                    bb = np.convolve(bl.astype(float), np.ones(2 * K_SLOW + 1), mode='same') > 0
                    D[bb] = 0.0
                sgD = 1.4826 * np.median(np.abs(D - np.median(D)))
                thrD = max(slow_k * sgD, slow_floor)
                cand = np.where((D[1:-1] > thrD) & (D[1:-1] >= D[:-2]) & (D[1:-1] > D[2:]))[0] + 1
                if len(cand) == 0:
                    break
                # не ближе NEAR_MIN к уже найденным и не ближе K_SLOW/2 друг к другу
                cand = cand[np.argsort(-D[cand])]
                tc = []
                for c in cand:
                    t_c = (rows[c] - SLOW_OFF) * T
                    if len(tq) and np.min(np.abs(tq - t_c)) < NEAR_MIN * T:
                        continue
                    if tc and np.min(np.abs(np.array(tc) - t_c)) < 0.5 * K_SLOW * T:
                        continue
                    tc.append(t_c)
                if not tc:
                    break
                tq = np.sort(np.concatenate([tq, np.array(tc)]))
                th, tq, rn = R(yy, tl_k, tq, X, T, n_iter=12, max_step=3.0)
                # спурии: амплитуда ниже минимального значимого шага — убрать и перерешать
                bad = th < 0.5 * slow_floor * STEP_662
                if bad.any():
                    tq = tq[~bad]
                    th, tq, rn = R(yy, tl_k, tq, X, T)
            for q in range(len(tq)):
                if not (n_lo * T <= tq[q] < n_hi * T):
                    continue
                if q > 0 and tq[q] - tq[q - 1] < gap_min or q + 1 < len(tq) and tq[q + 1] - tq[q] < gap_min:
                    continue
                nbh = int(np.sum(np.abs(tq - tq[q]) < W_n * T)) - 1
                est_t.append(tq[q])
                est_a.append(th[q] / STEP_662)
                est_m.append(1 + nbh)
    est_t, est_a, est_m = map(np.array, (est_t, est_a, est_m))

    # --- разность двух средних
    g_n = int(2.6 * TD / T) + 1
    arm = L_arm
    ti = (t_ev / T).astype(int)
    ok = (ti - arm >= 0) & (ti + g_n + arm < N) & is_pp
    vals = []
    for p in np.where(ok)[0]:
        if blank[ti[p] - arm:ti[p] + g_n + arm].any():
            continue
        before = y[ti[p] - arm:ti[p]].mean()
        after = y[ti[p] + g_n:ti[p] + g_n + arm].mean()
        vals.append((after - before) / STEP_662 / amp[p])

    out = dict(seed=seed, n_ev=n_ev, n_pp=int(is_pp.sum()), n_det=len(t_det), thr_rel=thr / STEP_662,
               resets=len(resets), dead=100.0 * blank.sum() / N)
    tol = max(2.5 * T, 25e-9)
    if len(est_t):
        _pk_all, _ok_all = unique_match(t_ev, est_t, tol)
        out['n_est'] = int(len(est_t))
        out['n_spur'] = int((~_ok_all).sum())      # оценки без истинного события (лишние/дубли)
    else:
        out['n_est'] = out['n_spur'] = 0
    for tag, sel in (('fit', np.ones(len(est_t), bool)), ('solo', est_m == 1)):
        et, ea = est_t[sel], est_a[sel]
        pick, g = unique_match(t_ev, et, tol)
        g = g & is_pp[pick]
        err = ea[g] / amp[pick][g] - 1.0
        fw, bias = robust_fwhm(err) if g.sum() >= 30 else (None, None)
        if debug and tag == 'fit':
            found = np.zeros(n_ev, bool)
            kk = np.clip(np.searchsorted(t_ev, est_t), 1, max(1, n_ev - 1))
            pk2 = np.where(np.abs(t_ev[kk] - est_t) < np.abs(t_ev[kk - 1] - est_t), kk, kk - 1)
            found[pk2[np.abs(t_ev[pk2] - est_t) < tol]] = True
            pp = pick[g]
            dprev = np.r_[np.inf, np.diff(t_ev)]
            dnext = np.r_[np.diff(t_ev), np.inf]
            nn = np.minimum(dprev, dnext)[pp]
            miss_nb = np.array([np.any(~found[(t_ev > t_ev[q] - 1.5e-6) & (t_ev < t_ev[q] + 1.5e-6)]) for q in pp])
            out['_dbg'] = dict(err=err.tolist(), nn=nn.tolist(), miss=miss_nb.tolist(),
                               found_frac=float(found.mean()),
                               miss_amp=amp[~found].tolist())
        out[tag] = dict(n=int(g.sum()), eff=float(g.sum()) / max(1, int(is_pp.sum())),
                        fwhm=fw, bias=bias)
    if len(vals) >= 30:
        fw, bias = robust_fwhm(np.array(vals) - 1.0)
    else:
        fw, bias = None, None
    out['twoavg'] = dict(n=len(vals), fwhm=fw, bias=bias)
    return out
