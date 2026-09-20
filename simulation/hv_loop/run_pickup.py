"""Чувствительность к наводке на FB: паразитная ёмкость Cs от узла умножителя a3 (размах ~Vpp) на FB.
Сравнение без C_COMP2 и с C_COMP2 = 22 пФ (рекомендация даташита: спад на 5·f_среза)."""
import sys, time
import numpy as np
from hv_deck import deck, fsw_from_rfreq
from spice_io import write, run, read_raw

T = 1 / fsw_from_rfreq()
CASES = [(0, 0), (1e-15, 0), (3e-15, 0), (10e-15, 0), (30e-15, 0), (10e-15, 22e-12), (30e-15, 22e-12), (100e-15, 22e-12)]
V = 1200


def name(cs, cc2):
    return f'pk_{int(cs*1e15)}f_{int(cc2*1e12)}p'


if __name__ == '__main__':
    if 'noSim' not in sys.argv:
        paths = []
        for cs, cc2 in CASES:
            L, p = deck(name(cs, cc2), Vctrl=V / 454.5, tstop=10e-3, tsave=6e-3, Cc2=cc2,
                        extra=f'Cpick a3 fb {cs}' if cs else '',
                        saves='V(cw) V(comp) V(src) V(fb) I(Vsense)')
            paths.append(write(name(cs, cc2), L))
        t = time.time(); run(paths, par=8); print('LTspice %.0f с' % (time.time() - t))
    ref = None
    for cs, cc2 in CASES:
        d = read_raw(f'out/{name(cs, cc2)}.raw'); t = d['time']
        n = int((t[-1] - t[0] - 1e-3) / T); t0 = t[-1] - n * T
        idx = np.searchsorted(t, t0 + np.arange(n + 1) * T)
        pk = np.array([d['v(src)'][idx[k]:idx[k + 1]].max() for k in range(n)])
        s = t >= t0
        fb = d['v(fb)'][s]; comp = d['v(comp)'][s]
        cw = np.trapezoid(d['v(cw)'][s], t[s]) / (t[s][-1] - t[s][0])
        ref = cw if ref is None else ref
        print(f'Cs {cs*1e15:4.0f} фФ, Cc2 {cc2*1e12:3.0f} пФ: FB пп {(fb.max()-fb.min())*1e3:7.2f} мВ, '
              f'COMP пп {(comp.max()-comp.min())*1e3:7.1f} мВ, Vcs пик {pk.mean()*1e3:5.1f}±{pk.std()*1e3:4.2f} мВ '
              f'(мин {pk.min()*1e3:4.1f}), CW {cw:8.2f} В (Δ {cw-ref:+.2f})')
