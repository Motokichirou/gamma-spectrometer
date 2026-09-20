"""Наводка на FB при разных ВВ: размах A-колонки растёт с напряжением."""
import sys, time
import numpy as np
from hv_deck import deck, fsw_from_rfreq
from spice_io import write, run, read_raw

T = 1 / fsw_from_rfreq()
CASES = {'1500 сеть': (1500, {}), '1500 сеть+Cc2 22п': (1500, dict(Cc2=22e-12)),
         '500 сеть': (500, {}), '500 сеть+Cc2 22п': (500, dict(Cc2=22e-12))}
NAMES = {k: f'fbv{i}' for i, k in enumerate(CASES)}
SAVES = 'V(cw) V(comp) V(src) V(fb) V(hvout) I(Vsense)'

if __name__ == '__main__':
    if 'noSim' not in sys.argv:
        paths = []
        for k, (v, kw) in CASES.items():
            L, p = deck(NAMES[k], Vctrl=min(v / 454.5, 3.3), tstop=10e-3, tsave=6e-3,
                        saves=SAVES, fbnet=2.75, **kw)
            paths.append(write(NAMES[k], L))
        t = time.time(); run(paths, par=4); print('LTspice %.0f с' % (time.time() - t), flush=True)
    for k, (v, kw) in CASES.items():
        d = read_raw(f'out/{NAMES[k]}.raw'); t = d['time']
        n = int((t[-1] - t[0] - 1e-3) / T); t0 = t[-1] - n * T
        idx = np.searchsorted(t, t0 + np.arange(n + 1) * T)
        pk = np.array([d['v(src)'][idx[i]:idx[i + 1]].max() for i in range(n)])
        s = t >= t0
        tu = np.arange(t[0], t[-1], 0.4e-3); cw = np.interp(tu, t, d['v(cw)'])
        print(f'{k:20s}: FB {np.ptp(d["v(fb)"][s])*1e3:6.1f} мВ пп, COMP {np.ptp(d["v(comp)"][s])*1e3:7.1f} мВ пп, '
              f'Vcs {pk.mean()*1e3:5.1f}±{pk.std()*1e3:5.2f}, уставка {-v:6.0f} В, CW по 0.4 мс: '
              + ' '.join(f'{x:8.1f}' for x in cw), flush=True)
