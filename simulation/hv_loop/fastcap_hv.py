# -*- coding: utf-8 -*-
"""
hv_loop/fastcap_hv.py — паразитные ёмкости цепи обратной связи платы ВВ (FastCap2).

Вход: out/hv_geom.json (extract_hv.py). Выход: out/hv.qui, out/hv.fc.txt, out/hv_caps.json.

Проводники: FB (вход усилителя ошибки), CH1…CH9 (узлы цепочки 1 ГОм), CWO (выход умножителя),
A1…A4 (толкающая колонка — агрессоры, размах ~Vpp на 252 кГц), SEC (T201.3), DRN (сток Q201),
SNB (снаббер), GND (земля + все прочие цепи: по переменному току они на земле).

Медь — тонкие панели на z = 0 (B.Cu) и z = T (F.Cu); мелкие панели в пределах ROI мм от меди
FB-цепей и A-колонки, дальше блоки K×HF. Среда однородная εr = 1 → множитель ×2.75 типично
(дорожки на поверхности двустороннего FR-4), ×4.5 — верхняя граница.
Корпуса компонентов (диоды, конденсаторы) не моделируются — связь через них не учтена.
"""
import os, re, sys, json, subprocess
import numpy as np
from matplotlib.path import Path
from scipy import ndimage

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
FASTCAP = r'C:\Users\motok\tools\fastfield\bin\fastcap.exe'
HF, K, ROI = 0.15, 4, 2.0
EPS_TYP, EPS_MAX = 2.75, 4.5

COND = {
    'Net-(U201-FB)': 'FB',
    'Net-(R207-Pad2)': 'CH1', 'Net-(R209-Pad2)': 'CH2', 'Net-(R210-Pad2)': 'CH3',
    'Net-(R211-Pad2)': 'CH4', 'Net-(R212-Pad2)': 'CH5', 'Net-(R213-Pad2)': 'CH6',
    'Net-(R214-Pad2)': 'CH7', 'Net-(R215-Pad2)': 'CH8', 'Net-(R216-Pad2)': 'CH9',
    'Net-(C13a201-Pad1)': 'CWO',
    'Net-(C210-Pad2)': 'A1', 'Net-(C212-Pad2)': 'A2', 'Net-(C214-Pad2)': 'A3', 'Net-(C216-Pad2)': 'A4',
    'Net-(C210-Pad1)': 'SEC', 'Net-(D201-A)': 'DRN', 'Net-(D201-K)': 'SNB',
    'Net-(U201-COMP)': 'COMP', 'HV_CONTROL': 'HVC',
}
AGGR = ('A1', 'A2', 'A3', 'A4', 'SEC', 'DRN', 'SNB')
VICT = ('FB', 'CH1', 'CH2', 'CH3', 'CH4', 'CH5', 'CH6', 'CH7', 'CH8', 'CH9', 'COMP', 'HVC')


def raster(g):
    cx, cy, r = g['board']['cx'], g['board']['cy'], g['board']['r']
    xs = np.arange(cx - r + HF / 2, cx + r, HF)
    n = len(xs)
    X, Y = np.meshgrid(xs, xs + (cy - cx))
    P = np.column_stack([X.ravel(), Y.ravel()])
    conds = ['GND'] + sorted(set(COND.values()))
    idx_of = {c: i for i, c in enumerate(conds)}
    lab = {L: np.full(len(P), -1, np.int32) for L in ('F.Cu', 'B.Cu')}
    for net, layers in g['nets'].items():
        ci = idx_of[COND.get(net, 'GND')]
        for L, polys in layers.items():
            if L not in lab:
                continue
            for poly in polys:
                o = np.asarray(poly['outline'])
                if len(o) < 3:
                    continue
                x0, y0 = o.min(0)
                x1, y1 = o.max(0)
                sel = np.nonzero((P[:, 0] >= x0) & (P[:, 0] <= x1) & (P[:, 1] >= y0) & (P[:, 1] <= y1))[0]
                if not len(sel):
                    continue
                m = Path(o).contains_points(P[sel])
                for h in poly['holes']:
                    if len(h) >= 3:
                        m &= ~Path(np.asarray(h)).contains_points(P[sel])
                cur = lab[L][sel[m]]
                # именованный проводник важнее земли, если пады/зоны перекрылись
                lab[L][sel[m]] = np.where((cur > 0) & (ci == 0), cur, ci)
    return xs, n, conds, {L: v.reshape(n, n) for L, v in lab.items()}


