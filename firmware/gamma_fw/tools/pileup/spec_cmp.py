# -*- coding: utf-8 -*-
"""spec_cmp.py — сравнение спектров (сумма файлов) двух прогонов run.py.
python -I spec_cmp.py tagA tagB name   (tagX — префикс через запятую: out/fw_mix1,out/fw_mix2,...)"""
import os
import subprocess
import sys
import numpy as np
from scipy.optimize import curve_fit
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

GP = r'D:\gamma-spectrometer\host\build\Release\gammapult.com'


def load(tag):
    h = np.zeros(8192)
    for t in tag.split(','):
        h += np.loadtxt(t + '.csv', delimiter=',', skiprows=1)[:, 1]
    return h


def gfit(h, lo, hi, npk, mu0):
    x = np.arange(lo, hi); y = h[lo:hi]

    def f(x, *p):
        r = p[0] + p[1] * (x - lo)
        for k in range(npk):
            a, m, s = p[2 + 3 * k: 5 + 3 * k]
            r = r + a * np.exp(-0.5 * ((x - m) / s) ** 2)
        return r
    p0 = [y[:5].mean(), (y[-5:].mean() - y[:5].mean()) / (hi - lo)]
    for m in mu0:
        p0 += [h[int(m)] - p0[0], m, 0.03 * m]
    p, cv = curve_fit(f, x, y, p0=p0, sigma=np.sqrt(np.maximum(y, 1)), maxfev=20000)
    out = []
    for k in range(npk):
        a, m, s = p[2 + 3 * k: 5 + 3 * k]
        area = a * abs(s) * np.sqrt(2 * np.pi)
        ea = area * np.sqrt(cv[2 + 3 * k, 2 + 3 * k] / a ** 2 + cv[4 + 3 * k, 4 + 3 * k] / s ** 2)
        out.append((m, abs(s), 235.48 * abs(s) / m, area, ea))
    return out


def gp_fit(h, name):
    p = f'out/_{name}.csv'
    with open(p, 'w') as f:
        f.write('channel,count\n')
        for i, c in enumerate(h):
            f.write(f'{i},{int(c)}\n')
    r = subprocess.run([GP, 'fit', p, '--lo', '1000', '--hi', '1500'], capture_output=True)
    return r.stdout.decode('utf-8', 'replace').strip()


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ta, tb, name = sys.argv[1:4]
    labels = sys.argv[4].split(',') if len(sys.argv) > 4 else ['fw', 'unfold']
    H = [load(ta), load(tb)]
    for lab, h in zip(labels, H):
        print(f'== {lab}: всего {int(h.sum())}')
        print('  gammapult fit 1000–1500:', gp_fit(h, f'{name}_{lab}'))
        try:
            am = gfit(h, 95, 210, 1, [145])[0]
            print(f'  Am 59.5: пик {am[0]:.1f}, FWHM {am[2]:.2f} %, площадь {am[3]:.0f} ± {am[4]:.0f}')
        except Exception as e:
            print('  Am: подгонка не сошлась', e)
        try:
            ra = gfit(h, 585, 720, 1, [645])[0]
            print(f'  Ra 352: пик {ra[0]:.1f}, FWHM {ra[2]:.2f} %, площадь {ra[3]:.0f} ± {ra[4]:.0f}')
        except Exception as e:
            print('  Ra352: подгонка не сошлась', e)
        try:
            bi, cs = gfit(h, 1000, 1400, 2, [1110, 1190])
            print(f'  Bi 609 + Cs 662 (2 гаусса): Cs пик {cs[0]:.1f}, FWHM {cs[2]:.2f} %, площадь {cs[3]:.0f} ± {cs[4]:.0f}; '
                  f'Bi пик {bi[0]:.1f} площадь {bi[3]:.0f}')
        except Exception as e:
            print('  Cs: подгонка не сошлась', e)
        lo = h[:250].sum(); hi = h[2600:].sum()
        print(f'  каналы < 250: {int(lo)}, 250–2600: {int(h[250:2600].sum())}, > 2600: {int(hi)}')
    # графики
    fig, ax = plt.subplots(3, 1, figsize=(10, 11))
    reb = 8
    x = np.arange(0, 8192, reb) + reb / 2
    hs = [h.reshape(-1, reb).sum(1) for h in H]
    for lab, hh in zip(labels, hs):
        ax[0].step(x, hh, where='mid', lw=0.8, label=lab)
    ax[0].set_yscale('log'); ax[0].set_xlim(0, 4200); ax[0].legend(); ax[0].grid(alpha=.3)
    ax[0].set_ylabel(f'отсчётов / {reb} кан.')
    with np.errstate(all='ignore'):
        ratio = hs[1] / hs[0]
    ax[1].plot(x, ratio, lw=0.8); ax[1].axhline(1, c='k', lw=.5)
    ax[1].set_xlim(0, 4200); ax[1].set_ylim(0.5, 2.0); ax[1].grid(alpha=.3)
    ax[1].set_ylabel(f'{labels[1]} / {labels[0]}')
    for lab, h in zip(labels, H):
        ax[2].step(np.arange(8192), h, where='mid', lw=0.8, label=lab)
    ax[2].set_xlim(950, 1450); ax[2].legend(); ax[2].grid(alpha=.3); ax[2].set_xlabel('канал')
    fig.tight_layout(); fig.savefig(f'out/spec_{name}.png', dpi=110)


if __name__ == '__main__':
    main()
