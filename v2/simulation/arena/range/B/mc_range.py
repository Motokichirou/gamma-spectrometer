# -*- coding: utf-8 -*-
"""mc_range.py — арена «входной диапазон АЦП», сторона B.

Сравниваются ВАРИАНТЫ ФРОНТЕНДА между ЗЧУ и ПЛИС при ОДНОМ И ТОМ ЖЕ:
  - потоке событий (моменты, амплитуды из реального Cs-137),
  - аналоговом шуме ЗЧУ (та же реализация),
  - шуме АЦП (та же нормальная последовательность, масштаб по SNR конкретного АЦП),
  - алгоритме обработки (детектор + кластерный МНК + 2 средних) — скопирован из
    v2/simulation/mc_csp.py без изменений, работает на «лестнице» в вольтах ЗЧУ.

Варианты:
  A_ideal  — как в mc_csp.py: идеальный 16-бит АЦП на 4.4 В, шума АЦП нет (эталон стенда).
  A_div    — реальный 16-бит АЦП 2 Вpp: делитель 2/4.4 перед драйвером, рампа 4 В.
  A_ramp   — реальный 16-бит АЦП 2 Вpp без делителя, рампа уменьшена до 1.5 В (сбросы чаще).
  B        — аналоговое выделение ступенек: CR с длинной τ_cr (+ ключ восстановления
             на время сброса), усиление G в 2 Вpp, цифровое обращение CR в ПЛИС
             s[n] = s[n-1] + (x[n] - d'·x[n-1])/c  (точное, d' с ошибкой ε по желанию).
  B14      — то же на 14-бит АЦП (свой SNR).
  B_r8     — B, но рампа ЗЧУ 8 В (±4 В): АЦП рампу не видит, сбросы вдвое реже.

Модель АЦП (новое против mc_csp.py, применено ко ВСЕМ «реальным» вариантам одинаково):
  σ_adc = (FS/(2√2))·10^(−SNR/20)  — тепловой шум АЦП, белый до Найквиста;
  клиппинг на краях окна; квантование bits на FS.
  Драйвер АЦП: e_drv·NG на выходе, полоса f_a. Резистор CR в B: √(4kT·R_cr) до усиления.

Запуск:  python mc_range.py [--seeds 5] [--dur 4e-3] [--rates 1e6] [--variants A_ideal,A_div,B]
"""
import argparse
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

# ---- константы ровно как в mc_csp.py
TD = 230e-9
CF = 100e-12
C_IN = 6e-12
Q_662 = 7.2e-12
STEP_662 = Q_662 / CF
EN = 4e-9
T_BLANK = 1e-6
KT4 = 4 * 1.380649e-23 * 300.0
NAV_DET = 3
HARD = 0.0
ADC_OFF = 0.0
AUTOZERO = True
REFINE = '--refine' in sys.argv     # ЛМ из mc_csp.py (D11: даёт те же числа, но медленнее)
_orig = None
if REFINE:
    sys.path.insert(0, os.path.abspath(os.path.join(HERE, '..', '..', '..')))
    import mc_csp as _orig          # только чтение функции solve_cluster

# ---- реальные компоненты (новое; одинаково для всех вариантов, где применимо)
ADC_FS = 2.0            # Vpp у быстрых 16/14-бит АЦП 125 Мвыб/с
SNR16 = 77.0            # dBFS: LTC2185 76.8, LTC2165 77, AD9268 78 — берём 77
SNR14 = 73.5            # dBFS: LTC2261-14 73.4, AD9255 ~74
E_DRV = 2e-9            # входной шум драйвера (класс ADA4930/ADA4932), нВ/√Гц
R_CR = 100.0            # резистор CR в варианте B (C = τ/R; 2 мкс -> 20 нФ)


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


def hstep(t, ta, td=TD):
    t = np.asarray(t, dtype=float)
    out = np.zeros(np.shape(t))
    m = t > 0
    out[m] = 1.0 - (td * np.exp(-t[m] / td) - ta * np.exp(-t[m] / ta)) / (td - ta)
    return out


def lp1(w, fs, f_c):
    """Белый шум через однополюсный ФНЧ (как в mc_csp.py)."""
    N = len(w)
    f = np.fft.rfftfreq(N, 1.0 / fs)
    return np.fft.irfft(np.fft.rfft(w) / (1.0 + 1j * f / f_c), N)


# ================================================================ генерация (общая)

