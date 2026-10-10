# -*- coding: utf-8 -*-
"""Арена D8, сторона A. Копия v2/simulation/mc_pileup.py (ветка TIA) с ОПЦИЯМИ.

Без опций (q_scale=1, cin=0, sys='none') физика и алгоритм совпадают с mc_pileup.py
(функция run, трапеция выкинута — она в споре не участвует); ГСЧ — зерно на прогон.

Опции:
  q_scale  множитель заряда на импульс. Стенд TIA написан под V_PEAK = 0.469 В при
           Zt = 5 кОм, то есть Q = 0.469·230н/5к = 21.6 пКл = (216 пКл v1)/10 — снижение
           усиления ФЭУ в 10 раз. Решение D4 (в силе) — в 30 раз, Q = 7.2 пКл, и именно
           7.2 пКл стоит в стенде ЗЧУ. q_scale = 1/3 ставит обе ветви на ОДИН заряд.
  rf       сопротивление обратной связи (по умолчанию 5 кОм, как в стенде)
  cin      входная ёмкость узла для шумового усиления e_n. В стенде ЗЧУ шум ОУ умножен
           на (1 + Cin/Cf), в стенде TIA — на 1. Для TIA шумовое усиление
           (1 + s·Rf·(Cin+Cf))/(1 + s·Rf·Cf), Cf = 1/(2π·Rf·f_tia): ноль на
           1/(2π·Rf·(Cin+Cf)), полюс — тот же f_tia, что у сигнала. cin = 6 пФ — как C_IN
           в стенде ЗЧУ.
  sys/slow систематика из common_sys.py, одинаковая с csp.py.
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
VREF = 2.5
ADC_BITS = 14
RF0 = 5000.0
V_PEAK_CS0 = 0.469
EN_OPAMP = 4e-9
KT4 = 4 * 1.380649e-23 * 300.0
TAIL = 20.0

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


def _pk(tt, td):
    t_pk = np.log(td / tt) / (1.0 / tt - 1.0 / td)
    return np.exp(-t_pk / td) - np.exp(-t_pk / tt)


def pulse(t, tt, td=TD):
    out = np.zeros(np.shape(t), dtype=float)
    t = np.asarray(t, dtype=float)
    m = t > 0
    out[m] = (np.exp(-t[m] / td) - np.exp(-t[m] / tt)) / _pk(tt, td)
    return out


def dpulse(t, tt, td=TD):
    out = np.zeros(np.shape(t), dtype=float)
    t = np.asarray(t, dtype=float)
    m = t > 0
    out[m] = (-np.exp(-t[m] / td) / td + np.exp(-t[m] / tt) / tt) / _pk(tt, td)
    return out


def pulse_b(t, tt, td):
    """Эталон, НОРМИРОВАННЫЙ НА ЗАРЯД, с бродкастом по td; при td = TD равен pulse()."""
    tp = np.maximum(t, 0.0)
    v = (np.exp(-tp / td) - np.exp(-tp / tt)) / (td - tt) * ((TD - tt) / _pk(tt, TD))
    return np.where(t > 0, v, 0.0)


def pulse_q(t, tt, td, fslow, tslow):
    """Отклик на заряд, нормированный так, что при td = TD и fslow = 0 он равен pulse()."""
    def one(tdx):
        out = np.zeros(np.shape(t), dtype=float)
        tq = np.asarray(t, dtype=float)
        m = tq > 0
        out[m] = (np.exp(-tq[m] / tdx) - np.exp(-tq[m] / tt)) / (tdx - tt)
        return out
    k = (TD - tt) / _pk(tt, TD)
    v = (1 - fslow) * one(td)
    if fslow > 0:
        v = v + fslow * one(tslow)
    return v * k


def robust_fwhm(err):
    q1, q3 = np.percentile(err, [15.865, 84.135])
    return 2.3548 * float((q3 - q1) / 2) * 100, float(np.median(err)) * 100


def run(seed, adc_snr=None, fs=125e6, rate=1e6, dur=4e-3, ratio=2.5, q_scale=1.0, rf=RF0, cin=0.0,
        sysmode='none', slow=0.0, solver='block', redetect=0, k_slow=16, slow_k=5.0,
        slow_floor=0.01, near_min=4, fit_td=False):
    global rng
    rng = np.random.default_rng(seed)
    f_pp, cont = cs_spectrum()
    V_PEAK_CS = V_PEAK_CS0 * q_scale * (rf / RF0)
    T = 1.0 / fs
    f_tia = fs / ratio
    tt = 1.0 / (2 * np.pi * f_tia)
    N = int(dur / T)
    fslow, tslow = slow_frac(slow)

    L = int(TAIL * TD / T) + 2
    Ls = L if fslow <= 0 else int(TAIL * tslow / T / 2) + 2   # хвост медленной компоненты
    NPH = 32
    bank = np.stack([pulse((np.arange(L) - ph / NPH) * T, tt) for ph in range(NPH)])

    n_ev = int(rng.poisson(rate * dur))
    t_ev = np.sort(rng.random(n_ev) * dur)
    amp, is_pp = draw_amplitudes(n_ev, f_pp, cont)
    td_ev = draw_td(seed, n_ev, t_ev, dur, sysmode, TD)

    LL = max(L, Ls)
    y = np.zeros(N + LL + 4)
    idx = (t_ev / T).astype(int)
    frac = t_ev / T - idx
    off = np.arange(LL)
    plain = (sysmode in (None, '', 'none')) and fslow <= 0
    for i in range(n_ev):
        j = idx[i]
        if j + L >= len(y):
            continue
        if plain:
            y[j:j + L] += amp[i] * pulse((off[:L] - frac[i]) * T, tt)
        else:
            seg = pulse_q((off - frac[i]) * T, tt, td_ev[i], fslow, tslow)
            n = min(LL, len(y) - j)
            y[j:j + n] += amp[i] * seg[:n]
    y = y[:N] * V_PEAK_CS

    # шум: Rf — белый через полюс; e_n — с шумовым усилением (если cin > 0)
    f = np.fft.rfftfreq(N, T)
    pole = 1.0 / (1.0 + 1j * f / f_tia)
    e_rf = np.sqrt(KT4 * rf)
    if cin <= 0:
        e_out = float(np.hypot(e_rf, EN_OPAMP))
        w = rng.standard_normal(N) * e_out * np.sqrt(fs / 2.0)
        nz = np.fft.irfft(np.fft.rfft(w) * pole, N)
    else:
        cf = 1.0 / (2 * np.pi * rf * f_tia)
        f_z = 1.0 / (2 * np.pi * rf * (cin + cf))
        w1 = rng.standard_normal(N) * e_rf * np.sqrt(fs / 2.0)
        w2 = rng.standard_normal(N) * EN_OPAMP * np.sqrt(fs / 2.0)
        nz = np.fft.irfft((np.fft.rfft(w1) + np.fft.rfft(w2) * (1 + 1j * f / f_z)) * pole, N)
    y = y + nz
    sigma_out = float(np.std(nz))

    if adc_snr is not None:   # СУДЬЯ: тепловой шум АЦП, модель стороны B, отдельный ГСЧ
        y = y + np.random.default_rng(seed + 99991).standard_normal(N) * (VREF / (2 * np.sqrt(2)) * 10 ** (-adc_snr / 20.0))
    lsb = VREF / (1 << ADC_BITS)
    y = np.round(np.clip(y, -VREF / 2, VREF) / lsb) * lsb

    NAV = 3
    d_pz = np.exp(-T / TD)
    s = np.empty(N)
    s[0] = y[0]
    s[1:] = y[1:] - d_pz * y[:-1]
    s = np.convolve(s, np.ones(NAV) / NAV, mode='same')
    probe = pulse(np.arange(L) * T, tt) * V_PEAK_CS
    ps = np.empty(L)
    ps[0] = probe[0]
    ps[1:] = probe[1:] - d_pz * probe[:-1]
    ps = np.convolve(ps, np.ones(NAV) / NAV, mode='same')
    s = s / (ps.max() / V_PEAK_CS)

    seg0 = s[:min(N, 1 << 15)]
    sg_s = 1.4826 * np.median(np.abs(seg0 - np.median(seg0)))
    thr = max(5.0 * sg_s, 0.02 * V_PEAK_CS)
    i0 = np.where((s[1:-1] > thr) & (s[1:-1] >= s[:-2]) & (s[1:-1] > s[2:]))[0] + 1
    a0, a1, a2 = s[i0 - 1], s[i0], s[i0 + 1]
    den = a0 - 2 * a1 + a2
    sub = np.where(np.abs(den) > 1e-15, 0.5 * (a0 - a2) / np.where(den == 0, 1, den), 0.0)
    t_det = (i0 + np.clip(sub, -1, 1)) * T

    W_n = int(4 * TD / T) + 3
    gap_min = 2.0 * tt
    MARG = L
    CORE = max(4 * MARG, int(20e-6 / T))
    d_idx = (t_det / T).astype(int)
    d_ph = np.rint((t_det / T - d_idx) * NPH).astype(int)
    d_idx = d_idx + (d_ph // NPH)
    d_ph = d_ph % NPH
    too_close = np.zeros(len(t_det), bool)
    if len(t_det) > 1:
        dd = np.diff(t_det) < gap_min
        too_close[:-1] |= dd
        too_close[1:] |= dd

    def slow_filter(v):
        pz = np.empty(len(v))
        pz[0] = v[0]
        pz[1:] = v[1:] - d_pz * v[:-1]
        c = np.concatenate([[0.0], np.cumsum(pz)])
        n = len(v)
        idx = np.arange(n)
        a_hi = np.minimum(idx + k_slow, n)
        return (c[a_hi] - c[idx]) / (SLOW_PK * V_PEAK_CS)

    SLOW_PK = 1.0
    rsp = slow_filter(np.concatenate([np.zeros(4 * k_slow), V_PEAK_CS * pulse(np.arange(L) * T, tt)]))
    SLOW_PK = float(rsp.max())
    SLOW_OFF = int(np.argmax(rsp)) - 4 * k_slow

    est_t, est_a, est_m = [], [], []
    for n_lo in range(0, N, CORE):
        n_hi = min(N, n_lo + CORE)
        s_lo, s_hi = max(0, n_lo - MARG), min(N, n_hi + MARG)
        sel = np.where((d_idx > s_lo - L) & (d_idx < s_hi))[0]
        if len(sel) == 0:
            continue
        M = s_hi - s_lo
        H = np.zeros((M, len(sel) + 1))
        for q, e in enumerate(sel):
            r0 = d_idx[e] - s_lo
            lo = max(0, r0)
            hi = min(M, r0 + L)
            if hi <= lo:
                continue
            H[lo:hi, q] = bank[d_ph[e]][lo - r0:hi - r0]
        H[:, -1] = 1.0
        if solver == 'vp':
            yy = y[s_lo:s_hi]
            tl = np.arange(s_lo, s_hi) * T
            X = np.ones((M, 1))
            fP = lambda t: pulse(t, tt)
            dP = lambda t: dpulse(t, tt)
            tq = t_det[sel].copy()
            gP = lambda t, td: pulse_b(t, tt, td)
            R = Refiner(fP, dP, gP, TD, fit_td)
            th, tq, rn = R(yy, tl, tq, X, T)
            for _pass in range(redetect):
                # тот же приём, что в csp.py: остаток -> полюс-нуль (импульс -> узкий пик)
                # -> сумма K_SLOW отсчётов. Для импульса это эквивалент разности двух
                # средних у ступеньки.
                HB = R.build(tl, X)
                res_b = yy - HB @ np.linalg.lstsq(HB, yy, rcond=None)[0]
                D = slow_filter(res_b)
                sgD = 1.4826 * np.median(np.abs(D - np.median(D)))
                thrD = max(slow_k * sgD, slow_floor)
                cand = np.where((D[1:-1] > thrD) & (D[1:-1] >= D[:-2]) & (D[1:-1] > D[2:]))[0] + 1
                if len(cand) == 0:
                    break
                cand = cand[np.argsort(-D[cand])]
                tc = []
                for c in cand:
                    t_c = (s_lo + c - SLOW_OFF) * T
                    if len(tq) and np.min(np.abs(tq - t_c)) < near_min * T:
                        continue
                    if tc and np.min(np.abs(np.array(tc) - t_c)) < 0.5 * k_slow * T:
                        continue
                    tc.append(t_c)
                if not tc:
                    break
                tq = np.sort(np.concatenate([tq, np.array(tc)]))
                th, tq, rn = R(yy, tl, tq, X, T, n_iter=12, max_step=3.0)
                bad = th < 0.5 * slow_floor * V_PEAK_CS
                if bad.any():
                    tq = tq[~bad]
                    th, tq, rn = R(yy, tl, tq, X, T)
            for q in range(len(tq)):
                if not (n_lo * T <= tq[q] < n_hi * T):
                    continue
                if q > 0 and tq[q] - tq[q - 1] < gap_min or q + 1 < len(tq) and tq[q + 1] - tq[q] < gap_min:
                    continue
                nb = int(np.sum(np.abs(tq - tq[q]) < 4 * TD)) - 1
                est_t.append(tq[q])
                est_a.append(th[q] / V_PEAK_CS)
                est_m.append(1 if nb == 0 else 1 + nb)
            continue
        G = H.T @ H
        try:
            th = np.linalg.solve(G + 1e-12 * np.eye(len(sel) + 1), H.T @ y[s_lo:s_hi])
        except np.linalg.LinAlgError:
            continue
        for q, e in enumerate(sel):
            if not (n_lo <= d_idx[e] < n_hi) or too_close[e]:
                continue
            nb = int(np.sum(np.abs(t_det - t_det[e]) < 4 * TD)) - 1
            est_t.append(t_det[e])
            est_a.append(th[q] / V_PEAK_CS)
            est_m.append(1 if nb == 0 else 1 + nb)
    est_t, est_a, est_m = map(np.array, (est_t, est_a, est_m))

    out = dict(seed=seed, n_ev=n_ev, n_pp=int(is_pp.sum()), n_det=len(t_det),
               sigma_out_uV=sigma_out * 1e6, v_peak=V_PEAK_CS)
    tol = max(2.5 * T, 25e-9)
    if len(est_t):
        _pk_all, _ok_all = unique_match(t_ev, est_t, tol)
        out['n_est'] = int(len(est_t))
        out['n_spur'] = int((~_ok_all).sum())      # оценки без истинного события (лишние/дубли)
    else:
        out['n_est'] = out['n_spur'] = 0
    for tag, sel in (('fit', np.ones(len(est_t), bool)), ('solo', est_m == 1)):
        et, ea = est_t[sel], est_a[sel]
        pick, ok = unique_match(t_ev, et, tol)
        ok = ok & is_pp[pick]
        err = ea[ok] / amp[pick][ok] - 1.0
        fw, bias = robust_fwhm(err) if ok.sum() >= 30 else (None, None)
        out[tag] = dict(n=int(ok.sum()), eff=float(ok.sum()) / max(1, int(is_pp.sum())),
                        fwhm=fw, bias=bias,
                        pk3=float(np.sum(np.abs(err) < 0.03)) / max(1, int(is_pp.sum())))
    return out
