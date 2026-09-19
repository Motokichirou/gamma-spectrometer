# -*- coding: utf-8 -*-
"""
ground_mcu/solve_ground.py — резистивная модель земли платы МК (постоянный ток и НЧ, примерно до 1 МГц).

Вход:  out/mcu_gnd_geom.json (extract_geom.py), out/transfer.npz (spice_transfer.py)
Выход: out/ground_results.json, out/map_*.png, таблицы в консоль

Модель:
  • медь GND обоих слоёв растрируется сеткой H мм; соседние клетки соединены проводимостью
    t/ρ (0.49 мОм на квадрат при 35 мкм) — не зависит от шага сетки;
  • GND-виа: R_VIA между слоями (распределено по клеткам пятака);
  • выходы земли в стек (цанги INTERCONNECT_PIN, стойки STANDOFF): кольцо пада на обоих слоях →
    узел выхода → R_EXIT → «земля стека» (0 В);
  • COAX3 (оплётка коаксиала от делителя): межслойное соединение + точка входа тока делителя.
Токи задаются шаблонами (1 А), напряжения меряются средним по клеткам пада.
Перевод на АЦП: e(f) = Σ_k H_k(f)·(V_k − V_VSSA), H_k — из spice_transfer.py.
"""
import os, sys, json, math
import numpy as np
from matplotlib.path import Path
import scipy.sparse as sp
from scipy.sparse.linalg import splu
from scipy.sparse.csgraph import connected_components

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')

H = 0.1                          # шаг сетки, мм
RHO, T_CU = 1.72e-8, 35e-6
G_SHEET = T_CU / RHO             # См на квадрат (0.491 мОм/кв)
R_VIA = 1.5e-3                   # металлизированное виа 0.3 мм, 20 мкм, 1.6 мм
R_EXIT = 3e-3                    # цанга/стойка до земли стека (оценка)
G_CONTACT = 1e4                  # пайка пада к пину
CX, CY, RB = 240.0, 100.0, 22.5
ANALOG_RECT = (225.9, 243.4, 84.0, 116.2)   # зона шейпера (по расстановке R305–R314, C314–C322, U301)
CH_PER_V = 8192 / 2.5
K_HV = 454.5                     # V_HV / V_CTRL

xs = np.arange(CX - RB - 0.3 + H / 2, CX + RB + 0.3, H)
ys = np.arange(CY - RB - 0.3 + H / 2, CY + RB + 0.3, H)
NX, NY = len(xs), len(ys)
GX, GY = np.meshgrid(xs, ys)
P = np.column_stack([GX.ravel(), GY.ravel()])
N = len(P)
INBOARD = (P[:, 0] - CX) ** 2 + (P[:, 1] - CY) ** 2 <= RB ** 2


def fill_poly(mask, poly):
    o = np.asarray(poly['outline'])
    if len(o) < 3:
        return
    x0, y0 = o.min(0)
    x1, y1 = o.max(0)
    idx = np.nonzero((P[:, 0] >= x0) & (P[:, 0] <= x1) & (P[:, 1] >= y0) & (P[:, 1] <= y1))[0]
    if not len(idx):
        return
    m = Path(o).contains_points(P[idx])
    for h in poly['holes']:
        hh = np.asarray(h)
        if len(hh) < 3:
            continue
        hx0, hy0 = hh.min(0)
        hx1, hy1 = hh.max(0)
        sub = np.nonzero(m & (P[idx, 0] >= hx0) & (P[idx, 0] <= hx1) & (P[idx, 1] >= hy0) & (P[idx, 1] <= hy1))[0]
        if len(sub):
            m[sub[Path(hh).contains_points(P[idx[sub]])]] = False
    mask[idx[m]] = True


def disk(x, y, r):
    return (P[:, 0] - x) ** 2 + (P[:, 1] - y) ** 2 <= r * r


