# -*- coding: utf-8 -*-
"""mc_pileup.py — Монте-Карло разделения наложенных импульсов для v2.

Отвечает на вопрос, задающий требования к АЦП, ОУ и ПЛИС: при какой частоте
выборки миллион импульсов в секунду с NaI(Tl) ещё обрабатывается и какой ценой
по разрешению и светимости.

Тракт v2 (аналогового шейпера НЕТ, цифруем прямо с преда):
  анод       i(t) = (Q/td)*exp(-t/td),  td = 230 нс — высвечивание NaI(Tl)
  TIA        однополюсный, полоса f_tia  ->  tt = 1/(2*pi*f_tia)
  выход      v(t) ~ exp(-t/td) - exp(-t/tt),  нормирован на единичный пик
  шум        белый e_out = sqrt(4kT*Rf) (+) e_opamp, отфильтрован полюсом TIA
  АЦП        fs, ADC_BITS на VREF

ВАЖНО про выбор f_tia. Полоса TIA здесь не «чем больше, тем лучше»:
  - сверху её ограничивает Найквист — без аналогового шейпера TIA сам является
    противоалиасинговым фильтром, поэтому f_tia <= fs/2 обязательно;
  - снизу — точность момента прихода: sigma_t ~ e_n/(A*sqrt(f_tia)), то есть
    более широкая полоса даёт более точное время.
Оптимум прижат к Найквисту, поэтому в модели f_tia = fs/RATIO, и RATIO свипуется.
Это и есть требование к ОУ: полоса привязана к частоте выборки, а не взята с запасом.

Обработка — ровно то, что ляжет в ПЛИС:
  1. согласованный фильтр (корреляция с эталоном) — оптимальное обнаружение
     и оценка момента; максимум корреляции уточняется параболой (суб-отсчётно).
     Это та же идея, что 32 фазы в dsp_template.h.
  2. амплитуды: МНК по эталонам с ФИКСИРОВАННЫМИ моментами. Задача линейна,
     решается нормальными уравнениями (A1, A2, B) 3x3. Прямое расширение
     y ~ A*h + B из dsp_template.h на два импульса.
  3. для сравнения — трапеция Jordanov-Knoll с деконволюцией спада, как
     в обычном цифровом МКА (без разделения наложений).

Стратегии:
  trap    — трапеция, высота вершины: эталон «обычного МКА»
  reject  — одиночные события фитом одного эталона, наложенные выброшены
  fit2    — наложенные пары разделяются фитом двух эталонов

Метрика: робастная sigma относительной ошибки амплитуды на фотопике Cs-137
(-> вклад в FWHM) и светимость — доля событий фотопика, дошедших до спектра.

Запуск:  python v2/simulation/mc_pileup.py [--quick]
"""
import json
import os
import re
import sys

import numpy as np

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..', '..'))
OUT = os.path.join(HERE, 'out')
QUICK = '--quick' in sys.argv


def _arg(name, default):
    for a in sys.argv:
        if a.startswith(name + '='):
            return a.split('=', 1)[1]
    return default


METHOD = _arg('--method', 'none')      # none | vp | off | off_vp | oracle
TDK = float(_arg('--tdk', '1.0'))
SEED = int(_arg('--seed', '20261008'))

TD = 230e-9
VREF = 2.5
ADC_BITS = 14
RF = 5000.0
V_PEAK_CS = 0.469
EN_OPAMP = 4e-9
KT4 = 4 * 1.380649e-23 * 300.0
TAIL = 20.0            # длина эталона в единицах td

rng = np.random.default_rng(SEED)


def cs_spectrum():
    """Относительные амплитуды Cs-137 из обратной CDF в firmware/core/pulsegen.c."""
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


def pulse(t, tt, td=TD):
    """Отклик на выходе TIA, нормирован на единичный пик."""
    out = np.zeros(np.shape(t), dtype=float)
    t = np.asarray(t, dtype=float)
    m = t > 0
    t_pk = np.log(td / tt) / (1.0 / tt - 1.0 / td)
    pk = np.exp(-t_pk / td) - np.exp(-t_pk / tt)
    out[m] = (np.exp(-t[m] / td) - np.exp(-t[m] / tt)) / pk
    return out


def dpulse(t, tt, td=TD):
    """d h / d t — нужна для уточнения моментов методом Гаусса-Ньютона."""
    out = np.zeros(np.shape(t), dtype=float)
    t = np.asarray(t, dtype=float)
    m = t > 0
    t_pk = np.log(td / tt) / (1.0 / tt - 1.0 / td)
    pk = np.exp(-t_pk / td) - np.exp(-t_pk / tt)
    out[m] = (-np.exp(-t[m] / td) / td + np.exp(-t[m] / tt) / tt) / pk
    return out


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


