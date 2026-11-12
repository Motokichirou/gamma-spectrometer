# -*- coding: utf-8 -*-
"""chk_resid.py — остаток после вычета хвоста A·S(n − t) на одиночных импульсах разных амплитуд,
и влияние сетки таблицы S (1/64, 1/16, 1/8, 1/4 отсчёта; линейная интерполяция / ближайший).
python -I chk_resid.py cs_only.bin out/fw_cs_ev.npz out/S.npz out/"""
import os, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pumodel as pm

def main():
    sys.stdout.reconfigure(encoding='utf-8')
    raw, evp, sp, outd = sys.argv[1:5]
    thr = int(sys.argv[5]) if len(sys.argv) > 5 else 17
    x = pm.load_portions(raw)
    ev = np.load(evp)['ev']
    sh = pm.Shape.load(sp)
    pre, post = 30, 150
    W, A, P, I, R = pm.collect_singles(x, ev, thr, 20, 4000, pre, post)
    print('одиночных всех амплитуд', len(A))
    n = np.arange(-pre, post + 1)
    t = sh.off[P]
    model = A[:, None] * sh(n[None, :] - t[:, None])
    res = W - model
    # неотловленные соседи в хвосте (остаток > 6σ) — вон, доля печатается
    keep = np.abs(res[:, n >= 6]).max(1) <= 6 * 4.08 + 0.02 * A
    print(f'чистых (остаток ≤ 6σ + 2 % A): {keep.mean() * 100:.1f} %')
    for lo_, hi_ in [(20, 150), (150, 500), (540, 650), (700, 4000)]:
        m_ = (A >= lo_) & (A < hi_)
        print(f'   A {lo_}–{hi_}: чистых {keep[m_].mean() * 100:.1f} % из {m_.sum()}')
    W, A, P, res, t = W[keep], A[keep], P[keep], res[keep], t[keep]
    bins = [(20, 60), (60, 150), (150, 300), (300, 500), (540, 650), (700, 1500), (1500, 4000)]
    fig, ax = plt.subplots(2, 1, figsize=(9, 7))
    print('A-диапазон  N   средний остаток (% от A) в τ∈[+6,+12] [+12,+25] [+25,+45] [+45,+150]   rms остатка (коды) τ∈[+6,+45]  σ шума 4.08')
    for lo, hi in bins:
        m = (A >= lo) & (A < hi)
        if m.sum() < 20:
            continue
        r = res[m] / A[m, None] * 100
        def seg(a, b):
            k = (n >= a) & (n < b)
            return r[:, k].mean()
        k = (n >= 6) & (n < 45)
        rms = np.sqrt((res[m][:, k] ** 2).mean())
        print(f'{lo:5d}–{hi:<5d} {m.sum():6d}   {seg(6,12):+.3f} {seg(12,25):+.3f} {seg(25,45):+.3f} {seg(45,151):+.3f}   {rms:.2f}')
        ax[0].plot(n, r.mean(0), lw=1, label=f'A {lo}–{hi} (N={m.sum()})')
        ax[1].plot(n, res[m].std(0), lw=1, label=f'A {lo}–{hi}')
    ax[0].set_xlim(-5, 150); ax[0].set_ylim(-3, 3); ax[0].grid(alpha=.3); ax[0].legend(fontsize=7)
    ax[0].set_ylabel('средний остаток, % от A'); ax[0].set_xlabel('отсчёт от максимума')
    ax[1].set_xlim(-5, 150); ax[1].set_ylim(0, 20); ax[1].grid(alpha=.3); ax[1].legend(fontsize=7)
    ax[1].set_ylabel('σ остатка, коды'); ax[1].set_xlabel('отсчёт от максимума')
    fig.tight_layout(); fig.savefig(os.path.join(outd, 'resid_by_amp.png'), dpi=110)
    # сетка таблицы на фотопике Cs
    m = (A >= 540) & (A <= 650)
    k = (n >= 6) & (n <= 150)
    tt = n[None, k] - t[m, None]
    ref = sh(tt)
    print('Сетка таблицы S (фотопик Cs, τ от +6 до +150 после максимума): макс. и rms ошибки, % от A')
    for g in (64, 16, 8, 4, 2):
        tau = np.arange(sh.tau[0], sh.tau[-1], 1.0 / g)
        Sg = np.round(sh(tau) * 32767) / 32767
        lin = np.interp(tt, tau, Sg)
        near = Sg[np.clip(np.round((tt - tau[0]) * g).astype(int), 0, tau.size - 1)]
        print(f'  1/{g:<3d} точек {tau.size:5d}: линейная max {np.abs(lin-ref).max()*100:.3f} rms {np.sqrt(((lin-ref)**2).mean())*100:.4f};'
              f'  ближайший max {np.abs(near-ref).max()*100:.3f} rms {np.sqrt(((near-ref)**2).mean())*100:.4f}')

if __name__ == '__main__':
    main()
