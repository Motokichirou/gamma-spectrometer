# -*- coding: utf-8 -*-
"""arena_b.py — арена D8, сторона B (TIA). Оба стенда в одном файле с ОДИНАКОВОЙ систематикой.

Основа — копии v2/simulation/mc_pileup.py (ветка TIA) и v2/simulation/mc_csp.py (ветка ЗЧУ).
Оригиналы не тронуты. При --scenario base код даёт БИТ В БИТ то же, что оригиналы
(проверяется ключом --selftest).

Что добавлено (каждый пункт применяется к ОБЕИМ ветвям одинаково, если не сказано иное):

  td_true      истинная постоянная высвечивания NaI(Tl) (дрейф с температурой);
               генерация нормируется по ЗАРЯДУ: у TIA пик падает как ~1/td, у ЗЧУ шаг не меняется
  ta_k         истинная полоса усилителя / номинальная (несоответствие эталона по фронту)
  f_slow,ts    медленная компонента высвечивания, доля f_slow с постоянной ts (эталон её не знает)
  proc         'nom'   — обработка с номинальным td = 230 нс (как в оригиналах);
               'adapt' — td оценивается из сигнала по первой 1 мс записи МНК-регрессией
                         v[n] = d*v[n-1] + c в тихих промежутках (5 сумматоров в ПЛИС),
                         TIA: v = y, ЗЧУ: v = первая разность y (та же форма импульса).
                         Обработка (полюс-нуль детектора, эталоны, нормировка на заряд)
                         ведётся с оценённым td.
  adc_snr      тепловой шум АЦП, дБFS по синусу полной шкалы: sigma = span/(2*sqrt(2))*10^(-SNR/20).
               span: TIA 2.5 В (как в стенде), ЗЧУ 4.4 В (как в стенде).
  phys         ТОЛЬКО ДЛЯ TIA — исправление двух упрощений стенда mc_pileup.py, оба в пользу TIA:
                 1) рабочая точка D4 (усиление ФЭУ /30, 7.2 пКл): пик 0.469 -> 0.1565 В при Zt = 5 кОм
                    (в стенде ЗЧУ D4 уже учтено: шаг 72 мВ);
                 2) шум ОУ с шумовым усилением TIA 1 + jwRf(Cin+Cf)/(1+jwRfCf), Cin = 6 пФ
                    (как C_IN в стенде ЗЧУ), Cf из полюса f_tia: Cf = 1/(2*pi*Rf*f_tia) = 0.64 пФ.
               В стенде ЗЧУ шумовое усиление e_n*(1+Cin/Cf) учтено изначально.
  vk           ТОЛЬКО TIA, стресс своей же стороны: множитель на пик (<1 = хуже).

Метрика как в оригиналах: робастная sigma (половина 16-84 %) относительной ошибки заряда на
фотопике -> FWHM, светимость = доля событий фотопика, дошедших до спектра. Дополнительно
печатается медианное смещение (сдвиг пика) — в оригинальной метрике его не видно.

Запуск: python arena_b.py --selftest
        python arena_b.py --scenario base --seeds 10
        python arena_b.py --all --seeds 10
"""
import argparse
import json
import os
import sys
import time

import numpy as np

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, SIM)
import mc_pileup as P   # noqa: E402  только константы, спектр и функции формы; run() не используется
import mc_csp as C      # noqa: E402

TD = 230e-9
KT4 = P.KT4


# ============================================================ общие помощники

def cs_spectrum():
    return P.cs_spectrum()


def draw_amplitudes(rng, n, f_pp, cont):
    a = np.ones(n)
    k = rng.random(n) >= f_pp
    a[k] = np.interp(rng.random(int(k.sum())) * (len(cont) - 1), np.arange(len(cont)), cont)
    return a, ~k


def tia_integral(td, tt):
    """Интеграл импульса pulse(t,tt,td), нормированного на единичный пик."""
    t_pk = np.log(td / tt) / (1.0 / tt - 1.0 / td)
    pk = np.exp(-t_pk / td) - np.exp(-t_pk / tt)
    return (td - tt) / pk