def build(geom, solid_b_rect=None):
    masks = {'F.Cu': np.zeros(N, bool), 'B.Cu': np.zeros(N, bool)}
    for L in masks:
        for p in geom['zones'][L]:
            fill_poly(masks[L], p)
    pads = geom['pads']
    padcells = [dict() for _ in pads]
    for i, pd in enumerate(pads):
        if pd['net'] != 'GND':
            continue
        for L, polys in pd['poly'].items():
            tmp = np.zeros(N, bool)
            for p in polys:
                fill_poly(tmp, p)
            if pd['tht'] and pd['drill'] > 0:
                tmp &= ~disk(pd['x'], pd['y'], pd['drill'] / 2)
            if not tmp.any():                      # мелкий пад: хотя бы ближайшая клетка
                tmp[np.argmin((P[:, 0] - pd['x']) ** 2 + (P[:, 1] - pd['y']) ** 2)] = True
            masks[L] |= tmp
            padcells[i][L] = np.nonzero(tmp)[0]
    for t in geom['tracks']:
        a = np.array([t['x1'], t['y1']])
        b = np.array([t['x2'], t['y2']])
        ab = b - a
        u = np.clip(((P - a) @ ab) / max(ab @ ab, 1e-12), 0, 1)
        d = np.hypot(*(P - (a + u[:, None] * ab)).T)
        if t['layer'] in masks:
            masks[t['layer']] |= d <= t['w'] / 2
    for v in geom['vias']:
        for L in masks:
            masks[L] |= disk(v['x'], v['y'], v['dia'] / 2)
    if solid_b_rect:
        x0, x1, y0, y1 = solid_b_rect
        masks['B.Cu'] |= (P[:, 0] >= x0) & (P[:, 0] <= x1) & (P[:, 1] >= y0) & (P[:, 1] <= y1)
    for i, pd in enumerate(pads):
        if pd['net'] == 'GND' and pd['tht'] and pd['drill'] > 0:
            for L in masks:
                masks[L] &= ~disk(pd['x'], pd['y'], pd['drill'] / 2)
    for L in masks:
        masks[L] &= INBOARD

    idF = -np.ones(N, np.int64)
    idB = -np.ones(N, np.int64)
    nF, nB = int(masks['F.Cu'].sum()), int(masks['B.Cu'].sum())
    idF[masks['F.Cu']] = np.arange(nF)
    idB[masks['B.Cu']] = nF + np.arange(nB)
    ids = {'F.Cu': idF, 'B.Cu': idB}
    rows, cols, vals = [], [], []

    def edge(a, b, g):
        rows.extend([a, b, a, b])
        cols.extend([a, b, b, a])
        vals.extend([g, g, -g, -g])

    for L, idx in ids.items():
        M = (idx >= 0).reshape(NY, NX)
        I = idx.reshape(NY, NX)
        hm = M[:, :-1] & M[:, 1:]
        vm = M[:-1, :] & M[1:, :]
        for a, b in ((I[:, :-1][hm], I[:, 1:][hm]), (I[:-1, :][vm], I[1:, :][vm])):
            g = np.full(len(a), G_SHEET)
            rows.extend(np.concatenate([a, b, a, b]))
            cols.extend(np.concatenate([a, b, b, a]))
            vals.extend(np.concatenate([g, g, -g, -g]))
    nodes = nF + nB
    # виа
    for v in geom['vias']:
        cells = np.nonzero(disk(v['x'], v['y'], v['dia'] / 2) & (idF >= 0) & (idB >= 0))[0]
        if not len(cells):
            cells = np.array([np.argmin((P[:, 0] - v['x']) ** 2 + (P[:, 1] - v['y']) ** 2)])
            cells = cells[(idF[cells] >= 0) & (idB[cells] >= 0)]
        for c in cells:
            edge(idF[c], idB[c], (1 / R_VIA) / len(cells))
    exits = []
    diag_extra = {}
    for i, pd in enumerate(pads):
        if pd['net'] != 'GND' or not pd['tht']:
            continue
        ring = []
        for L, c in padcells[i].items():
            c = c[ids[L][c] >= 0]
            c = c[disk(pd['x'], pd['y'], pd['drill'] / 2 + 0.3)[c]]
            ring.extend(ids[L][c].tolist())
        if not ring:
            continue
        if pd['fpid'].startswith(('INTERCONNECT_PIN', 'STANDOFF')):
            e = nodes
            nodes += 1
            for r in ring:
                edge(r, e, G_CONTACT / len(ring))
            diag_extra[e] = 1 / R_EXIT
            exits.append((i, e))
        else:                                       # COAX3: пайка оплётки связывает слои
            fr = [r for r in ring if r < nF]
            br = [r for r in ring if r >= nF]
            for a in fr:
                for b in br:
                    edge(a, b, (1 / R_VIA) / (len(fr) * len(br)))
    A = sp.coo_matrix((np.asarray(vals, float), (np.asarray(rows, np.int64), np.asarray(cols, np.int64))),
                      shape=(nodes, nodes)).tocsr()
    d = np.zeros(nodes)
    for e, g in diag_extra.items():
        d[e] += g
    # острова без выхода в стек
    ncomp, lab = connected_components(A, directed=False)
    grounded = {lab[e] for _, e in exits}
    floating = ~np.isin(lab, list(grounded))
    d[floating] += 1e-3
    A = (A + sp.diags(d)).tocsc()
    padnodes = []
    for i, pd in enumerate(pads):
        nn = []
        for L, c in padcells[i].items():
            nn.extend(ids[L][c][ids[L][c] >= 0].tolist())
        padnodes.append(np.array(sorted(set(nn)), np.int64))
    return dict(A=A, lu=splu(A), nF=nF, nB=nB, nodes=nodes, exits=exits, ids=ids, masks=masks,
                padnodes=padnodes, floating=floating, lab=lab, grounded=grounded)


