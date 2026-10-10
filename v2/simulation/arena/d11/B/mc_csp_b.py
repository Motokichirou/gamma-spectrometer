# -*- coding: utf-8 -*-
"""mc_csp_b.py — арена D11, сторона B: «независимый быстрый канал момента».

Основа — копия v2/simulation/mc_csp.py (модель тракта, шум, АЦП, сбросы, обнаружение,
кластеры, МНК по ступенчатым эталонам — БЕЗ ИЗМЕНЕНИЙ, тот же генератор и сид
20261009, тот же порядок вызовов rng, поэтому запись при (fs, rate) та же, что в оригинале).

На ОДНОЙ И ТОЙ ЖЕ записи и ОДНОМ И ТОМ ЖЕ наборе обнаруженных событий/кластеров
считаются несколько способов задать моменты для амплитудного МНК:

  det      моменты из быстрого канала как в оригинале (парабола по 3 точкам, без поправки)
  lm       оригинальный защищённый ЛМ (код скопирован как есть)
  lmfix    тот же ЛМ, но с исправленным знаком шага (см. ниже) — для честности сравнения
  B_lut    СТОРОНА B: независимый канал момента = парабола + поправка задержки/фазы по
           таблице, снятой с бесшумного эталона быстрого канала (32..256 фаз). Амплитудный
           МНК решается при фиксированных моментах.
  B_tm     СТОРОНА B: независимый канал момента = локальный МНК сдвига эталона быстрого
           канала (обострённый импульс) по 2·K+1 точкам вокруг пика, старт с B_lut.
           Амплитудный МНК — при фиксированных моментах.
  oracle   истинные моменты (потолок: что даст ЛЮБОЕ уточнение моментов)

Сопоставление с истиной для ВСЕХ вариантов делается по моменту обнаружения t_det
(одинаковому), поэтому набор событий, по которым считается FWHM, идентичен —
разница в FWHM обусловлена только точностью амплитуды.
Дополнительно печатается вариант «match_own» — сопоставление по собственному моменту,
как в оригинальном стенде.

Запуск:
  python mc_csp_b.py                      # fs 40/65/125, загрузки 0.1/0.3/1 М, 4 мс — как оригинал
  python mc_csp_b.py --seeds 5 --only1M   # 5 независимых зёрен, только 1 Мимп/с (статистика)
"""
import json
import os
import re
import sys
import time

import numpy as np

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..', '..'))
OUT = os.path.join(HERE, 'out')


def argval(name, default):
    if name in sys.argv:
        return type(default)(sys.argv[sys.argv.index(name) + 1])
    return default


QUICK = '--quick' in sys.argv
ONLY1M = '--only1M' in sys.argv
NSEEDS = argval('--seeds', 1)
FS_ONLY = argval('--fs', 0.0)        # 0 = все
LM_MAXIT = argval('--maxit', 4)        # итераций ЛМ (оригинал: 4)
TD_TRUE_NS = argval('--td_true', 230.0)  # td в ГЕНЕРАТОРЕ; обработка всегда считает 230 нс

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


# ---------------------------------------------------------------- спектр (копия)

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


def dhstep(t, ta, td=TD):
    t = np.asarray(t, dtype=float)
    out = np.zeros(np.shape(t))
    m = t > 0
    out[m] = (np.exp(-t[m] / td) - np.exp(-t[m] / ta)) / (td - ta)
    return out


# ---------------------------------------------------------------- решатель кластера

def solve_cluster(seg, tl, tq, ta, refine=False, maxit=4, sign=-1.0):
    """Копия оригинала. sign=-1 — как в оригинале (tq_try = tq - dt),
    sign=+1 — исправленный знак шага Гаусса-Ньютона (tq_try = tq + dt)."""
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
    n_acc = 0
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
        tq_try = tq + sign * dt
        if np.any(np.diff(np.sort(tq_try)) < 0.3 * T):
            lam *= 10
            continue
        th2, H2, rn2 = fit(tq_try)
        if th2 is not None and rn2 < rn:
            tq, th, H, rn = tq_try, th2, H2, rn2
            lam = max(lam / 3, 1e-6)
            n_acc += 1
        else:
            lam *= 10
            if lam > 1e3:
                break
    solve_cluster.accepted += n_acc
    return th[:-1], tq, rn


solve_cluster.accepted = 0


# ---------------------------------------------------------------- быстрый канал

def make_sharpen(T):
    NAV = 3
    d_pz = np.exp(-T / TD)

    def sharpen(v):
        a = np.empty(len(v))
        a[0] = 0.0
        a[1:] = v[1:] - v[:-1]
        b = np.empty(len(a))
        b[0] = a[0]
        b[1:] = a[1:] - d_pz * a[:-1]
        return np.convolve(b, np.ones(NAV) / NAV, mode='same')
    return sharpen