def est_decay_ratio(v, i_det, T, n_use, k, pre_s, post_s, t_max, blank=None):
    """Оценка td отношением сумм: sum v[n+k] / sum v[n] = d^k.

    Пары (n, n+k) берутся из тихих промежутков: между n-post_s.. n+k+pre_s нет обнаружений,
    и с последнего обнаружения прошло не больше t_max. Отбор только по ВРЕМЕНИ, не по значению
    отсчёта — поэтому нет смещения из-за отбора по шуму (в отличие от регрессии с порогом).
    В ПЛИС: два сумматора и счётчик. Одна и та же функция для обеих ветвей.
    """
    n_use = min(n_use, len(v) - k - 1)
    i_det = np.sort(i_det)
    det = np.zeros(len(v), bool)
    det[i_det[i_det < len(v)]] = True
    if blank is not None:
        det |= blank
    # индекс последнего обнаружения на или до n
    last = np.maximum.accumulate(np.where(det, np.arange(len(v)), -10 ** 9))
    # следующее обнаружение строго после n
    nxt = np.minimum.accumulate(np.where(det, np.arange(len(v)), 10 ** 12)[::-1])[::-1]
    n = np.arange(n_use)
    since = n - last[n]
    ok = (since >= post_s) & (since * T <= t_max) & (nxt[np.minimum(n + 1, len(v) - 1)] > n + k + pre_s)
    n = n[ok]
    if len(n) < 50:
        return TD, len(n)
    s0 = float(v[n].sum())
    s1 = float(v[n + k].sum())
    if s0 <= 0 or s1 <= 0:
        return TD, len(n)
    d = (s1 / s0) ** (1.0 / k)
    d = float(np.clip(d, 0.5, 0.99999))
    return float(-T / np.log(d)), int(len(n))


def est_decay(v, i_det, T, n_use, v_min, pre_s, post_s, blank=None):
    """МНК-оценка d в v[n] = d*v[n-1] + c на тихих отсчётах первых n_use отсчётов.

    Тихий отсчёт: ни одного обнаружения в окне [n - post_s, n + pre_s] (то есть нет
    свежего фронта перед отсчётом и нет нового фронта сразу после). v[n-1] > v_min
    отсекает чистый шум. В ПЛИС это 5 сумматоров: Sx, Sy, Sxx, Sxy, N.
    Возвращает (td_hat, число пар).
    """
    n_use = min(n_use, len(v))
    bad = np.zeros(n_use + post_s + pre_s + 2, bool)
    for i in i_det[i_det < n_use + post_s]:
        lo = max(0, i - pre_s)
        bad[lo:i + post_s + 1] = True
    bad = bad[:n_use]
    if blank is not None:
        bad |= blank[:n_use]
    ok = ~bad
    ok[0] = False
    ok[1:] &= ~bad[:-1]
    x = v[:n_use - 1][ok[1:]]
    y = v[1:n_use][ok[1:]]
    m = x > v_min
    x, y = x[m], y[m]
    if len(x) < 50:
        return TD, len(x)
    A = np.vstack([x, np.ones_like(x)]).T
    d, c = np.linalg.lstsq(A, y, rcond=None)[0]
    d = float(np.clip(d, 0.5, 0.99999))
    return float(-T / np.log(d)), int(len(x))


# ============================================================ ветка TIA