def sel(geom, ref, pins=None):
    return [i for i, p in enumerate(geom['pads'])
            if p['ref'] == ref and p['net'] == 'GND' and (pins is None or p['num'] in pins)]


def main():
    geom = json.load(open(os.path.join(OUT, 'mcu_gnd_geom.json'), encoding='utf-8'))
    tr = np.load(os.path.join(OUT, 'transfer.npz'))
    f_tr, H_tr = tr['f'], tr['H']
    src = list(tr['src'])

    def Hat(k, f):
        lf = np.log10(f_tr)
        return np.interp(np.log10(f), lf, H_tr[k].real) + 1j * np.interp(np.log10(f), lf, H_tr[k].imag)

    pads = geom['pads']
    VSS = sel(geom, 'U302', {'15', '40', '50', '61', '79'})
    VSSA = sel(geom, 'U302', {'27'})
    groups = {
        'VSSA': VSSA,
        'gA': sel(geom, 'R313') + sel(geom, 'C321'),
        'gB': sel(geom, 'R311'),
        'gR': sel(geom, 'U301') + sel(geom, 'C303') + sel(geom, 'C322'),
        'gT': sel(geom, 'COAX3'),
        'C313': sel(geom, 'C313'),
        'C312': sel(geom, 'C312'),
    }
    patterns = {
        'A_DIG':   ([(VSS, 1.0)], 'возврат цифрового тока МК (VSS) в стек'),
        'A_ANA':   ([(VSSA, 1.0)], 'возврат тока VDDA (VSSA) в стек'),
        'A_FLASH': ([(sel(geom, 'U303'), 1.0)], 'возврат тока флеша в стек'),
        'A_DIV':   ([(groups['gT'], 1.0)], 'ток делителя через оплётку COAX3 в стек'),
        'L_PWM':   ([(groups['C312'], 1.0), (VSS, -1.0)], 'ШИМ HV_CONTROL: C312 → VSS МК (местная петля)'),
        'L_DIG':   ([(sel(geom, 'C304') + sel(geom, 'C305') + sel(geom, 'C306') + sel(geom, 'C307') +
                      sel(geom, 'C308') + sel(geom, 'C309'), 1.0), (VSS, -1.0)], 'ВЧ ток МК: развязка VDD → VSS'),
        'L_ANA':   ([(sel(geom, 'C310') + sel(geom, 'C311'), 1.0), (VSSA, -1.0)], 'ВЧ ток VDDA: C310/C311 → VSSA'),
    }
    # оценки амплитуд переменной составляющей (см. README)
    amps = {'A_DIG': 10e-3, 'A_ANA': 1e-3, 'A_FLASH': 15e-3, 'A_DIV': 0.7e-3,
            'L_PWM': 0.165e-3, 'L_DIG': 10e-3, 'L_ANA': 1e-3}
    missing = [k for k, v in groups.items() if not v] + [k for k, (pl, _) in patterns.items() if any(not g for g, _ in pl)]
    print('Пустые группы/шаблоны:', missing or 'нет')

    res = {}
    for variant, rect in (('as_is', None), ('solid_B_analog', ANALOG_RECT)):
        M = build(geom, rect)
        print(f'\n=== {variant}: клеток F {M["nF"]}, B {M["nB"]}, выходов {len(M["exits"])}')
        fl_pads = sorted({(pads[i]['ref'], pads[i]['num']) for i in range(len(pads))
                          if len(M['padnodes'][i]) and M['floating'][M['padnodes'][i]].all()})
        print('  GND-пады на островах без выхода в стек:', fl_pads or 'нет')

        def V_of(v, idxs):
            nn = np.concatenate([M['padnodes'][i] for i in idxs]) if idxs else np.array([], np.int64)
            return float(v[nn].mean()) if len(nn) else float('nan')

        # сопротивление VSSA -> стек
        rhs = np.zeros(M['nodes'])
        for i in VSSA:
            rhs[M['padnodes'][i]] += 1 / len(VSSA) / len(M['padnodes'][i])
        print(f'  R(VSSA → земля стека) = {V_of(M["lu"].solve(rhs), VSSA) * 1e3:.3f} мОм')
        out = {}
        for pname, (plist, desc) in patterns.items():
            rhs = np.zeros(M['nodes'])
            for idxs, w in plist:
                tot = sum(len(M['padnodes'][i]) for i in idxs)
                for i in idxs:
                    rhs[M['padnodes'][i]] += w / tot
            v = M['lu'].solve(rhs)
            Vg = {g: V_of(v, idxs) for g, idxs in groups.items()}
            dV = {k: Vg[k] - Vg['VSSA'] for k in src}
            e = {f: abs(sum(Hat(src.index(k), f) * dV[k] for k in src)) for f in (1e3, 1.6e5, 1e6)}
            out[pname] = dict(desc=desc, Z_mOhm={k: dV[k] * 1e3 for k in src},
                              VSSA_mOhm=Vg['VSSA'] * 1e3, C313_mOhm=Vg['C313'] * 1e3,
                              adc_uV_per_mA={f: e[f] * 1e6 * 1e-3 for f in e})
            if variant == 'as_is' and pname in ('A_DIG', 'L_DIG'):
                try:
                    import matplotlib
                    matplotlib.use('Agg')
                    import matplotlib.pyplot as plt
                    fig, ax = plt.subplots(1, 2, figsize=(12, 6))
                    for a, L in zip(ax, ('F.Cu', 'B.Cu')):
                        img = np.full(N, np.nan)
                        m = M['ids'][L] >= 0
                        img[m] = v[M['ids'][L][m]] * 1e3
                        im = a.imshow(img.reshape(NY, NX), origin='upper', extent=(xs[0], xs[-1], ys[-1], ys[0]), cmap='viridis')
                        for g, idxs in groups.items():
                            for i in idxs:
                                a.plot(pads[i]['x'], pads[i]['y'], 'r.', ms=4)
                            if idxs:
                                a.annotate(g, (pads[idxs[0]]['x'], pads[idxs[0]]['y']), color='w', fontsize=7)
                        for i, _ in M['exits']:
                            a.plot(pads[i]['x'], pads[i]['y'], 'ws', ms=5, mfc='none')
                        a.set_title(f'{L}: {pname}, мВ на 1 А')
                        fig.colorbar(im, ax=a, shrink=0.7)
                    fig.tight_layout()
                    fig.savefig(os.path.join(OUT, f'map_{pname}.png'), dpi=110)
                    plt.close(fig)
                except Exception as ex:
                    print('  карта не построена:', ex)
        res[variant] = out

    json.dump(res, open(os.path.join(OUT, 'ground_results.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)

    print('\n### Передаточные сопротивления (мОм = мкВ на 1 мА), относительно VSSA; C313 и VSSA — относительно стека')
    print('| шаблон | gA | gB | gR | gT | VSSA↔стек | C313↔стек |')
    print('|---|---|---|---|---|---|---|')
    for pn, r in res['as_is'].items():
        z = r['Z_mOhm']
        print(f"| {pn} | {z['gA']:+.3f} | {z['gB']:+.3f} | {z['gR']:+.3f} | {z['gT']:+.3f} | {r['VSSA_mOhm']:+.3f} | {r['C313_mOhm']:+.3f} |")

    print('\n### Оценка на входе АЦП при ожидаемых амплитудах токов')
    print('| шаблон | ток | АЦП @1 кГц | АЦП @160 кГц | в каналах @160 кГц | HV_CONTROL→ВВ | то же при сплошном B.Cu под аналогом @160 кГц |')
    print('|---|---|---|---|---|---|---|')
    for pn, r in res['as_is'].items():
        I = amps[pn]
        e1 = r['adc_uV_per_mA'][1e3] * I * 1e3
        e160 = r['adc_uV_per_mA'][1.6e5] * I * 1e3
        e160s = res['solid_B_analog'][pn]['adc_uV_per_mA'][1.6e5] * I * 1e3
        hv = abs(r['C313_mOhm'] * 1e-3 * I) * K_HV
        print(f"| {pn} | {I * 1e3:g} мА | {e1:.3f} мкВ | {e160:.3f} мкВ | {e160 * 1e-6 * CH_PER_V:.4f} | "
              f"{hv * 1e3:.3f} мВ | {e160s:.3f} мкВ |")
    print('\nОриентиры: шаг АЦП 610 мкВ (1 канал = 305 мкВ), шум тракта 78 мкВ rms, пик Cs-137 на АЦП 354 мВ.')


if __name__ == '__main__':
    main()