def parabola(s, i0):
    a0, a1, a2 = s[i0 - 1], s[i0], s[i0 + 1]
    den = a0 - 2 * a1 + a2
    sub = np.where(np.abs(den) > 1e-18, 0.5 * (a0 - a2) / np.where(den == 0, 1, den), 0.0)
    return np.clip(sub, -1, 1)


def build_timing_lut(T, ta, sharpen, nph=256):
    """Бесшумная калибровка быстрого канала: истинная фаза phi -> показание параболы.
    Возвращает (x_sorted, corr_sorted) для периодической интерполяции
    corr(x mod 1) = (t_det - t_true)/T, а также банк обострённых эталонов для B_tm."""
    L = int(8 * TD / T) + 16
    j0 = 8
    xs, cs = [], []
    for k in range(nph):
        phi = k / nph
        v = STEP_662 * hstep((np.arange(L) - j0 - phi) * T, ta)
        s = sharpen(v)
        i0 = int(np.argmax(s[1:-1])) + 1
        sub = float(parabola(s, np.array([i0]))[0])
        x = i0 + sub
        xs.append(x % 1.0)
        cs.append(x - (j0 + phi))
    xs, cs = np.array(xs), np.array(cs)
    o = np.argsort(xs)
    return xs[o], cs[o]


def sharp_template(T, ta, sharpen):
    """Обострённый эталон на тонкой сетке (шаг T/64) для локального МНК сдвига."""
    OS = 64
    L = int(8 * TD / T) + 16
    j0 = 8
    # значения обострённого эталона в точке n при сдвиге: f(n - t0/T)
    grid = np.arange(-4 * OS, 8 * OS + 1) / OS     # относительное время в отсчётах
    vals = np.empty(len(grid))
    # строим через фазы: для фазы phi обостряем ступеньку и берём отсчёты
    for k in range(OS):
        phi = k / OS
        v = STEP_662 * hstep((np.arange(L) - j0 - phi) * T, ta)
        s = sharpen(v)
        for m in range(-4, 9):
            # отсчёт n = j0 + m, относительное время (n - j0 - phi) = m - phi
            g = m - phi
            idx = int(round((g + 4) * OS))
            if 0 <= idx < len(grid):
                vals[idx] = s[j0 + m]
    # недостающие (m=8, phi=0 и т.п.) заполнены; m - phi для m=-4..8, phi in [0,1)
    # покрывает [-4.98, 8]; grid[0]=-4 включительно
    return grid, vals


# ---------------------------------------------------------------- прогон

VARIANTS = ['det', 'lm', 'lmfix', 'B_const', 'B_lut', 'B_tm', 'B_tm+lm', 'oracle']


