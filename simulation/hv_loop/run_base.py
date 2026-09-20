"""Базовые прогоны: установление при 1200 и 1500 В, ток стабилитронов, полка стока."""
import sys, time
import numpy as np
from hv_deck import deck
from spice_io import write, run, read_raw, read_log

cases = {'b1200': 1200 / 454.5, 'b1500': 3.3}
paths = []
for n, vc in cases.items():
    L, p = deck(n, Vctrl=vc, tstop=30e-3, tsave=0)
    paths.append(write(n, L))
t = time.time(); run(paths); print('LTspice %.0f с' % (time.time() - t))
for n in cases:
    d = read_raw(f'out/{n}.raw')
    t = d['time']
    print(f'== {n}')
    for a in np.arange(0, 30e-3, 3e-3):
        s = (t >= a) & (t < a + 3e-3)
        print(f'  {a*1e3:4.0f}–{a*1e3+3:2.0f} мс: CW {d["v(cw)"][s].mean():8.1f}  HVout {d["v(hvout)"][s].mean():8.1f}'
              f'  COMP {d["v(comp)"][s].mean():.3f}  сток max {d["v(drain)"][s].max():5.1f}'
              f'  Vcs max {d["v(src)"][s].max()*1e3:5.1f} мВ  Iz ср {d["i(d202)"][s].mean()*-1e3:6.3f} мА'
              f'  Iin {d["i(lpri)"][s].mean()*1e3:6.1f} мА')