def generate(fs, rate, dur, seed, f_pp, cont, v_ramp):
    """События + чистая лестница со сбросами + реализации шумов. Зависит от seed,
    а НЕ от варианта (кроме v_ramp, влияющего только на моменты сбросов)."""
    T = 1.0 / fs
    f_a = fs / 2.5
    ta = 1.0 / (2 * np.pi * f_a)
    N = int(dur / T)
    L = int(8 * TD / T) + 2
    r_ev = np.random.default_rng([seed, 1])
    n_ev = int(r_ev.poisson(rate * dur))
    t_ev = np.sort(r_ev.random(n_ev) * dur)
    amp = np.ones(n_ev)
    k = r_ev.random(n_ev) >= f_pp
    amp[k] = np.interp(r_ev.random(int(k.sum())) * (len(cont) - 1), np.arange(len(cont)), cont)
    is_pp = ~k
    if HARD > 0:                      # доля событий континуума -> потолок 3.5 МэВ (380 мВ); для ВСЕХ вариантов
        hh = np.where(k)[0]
        hh = hh[r_ev.random(len(hh)) < HARD / max(1e-9, 1 - f_pp)]
        amp[hh] = 0.380 / STEP_662

    y = np.zeros(N + L + 4)
    idx = (t_ev / T).astype(int)
    frac = t_ev / T - idx
    off = np.arange(L)
    for i in range(n_ev):
        j = idx[i]
        if j + L >= len(y):
            continue
        y[j:j + L] += amp[i] * hstep((off - frac[i]) * T, ta)
        y[j + L:] += amp[i]
    y = y[:N] * STEP_662

    blank = np.zeros(N, bool)
    nb = int(T_BLANK / T) + 1
    lvl_arr = np.empty(N)
    lvl = 0.0
    resets = []
    for i in range(N):
        if y[i] - lvl > v_ramp:
            resets.append(i)
            lvl = y[i]
            blank[i:i + nb] = True
        lvl_arr[i] = lvl
    y = y - lvl_arr

    r_n = np.random.default_rng([seed, 2])
    e_out = EN * (1.0 + C_IN / CF)
    nz = lp1(r_n.standard_normal(N) * e_out * np.sqrt(fs / 2.0), fs, f_a)
    # нормированные реализации для драйвера / резистора CR / АЦП — общие для всех вариантов
    w_drv = lp1(np.random.default_rng([seed, 3]).standard_normal(N) * np.sqrt(fs / 2.0), fs, f_a)
    w_r = lp1(np.random.default_rng([seed, 4]).standard_normal(N) * np.sqrt(fs / 2.0), fs, f_a)
    w_adc = np.random.default_rng([seed, 5]).standard_normal(N)
    return dict(T=T, f_a=f_a, ta=ta, N=N, L=L, t_ev=t_ev, amp=amp, is_pp=is_pp,
                y=y, nz=nz, w_drv=w_drv, w_r=w_r, w_adc=w_adc, blank=blank,
                resets=resets, nb=nb)


# ================================================================ фронтенды

def adc(v, lo, bits, snr, w_adc):
    """Окно [lo, lo+ADC_FS], тепловой шум по SNR, клиппинг, квантование."""
    sig = ADC_FS / (2 * np.sqrt(2)) * 10 ** (-snr / 20.0) if snr else 0.0
    v = v + sig * w_adc
    clip = (v < lo) | (v > lo + ADC_FS)
    v = np.clip(v, lo, lo + ADC_FS)
    lsb = ADC_FS / (1 << bits)
    q = np.round((v - lo) / lsb) * lsb + lo
    return q, float(clip.mean()), sig, lsb


def fe_ideal(g):
    lsb = 4.4 / (1 << 16)
    y = np.round((g['y'] + g['nz']) / lsb) * lsb
    return y, dict(clip=0.0, sig_adc_ref_uV=0.0, lsb_ref_uV=lsb * 1e6)


def fe_dc(g, k, lo, bits=16, snr=SNR16):
    """DC-связь: ЗЧУ -> (делитель k) -> драйвер с усилением 1 (NG=2) -> АЦП."""
    v = (g['y'] + g['nz']) * k + E_DRV * 2.0 * g['w_drv']
    q, clip, sig, lsb = adc(v, lo, bits, snr, g['w_adc'])
    return q / k, dict(clip=clip, sig_adc_ref_uV=sig / k * 1e6, lsb_ref_uV=lsb / k * 1e6)


