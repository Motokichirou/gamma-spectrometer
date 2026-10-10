# -*- coding: utf-8 -*-
"""mc_csp.py — Монте-Карло для выбранной ветки v2: ЗЧУ со сбросом.

Отличие от mc_pileup.py: там тракт TIA, сигнал — импульс. Здесь зарядочувствительный
предусилитель, сигнал — **монотонная лестница без спада**, канал = высота ступеньки.
Решение зафиксировано 2026-10-09, см. v2/design.md §4.

Модель тракта:
  анод      i(t) = (Q/td)·exp(-t/td),  td = 230 нс — высвечивание NaI(Tl)
  ЗЧУ       Cf интегрирует заряд; усилитель однополюсный с полосой f_a
  ступенька h(t) = 1 - (td·e^(-t/td) - ta·e^(-t/ta))/(td - ta),  нормирована на 1
            фронт: 90 % за 2.3·td ≈ 530 нс; начинается с нулевой производной (полюс ОУ)
  шум       белый e_n·(1 + Cвх/Cf) на выходе, ограничен полюсом усилителя
  сброс     каждые T_RESET уровень обнуляется, окно T_BLANK выбрасывается
  АЦП       fs, ADC_BITS на V_FS

Рабочая точка из ТЗ §4.2-4.3: усиление ФЭУ снижено в 30 раз, Cf = 100 пФ,
шаг на фотопике Cs-137 = 72 мВ, на потолке 3.5 МэВ = 380 мВ.

Обработка:
  1. ОБНАРУЖЕНИЕ: первая разность (производная лестницы = импульс) + короткий бокскар.
     Деконволюция спада НЕ нужна — спада нет, это главное упрощение против TIA.
  2. АМПЛИТУДЫ: кластер решается целиком, МНК по ступенчатым эталонам + постоянная.
     События до кластера дают постоянный уровень и впитываются постоянной — поэтому
     поле нужно всего ~5·td, а не 20·td, как в импульсной модели.
  3. УТОЧНЕНИЕ МОМЕНТОВ: Левенберг-Марквардт с демпфированием и откатом.
     Шаг принимается только если норма остатка падает. В импульсной версии
     неукрощённый Гаусс-Ньютон ухудшал результат (4.5 % -> 7.1 %).
  4. Для сравнения: разность двух средних (трапеция с нулевой вершиной) —
     оптимальный измеритель ступеньки в белом шуме.

Запуск:  python v2/simulation/mc_csp.py [--quick] [--no-refine]
"""
import json
import os
import re
import sys

import numpy as np

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
OUT = os.path.join(HERE, 'out')
QUICK = '--quick' in sys.argv
NO_REFINE = '--no-refine' in sys.argv

TD = 230e-9              # высвечивание NaI(Tl)
CF = 100e-12             # ёмкость интегратора, ТЗ §4.3
C_IN = 6e-12             # входная ёмкость узла
Q_662 = 7.2e-12          # заряд на фотопике Cs-137 при усилении 30x ниже v1
STEP_662 = Q_662 / CF    # = 72 мВ
V_FS = 4.4               # размах АЦП (рампа 4 В + максимальное событие 380 мВ)
ADC_BITS = 16             # 16, а не 14: детектору нужен дизер, см. README
EN = 4e-9                # шум ОУ по напряжению
V_RAMP = 4.0             # рабочий размах рампы до сброса
T_BLANK = 1e-6           # окно сброса

rng = np.random.default_rng(20261009)


# ---------------------------------------------------------------- спектр

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


# ---------------------------------------------------------------- эталон ступеньки

def hstep(t, ta, td=TD):
    """Ступенька на выходе ЗЧУ, нормирована на 1 при t -> inf."""
    t = np.asarray(t, dtype=float)
    out = np.zeros(np.shape(t))
    m = t > 0
    out[m] = 1.0 - (td * np.exp(-t[m] / td) - ta * np.exp(-t[m] / ta)) / (td - ta)
    return out


def dhstep(t, ta, td=TD):
    """Производная — она же форма импульса на выходе TIA, с точностью до масштаба."""
    t = np.asarray(t, dtype=float)
    out = np.zeros(np.shape(t))
    m = t > 0
    out[m] = (np.exp(-t[m] / td) - np.exp(-t[m] / ta)) / (td - ta)
    return out


# ---------------------------------------------------------------- решение кластера

def solve_cluster(seg, tl, tq, ta, refine=True, maxit=4):
    """МНК по ступенчатым эталонам при моментах tq + ЛМ-уточнение моментов.

    Возвращает (амплитуды, моменты, норма остатка) либо None.
    Шаг по моментам принимается только если норма остатка уменьшилась — поэтому
    уточнение не может сделать хуже, в отличие от неукрощённого Гаусса-Ньютона.
    """
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
        tq_try = tq + dt   # J = dm/dtq => шаг ГН со знаком «+» (до 2026-10-11 стоял «−», см. decisions.md D11)
        if np.any(np.diff(np.sort(tq_try)) < 0.3 * T):     # не склеивать события
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


# ---------------------------------------------------------------- прогон