def e_out_density():
    return float(np.hypot(np.sqrt(KT4 * RF), EN_OPAMP))


def trapezoid(y, k, m, td, T):
    """Jordanov-Knoll: dk[n] = y[n]-y[n-k]-y[n-l]+y[n-k-l], l = k+m."""
    l = k + m
    dk = y.astype(float).copy()
    dk[k:] -= y[:-k]
    dk[l:] -= y[:-l]
    dk[k + l:] += y[:-(k + l)]
    M = 1.0 / (np.exp(T / td) - 1.0)
    p = np.cumsum(dk)
    s = np.cumsum(p + M * dk)
    return s / (k * (M + 0.5))     # нормировка подбирается калибровкой ниже


def xcorr(y, h):
    """Корреляция y с эталоном h через FFT. Возвращает массив длины len(y)."""
    n = 1
    while n < len(y) + len(h):
        n <<= 1
    C = np.fft.rfft(y, n) * np.conj(np.fft.rfft(h, n))
    c = np.fft.irfft(C, n)
    return c[:len(y)]


def run(fs, ratio, rate, dur, f_pp, cont, trap_k_ns=300.0, trap_m_ns=200.0):
    T = 1.0 / fs
    f_tia = fs / ratio
    tt = 1.0 / (2 * np.pi * f_tia)
    N = int(dur / T)

    # ---------------- эталон
    L = int(TAIL * TD / T) + 2
    h = pulse(np.arange(L) * T, tt)
    hh = float(h @ h)
    # банк эталона по NPH подотсчётным фазам — то же, что 32 фазы в dsp_template.h.
    # Нужен, чтобы колонки матрицы плана строились срезом, а не вызовом exp().
    NPH = 32
    bank = np.stack([pulse((np.arange(L) - ph / NPH) * T, tt) for ph in range(NPH)])

    # ---------------- события
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
        y[j:j + L] += amp[i] * pulse((off - frac[i]) * T, tt, td=TD * TDK)
    y = y[:N] * V_PEAK_CS

    # ---------------- шум: белый, ограниченный полюсом TIA (TIA = противоалиасинг)
    e_out = e_out_density()
    w = rng.standard_normal(N) * e_out * np.sqrt(fs / 2.0)
    f = np.fft.rfftfreq(N, T)
    nz = np.fft.irfft(np.fft.rfft(w) / (1.0 + 1j * f / f_tia), N)
    y = y + nz

    # ---------------- АЦП
    lsb = VREF / (1 << ADC_BITS)
    y = np.round(np.clip(y, -VREF / 2, VREF) / lsb) * lsb
    sigma_out = float(np.std(nz))

    # ---------------- 1. ОБНАРУЖЕНИЕ: деконволюция спада + короткий бокскар
    # Согласованный фильтр здесь НЕ годится: он усредняет по всей длине эталона
    # (20*td = 4.6 мкс) и при 1 Мимп/с сливает события — проверено, находил
    # 537 из 1977. Для разделения нужен обостряющий фильтр:
    #   s[n] = y[n] - d*y[n-1]   — снимает спад 230 нс, остаётся узкий импульс
    #   затем бокскар NAV отсчётов — возвращает отношение сигнал/шум
    # Ширина s становится ~NAV отсчётов, то есть предел разделения задаётся
    # частотой выборки напрямую. В ПЛИС это одно умножение-сложение и сумматор.
    NAV = 3
    d_pz = np.exp(-T / TD)
    s = np.empty(N)
    s[0] = y[0]
    s[1:] = y[1:] - d_pz * y[:-1]
    if NAV > 1:
        k = np.ones(NAV) / NAV
        s = np.convolve(s, k, mode='same')
    # нормировка: отклик на единичную амплитуду
    probe = pulse(np.arange(L) * T, tt) * V_PEAK_CS
    ps = np.empty(L)
    ps[0] = probe[0]
    ps[1:] = probe[1:] - d_pz * probe[:-1]
    if NAV > 1:
        ps = np.convolve(ps, np.ones(NAV) / NAV, mode='same')
    s = s / (ps.max() / V_PEAK_CS)

    seg0 = s[:min(N, 1 << 15)]
    sg_s = 1.4826 * np.median(np.abs(seg0 - np.median(seg0)))
    thr = max(5.0 * sg_s, 0.02 * V_PEAK_CS)
    i0 = np.where((s[1:-1] > thr) & (s[1:-1] >= s[:-2]) & (s[1:-1] > s[2:]))[0] + 1
    if len(i0) == 0:
        return None
    a0, a1, a2 = s[i0 - 1], s[i0], s[i0 + 1]
    den = a0 - 2 * a1 + a2
    sub = np.where(np.abs(den) > 1e-15, 0.5 * (a0 - a2) / np.where(den == 0, 1, den), 0.0)
    t_det = (i0 + np.clip(sub, -1, 1)) * T
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

    # ---------------- 2. амплитуды: МНК по эталонам, моменты фиксированы
    # Запись решается БЛОКАМИ С ПОЛЯМИ. В модель блока входят все события, чьи
    # хвосты в него дотягиваются, включая ушедшие влево за край. Без этого при
    # 1 Мимп/с хвост предыдущего события (1.3 % пика на 1 мкс) даёт ошибку, которая
    # НЕ убирается ни базовой линией, ни ростом частоты выборки — проверено.
    # Оставляются оценки только для событий из внутренней части блока.
    W_n = int(4 * TD / T) + 3            # окно "сосед рядом" для статистики
    gap_min = 2.0 * tt                   # ближе этого два эталона неразличимы
    MARG = L                             # поле = длина эталона (20*td)
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
        G = H.T @ H
        try:
            th = np.linalg.solve(G + 1e-12 * np.eye(len(sel) + 1), H.T @ y[s_lo:s_hi])
        except np.linalg.LinAlgError:
            continue
        # ⚠ ОТКРЫТО: моменты берутся из быстрого канала и НЕ уточняются. Именно это
        # ограничивает точность амплитуды — см. README. Пробная совместная поправка
        # (Гаусс-Ньютон по t) ухудшила результат с 4.5 % до 7.1 % при 1 Мимп/с:
        # задача плохо обусловлена для близких импульсов и нужна регуляризация.
        t_det_loc = t_det[sel]
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
        # сколько соседей в пределах окна 4*td — для колонки reject
        for q, e in enumerate(sel):
            if not (n_lo <= d_idx[e] < n_hi) or too_close[e]:
                continue
            nb = int(np.sum(np.abs(t_det - t_det[e]) < 4 * TD)) - 1
            est_t.append(t_det_loc[q])
            est_a.append(th[q] / V_PEAK_CS)
            est_m.append(1 if nb == 0 else 1 + nb)
    est_t, est_a, est_m = map(np.array, (est_t, est_a, est_m))

    # ---------------- 3. трапеция для сравнения (с калибровкой масштаба)
    k_n = max(2, int(trap_k_ns * 1e-9 / T))
    m_n = max(1, int(trap_m_ns * 1e-9 / T))
    tr = trapezoid(y, k_n, m_n, TD, T)
    cal = trapezoid(V_PEAK_CS * pulse(np.arange(4 * (k_n + m_n)) * T, tt), k_n, m_n, TD, T)
    tr_gain = float(cal.max()) / V_PEAK_CS or 1.0

    # ---------------- сопоставление с истиной
    tol = max(2.5 * T, 25e-9)
    res = {}
    for tag, sel in (('fit2', np.ones(len(est_t), bool)), ('reject', est_m == 1)):
        et, ea = est_t[sel], est_a[sel]
        if len(et) < 30:
            res[tag] = dict(n=0, eff=0.0, sigma=None, fwhm=None)
            continue
        k = np.clip(np.searchsorted(t_ev, et), 1, max(1, n_ev - 1))
        pick = np.where(np.abs(t_ev[k] - et) < np.abs(t_ev[k - 1] - et), k, k - 1)
        ok = (np.abs(t_ev[pick] - et) < tol) & is_pp[pick]
        err = ea[ok] / amp[pick][ok] - 1.0
        dtn = (et[ok] - t_ev[pick][ok]) / T
        tstat = {}
        if ok.sum() >= 30:
            t1, t2_, t3 = np.percentile(dtn, [15.865, 50, 84.135])
            tstat = dict(dt_med=float(t2_), dt_sig=float((t3 - t1) / 2))
        if ok.sum() >= 30:
            q1, q3 = np.percentile(err, [15.865, 84.135])
            sg = float((q3 - q1) / 2)
        else:
            sg = None
        res[tag] = dict(n=int(ok.sum()), eff=float(ok.sum()) / max(1, int(is_pp.sum())),
                        sigma=sg, fwhm=(2.3548 * sg * 100 if sg else None), **tstat)

    # трапеция: локальный максимум в окне после события
    ti = (t_ev / T).astype(int)
    lo_ok = (ti + k_n + m_n + 2 < N) & is_pp
    vals = []
    for p in np.where(lo_ok)[0]:
        a, b = ti[p], ti[p] + k_n + m_n + 2
        vals.append(tr[a:b].max() / tr_gain / V_PEAK_CS / amp[p])
    if len(vals) >= 30:
        q1, q3 = np.percentile(np.array(vals) - 1.0, [15.865, 84.135])
        sgt = float((q3 - q1) / 2)
    else:
        sgt = None
    res['trap'] = dict(n=len(vals), eff=1.0, sigma=sgt,
                       fwhm=(2.3548 * sgt * 100 if sgt else None))

    res['_'] = dict(fs_MHz=fs / 1e6, ratio=ratio, f_tia_MHz=f_tia / 1e6, tt_ns=tt * 1e9,
                    rate=rate, n_ev=n_ev, n_det=len(t_det), sigma_out_uV=sigma_out * 1e6,
                    lsb_uV=lsb * 1e6, e_out_nV=e_out * 1e9, method=METHOD, seed=SEED, det_bias=det_bias,
                    trap_k=k_n, trap_m=m_n, gap_min_ns=gap_min * 1e9,
                    pileup_theory=float(1 - np.exp(-rate * W_n * T)))
    return res