def fe_cr(g, tau, G, lo, bits=16, snr=SNR16, eps=0.0):
    """B: CR(τ) с ключом восстановления на время сброса, усиление G, АЦП,
    цифровое обращение CR с τ' = τ·(1+eps)."""
    T, N, blank = g['T'], g['N'], g['blank']
    v_in = g['y'] + g['nz'] + np.sqrt(KT4 * R_CR) * g['w_r']
    d = np.exp(-T / tau)
    c = tau / T * (1.0 - d)                       # точный CR для линейного хода между отсчётами
    dy = np.diff(v_in, prepend=v_in[0])
    x = np.empty(N)
    s = 0.0
    for i in range(N):                            # IIR; в окне сброса ключ держит выход в нуле
        if blank[i]:
            s = 0.0
        else:
            s = d * s + c * dy[i]
        x[i] = s
    # драйвер с усилением G: шум на выходе e_drv·(1+G)
    v = G * x + E_DRV * (1.0 + G) * g['w_drv'] + ADC_OFF
    q, clip, sig, lsb = adc(v, lo, bits, snr, g['w_adc'])
    if AUTOZERO:      # в окне сброса ключ держит x = 0 => АЦП читает собственный сдвиг
        q = q - q[blank].mean()
    xr = q / G
    # цифровое обращение (в ПЛИС: одно умножение-сложение и аккумулятор)
    tp = tau * (1.0 + eps)
    dp = np.exp(-T / tp)
    cp = tp / T * (1.0 - dp)
    inc = np.empty(N)
    inc[0] = 0.0
    inc[1:] = (xr[1:] - dp * xr[:-1]) / cp
    inc[blank] = 0.0
    # аккумулятор обнуляется в окне сброса (как у ЗЧУ)
    st = np.cumsum(inc)
    bstart = np.where(blank & ~np.r_[False, blank[:-1]])[0]
    seg_id = np.searchsorted(bstart, np.arange(N), side='right')
    base = np.r_[0.0, st[bstart]]
    st = st - base[seg_id]
    st[blank] = 0.0
    return st, dict(clip=clip, sig_adc_ref_uV=sig / G * 1e6, lsb_ref_uV=lsb / G * 1e6,
                    x_mean_mV=float(x[~blank].mean() * 1e3),
                    x_max_mV=float(x.max() * 1e3), x_p999_mV=float(np.percentile(x, 99.9) * 1e3))


# ================================================================ обработка (из mc_csp.py без изменений)

def solve_cluster(seg, tl, tq, ta):
    H = np.empty((len(seg), len(tq) + 1))
    for q, t0 in enumerate(tq):
        H[:, q] = hstep(tl - t0, ta)
    H[:, -1] = 1.0
    G = H.T @ H
    try:
        th = np.linalg.solve(G + 1e-12 * np.trace(G) / len(G) * np.eye(len(G)), H.T @ seg)
    except np.linalg.LinAlgError:
        return None
    return th[:-1], tq


def process(g, y, L_arm=64):
    T, N, L, ta, blank = g['T'], g['N'], g['L'], g['ta'], g['blank']
    t_ev, amp, is_pp = g['t_ev'], g['amp'], g['is_pp']
    n_ev = len(t_ev)
    NAV = NAV_DET              # 3 как в mc_csp.py; --nav меняет ОДИНАКОВО для всех вариантов
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
    above = np.where(s > thr)[0]
    above = above[(above > 0) & (above < N - 1)]
    if len(above) < 30:
        return None
    brk = np.where(np.diff(above) > HOLD)[0]
    groups = np.split(above, brk + 1)
    i0 = np.array([gg[int(np.argmax(s[gg]))] for gg in groups])
    i0 = i0[(i0 > 0) & (i0 < N - 1)]
    a0, a1, a2 = s[i0 - 1], s[i0], s[i0 + 1]
    den = a0 - 2 * a1 + a2
    sub = np.where(np.abs(den) > 1e-18, 0.5 * (a0 - a2) / np.where(den == 0, 1, den), 0.0)
    t_det = (i0 + np.clip(sub, -1, 1)) * T

    W_n = int(4 * TD / T) + 3
    MARG = int(5 * TD / T) + 2
    gap_min = 2.0 * ta
    K_MAX = 8
    est_t, est_a, est_m = [], [], []
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
        if REFINE:
            res = _orig.solve_cluster(y[n_lo:n_hi], np.arange(n_lo, n_hi) * T, tg.copy(), ta, refine=True)
        else:
            res = solve_cluster(y[n_lo:n_hi], np.arange(n_lo, n_hi) * T, tg.copy(), ta)
        if res is None:
            continue
        aa, tt_ = res[0], res[1]
        for q in range(len(grp)):
            est_t.append(tt_[q])
            est_a.append(aa[q] / STEP_662)
            est_m.append(len(grp))
    est_t, est_a, est_m = map(np.array, (est_t, est_a, est_m))

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

    out = {}
    tol = max(2.5 * T, 25e-9)
    n_pp = max(1, int(is_pp.sum()))
    for tag, sel in (('fit', np.ones(len(est_t), bool)), ('solo', est_m == 1)):
        et, ea = est_t[sel], est_a[sel]
        if len(et) < 30:
            out[tag] = dict(n=0, eff=0.0, err=[])
            continue
        k = np.clip(np.searchsorted(t_ev, et), 1, max(1, n_ev - 1))
        pick = np.where(np.abs(t_ev[k] - et) < np.abs(t_ev[k - 1] - et), k, k - 1)
        gg = (np.abs(t_ev[pick] - et) < tol) & is_pp[pick]
        err = ea[gg] / amp[pick][gg] - 1.0
        out[tag] = dict(n=int(gg.sum()), eff=float(gg.sum()) / n_pp, err=err.tolist())
    out['twoavg'] = dict(n=len(vals), eff=1.0, err=(np.array(vals) - 1.0).tolist())
    out['_'] = dict(n_ev=n_ev, n_pp=n_pp, n_det=nd, resets=len(g['resets']),
                    deadtime_pct=100.0 * blank.sum() / N, thr_mV=thr * 1e3, sg_det_uV=sg * 1e6)
    return out