def run(fs, f_a, rate, dur, f_pp, cont, L_arm=64, refine=True):
    T = 1.0 / fs
    ta = 1.0 / (2 * np.pi * f_a)
    N = int(dur / T)
    L = int(8 * TD / T) + 2                     # ступенька выходит на плато за ~5*td

    n_ev = int(rng.poisson(rate * dur))
    t_ev = np.sort(rng.random(n_ev) * dur)
    amp, is_pp = draw_amplitudes(n_ev, f_pp, cont)

    # --- лестница
    y = np.zeros(N + L + 4)
    idx = (t_ev / T).astype(int)
    frac = t_ev / T - idx
    off = np.arange(L)
    for i in range(n_ev):
        j = idx[i]
        if j + L >= len(y):
            continue
        y[j:j + L] += amp[i] * hstep((off - frac[i]) * T, ta)
        y[j + L:] += amp[i]                     # после фронта — постоянный вклад
    y = y[:N] * STEP_662

    # --- сбросы: уровень обнуляется, когда рампа ушла на V_RAMP от последнего сброса
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
    y = y - lvl_arr          # после каждого сброса отсчёт идёт от нуля

    # --- шум: белый на выходе, ограничен полюсом усилителя
    e_out = EN * (1.0 + C_IN / CF)
    w = rng.standard_normal(N) * e_out * np.sqrt(fs / 2.0)
    f = np.fft.rfftfreq(N, T)
    nz = np.fft.irfft(np.fft.rfft(w) / (1.0 + 1j * f / f_a), N)
    y = y + nz
    sigma_out = float(np.std(nz))

    # --- АЦП
    lsb = V_FS / (1 << ADC_BITS)
    y = np.round(y / lsb) * lsb

    # --- 1. обнаружение: разность -> полюс-нуль -> бокскар
    # У ЗЧУ фронт ступеньки — ИНТЕГРАЛ высвечивания, поэтому первая разность даёт
    # импульс шириной td = 230 нс, а не узкий пик. Для разделения нужна ещё
    # деконволюция спада, то есть детектор дифференцирует дважды. Это цена ветки
    # ЗЧУ: в варианте TIA полюс-нуль был один.
    NAV = 3
    d_pz = np.exp(-T / TD)

    def sharpen(v):
        a = np.empty(len(v))
        a[0] = 0.0
        a[1:] = v[1:] - v[:-1]          # разность: лестница -> импульс
        b = np.empty(len(a))
        b[0] = a[0]
        b[1:] = a[1:] - d_pz * a[:-1]   # полюс-нуль: импульс -> узкий пик
        return np.convolve(b, np.ones(NAV) / NAV, mode='same')

    s = sharpen(y)
    ps = sharpen(STEP_662 * hstep(np.arange(L) * T, ta))
    s = s / (ps.max() / STEP_662)
    s[blank] = 0.0

    seg0 = s[:min(N, 1 << 15)]
    sg = 1.4826 * np.median(np.abs(seg0 - np.median(seg0)))
    thr = max(5.0 * sg, 0.03 * STEP_662)

    # Пики ищем с группировкой: квантование АЦП создаёт плоские участки и ничьи,
    # из-за которых наивный поиск локальных максимумов даёт десятки срабатываний
    # на одно событие. Группа = соседние кандидаты ближе HOLD отсчётов, берём максимум.
    HOLD = max(2, int(2.0 * ta / T) + 1)
    above = np.where(s > thr)[0]
    above = above[(above > 0) & (above < N - 1)]
    if len(above) < 30:
        return None
    brk = np.where(np.diff(above) > HOLD)[0]
    groups = np.split(above, brk + 1)
    i0 = np.array([g[int(np.argmax(s[g]))] for g in groups])
    i0 = i0[(i0 > 0) & (i0 < N - 1)]
    if len(i0) < 30:
        return None
    a0, a1, a2 = s[i0 - 1], s[i0], s[i0 + 1]
    den = a0 - 2 * a1 + a2
    sub = np.where(np.abs(den) > 1e-18, 0.5 * (a0 - a2) / np.where(den == 0, 1, den), 0.0)
    t_det = (i0 + np.clip(sub, -1, 1)) * T

    # --- 2-3. кластеры: МНК + ЛМ
    W_n = int(4 * TD / T) + 3
    MARG = int(5 * TD / T) + 2
    gap_min = 2.0 * ta
    K_MAX = 8
    est_t, est_a, est_m = [], [], []
    n_clust = n_skip_big = n_skip_close = n_skip_blank = 0
    nd = len(t_det)
    i = 0
    while i < nd:
        j = i
        while j + 1 < nd and (t_det[j + 1] - t_det[j]) < W_n * T:
            j += 1
        grp = list(range(i, j + 1))
        i = j + 1
        n_clust += 1
        if len(grp) > K_MAX:
            n_skip_big += 1
            continue
        tg = t_det[grp]
        if len(tg) > 1 and np.min(np.diff(tg)) < gap_min:
            n_skip_close += 1
            continue
        n_lo = max(0, int(tg[0] / T) - 2)
        n_hi = min(N, int(tg[-1] / T) + MARG)
        if n_hi - n_lo < 8 + len(grp) or blank[n_lo:n_hi].any():
            n_skip_blank += 1
            continue
        seg = y[n_lo:n_hi]
        tl = np.arange(n_lo, n_hi) * T
        res = solve_cluster(seg, tl, tg.copy(), ta, refine=refine)
        if res is None:
            continue
        aa, tt_, _ = res
        for q, g in enumerate(grp):
            est_t.append(tt_[q])
            est_a.append(aa[q] / STEP_662)
            est_m.append(len(grp))
    est_t, est_a, est_m = map(np.array, (est_t, est_a, est_m))

    # --- 4. разность двух средних (эталон обычного МКА)
    g_n = int(2.6 * TD / T) + 1                # разнос ≥ фронта 530 нс
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
    if len(vals) >= 30:
        q1, q3 = np.percentile(np.array(vals) - 1.0, [15.865, 84.135])
        sgt = float((q3 - q1) / 2)
    else:
        sgt = None

    # --- сопоставление
    out = {}
    tol = max(2.5 * T, 25e-9)
    for tag, sel in (('fit', np.ones(len(est_t), bool)), ('solo', est_m == 1)):
        et, ea = est_t[sel], est_a[sel]
        if len(et) < 30:
            out[tag] = dict(n=0, eff=0.0, fwhm=None)
            continue
        k = np.clip(np.searchsorted(t_ev, et), 1, max(1, n_ev - 1))
        pick = np.where(np.abs(t_ev[k] - et) < np.abs(t_ev[k - 1] - et), k, k - 1)
        g = (np.abs(t_ev[pick] - et) < tol) & is_pp[pick]
        err = ea[g] / amp[pick][g] - 1.0
        if g.sum() >= 30:
            q1, q3 = np.percentile(err, [15.865, 84.135])
            sgv = float((q3 - q1) / 2)
        else:
            sgv = None
        out[tag] = dict(n=int(g.sum()), eff=float(g.sum()) / max(1, int(is_pp.sum())),
                        fwhm=(2.3548 * sgv * 100 if sgv else None))
    out['twoavg'] = dict(n=len(vals), eff=1.0,
                         fwhm=(2.3548 * sgt * 100 if sgt else None))
    out['_'] = dict(n_clust=int(n_clust), n_skip_big=int(n_skip_big),
                    n_skip_close=int(n_skip_close), n_skip_blank=int(n_skip_blank),
                    n_est=int(len(est_t)), ymax=float(np.max(np.abs(y))),
                    fs_MHz=fs / 1e6, f_a_MHz=f_a / 1e6, ta_ns=ta * 1e9, rate=rate,
                    n_ev=n_ev, n_det=nd, resets=len(resets),
                    deadtime_pct=100.0 * blank.sum() / N,
                    sigma_out_uV=sigma_out * 1e6, lsb_uV=lsb * 1e6,
                    step662_mV=STEP_662 * 1e3, L_arm=L_arm, refine=refine)
    return out


