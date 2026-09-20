"""Петлевое усиление MAX1847 на переключательной модели: синус Vinj между FB и входом EA (fbx).
T(f) = −V(fb)/V(fbx) на частоте инжекции (синхронное детектирование по целому числу периодов)."""
import sys, time
import numpy as np
from hv_deck import deck
from spice_io import write, run, read_raw

AMP = 1e-4


def case_name(v, f):
    return f'fra_{v}_{int(f)}'


def plan(v, freqs):
    paths = []
    for f in freqs:
        per = 1 / f
        settle = max(3e-3, 1.5 * per)
        nper = max(3, int(np.ceil(4e-3 / per)))
        tstop = settle + nper * per
        L, p = deck(case_name(v, f), Vctrl=min(v / 454.5, 3.3), inj=(AMP, f), tstop=tstop, tsave=0,
                    saves='V(fb) V(fbx) V(cw) V(comp)', opts='reltol=1e-4')
        L.insert(-1, f'* settle={settle} nper={nper}')
        paths.append(write(case_name(v, f), L))
    return paths


def lockin(t, y, f, t0, t1):
    s = (t >= t0) & (t <= t1)
    tt, yy = t[s], y[s]
    tu = np.linspace(t0, t1, 20000, endpoint=False)
    yu = np.interp(tu, tt, yy)
    A = np.vstack([np.sin(2 * np.pi * f * tu), np.cos(2 * np.pi * f * tu), np.ones_like(tu), tu - tu.mean()]).T
    c, *_ = np.linalg.lstsq(A, yu, rcond=None)
    return c[0] + 1j * c[1]           # фазор: y = a·sin + b·cos = Im{(a + jb)·e^{jwt}}


def measure(v, f):
    d = read_raw(f'out/{case_name(v, f)}.raw')
    t = d['time']
    per = 1 / f
    nper = max(3, int(np.ceil(4e-3 / per)))
    t1 = t[-1]; t0 = t1 - nper * per
    Vfb = lockin(t, d['v(fb)'], f, t0, t1)
    Vfx = lockin(t, d['v(fbx)'], f, t0, t1)
    return -Vfb / Vfx


if __name__ == '__main__':
    sets = {1200: [50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000],
            500: [100, 500, 2000, 5000, 20000],
            1500: [100, 500, 2000, 5000, 20000]}
    if 'noSim' not in sys.argv:
        paths = sum((plan(v, fs) for v, fs in sets.items()), [])
        t = time.time(); run(paths, par=8); print('LTspice %.0f с' % (time.time() - t))
    for v, fs in sets.items():
        print(f'== {v} В')
        for f in fs:
            try:
                T = measure(v, f)
                print(f'  {f:6d} Гц: |T| {20*np.log10(abs(T)):6.1f} дБ  фаза {np.degrees(np.angle(T)):7.1f}°')
            except Exception as e:
                print(f'  {f:6d} Гц: ошибка {e}')