def rsig(err):
    err = np.asarray(err)
    if len(err) < 30:
        return None
    q1, q3 = np.percentile(err, [15.865, 84.135])
    return float((q3 - q1) / 2)


# ================================================================ конфигурации

def variants(args):
    global SNR16, SNR14
    SNR16, SNR14 = args.snr16, args.snr14
    tau, G, lo = args.tau, args.gain, args.lo
    k = ADC_FS / 4.5            # 4.4 В + поля на шум
    return {
        'A_ideal': (4.0, lambda g: fe_ideal(g)),
        'A_div':   (4.0, lambda g: fe_dc(g, k, -0.04, 16, SNR16)),
        'A_div_q': (4.0, lambda g: fe_dc(g, k, -0.04, 16, None)),   # только кванты+драйвер, без шума АЦП
        'A_div14': (4.0, lambda g: fe_dc(g, k, -0.04, 14, SNR14)),
        'A_ramp':  (1.5, lambda g: fe_dc(g, 1.0, -0.05, 16, SNR16)),
        'B':       (4.0, lambda g: fe_cr(g, tau, G, lo, 16, SNR16, args.eps)),
        'B14':     (4.0, lambda g: fe_cr(g, tau, G, lo, 14, SNR14, args.eps)),
        'B_r8':    (8.0, lambda g: fe_cr(g, tau, G, lo, 16, SNR16, args.eps)),
        'B_eps':   (4.0, lambda g: fe_cr(g, tau, G, lo, 16, SNR16, args.eps2)),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seeds', type=int, default=5)
    ap.add_argument('--seed0', type=int, default=20261009)
    ap.add_argument('--dur', type=float, default=4e-3)
    ap.add_argument('--fs', type=float, default=125e6)
    ap.add_argument('--rates', default='1e6')
    ap.add_argument('--variants', default='A_ideal,A_div,A_ramp,B,B14,B_r8')
    ap.add_argument('--tau', type=float, default=2e-6, help='τ CR в варианте B')
    ap.add_argument('--gain', type=float, default=2.5, help='усиление после CR')
    ap.add_argument('--lo', type=float, default=-0.25, help='нижний край окна АЦП, В на входе АЦП')
    ap.add_argument('--eps', type=float, default=0.0, help='ошибка τ при обращении CR')
    ap.add_argument('--eps2', type=float, default=0.01, help='ошибка τ для варианта B_eps')
    ap.add_argument('--out', default='range_B.json')
    ap.add_argument('--refine', action='store_true')
    ap.add_argument('--snr16', type=float, default=77.0)
    ap.add_argument('--snr14', type=float, default=73.5)
    ap.add_argument('--nav', type=int, default=3)
    ap.add_argument('--adc_off', type=float, default=0.0, help='сдвиг нуля АЦП, В')
    ap.add_argument('--no_autozero', action='store_true')
    ap.add_argument('--hard', type=float, default=0.0, help='доля всех событий с энергией 3.5 МэВ')
    args = ap.parse_args()
    global NAV_DET, HARD
    NAV_DET = args.nav
    HARD = args.hard
    global ADC_OFF, AUTOZERO
    ADC_OFF = args.adc_off
    AUTOZERO = not args.no_autozero

    f_pp, cont = cs_spectrum()
    Ea = f_pp + (1 - f_pp) * cont.mean()
    Ea2 = f_pp + (1 - f_pp) * (cont ** 2).mean()
    print('Cs-137: доля фотопика %.3f, <a>=%.3f, <a²>=%.3f (в долях фотопика)' % (f_pp, Ea, Ea2))
    print('B: τ_cr=%.1f мкс, G=%.2f, окно АЦП [%.2f, %.2f] В => на входе CR [%.0f, %.0f] мВ'
          % (args.tau * 1e6, args.gain, args.lo, args.lo + ADC_FS,
             args.lo / args.gain * 1e3, (args.lo + ADC_FS) / args.gain * 1e3))
    vs = variants(args)
    names = args.variants.split(',')
    rates = [float(r) for r in args.rates.split(',')]
    allres = []
    for rate in rates:
        acc = {n: dict(fit=[], solo=[], twoavg=[], eff=[], effs=[], dead=[], clip=[], meta=[])
               for n in names}
        for si in range(args.seeds):
            seed = args.seed0 + si
            gens = {}
            for n in names:
                ramp, fe = vs[n]
                if ramp not in gens:
                    gens[ramp] = generate(args.fs, rate, args.dur, seed, f_pp, cont, ramp)
                g = gens[ramp]
                t0 = time.time()
                yd, meta = fe(g)
                r = process(g, yd)
                if r is None:
                    print('  %s seed %d: детектор пуст' % (n, seed))
                    continue
                a = acc[n]
                a['fit'].append(rsig(r['fit']['err']))
                a['solo'].append(rsig(r['solo']['err']))
                a['twoavg'].append(rsig(r['twoavg']['err']))
                a['eff'].append(r['fit']['eff'])
                a['effs'].append(r['solo']['eff'])
                a['dead'].append(r['_']['deadtime_pct'])
                a['clip'].append(meta['clip'])
                meta.update(r['_'])
                a['meta'].append(meta)
                print('  rate %.0e seed %d %-8s fit %6.2f%% свет %5.1f%% | 2ср %7.2f%% | мёртв %.2f%% '
                      'клип %.1e | σ_АЦП %5.0f мкВ thr %.2f мВ  (%.0f с)' %
                      (rate, seed, n, 235.48 * (a['fit'][-1] or np.nan), 100 * a['eff'][-1],
                       235.48 * (a['twoavg'][-1] or np.nan), a['dead'][-1], a['clip'][-1],
                       meta['sig_adc_ref_uV'], meta['thr_mV'], time.time() - t0))
                sys.stdout.flush()
        print()
        print('=== %.0f имп/с, fs %.0f МГц, %d зёрен × %.1f мс ===' %
              (rate, args.fs / 1e6, args.seeds, args.dur * 1e3))
        print('%-8s | %-16s | %-13s | %-16s | %-16s | %-6s | %s' %
              ('вариант', 'FWHM фит, %', 'свет, %', 'FWHM одиноч, %', 'FWHM 2 ср., %', 'мёртв', 'клип'))
        for n in names:
            a = acc[n]
            def ms(v, k=235.48):
                v = np.array([x for x in v if x is not None]) * k
                return v.mean(), v.std(ddof=1) / np.sqrt(len(v)) if len(v) > 1 else 0.0
            f = ms(a['fit']); so = ms(a['solo']); tw = ms(a['twoavg']); e = ms(a['eff'], 100)
            print('%-8s | %6.2f ± %5.2f   | %5.1f ± %4.1f  | %6.2f ± %5.2f   | %7.2f ± %5.2f  | %5.2f%% | %.1e' %
                  (n, f[0], f[1], e[0], e[1], so[0], so[1], tw[0], tw[1],
                   np.mean(a['dead']), np.mean(a['clip'])))
            allres.append(dict(variant=n, rate=rate, fs=args.fs, fwhm_fit=f, eff=e, fwhm_solo=so,
                               fwhm_twoavg=tw, dead=float(np.mean(a['dead'])),
                               clip=float(np.mean(a['clip'])), meta=a['meta'],
                               per_seed_fit=[235.48 * x if x else None for x in a['fit']]))
        print()
    with open(os.path.join(HERE, args.out), 'w', encoding='utf-8') as fh:
        json.dump(dict(args=vars(args), results=allres), fh, ensure_ascii=False, indent=1)
    print('-> %s' % os.path.join(HERE, args.out))


if __name__ == '__main__':
    main()
