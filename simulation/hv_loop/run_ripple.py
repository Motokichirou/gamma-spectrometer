"""Пульсации: ток умножителя I(Vsense) из переключательной модели → линейная сеть выходных фильтров."""
import time
import numpy as np
from hv_deck import deck, fsw_from_rfreq
from spice_io import write, run, read_raw

VS = [500, 800, 1200, 1500]
FSW = fsw_from_rfreq()
T = 1 / FSW


def sim():
    paths = []
    for v in VS:
        L, p = deck(f'r{v}', Vctrl=min(v / 454.5, 3.3), tstop=14e-3, tsave=6e-3)
        paths.append(write(f'r{v}', L))
    t = time.time(); run(paths); print('LTspice %.0f с' % (time.time() - t))


def net_response(f, par):
    """Передача тока умножителя (A) в напряжения узлов cw,n1,n2,n3,hvout (комплексно)."""
    w = 2j * np.pi * f
    Zc = lambda C, esl=0, esr=0: 1 / (w * C) + w * esl + esr
    Zr = lambda R, cp: 1 / (1 / R + w * cp)            # резистор 0402 с ёмкостью 0.05 пФ
    Cd, Cf = par['Cd'], par['Cf']
    Rl = 1 / (1 / 7.73e6 + 1 / 68.11e6)
    # от выхода к входу: hvout
    Zhv = 1 / (1 / Rl + 1 / Zc(110e-12, 1e-9))
    Z3 = Zr(10, 0) + Zhv
    Zn3 = 1 / (1 / Zc(Cf, 2e-9, 0.04) + 1 / Z3)
    Z2 = Zr(100e3, 0.05e-12) + Zn3
    Zn2 = 1 / (1 / Zc(Cf, 2e-9, 0.04) + 1 / Z2)
    Z1 = Zr(100e3, 0.05e-12) + Zn2
    Zn1 = 1 / (1 / Zc(Cf, 2e-9, 0.04) + 1 / Z1)
    Z0 = Zr(100e3, 0.05e-12) + Zn1
    Zcw = 1 / (2 / Zc(Cd, 2e-9, 0.04) + 1 / Z0 + 1 / 1e9)
    vcw = Zcw
    vn1 = vcw * Zn1 / Z0
    vn2 = vn1 * Zn2 / Z1
    vn3 = vn2 * Zn3 / Z2
    vhv = vn3 * Zhv / Z3
    return dict(cw=vcw, n1=vn1, n2=vn2, n3=vn3, hvout=vhv)


def analyze():
    from hv_deck import DEFAULTS
    res = {}
    for v in VS:
        d = read_raw(f'out/r{v}.raw')
        t = d['time']
        # окно — целое число периодов в конце
        n = int((t[-1] - t[0] - 2e-3) / T)
        t0 = t[-1] - n * T
        dt = T / 512
        tu = np.arange(t0, t0 + n * T, dt)
        i = np.interp(tu, t, d['i(vsense)'])
        I = np.fft.rfft(i) / len(i)
        f = np.fft.rfftfreq(len(i), dt)
        H = net_response(np.where(f == 0, 1e-3, f), DEFAULTS)
        out = {}
        for k, h in H.items():
            spec = I * h
            spec[f < 5e3] = 0          # ниже 5 кГц — дрейф установления из НУ, не пульсации
            y = np.fft.irfft(spec * len(i), len(i))
            out[k] = (y.max() - y.min(), spec)
        # низкочастотная часть тока (< fsw/2) — признак пачек/субгармоник
        lf = np.abs(I[(f > 0) & (f < FSW / 2)]).max() * 2
        h1 = np.abs(I[np.argmin(abs(f - FSW))]) * 2
        # пики Vcs по циклам — стабильность цикла
        vcs = d['v(src)']
        pk = []
        for k in range(n):
            s = (t >= t0 + k * T) & (t < t0 + (k + 1) * T)
            pk.append(vcs[s].max())
        pk = np.array(pk)
        vin = d['v(vhv)'][t >= t0]
        res[v] = out
        print(f'== {v} В: I_dc {abs(I[0].real)*1e6:.1f} мкА, ток на fsw {h1*1e6:.1f} мкА, макс. НЧ (<fsw/2) {lf*1e6:.2f} мкА')
        print(f'   Vcs пик по циклам: {pk.mean()*1e3:.1f} ± {pk.std()*1e3:.2f} мВ (мин {pk.min()*1e3:.1f}, макс {pk.max()*1e3:.1f});'
              f' +5V_HV пульсации {(vin.max()-vin.min())*1e3:.0f} мВ пп')
        print('   пульсации пп (>5 кГц): ' + '  '.join(f'{k} {o[0]*1e3:.3g} мВ' for k, o in out.items()))
        # гармоники на hvout
        sp = out['hvout'][1]
        hs = [np.abs(sp[np.argmin(abs(f - m * FSW))]) * 2 for m in (0.5, 1, 2, 10)]
        print('   hvout: fsw/2, fsw, 2fsw, 10fsw: ' + ', '.join(f'{x*1e6:.3g} мкВ' for x in hs))
    return res


if __name__ == '__main__':
    import sys
    if 'noSim' not in sys.argv:
        sim()
    analyze()
