"""Наводка на FB по реальной матрице ёмкостей (FastCap) и реальной форме A-колонки (LTspice).

Сеть: FB, CH1…CH9, CWO + резисторы 100 МОм (R207, R209–R217) и 2.2 МОм (R208 → HV_CONTROL, земля по AC).
Агрессоры A1…A4, SEC, DRN, SNB — источники напряжения с формой из переключательной модели.
Решение по гармоникам: Y(jw)·V = I, где I = jw·C_aggr·V_aggr.
"""
import json
import numpy as np
from spice_io import read_raw
from hv_deck import fsw_from_rfreq

EPS = {'типично': 2.75, 'максимум': 4.5}
CPIN = 3e-12          # ёмкость вывода FB MAX1847 (оценка; в даташите нет)
FSW = fsw_from_rfreq()
T = 1 / FSW
NODES = ['FB'] + [f'CH{i}' for i in range(1, 10)]     # CWO — жёсткий узел (70 нФ) → земля по AC
AGGR = ['A1', 'A2', 'A3', 'A4', 'SEC', 'DRN', 'SNB']
# цепочка: CWO -R207- CH1 -R209- CH2 ... -R216- CH9 -R217- FB ; FB -R208- земля(AC)
LINKS = [('CWO', 'CH1'), ('CH1', 'CH2'), ('CH2', 'CH3'), ('CH3', 'CH4'), ('CH4', 'CH5'),
         ('CH5', 'CH6'), ('CH6', 'CH7'), ('CH7', 'CH8'), ('CH8', 'CH9'), ('CH9', 'FB')]


def solve(vspec, eps, nharm=40):
    d = json.load(open('out/hv_caps.json'))
    names, C = d['names'], np.array(d['C_pF_air']) * 1e-12 * eps
    ix = {n: i for i, n in enumerate(names)}
    N = len(NODES)
    n_i = {n: k for k, n in enumerate(NODES)}
    out = np.zeros(nharm + 1, complex)
    for h in range(1, nharm + 1):
        w = 2j * np.pi * h * FSW
        Y = np.zeros((N, N), complex)
        I = np.zeros(N, complex)
        for a in NODES:
            ka = n_i[a]
            cg = -C[ix[a], ix['GND']] + (C[ix[a], ix[a]] - sum(-C[ix[a], ix[b]] for b in names if b != a))
            Y[ka, ka] += w * cg                                   # на землю (взаимная с GND + остаток)
            if a == 'FB':
                Y[ka, ka] += w * CPIN + 1 / 2.2e6                 # вывод MAX1847 + R208
            for b in NODES:
                if b != a:
                    cm = -C[ix[a], ix[b]]
                    Y[ka, ka] += w * cm
                    Y[ka, n_i[b]] -= w * cm
            for g in AGGR + ['CWO']:
                cm = -C[ix[a], ix[g]]
                Y[ka, ka] += w * cm
                I[ka] += w * cm * vspec[g][h]
        for a, b in LINKS:                                        # резисторы цепочки
            if a in n_i and b in n_i:
                Y[n_i[a], n_i[a]] += 1 / 100e6; Y[n_i[b], n_i[b]] += 1 / 100e6
                Y[n_i[a], n_i[b]] -= 1 / 100e6; Y[n_i[b], n_i[a]] -= 1 / 100e6
            elif b in n_i:                                        # CWO — источник (земля по AC)
                Y[n_i[b], n_i[b]] += 1 / 100e6; I[n_i[b]] += vspec[a][h] / 100e6
        out[h] = np.linalg.solve(Y, I)[n_i['FB']]
    return out


def main():
    d = read_raw('out/r1200.raw'); t = d['time']
    n = int((t[-1] - t[0] - 2e-3) / T); t0 = t[-1] - n * T
    tu = np.arange(t0, t0 + n * T, T / 512)
    # A-колонка: все узлы качаются синфазно с формой a1 (переменная часть)
    a1 = np.interp(tu, t, d['v(a1)'])
    sp_a = np.fft.rfft(a1) / len(a1) * 2
    f = np.fft.rfftfreq(len(a1), T / 512)
    harm = lambda sp: np.array([sp[np.argmin(abs(f - h * FSW))] for h in range(41)])
    ha = harm(sp_a)
    sp_d = harm(np.fft.rfft(np.interp(tu, t, d['v(drain)'])) / len(a1) * 2)
    sp_s = harm(np.fft.rfft(np.interp(tu, t, d['v(sec)'])) / len(a1) * 2)
    sp_k = harm(np.fft.rfft(np.interp(tu, t, d['v(snk)'])) / len(a1) * 2)
    sp_c = harm(np.fft.rfft(np.interp(tu, t, d['v(cw)'])) / len(a1) * 2)
    vspec = {'A1': ha, 'A2': ha, 'A3': ha, 'A4': ha, 'SEC': sp_s, 'DRN': sp_d, 'SNB': sp_k, 'CWO': sp_c}
    print(f'размах A-колонки {np.ptp(a1):.0f} В, стока {np.ptp(d["v(drain)"][t>=t0]):.1f} В')
    for tag, eps in EPS.items():
        V = solve(vspec, eps)
        tt = np.linspace(0, T, 400)
        y = sum((V[h] * np.exp(2j * np.pi * h * FSW * tt)).real for h in range(1, len(V)))
        print(f'{tag} (×{eps}): FB наводка {np.ptp(y)*1e3:7.2f} мВ пп; '
              f'1-я гармоника {abs(V[1])*1e3:.2f} мВ, 2-я {abs(V[2])*1e3:.2f} мВ')
        print('   эквивалентная Cs (по сравнению с прогоном Cs=1 фФ → 61 мВ пп): '
              f'{np.ptp(y)*1e3/61:.2f} фФ')


main()
