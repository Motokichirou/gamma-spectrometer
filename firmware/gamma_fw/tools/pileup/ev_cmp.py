# -*- coding: utf-8 -*-
"""ev_cmp.py — сопоставление событий двух прогонов (fw и unfold) по (файл, порция, ip±1).
python -I ev_cmp.py out/fw_mix1,out/fw_mix2,out/fw_mix3 out/uf_mix1,out/uf_mix2,out/uf_mix3 name"""
import subprocess
import sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

GP = r'D:\gamma-spectrometer\host\build\Release\gammapult.com'


def load(tags):
    out = []
    for k, t in enumerate(tags.split(',')):
        e = np.load(t + '_ev.npz')['ev'].copy()
        e[:, 0] = k
        out.append(e)
    return np.concatenate(out)


def key(e):
    return (e[:, 0].astype(np.int64) * 100000 + e[:, 1].astype(np.int64)) * 10000 + e[:, 2].astype(np.int64)


def gp(h, name):
    p = f'out/_{name}.csv'
    with open(p, 'w') as f:
        f.write('channel,count\n')
        for i, c in enumerate(h):
            f.write(f'{i},{int(c)}\n')
    r = subprocess.run([GP, 'fit', p, '--lo', '1000', '--hi', '1500'], capture_output=True)
    return r.stdout.decode('utf-8', 'replace').strip()


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    a = load(sys.argv[1]); b = load(sys.argv[2]); name = sys.argv[3]
    fa = a[a[:, 5] == 0]; fb = b[b[:, 5] == 0]
    ka = key(fa); kb = key(fb)
    # совпадение с допуском ±1 по ip
    sa = {int(k): i for i, k in enumerate(ka)}
    match_b = np.full(len(fb), -1)
    for j, k in enumerate(kb):
        for dd in (0, -1, 1):
            i = sa.get(int(k) + dd)
            if i is not None:
                match_b[j] = i
                break
    both = match_b >= 0
    print(f'принято fw {len(fa)}, unfold {len(fb)}; общих {both.sum()}, только unfold {(~both).sum()}, '
          f'только fw {len(fa) - len(set(match_b[both]))}')
    dA = fb[both, 3] - fa[match_b[both], 3]
    rel = dA / fa[match_b[both], 3]
    print(f'общие: ΔA (unfold − fw) среднее {dA.mean():+.3f} кода, σ {dA.std():.3f}; |ΔA| > 1 кода у {np.mean(np.abs(dA) > 1)*100:.2f} %')
    pw = fb[both, 6]
    for lo, hi in [(-1e9, 1), (1, 3), (3, 10), (10, 1e9)]:
        m = (pw >= lo) & (pw < hi)
        if m.sum():
            print(f'   хвост в окне (pred) {lo:g}…{hi:g} кодов: N {m.sum()}, ΔA ср {dA[m].mean():+.2f} σ {dA[m].std():.2f}, '
                  f'ΔA/A ср {rel[m].mean()*100:+.3f} %')
    # Cs-фотопик среди общих (A 560..630): смещение
    mcs = (fa[match_b[both], 3] > 560) & (fa[match_b[both], 3] < 630)
    print(f'   Cs-фотопик (общие): ΔA ср {dA[mcs].mean():+.3f} кода ({rel[mcs].mean()*100:+.3f} %), σ {dA[mcs].std():.3f}')
    ch = lambda A: np.clip(np.floor(2 * A + 0.5), 0, 8191).astype(int)
    h_new = np.bincount(ch(fb[~both, 3]), minlength=8192)
    h_com = np.bincount(ch(fb[both, 3]), minlength=8192)
    only_fw = np.setdiff1d(np.arange(len(fa)), match_b[both])
    h_lost = np.bincount(ch(fa[only_fw, 3]), minlength=8192)
    print('  спектр только-unfold (восстановленные):', gp(h_new, name + '_new'))
    print('  спектр общих (unfold):', gp(h_com, name + '_com'))
    print('  спектр только-fw (потеряны в unfold):', int(h_lost.sum()), 'событий;', gp(h_lost, name + '_lost') if h_lost.sum() > 500 else '')
    # доля восстановленных по типу
    nb = fb[~both]
    print(f'  восстановленные: с хвостом в окне > 3 кодов {np.mean(nb[:, 6] > 3)*100:.1f} %')
    reb = 8
    x = np.arange(0, 8192, reb) + reb / 2
    fig, ax = plt.subplots(2, 1, figsize=(10, 8))
    for h, lab in [(h_com, 'общие'), (h_new, 'только unfold (восстановлены)'), (h_lost, 'только fw (отброшены unfold)')]:
        ax[0].step(x, h.reshape(-1, reb).sum(1), where='mid', lw=0.8, label=f'{lab}: {int(h.sum())}')
    ax[0].set_yscale('log'); ax[0].set_xlim(0, 4200); ax[0].legend(); ax[0].grid(alpha=.3)
    hn = h_new.reshape(-1, reb).sum(1); hc = h_com.reshape(-1, reb).sum(1)
    with np.errstate(all='ignore'):
        ax[1].plot(x, hn / hc * hc.sum() / hn.sum(), lw=.8)
    ax[1].axhline(1, c='k', lw=.5); ax[1].set_ylim(0, 3); ax[1].set_xlim(0, 4200); ax[1].grid(alpha=.3)
    ax[1].set_ylabel('форма: восстановленные / общие (норм.)'); ax[1].set_xlabel('канал')
    fig.tight_layout(); fig.savefig(f'out/evcmp_{name}.png', dpi=110)


if __name__ == '__main__':
    main()
