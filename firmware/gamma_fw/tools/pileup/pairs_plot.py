# -*- coding: utf-8 -*-
"""pairs_plot.py — сводка и графики по out/pairs_<tag>.npz.
python -I pairs_plot.py tag [режимы через запятую] [--table]"""
import sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def stats(arr):
    # arr: Δ × n × 7 (a1, a2, f1, f2, r1, r2, extra)
    out = {}
    out['p1'] = (arr[:, :, 2] == 1).mean(1)
    out['p2'] = (arr[:, :, 3] == 1).mean(1)
    r1 = arr[:, :, 4]; r2 = arr[:, :, 5]
    with np.errstate(all='ignore'):
        out['b1'] = np.nanmean(r1, 1); out['s1'] = np.nanstd(r1, 1)
        out['b2'] = np.nanmean(r2, 1); out['s2'] = np.nanstd(r2, 1)
        out['m2'] = np.nanmean(arr[:, :, 1], 1)
        out['m1'] = np.nanmean(arr[:, :, 0], 1)
        # «тяжёлый хвост»: доля |r2| > 3σ_пика(Cs 17 кодов) среди принятых
        out['o2'] = np.nanmean(np.abs(r2) > 0.05 * arr[:, :, 1], 1)
    out['ex'] = arr[:, :, 6].mean(1)
    return out


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    tag = sys.argv[1]
    z = np.load(f'out/pairs_{tag}.npz')
    d = z['d']
    names = list(z['names'])
    if len(sys.argv) > 2 and not sys.argv[2].startswith('--'):
        names = sys.argv[2].split(',')
    S = {n: stats(z[n]) for n in names}
    if '--table' in sys.argv:
        print('Δ    ' + '  '.join(f'{n:>44s}' for n in names))
        print('     ' + '  '.join(f'{"P1   P2   b1    s1    b2    s2   ex":>44s}' for n in names))
        for k, dd in enumerate(d):
            if dd <= 20 or dd % 5 == 0:
                row = []
                for n in names:
                    s = S[n]
                    row.append(f'{s["p1"][k]:.2f} {s["p2"][k]:.2f} {s["b1"][k]:+5.1f} {s["s1"][k]:5.1f} {s["b2"][k]:+5.1f} {s["s2"][k]:5.1f} {s["ex"][k]:.2f}')
                print(f'{dd:4d} ' + '  '.join(f'{r:>44s}' for r in row))
    fig, ax = plt.subplots(3, 1, figsize=(10, 11), sharex=True)
    for n in names:
        s = S[n]
        ax[0].plot(d, s['p1'], '--', lw=1, label=f'{n}: 1-й принят')
        ax[0].plot(d, s['p2'], '-', lw=1.2, label=f'{n}: 2-й принят')
        m2 = np.nanmean(s['m2']); m1 = np.nanmean(s['m1'])
        ax[1].plot(d, 100 * s['b2'] / m2, '-', lw=1.2, label=f'{n}: 2-й')
        ax[1].plot(d, 100 * s['b1'] / m1, '--', lw=1, label=f'{n}: 1-й')
        ax[2].plot(d, 100 * s['s2'] / m2, '-', lw=1.2, label=f'{n}: 2-й')
        ax[2].plot(d, 100 * s['s1'] / m1, '--', lw=1, label=f'{n}: 1-й')
    ax[0].set_ylabel('доля принятых'); ax[0].legend(fontsize=7, ncol=2); ax[0].grid(alpha=.3)
    ax[1].set_ylabel('смещение A, % от A'); ax[1].set_ylim(-6, 6); ax[1].legend(fontsize=7, ncol=2); ax[1].grid(alpha=.3)
    ax[2].set_ylabel('разброс A (σ ошибки), % от A'); ax[2].set_ylim(0, 6); ax[2].legend(fontsize=7, ncol=2); ax[2].grid(alpha=.3)
    ax[2].set_xlabel('Δ, отсчётов между максимумами')
    ax[0].set_title(f'Синтетические пары {tag} (порог {int(z["thr"])})')
    fig.tight_layout(); fig.savefig(f'out/pairs_{tag}.png', dpi=110)


if __name__ == '__main__':
    main()
