"""Сквозная проверка: реальная сеть FB (FastCap) в переключательной модели + варианты лечения."""
import sys, time
import numpy as np
from hv_deck import deck, fsw_from_rfreq
from spice_io import write, run, read_raw

T = 1 / fsw_from_rfreq()
V = 1200
SAVES = 'V(cw) V(comp) V(src) V(fb) V(hvout) I(Vsense)'
CASES = {
    'идеал (без сети)':        dict(),
    'сеть ×2.75':              dict(fbnet=2.75),
    'сеть ×4.5':               dict(fbnet=4.5),
    'сеть + Cc2 22п':          dict(fbnet=2.75, Cc2=22e-12),
    'сеть + Cc2 100п':         dict(fbnet=2.75, Cc2=100e-12),
    'сеть + Cfb 22п':          dict(fbnet=2.75, extra='Cfbx fb 0 22p'),
    'сеть + Cfb 100п':         dict(fbnet=2.75, extra='Cfbx fb 0 100p'),
    'сеть + Cc2 100п + Cfb 22п': dict(fbnet=2.75, Cc2=100e-12, extra='Cfbx fb 0 22p'),
}
NAMES = {k: f'fbn{i}' for i, k in enumerate(CASES)}

if __name__ == '__main__':
    if 'noSim' not in sys.argv:
        paths = []
        for k, kw in CASES.items():
            L, p = deck(NAMES[k], Vctrl=V / 454.5, tstop=10e-3, tsave=6e-3, saves=SAVES, **kw)
            paths.append(write(NAMES[k], L))
        t = time.time(); run(paths, par=4); print('LTspice %.0f с' % (time.time() - t), flush=True)
    for k in CASES:
        d = read_raw(f'out/{NAMES[k]}.raw'); t = d['time']
        n = int((t[-1] - t[0] - 1e-3) / T); t0 = t[-1] - n * T
        idx = np.searchsorted(t, t0 + np.arange(n + 1) * T)
        pk = np.array([d['v(src)'][idx[i]:idx[i + 1]].max() for i in range(n)])
        s = t >= t0
        cw = np.trapezoid(d['v(cw)'][s], t[s]) / (t[s][-1] - t[s][0])
        hv = np.trapezoid(d['v(hvout)'][s], t[s]) / (t[s][-1] - t[s][0])
        print(f'{k:26s}: FB {np.ptp(d["v(fb)"][s])*1e3:7.1f} мВ пп, COMP {np.ptp(d["v(comp)"][s])*1e3:7.1f} мВ пп, '
              f'Vcs {pk.mean()*1e3:5.1f}±{pk.std()*1e3:4.2f} мВ, CW {cw:8.2f} В, HVout {hv:8.2f} В', flush=True)