def run_tia(rng, fs, rate, dur, f_pp, cont, cfg, ratio=2.5):
    td_true = cfg.get('td_true', TD)
    ta_k = cfg.get('ta_k', 1.0)
    f_slow = cfg.get('f_slow', 0.0)
    ts = cfg.get('ts', 1.5e-6)
    proc = cfg.get('proc', 'nom')
    snr = cfg.get('adc_snr', None)
    phys = cfg.get('phys', False)
    vk = cfg.get('vk', 1.0)

    T = 1.0 / fs
    f_tia = fs / ratio
    tt = 1.0 / (2 * np.pi * f_tia)          # номинальная полоса (эталон)
    tt_true = tt * ta_k                      # истинная
    N = int(dur / T)
    V_PK = (0.1565 if phys else P.V_PEAK_CS) * vk

    I_ref = tia_integral(TD, tt)
    gen_default = (td_true == TD and ta_k == 1.0 and f_slow == 0.0)

    def g_true(t):
        """Истинная форма, нормирована так, что заряд = заряду номинального импульса единичного пика."""
        if gen_default:
            return P.pulse(t, tt)
        out = (1 - f_slow) * P.pulse(t, tt_true, td_true) * (I_ref / tia_integral(td_true, tt_true))
        if f_slow:
            out = out + f_slow * P.pulse(t, tt_true, ts) * (I_ref / tia_integral(ts, tt_true))
        return out

    L = int(P.TAIL * TD / T) + 2
    L_gen = L if f_slow == 0 else max(L, int(8 * ts / T) + 2)

    n_ev = int(rng.poisson(rate * dur))
    t_ev = np.sort(rng.random(n_ev) * dur)
    amp, is_pp = draw_amplitudes(rng, n_ev, f_pp, cont)

    y = np.zeros(N + L_gen + 4)
    idx = (t_ev / T).astype(int)
    frac = t_ev / T - idx
    off = np.arange(L_gen)
    for i in range(n_ev):
        j = idx[i]
        if j + L_gen >= len(y):
            continue
        y[j:j + L_gen] += amp[i] * g_true((off - frac[i]) * T)
    y = y[:N] * V_PK

    f = np.fft.rfftfreq(N, T)
    Hf = 1.0 / (1.0 + 1j * f / f_tia)
    if not phys:
        e_out = P.e_out_density()
        w = rng.standard_normal(N) * e_out * np.sqrt(fs / 2.0)
        nz = np.fft.irfft(np.fft.rfft(w) * Hf, N)
    else:
        Rf, Cin = 5000.0, C.C_IN
        Cf = 1.0 / (2 * np.pi * Rf * f_tia)
        w1 = rng.standard_normal(N) * np.sqrt(KT4 * Rf) * np.sqrt(fs / 2.0)
        w2 = rng.standard_normal(N) * P.EN_OPAMP * np.sqrt(fs / 2.0)
        NG = 1.0 + 1j * 2 * np.pi * f * Rf * (Cin + Cf)      # *Hf = полное шумовое усиление
        nz = np.fft.irfft((np.fft.rfft(w1) + np.fft.rfft(w2) * NG) * Hf, N)
    y = y + nz
    sigma_an = float(np.std(nz))
    span = P.VREF
    sig_adc = 0.0
    if snr is not None:
        sig_adc = span / (2 * np.sqrt(2)) * 10 ** (-snr / 20.0)
        y = y + rng.standard_normal(N) * sig_adc

    lsb = P.VREF / (1 << P.ADC_BITS)
    y = np.round(np.clip(y, -P.VREF / 2, P.VREF) / lsb) * lsb

    NAV = 3

    def detect(td_p):
        d_pz = np.exp(-T / td_p)
        s = np.empty(N)
        s[0] = y[0]
        s[1:] = y[1:] - d_pz * y[:-1]
        s = np.convolve(s, np.ones(NAV) / NAV, mode='same')
        probe = P.pulse(np.arange(L) * T, tt, td_p) * V_PK
        ps = np.empty(L)
        ps[0] = probe[0]
        ps[1:] = probe[1:] - d_pz * probe[:-1]
        ps = np.convolve(ps, np.ones(NAV) / NAV, mode='same')
        s = s / (ps.max() / V_PK)
        seg0 = s[:min(N, 1 << 15)]
        sg_s = 1.4826 * np.median(np.abs(seg0 - np.median(seg0)))
        thr = max(5.0 * sg_s, 0.02 * V_PK)
        i0 = np.where((s[1:-1] > thr) & (s[1:-1] >= s[:-2]) & (s[1:-1] > s[2:]))[0] + 1
        if len(i0) == 0:
            return None, None
        a0, a1, a2 = s[i0 - 1], s[i0], s[i0 + 1]
        den = a0 - 2 * a1 + a2
        sub = np.where(np.abs(den) > 1e-15, 0.5 * (a0 - a2) / np.where(den == 0, 1, den), 0.0)
        return (i0 + np.clip(sub, -1, 1)) * T, i0

    td_p = TD
    n_pairs = 0
    t_det, i0 = detect(td_p)
    if t_det is None:
        return None
    if proc == 'adapt':
        for _ in range(2):
            td_p, n_pairs = est_decay_ratio(y, i0, T, int(1e-3 / T), 10, pre_s=2,
                                            post_s=int(np.ceil(40e-9 / T)) + 2, t_max=4 * TD)
            t_det, i0 = detect(td_p)
    elif isinstance(proc, float):
        td_p = proc
        t_det, i0 = detect(td_p)

    NPH = 32
    bank = np.stack([P.pulse((np.arange(L) - ph / NPH) * T, tt, td_p) for ph in range(NPH)])
    q_norm = tia_integral(td_p, tt) / I_ref        # A (пик эталона) -> заряд

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

    est_t, est_a = [], []
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
        G = H.T @ H
        try:
            th = np.linalg.solve(G + 1e-12 * np.eye(len(sel) + 1), H.T @ y[s_lo:s_hi])
        except np.linalg.LinAlgError:
            continue
        for q, e in enumerate(sel):
            if not (n_lo <= d_idx[e] < n_hi) or too_close[e]:
                continue
            est_t.append(t_det[e])
            est_a.append(th[q] / V_PK * q_norm)
    est_t, est_a = np.array(est_t), np.array(est_a)
    out = score(t_ev, amp, is_pp, est_t, est_a, T)
    out['_'] = dict(branch='TIA', td_used_ns=td_p * 1e9, td_true_ns=td_true * 1e9, n_pairs=n_pairs,
                    sigma_an_uV=sigma_an * 1e6, sigma_adc_uV=sig_adc * 1e6, v_pk_mV=V_PK * 1e3,
                    n_ev=n_ev, n_det=len(t_det))
    return out