def fmt(d):
    if not d or d['fwhm'] is None:
        return '    n/a'
    return '%6.2f%% /%5.1f%%' % (d['fwhm'], 100 * d['eff'])


def main():
    os.makedirs(OUT, exist_ok=True)
    f_pp, cont = cs_spectrum()
    print('Спектр Cs-137 из pulsegen.c: доля фотопика %.3f' % f_pp)
    print('Zt = %.0f Ом -> шум на выходе %.1f нВ/√Гц; АЦП %d бит на %.1f В, LSB %.1f мкВ'
          % (RF, e_out_density() * 1e9, ADC_BITS, VREF, VREF / (1 << ADC_BITS) * 1e6))
    print('f_tia = fs / RATIO (TIA работает противоалиасинговым фильтром)')
    print()

    fs_list = [20e6, 40e6, 65e6, 125e6]
    ratios = [2.5] if QUICK else [2.5, 4.0, 8.0]
    rates = [100e3, 1e6] if QUICK else [30e3, 100e3, 300e3, 1e6]
    dur = 2e-3 if QUICK else 8e-3
    if _arg('--fs', None):
        fs_list = [float(x) * 1e6 for x in _arg('--fs', '').split(',')]
    if _arg('--ratios', None):
        ratios = [float(x) for x in _arg('--ratios', '').split(',')]
    if _arg('--rates', None):
        rates = [float(x) for x in _arg('--rates', '').split(',')]
    dur = float(_arg('--dur', dur))

    rows = []
    for ratio in ratios:
        print('=' * 78)
        print('RATIO = %.1f   (f_tia = fs/%.1f)' % (ratio, ratio))
        print('=' * 78)
        print('%7s %8s %8s | %-15s %-15s %-9s' %
              ('fs,МГц', 'f_tia', 'имп/с', 'fit2 FWHM/свет', 'reject', 'трапеция'))
        print('-' * 78)
        for fs in fs_list:
            for rate in rates:
                r = run(fs, ratio, rate, dur, f_pp, cont)
                if r is None:
                    continue
                print('%7.0f %8.1f %8.0f | %-15s %-15s %-9s' %
                      (fs / 1e6, r['_']['f_tia_MHz'], rate,
                       fmt(r['fit2']), fmt(r['reject']),
                       ('%6.2f%%' % r['trap']['fwhm']) if r['trap']['fwhm'] else 'n/a'),
                      ' dt[отсч] %+.3f ± %.3f' % (r['fit2'].get('dt_med', np.nan),
                                                 r['fit2'].get('dt_sig', np.nan)))
                rows.append(r)
        print()

    with open(os.path.join(OUT, 'mc_pileup_%s_s%d.json' % (METHOD, SEED)), 'w', encoding='utf-8') as f:
        json.dump(rows, f, ensure_ascii=False, indent=1)
    print('результаты: %s' % os.path.join(OUT, 'mc_pileup_%s_s%d.json' % (METHOD, SEED)))


if __name__ == '__main__':
    main()