def write_qui(g, path):
    xs, n, conds, lab = raster(g)
    T = g['board']['thickness_mm']
    keep = [conds.index(c) for c in VICT + AGGR if c in conds]
    near = np.zeros((n, n), bool)
    for L in lab:
        near |= np.isin(lab[L], keep)
    fine = ndimage.distance_transform_edt(~near) * HF <= ROI
    x0 = xs[0] - HF / 2
    y0 = xs[0] + (g['board']['cy'] - g['board']['cx']) - HF / 2
    lines = ['0 HV board FB parasitics']
    cnt = 0

    def quad(name, xa, ya, xb, yb, z):
        nonlocal cnt
        cnt += 1
        s = 1e-3
        lines.append(f'Q {name} {xa*s:.7e} {ya*s:.7e} {z*s:.7e} {xb*s:.7e} {ya*s:.7e} {z*s:.7e} '
                     f'{xb*s:.7e} {yb*s:.7e} {z*s:.7e} {xa*s:.7e} {yb*s:.7e} {z*s:.7e}')

    for L, z in (('F.Cu', T), ('B.Cu', 0.0)):
        A = lab[L]
        for bi in range(0, n, K):
            for bj in range(0, n, K):
                blk = A[bi:bi + K, bj:bj + K]
                if (blk < 0).all():
                    continue
                if blk.shape == (K, K) and not fine[bi:bi + K, bj:bj + K].any() and (blk == blk[0, 0]).all():
                    quad(conds[blk[0, 0]], x0 + bj * HF, y0 + bi * HF,
                         x0 + (bj + K) * HF, y0 + (bi + K) * HF, z)
                    continue
                for di in range(blk.shape[0]):
                    for dj in range(blk.shape[1]):
                        v = blk[di, dj]
                        if v >= 0:
                            i, j = bi + di, bj + dj
                            quad(conds[v], x0 + j * HF, y0 + i * HF, x0 + (j + 1) * HF, y0 + (i + 1) * HF, z)
    open(path, 'w', encoding='ascii').write('\n'.join(lines) + '\n')
    return cnt, conds


UNITS = {'attofarads': 1e-6, 'femtofarads': 1e-3, 'picofarads': 1.0, 'nanofarads': 1e3, 'microfarads': 1e6}


def run_fastcap(qui):
    out = qui.replace('.qui', '.fc.txt')
    if '--parse-only' not in sys.argv:
        with open(out, 'w', encoding='ascii', errors='replace') as f:
            subprocess.run([FASTCAP, os.path.basename(qui)], cwd=OUT, stdout=f, stderr=subprocess.STDOUT)
    txt = open(out, encoding='ascii', errors='replace').read()
    m = re.search(r'CAPACITANCE MATRIX, (\w+)', txt)
    if not m:
        raise RuntimeError('FastCap: нет матрицы в ' + out + '\n' + txt[-2000:])
    unit = UNITS[m.group(1)]
    rows, order = {}, []
    for line in txt[m.end():].splitlines():
        g = re.match(r'^\s*(\S+?)%\S*\s+(\d+)\s+(.*)$', line)
        if g:
            k = int(g.group(2))
            if k not in rows:
                rows[k] = (g.group(1), [])
                order.append(k)
            rows[k][1].extend(float(v) for v in g.group(3).split())
    names = [rows[k][0] for k in sorted(rows)]
    C = np.array([rows[k][1] for k in sorted(rows)]) * unit      # пФ
    return names, C


def main():
    g = json.load(open(os.path.join(OUT, 'hv_geom.json'), encoding='utf-8'))
    q = os.path.join(OUT, 'hv.qui')
    if '--parse-only' not in sys.argv:
        cnt, conds = write_qui(g, q)
        print(f'панелей {cnt}, проводников {len(conds)}: {conds}')
    names, C = run_fastcap(q)
    json.dump({'names': names, 'C_pF_air': C.tolist()}, open(os.path.join(OUT, 'hv_caps.json'), 'w'))
    ix = {n: i for i, n in enumerate(names)}

    def c(a, b):
        return -C[ix[a], ix[b]] if a in ix and b in ix else float('nan')

    print(f'\nвзаимные ёмкости, фФ (воздух / ×{EPS_TYP} типично / ×{EPS_MAX} максимум)')
    print('          ' + ''.join(f'{a:>10s}' for a in AGGR))
    for v in VICT:
        if v not in ix:
            continue
        print(f'{v:8s}' + ''.join(f'{c(v, a)*1e3*EPS_TYP:10.2f}' if a in ix else '         —' for a in AGGR))
    print('\nсобственные ёмкости на землю, фФ (×%.2f):' % EPS_TYP)
    for v in VICT:
        if v in ix:
            print(f'  {v:6s} {C[ix[v], ix[v]]*1e3*EPS_TYP:8.1f}')


if __name__ == '__main__':
    main()