# ============================================================ ветка ЗЧУ

def solve_cluster(seg, tl, tq, ta, td, refine=True, maxit=4):
    """Копия mc_csp.solve_cluster с параметром td."""
    def build(tt_):
        H = np.empty((len(seg), len(tt_) + 1))
        for q, t0 in enumerate(tt_):
            H[:, q] = C.hstep(tl - t0, ta, td)
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
            J[:, q] = -th[q] * C.dhstep(tl - tq[q], ta, td)
        A = J.T @ J
        d = np.diag(A).copy()
        d[d <= 0] = 1.0
        try:
            dt = np.linalg.solve(A + lam * np.diag(d), J.T @ r)
        except np.linalg.LinAlgError:
            break
        dt = np.clip(dt, -0.5 * T, 0.5 * T)
        tq_try = tq - dt
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


def run_csp(rng, fs, rate, dur, f_pp, cont, cfg, refine=True):
    td_true = cfg.get('td_true', TD)
    ta_k = cfg.get('ta_k', 1.0)
    f_slow = cfg.get('f_slow', 0.0)
    ts = cfg.get('ts', 1.5e-6)
    proc = cfg.get('proc', 'nom')
    snr = cfg.get('adc_snr', None)
    v_ramp = cfg.get('v_ramp', C.V_RAMP)
    v_fs = cfg.get('v_fs', C.V_FS)

    T = 1.0 / fs
    f_a = fs / 2.5
    ta = 1.0 / (2 * np.pi * f_a)
    ta_true = ta * ta_k
    N = int(dur / T)
    L = int(8 * TD / T) + 2
    gen_default = (td_true == TD and ta_k == 1.0 and f_slow == 0.0)
    L_gen = L if gen_default else max(L, int(8 * max(td_true, ts if f_slow else 0) / T) + 2)

    def h_true(t):
        if gen_default:
            return C.hstep(t, ta)
        out = (1 - f_slow) * C.hstep(t, ta_true, td_true)
        if f_slow:
            out = out + f_slow * C.hstep(t, ta_true, ts)
        return out

    n_ev = int(rng.poisson(rate * dur))
    t_ev = np.sort(rng.random(n_ev) * dur)
    amp, is_pp = draw_amplitudes(rng, n_ev, f_pp, cont)

    y = np.zeros(N + L_gen + 4)
    idx = (t_ev / T).astype(int)
    frac = t_ev / T - idx
    off = np.arange(L_gen)
    for i in range(n_ev):
        j = idx[i]
        if j + L_gen >= len(y):
            continue
        y[j:j + L_gen] += amp[i] * h_true((off - frac[i]) * T)
        y[j + L_gen:] += amp[i]
    y = y[:N] * C.STEP_662

    blank = np.zeros(N, bool)
    nb = int(C.T_BLANK / T) + 1
    lvl_arr = np.empty(N)
    lvl = 0.0
    resets = 0
    for i in range(N):
        if y[i] - lvl > v_ramp:
            resets += 1
            lvl = y[i]
            blank[i:i + nb] = True
        lvl_arr[i] = lvl
    y = y - lvl_arr

    e_out = C.EN * (1.0 + C.C_IN / C.CF)
    w = rng.standard_normal(N) * e_out * np.sqrt(fs / 2.0)
    f = np.fft.rfftfreq(N, T)
    nz = np.fft.irfft(np.fft.rfft(w) / (1.0 + 1j * f / f_a), N)
    y = y + nz
    sigma_an = float(np.std(nz))
    span = v_fs
    sig_adc = 0.0
    if snr is not None:
        sig_adc = span / (2 * np.sqrt(2)) * 10 ** (-snr / 20.0)
        y = y + rng.standard_normal(N) * sig_adc

    lsb = v_fs / (1 << C.ADC_BITS)
    y = np.round(y / lsb) * lsb

    NAV = 3
    HOLD = max(2, int(2.0 * ta / T) + 1)

    def detect(td_p):
        d_pz = np.exp(-T / td_p)

        def sharpen(v):
            a = np.empty(len(v))
            a[0] = 0.0
            a[1:] = v[1:] - v[:-1]
            b = np.empty(len(a))
            b[0] = a[0]
            b[1:] = a[1:] - d_pz * a[:-1]
            return np.convolve(b, np.ones(NAV) / NAV, mode='same')

        s = sharpen(y)
        ps = sharpen(C.STEP_662 * C.hstep(np.arange(L) * T, ta, td_p))
        s = s / (ps.max() / C.STEP_662)
        s[blank] = 0.0
        seg0 = s[:min(N, 1 << 15)]
        sg = 1.4826 * np.median(np.abs(seg0 - np.median(seg0)))
        thr = max(5.0 * sg, 0.03 * C.STEP_662)
        above = np.where(s > thr)[0]
        above = above[(above > 0) & (above < N - 1)]
        if len(above) < 30:
            return None, None
        brk = np.where(np.diff(above) > HOLD)[0]
        groups = np.split(above, brk + 1)
        i0 = np.array([g[int(np.argmax(s[g]))] for g in groups])
        i0 = i0[(i0 > 0) & (i0 < N - 1)]
        if len(i0) < 30:
            return None, None
        a0, a1, a2 = s[i0 - 1], s[i0], s[i0 + 1]
        den = a0 - 2 * a1 + a2
        sub = np.where(np.abs(den) > 1e-18, 0.5 * (a0 - a2) / np.where(den == 0, 1, den), 0.0)
        return (i0 + np.clip(sub, -1, 1)) * T, i0

    td_p = TD
    n_pairs = 0
    t_det, i0 = detect(td_p)
    if t_det is None:
        return None
    if proc == 'adapt':
        dy = np.zeros(N)
        dy[1:] = y[1:] - y[:-1]
        bl = blank.copy()
        bl[1:] |= blank[:-1]
        for _ in range(2):
            td_p, n_pairs = est_decay_ratio(dy, i0, T, int(1e-3 / T), 10, pre_s=2,
                                            post_s=int(np.ceil(40e-9 / T)) + 2, t_max=4 * TD, blank=bl)
            t_det, i0 = detect(td_p)
    elif isinstance(proc, float):
        td_p = proc
        t_det, i0 = detect(td_p)

    W_n = int(4 * TD / T) + 3
    MARG = int(5 * TD / T) + 2
    gap_min = 2.0 * ta
    K_MAX = 8
    est_t, est_a = [], []
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
        seg = y[n_lo:n_hi]
        tl = np.arange(n_lo, n_hi) * T
        res = solve_cluster(seg, tl, tg.copy(), ta, td_p, refine=refine)
        if res is None:
            continue
        aa, tt_, _ = res
        for q, g in enumerate(grp):
            est_t.append(tt_[q])
            est_a.append(aa[q] / C.STEP_662)
    est_t, est_a = np.array(est_t), np.array(est_a)
    out = score(t_ev, amp, is_pp, est_t, est_a, T)
    out['_'] = dict(branch='CSP', td_used_ns=td_p * 1e9, td_true_ns=td_true * 1e9, n_pairs=n_pairs,
                    sigma_an_uV=sigma_an * 1e6, sigma_adc_uV=sig_adc * 1e6,
                    n_ev=n_ev, n_det=nd, resets=resets, deadtime_pct=100.0 * blank.sum() / N)
    return out


