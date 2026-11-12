# -*- coding: utf-8 -*-
"""shape.py — длинная форма S по одиночным импульсам Cs-137.
python -I shape.py tpl_cs.h cs_only.bin out/fw_cs_ev.npz out/
Одиночный: принят, A в фотопике (540…650), в окне −30…+150 от максимума нет других событий и
нет отсчётов выше порога вне своего импульса. Время импульса — по 50 % фронта (CFD), затем
таблица off[фаза] = медиана(t_cfd − imax) по фазе эталона, и S строится заново от t = imax + off[ф].
Выход: S.npz (tau, S, off), singles.npz (окна одиночных), графики."""
import os
import sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pumodel as pm

PRE, POST = 30, 400
THR = 17
GRID = 16      # шагов на отсчёт для S


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    tpl_path, raw, evp, outd = sys.argv[1:5]
    x = pm.load_portions(raw)
    ev = np.load(evp)['ev']                 # file, portion, ip, A, phase, kind, pred
    wins, A, ph, ipk, cfd = [], [], [], [], []
    for por in np.unique(ev[:, 1]).astype(int):
        e = ev[ev[:, 1] == por]
        e = e[np.argsort(e[:, 2])]
        b, v = pm.portion_base(x[por], THR)
        y = x[por].astype(float) - b
        for j in range(len(e)):
            if e[j, 5] != 0 or not (540 <= e[j, 3] <= 650):
                continue
            ip = int(e[j, 2])
            if ip - PRE < 0 or ip + POST >= pm.BLK:
                continue
            others = (e[:, 2] > ip - PRE) & (e[:, 2] < ip + POST)
            others[j] = False
            if others.any():
                continue
            w = y[ip - PRE: ip + POST + 1]
            # вне своего импульса (до −8 и после +60) — ничего выше порога
            if (w[:PRE - 8] > THR).any() or (np.abs(w[PRE + 60:]) > THR).any():
                continue
            a = e[j, 3]
            # CFD 50 %
            k = PRE - 1
            while k > 0 and w[k] >= 0.5 * a:
                k -= 1
            # w[k] < 0.5a <= w[k+1]
            t = k + (0.5 * a - w[k]) / (w[k + 1] - w[k]) - PRE
            wins.append(w); A.append(a); ph.append(int(e[j, 4])); ipk.append(ip); cfd.append(t)
    wins = np.array(wins); A = np.array(A); ph = np.array(ph); cfd = np.array(cfd)
    print('одиночных', len(A))
    # таблица смещений по фазе
    off = np.zeros(pm.NPH)
    for p in range(pm.NPH):
        m = ph == p
        off[p] = np.median(cfd[m]) if m.sum() > 5 else np.nan
    print('off[фаза] (t_cfd − imax):', np.round(off, 3))
    # гладкая монотонная поправка: off должна идти линейно по фазе с переносом через 1 отсчёт
    pp = np.arange(pm.NPH)
    ok = ~np.isnan(off)
    off = np.interp(pp, pp[ok], off[ok])
    t_sp = np.array([off[p] for p in ph])            # время импульса от imax
    jit = cfd - t_sp
    print(f'разброс t_cfd − t_tpl: σ {jit.std():.3f} отсчёта')
    def build(wins, A, t_sp):
        taus = np.arange(-PRE, POST + 1)[None, :] - t_sp[:, None]
        vals = wins / A[:, None]
        tau_g = np.arange(-PRE + 2, POST - 1, 1.0 / GRID)
        idx = np.round((taus - tau_g[0]) * GRID).astype(int)
        good = (idx >= 0) & (idx < tau_g.size)
        s_sum = np.bincount(idx[good], weights=vals[good], minlength=tau_g.size)
        s_cnt = np.bincount(idx[good], minlength=tau_g.size)
        S = s_sum / np.maximum(s_cnt, 1)
        # сглаживание только хвоста (τ > 15): скользящее среднее ±1 отсчёт
        Ssm = S.copy()
        k = GRID
        cs = np.cumsum(np.r_[0, S])
        for i in range(tau_g.size):
            if tau_g[i] > 15:
                lo, hi = max(0, i - k), min(tau_g.size, i + k + 1)
                Ssm[i] = (cs[hi] - cs[lo]) / (hi - lo)
        return tau_g, Ssm
    # чистка: в окне одиночного не должно быть неотловленных импульсов (в хвосте, ниже порога
    # повторного роста) — остаток после вычета A·S в отсчётах ≥ +6 от максимума не больше CUT
    CUT = 6 * 4.08
    n_all = np.arange(-PRE, POST + 1)
    keep = np.ones(len(A), bool)
    for it_ in range(3):
        tau_g, S = build(wins[keep], A[keep], t_sp[keep])
        res = wins - A[:, None] * np.interp(n_all[None, :] - t_sp[:, None], tau_g, S)
        mx = np.abs(res[:, n_all >= 6]).max(1)
        keep = mx <= CUT
        print(f'чистка {it_}: оставлено {keep.sum()} из {len(A)} ({100 * keep.mean():.1f} %)')
    tau_g, S = build(wins[keep], A[keep], t_sp[keep])
    wins, A, ph, cfd, t_sp = wins[keep], A[keep], ph[keep], cfd[keep], t_sp[keep]
    ipk = list(np.array(ipk)[keep])
    # нормировка — максимум 1 (A эталона ≈ максимум формы)
    print(f'максимум S = {S.max():.4f} при τ = {tau_g[S.argmax()]:.3f}')
    np.savez(os.path.join(outd, 'S.npz'), tau=tau_g, S=S, off=off)
    np.savez_compressed(os.path.join(outd, 'singles.npz'), w=wins, A=A, ph=ph, t=t_sp, ip=np.array(ipk))
    # длина хвоста
    tp = tau_g[S.argmax()]
    for lev in (0.05, 0.01, 0.005, 0.001):
        last = tau_g[np.flatnonzero(np.abs(S) > lev)[-1]]
        print(f'|S| > {lev*100:g} % до τ = {last:.1f} (от максимума +{last - tp:.1f})')
    neg = S[tau_g > tp]
    i0 = np.flatnonzero((tau_g > tp) & (S < 0))
    print(f'выброс в минус: min S = {S.min():.4f} при τ = {tau_g[S.argmin()]:.1f}; '
          f'первый S<0 при τ = {tau_g[i0[0]] if i0.size else float("nan"):.1f}')
    print(f'площадь хвоста ниже нуля: {S[S < 0].sum() / GRID:.3f} отсч.·A, всего ∫S = {S.sum() / GRID:.3f}')
    # графики
    fig, ax = plt.subplots(2, 1, figsize=(9, 7))
    ax[0].plot(tau_g, S, lw=1)
    ax[0].set_xlabel('τ, отсчёты (от времени импульса)'); ax[0].set_ylabel('S'); ax[0].grid(alpha=.3)
    ax[0].set_title(f'Длинная форма S, одиночных Cs-137: {len(A)}')
    ax[1].plot(tau_g, S * 100, lw=1)
    ax[1].set_ylim(-1, 2); ax[1].axhline(0.5, ls=':', c='r'); ax[1].axhline(0.1, ls=':', c='g')
    ax[1].axhline(-0.1, ls=':', c='g')
    ax[1].set_xlabel('τ, отсчёты'); ax[1].set_ylabel('S, % от A'); ax[1].grid(alpha=.3)
    fig.tight_layout(); fig.savefig(os.path.join(outd, 'shape_S.png'), dpi=110)


if __name__ == '__main__':
    main()