def fmt(d):
    if not d or d['fwhm'] is None:
        return '      n/a'
    return '%6.2f%% /%5.1f%%' % (d['fwhm'], 100 * d['eff'])


def main():
    os.makedirs(OUT, exist_ok=True)
    f_pp, cont = cs_spectrum()
    print('ЗЧУ со сбросом: Cf = %.0f пФ, шаг на фотопике Cs-137 = %.1f мВ, АЦП %d бит на %.1f В'
          % (CF * 1e12, STEP_662 * 1e3, ADC_BITS, V_FS))
    print('спектр Cs-137 из pulsegen.c, доля фотопика %.3f; уточнение моментов: %s'
          % (f_pp, 'выключено' if NO_REFINE else 'ЛМ с откатом'))
    print()
    fs_list = [40e6, 65e6, 125e6]
    rates = [3e5, 1e6] if QUICK else [1e5, 3e5, 1e6]
    dur = 2e-3 if QUICK else 4e-3
    rows = []
    print('%7s %8s %9s | %-18s %-18s %-14s %8s' %
          ('fs,МГц', 'f_a,МГц', 'имп/с', 'фит FWHM/свет', 'одиночные', '2 средних', 'сбросы'))
    print('-' * 92)
    for fs in fs_list:
        f_a = fs / 2.5
        for rate in rates:
            r = run(fs, f_a, rate, dur, f_pp, cont, refine=not NO_REFINE)
            if r is None:
                continue
            print('%7.0f %8.1f %9.0f | %-18s %-18s %-14s %5d (%.2f%%)' %
                  (fs / 1e6, f_a / 1e6, rate, fmt(r['fit']), fmt(r['solo']),
                   fmt(r['twoavg']), r['_']['resets'], r['_']['deadtime_pct']))
            rows.append(r)
    with open(os.path.join(OUT, 'mc_csp.json'), 'w', encoding='utf-8') as f:
        json.dump(rows, f, ensure_ascii=False, indent=1)
    print()
    print('результаты: %s' % os.path.join(OUT, 'mc_csp.json'))


if __name__ == '__main__':
    main()