# ============================================================ метрика

def score(t_ev, amp, is_pp, et, ea, T):
    tol = max(2.5 * T, 25e-9)
    n_ev = len(t_ev)
    if len(et) < 30:
        return dict(n=0, eff=0.0, fwhm=None, bias=None, err=np.array([]))
    k = np.clip(np.searchsorted(t_ev, et), 1, max(1, n_ev - 1))
    pick = np.where(np.abs(t_ev[k] - et) < np.abs(t_ev[k - 1] - et), k, k - 1)
    ok = (np.abs(t_ev[pick] - et) < tol) & is_pp[pick]
    err = ea[ok] / amp[pick][ok] - 1.0
    if ok.sum() >= 30:
        q1, q3 = np.percentile(err, [15.865, 84.135])
        sg = float((q3 - q1) / 2)
        bias = float(np.median(err))
    else:
        sg, bias = None, None
    return dict(n=int(ok.sum()), eff=float(ok.sum()) / max(1, int(is_pp.sum())),
                fwhm=(2.3548 * sg * 100 if sg else None),
                bias=(bias * 100 if bias is not None else None), err=err)


# ============================================================ сценарии

SCEN = {
    # имя: (cfg TIA, cfg ЗЧУ, пояснение)
    'base':        ({}, {}, 'как в оригинальных стендах'),
    'td+5_nom':    ({'td_true': 1.05 * TD}, {'td_true': 1.05 * TD}, 'td +5 % (~ -8 °C), эталон номинальный'),
    'td-5_nom':    ({'td_true': 0.95 * TD}, {'td_true': 0.95 * TD}, 'td -5 % (~ +8 °C), эталон номинальный'),
    'td+10_nom':   ({'td_true': 1.10 * TD}, {'td_true': 1.10 * TD}, 'td +10 %, эталон номинальный'),
    'td+10_adapt': ({'td_true': 1.10 * TD, 'proc': 'adapt'}, {'td_true': 1.10 * TD, 'proc': 'adapt'},
                    'td +10 %, td оценивается по 1 мс'),
    'td-10_nom':   ({'td_true': 0.90 * TD}, {'td_true': 0.90 * TD}, 'td -10 %, эталон номинальный'),
    'td-10_adapt': ({'td_true': 0.90 * TD, 'proc': 'adapt'}, {'td_true': 0.90 * TD, 'proc': 'adapt'},
                    'td -10 %, td оценивается по 1 мс'),
    'base_adapt':  ({'proc': 'adapt'}, {'proc': 'adapt'}, 'td номинальный, но всё равно оценивается'),
    'ta+20':       ({'ta_k': 1.2}, {'ta_k': 1.2}, 'полоса усилителя на 20 % уже эталонной'),
    'slow5':       ({'f_slow': 0.05}, {'f_slow': 0.05}, '5 % медленной компоненты 1.5 мкс, эталон её не знает'),
    'adc_eq74':    ({'adc_snr': 74.0}, {'adc_snr': 74.0}, 'шум АЦП, одинаковый SNR 74 дБFS'),
    'adc_real':    ({'adc_snr': 73.0}, {'adc_snr': 78.0}, 'шум АЦП: 14 бит 73 дБFS / 16 бит 78 дБFS'),
    'td-10_oracle': ({'td_true': 0.90 * TD, 'proc': 0.90 * TD}, {'td_true': 0.90 * TD, 'proc': 0.90 * TD},
                     'td -10 %, обработке сообщён точный td (оракул) — потолок для обеих'),
    'adc_eq78':    ({'adc_snr': 78.0}, {'adc_snr': 78.0}, 'шум АЦП, одинаковый SNR 78 дБFS'),
    'csp_adc82':   ({'adc_snr': 73.0}, {'adc_snr': 82.0}, 'АЦП: TIA 73, ЗЧУ 82 дБFS (лучше рыночного для 16 бит/125 М)'),
    'csp_adc86':   ({'adc_snr': 73.0}, {'adc_snr': 86.0}, 'АЦП: TIA 73, ЗЧУ 86 дБFS (фантастика)'),
    'csp_span2':   ({'adc_snr': 73.0}, {'adc_snr': 78.0, 'v_ramp': 1.6, 'v_fs': 2.0},
                    'ЗЧУ ход противника: рампа 1.6 В, диапазон 2.0 В (чаще сбросы)'),
    'phys':        ({'phys': True}, {}, 'TIA в рабочей точке D4 + шумовое усиление (ЗЧУ без изменений)'),
    'phys_adc':    ({'phys': True, 'adc_snr': 73.0}, {'adc_snr': 78.0}, 'phys + adc_real'),
    'tia_vk10':    ({'phys': True, 'adc_snr': 73.0, 'vk': 0.1}, {'adc_snr': 78.0},
                    'стресс TIA: сигнал ещё в 10 раз меньше рабочей точки D4 (15.6 мВ)'),
    'tia_vk03':    ({'phys': True, 'adc_snr': 73.0, 'vk': 0.03}, {'adc_snr': 78.0},
                    'стресс TIA: сигнал в 33 раза меньше рабочей точки D4 (4.7 мВ)'),
    'phys_adc_tdm': ({'phys': True, 'adc_snr': 73.0, 'td_true': 0.90 * TD, 'proc': 'adapt'},
                     {'adc_snr': 78.0, 'td_true': 0.90 * TD, 'proc': 0.90 * TD},
                     'phys + adc_real + td -10 %: TIA с адаптацией, ЗЧУ с ОРАКУЛОМ td'),
    'phys_adc_td': ({'phys': True, 'adc_snr': 73.0, 'td_true': 1.10 * TD, 'proc': 'adapt'},
                    {'adc_snr': 78.0, 'td_true': 1.10 * TD},
                    'phys + adc_real + td +10 %: TIA с адаптацией, ЗЧУ без (у ЗЧУ так лучше)'),
}