def run(fs, f_a, rate, dur, f_pp, cont, variants=VARIANTS, K_TM=2):
    T = 1.0 / fs
    ta = 1.0 / (2 * np.pi * f_a)
    N = int(dur / T)
    L = int(8 * TD / T) + 2

    n_ev = int(rng.poisson(rate * dur))
    t_ev = np.sort(rng.random(n_ev) * dur)
    amp, is_pp = draw_amplitudes(n_ev, f_pp, cont)

    y = np.zeros(N + L + 4)
    idx = (t_ev / T).astype(int)
    frac = t_ev / T - idx
    off = np.arange(L)
    for i in range(n_ev):
        j = idx[i]
        if j + L >= len(y):
            continue
        y[j:j + L] += amp[i] * hstep((off - frac[i]) * T, ta, td=TD_TRUE_NS * 1e-9)
        y[j + L:] += amp[i]
    y = y[:N] * STEP_662

    blank = np.zeros(N, bool)
    nb = int(T_BLANK / T) + 1
    lvl_arr = np.empty(N)
    lvl = 0.0
    for i in range(N):
        if y[i] - lvl > V_RAMP:
            lvl = y[i]
            blank[i:i + nb] = True
        lvl_arr[i] = lvl
    y = y - lvl_arr

    e_out = EN * (1.0 + C_IN / CF)
    w = rng.standard_normal(N) * e_out * np.sqrt(fs / 2.0)
    f = np.fft.rfftfreq(N, T)
    nz = np.fft.irfft(np.fft.rfft(w) / (1.0 + 1j * f / f_a), N)
    y = y + nz
    lsb = V_FS / (1 << ADC_BITS)
    y = np.round(y / lsb) * lsb

    # --- обнаружение (копия)
    sharpen = make_sharpen(T)
    s = sharpen(y)
    ps = sharpen(STEP_662 * hstep(np.arange(L) * T, ta))
    s = s / (ps.max() / STEP_662)
    s[blank] = 0.0
    seg0 = s[:min(N, 1 << 15)]
    sg = 1.4826 * np.median(np.abs(seg0 - np.median(seg0)))
    thr = max(5.0 * sg, 0.03 * STEP_662)
    HOLD = max(2, int(2.0 * ta / T) + 1)
    above = np.where(s > thr)[0]
    above = above[(above > 0) & (above < N - 1)]
    brk = np.where(np.diff(above) > HOLD)[0]
    groups = np.split(above, brk + 1)
    i0 = np.array([g[int(np.argmax(s[g]))] for g in groups])
    i0 = i0[(i0 > 0) & (i0 < N - 1)]
    sub = parabola(s, i0)
    t_det = (i0 + sub) * T
    nd = len(t_det)

    # --- СТОРОНА B: независимый канал момента -------------------------------
    t0_clock = time.time()
    xs, cs = build_timing_lut(T, ta, sharpen)
    xm = (i0 + sub) % 1.0
    corr = np.interp(xm, xs, cs, period=1.0)
    t_lut = (i0 + sub - corr) * T

    # B_tm: локальный МНК сдвига обострённого эталона по 2K+1 точкам вокруг пика:
    # s[n] ≈ a·g(n - u) + b, неизвестные a, b, u; Гаусс-Ньютон по u, 4 итерации,
    # старт с B_lut. Использует ТОЛЬКО быстрый канал, амплитудный МНК его не видит.
    grid, gv = sharp_template(T, ta, sharpen)
    gv = gv / (ps.max() / STEP_662)
    dg = np.gradient(gv, grid)
    u = t_lut / T
    n_loc = i0[:, None] + np.arange(-K_TM, K_TM + 1)[None, :]
    n_loc = np.clip(n_loc, 0, N - 1)
    S = s[n_loc]
    for _ in range(4):
        rel = n_loc - u[:, None]
        G = np.interp(rel, grid, gv, left=0.0, right=0.0)
        D = np.interp(rel, grid, dg, left=0.0, right=0.0)
        # линейные a, b при фиксированном u
        g_m = G.mean(1, keepdims=True)
        s_m = S.mean(1, keepdims=True)
        Gc, Sc = G - g_m, S - s_m
        a = (Gc * Sc).sum(1) / np.maximum((Gc * Gc).sum(1), 1e-30)
        r = Sc - a[:, None] * Gc
        Jc = -(a[:, None] * (D - D.mean(1, keepdims=True)))
        du = (Jc * r).sum(1) / np.maximum((Jc * Jc).sum(1), 1e-30)
        du = np.clip(du, -0.25, 0.25)
        u = u + du
    u = np.where(np.abs(u - t_lut / T) < 1.0, u, t_lut / T)    # страховка от ухода
    t_tm = u * T
    t_b_cost = time.time() - t0_clock

    # --- оракул: истинный момент ближайшего события (по t_det)
    k = np.clip(np.searchsorted(t_ev, t_det), 1, max(1, n_ev - 1))
    pick_det = np.where(np.abs(t_ev[k] - t_det) < np.abs(t_ev[k - 1] - t_det), k, k - 1)
    t_or = t_ev[pick_det].copy()
    # дубли: одно истинное событие найдено дважды -> оракул отдаёт истину только
    # ближайшей детекции, остальным — момент B_tm (иначе две одинаковые колонки МНК)
    best = {}
    for q in range(nd):
        p_ = pick_det[q]
        dq = abs(t_ev[p_] - t_det[q])
        if p_ not in best or dq < best[p_][1]:
            best[p_] = (q, dq)
    keep = np.zeros(nd, bool)
    keep[[v[0] for v in best.values()]] = True
    t_or[~keep] = t_tm[~keep]
    # только постоянная задержка быстрого канала (среднее по фазам из той же калибровки)
    t_const = t_det - float(np.mean(cs)) * T

    tsrc = {'det': (t_det, False, -1.0), 'lm': (t_det, True, -1.0),
            'lmfix': (t_det, True, +1.0), 'B_lut': (t_lut, False, -1.0),
            'B_tm': (t_tm, False, -1.0), 'oracle': (t_or, False, -1.0),
            'B_const': (t_const, False, -1.0), 'B_tm+lm': (t_tm, True, +1.0)}

    # --- кластеры (группировка по t_det — одинакова для всех вариантов)
    W_n = int(4 * TD / T) + 3
    MARG = int(5 * TD / T) + 2
    gap_min = 2.0 * ta
    K_MAX = 8
    clusters = []
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
        clusters.append((grp, n_lo, n_hi))

    tol = max(2.5 * T, 25e-9)
    out = {}
    for v in variants:
        tt_all, refine, sign = tsrc[v]
        solve_cluster.accepted = 0
        ea = np.full(nd, np.nan)
        et = np.full(nd, np.nan)
        for grp, n_lo, n_hi in clusters:
            seg = y[n_lo:n_hi]
            tl = np.arange(n_lo, n_hi) * T
            tq = tt_all[grp].copy()
            res = solve_cluster(seg, tl, tq, ta, refine=refine, sign=sign, maxit=LM_MAXIT)
            if res is None:
                continue
            aa, tt_, _ = res
            ea[grp] = aa / STEP_662
            et[grp] = tt_
        have = ~np.isnan(ea)
        # сопоставление по t_det (одинаковый набор событий для всех вариантов)
        g = have & (np.abs(t_ev[pick_det] - t_det) < tol) & is_pp[pick_det]
        err = ea[g] / amp[pick_det][g] - 1.0
        q1, q3 = np.percentile(err, [15.865, 84.135])
        fw = 2.3548 * (q3 - q1) / 2 * 100
        # сопоставление по собственному моменту (как в оригинальном стенде)
        e2 = et[have]
        a2 = ea[have]
        kk = np.clip(np.searchsorted(t_ev, e2), 1, max(1, n_ev - 1))
        pk = np.where(np.abs(t_ev[kk] - e2) < np.abs(t_ev[kk - 1] - e2), kk, kk - 1)
        g2 = (np.abs(t_ev[pk] - e2) < tol) & is_pp[pk]
        err2 = a2[g2] / amp[pk][g2] - 1.0
        q1, q3 = np.percentile(err2, [15.865, 84.135])
        fw2 = 2.3548 * (q3 - q1) / 2 * 100
        # ошибка момента на фотопике (относительно истины, в нс)
        dtm = (et[g] - t_ev[pick_det][g]) * 1e9
        out[v] = dict(fwhm=float(fw), n=int(g.sum()), eff=float(g.sum()) / max(1, int(is_pp.sum())),
                      fwhm_own=float(fw2), n_own=int(g2.sum()),
                      eff_own=float(g2.sum()) / max(1, int(is_pp.sum())),
                      dt_bias_ns=float(np.median(dtm)),
                      dt_sig_ns=float(1.4826 * np.median(np.abs(dtm - np.median(dtm)))),
                      lm_accepted=int(solve_cluster.accepted))
    out['_'] = dict(fs_MHz=fs / 1e6, rate=rate, n_ev=n_ev, n_det=nd, n_clusters=len(clusters),
                    t_b_cost_s=t_b_cost, T_ns=T * 1e9)
    return out


def main():
    global rng
    os.makedirs(OUT, exist_ok=True)
    f_pp, cont = cs_spectrum()
    fs_list = [40e6, 65e6, 125e6]
    rates = [3e5, 1e6] if QUICK else [1e5, 3e5, 1e6]
    dur = 2e-3 if QUICK else 4e-3
    rows = []
    for si in range(NSEEDS):
        seed = 20261009 + si          # si = 0 — ровно сид оригинала
        rng = np.random.default_rng(seed)
        print('=== сид %d' % seed)
        hdr = '%5s %8s | ' % ('fs', 'имп/с') + ' '.join('%-18s' % v for v in VARIANTS)
        print(hdr)
        print('-' * len(hdr))
        for fs in fs_list:
            for rate in rates:
                skip = (ONLY1M and rate != 1e6) or (FS_ONLY and abs(fs - FS_ONLY * 1e6) > 1)
                if skip:
                    # rng всё равно надо прокрутить так же, как оригинал — поэтому
                    # пропуск делается только для сида оригинала через полный прогон
                    # без вариантов (дешёвая генерация).
                    run(fs, fs / 2.5, rate, dur, f_pp, cont, variants=[])
                    continue
                r = run(fs, fs / 2.5, rate, dur, f_pp, cont)
                r['_']['seed'] = seed
                rows.append(r)
                print('%5.0f %8.0f | ' % (fs / 1e6, rate) +
                      ' '.join('%5.2f%% %+5.1f±%4.2f' % (r[v]['fwhm'], r[v]['dt_bias_ns'], r[v]['dt_sig_ns'])
                               for v in VARIANTS), flush=True)
    name = 'mc_csp_b%s%s_s%d_td%.0f%s.json' % ('_quick' if QUICK else '', '_1M' if ONLY1M else '', NSEEDS, TD_TRUE_NS, ('_fs%.0f' % FS_ONLY) if FS_ONLY else '') if LM_MAXIT == 4 else 'mc_csp_b_maxit%d_s%d_fs%.0f.json' % (LM_MAXIT, NSEEDS, FS_ONLY)
    with open(os.path.join(OUT, name), 'w', encoding='utf-8') as fo:
        json.dump(rows, fo, ensure_ascii=False, indent=1)
    print('результаты: %s' % os.path.join(OUT, name))


if __name__ == '__main__':
    main()