def run_scenario(name, seeds, dur, rate, fs, branches=('TIA', 'CSP')):
    f_pp, cont = cs_spectrum()
    cfg_t, cfg_c, note = SCEN[name]
    res = {}
    for br, cfg, fn in (('TIA', cfg_t, run_tia), ('CSP', cfg_c, run_csp)):
        if br not in branches:
            continue
        errs, effs, fw_seed, bias_seed, tdu = [], [], [], [], []
        extra = None
        for s in range(seeds):
            rng = np.random.default_rng(1000 + s)
            r = fn(rng, fs, rate, dur, f_pp, cont, cfg)
            if r is None or r['fwhm'] is None:
                continue
            errs.append(r['err'])
            effs.append(r['eff'])
            fw_seed.append(r['fwhm'])
            bias_seed.append(r['bias'])
            tdu.append(r['_']['td_used_ns'])
            extra = r['_']
        e = np.concatenate(errs)
        q1, q3 = np.percentile(e, [15.865, 84.135])
        fw = 2.3548 * (q3 - q1) / 2 * 100
        fws = np.array(fw_seed)
        res[br] = dict(fwhm_pooled=fw, fwhm_mean=float(fws.mean()),
                       fwhm_sem=float(fws.std(ddof=1) / np.sqrt(len(fws))) if len(fws) > 1 else None,
                       eff=float(np.mean(effs)), bias=float(np.median(e) * 100), n=int(len(e)),
                       td_used_ns=float(np.mean(tdu)), td_used_sd=float(np.std(tdu)),
                       extra={k: v for k, v in extra.items()})
    return note, res


def selftest():
    """base-сценарий обязан давать бит в бит то же, что оригинальные run()."""
    f_pp, cont = cs_spectrum()
    ok = True
    for rate in (1e6,):
        P.rng = np.random.default_rng(7)
        a = P.run(125e6, 2.5, rate, 2e-3, f_pp, cont)
        b = run_tia(np.random.default_rng(7), 125e6, rate, 2e-3, f_pp, cont, {})
        print('TIA  оригинал %.6f%% / %.4f   копия %.6f%% / %.4f' %
              (a['fit2']['fwhm'], a['fit2']['eff'], b['fwhm'], b['eff']))
        ok &= abs(a['fit2']['fwhm'] - b['fwhm']) < 1e-12 and a['fit2']['eff'] == b['eff']
        C.rng = np.random.default_rng(7)
        a = C.run(125e6, 50e6, rate, 2e-3, f_pp, cont)
        b = run_csp(np.random.default_rng(7), 125e6, rate, 2e-3, f_pp, cont, {})
        print('ЗЧУ  оригинал %.6f%% / %.4f   копия %.6f%% / %.4f' %
              (a['fit']['fwhm'], a['fit']['eff'], b['fwhm'], b['eff']))
        ok &= abs(a['fit']['fwhm'] - b['fwhm']) < 1e-12 and a['fit']['eff'] == b['eff']
    print('SELFTEST', 'OK' if ok else 'FAIL')
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--scenario', default='base')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--seeds', type=int, default=10)
    ap.add_argument('--dur', type=float, default=4e-3)
    ap.add_argument('--rate', type=float, default=1e6)
    ap.add_argument('--fs', type=float, default=125e6)
    ap.add_argument('--branch', default='TIA,CSP')
    a = ap.parse_args()
    if a.selftest:
        sys.exit(0 if selftest() else 1)
    names = list(SCEN) if a.all else a.scenario.split(',')
    rows = {}
    print('fs = %.0f Мвыб/с, %.0f имп/с, %d зёрен x %.1f мс (зёрна 1000..%d)'
          % (a.fs / 1e6, a.rate, a.seeds, a.dur * 1e3, 999 + a.seeds))
    print('%-13s | %-34s | %-34s' % ('сценарий', 'TIA: FWHM ±σ / свет / сдвиг / td', 'ЗЧУ: FWHM ±σ / свет / сдвиг / td'))
    print('-' * 90)
    for nm in names:
        t0 = time.time()
        note, r = run_scenario(nm, a.seeds, a.dur, a.rate, a.fs, tuple(a.branch.split(',')))
        cells = []
        for br in ('TIA', 'CSP'):
            if br not in r:
                cells.append('-')
                continue
            x = r[br]
            cells.append('%5.2f±%.2f%% %4.1f%% %+5.2f%% %5.1fнс' %
                         (x['fwhm_pooled'], x['fwhm_sem'] or 0, 100 * x['eff'], x['bias'], x['td_used_ns']))
        print('%-13s | %-34s | %-34s  (%.0f с) %s' % (nm, cells[0], cells[1], time.time() - t0, note))
        rows[nm] = dict(note=note, **r)
    tag = '_'.join(names) if len(names) < 4 else 'all'
    fn = os.path.join(HERE, 'out_%s_r%.0fk_fs%.0f_s%d.json' % (tag, a.rate / 1e3, a.fs / 1e6, a.seeds))
    with open(fn, 'w', encoding='utf-8') as f:
        json.dump(rows, f, ensure_ascii=False, indent=1, default=float)
    print('-> %s' % fn)


if __name__ == '__main__':
    main()
