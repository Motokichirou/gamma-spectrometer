# -*- coding: utf-8 -*-
"""
tia_parasitics/fastcap_run.py — паразитные ёмкости узлов TIA на плате делителя (FastCap2).

Вход: out/<main|alt>_div.json (extract_div.py). Выход: out/*.qui, out/*.fc.txt, out/cap_results.json.

Модель:
  • медь цепей платы делителя — тонкие (нулевой толщины) панели на z = 0 (B.Cu) и z = T (F.Cu);
    мелкие панели HF в пределах ROI мм от меди P/FeedBack/AMP_OUT, дальше блоки K×HF;
  • по желанию — выводы ФЭУ: вертикальные квадратные трубки LEAD_W × LEAD_H от B.Cu вниз (ФЭУ со стороны B);
  • однородная среда εr = 1 (FastCap: координаты в метрах → фарады). Реальный FR-4 учитывается
    множителем: ×2.75 (типично для дорожек на поверхности), ×4.5 (верхняя граница).
Выводы: Cin = собственная ёмкость узла P + вход ADA4817 (1.3 пФ синфазная + 0.1 пФ дифф.),
C(P↔FB) — параллельно Cf 4.7 пФ, связь анода с динодами и землёй.
"""
import os, re, sys, json, math, subprocess, concurrent.futures as cf
import numpy as np
from matplotlib.path import Path
from scipy import ndimage

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
FASTCAP = r'C:\Users\motok\tools\fastfield\bin\fastcap.exe'
HF, K = 0.125, 4            # мелкая панель, мм; крупный блок = K*HF
ROI = 3.0                   # мм от меди P/FB/OUT, где панели мелкие
LEAD_W, LEAD_H, LEAD_DZ = 0.7, 20.0, 1.0
EPS_TYP, EPS_MAX = 2.75, 4.5
C_ADA = 1.3 + 0.1           # пФ, вход ADA4817 (+IN на земле, поэтому дифф. тоже к земле)
KEY = {'P': ('/Divider_Board/P', 'P'), 'FB': ('FeedBack', '/Divider_Board/FeedBack'),
       'OUT': ('AMP_OUT', '/Divider_Board/AMP_OUT'), 'GND': ('GND',)}


def canon(nets):
    """имя цепи -> короткий идентификатор проводника для FastCap"""
    ids, rev = {}, {}
    for n in sorted(nets):
        short = None
        for k, al in KEY.items():
            if n in al:
                short = k
        if short is None:
            m = re.search(r'DY(\d)', n)
            short = f'DY{m.group(1)}' if m else None
        if short is None:
            short = 'N' + re.sub(r'[^A-Za-z0-9]', '', n.replace('−', 'M').replace('+', 'P'))[:14]
        while short in rev:
            short += 'x'
        ids[n], rev[short] = short, n
    return ids, rev


def raster(g):
    cx, cy, r = g['board']['cx'], g['board']['cy'], g['board']['r']
    xs = np.arange(cx - r + HF / 2, cx + r, HF)
    n = len(xs)
    X, Y = np.meshgrid(xs, xs + (cy - cx))
    P = np.column_stack([X.ravel(), Y.ravel()])
    names = sorted(g['nets'])
    lab = {L: np.full(len(P), -1, np.int32) for L in ('F.Cu', 'B.Cu')}
    for i, net in enumerate(names):
        for L, polys in g['nets'][net].items():
            for poly in polys:
                o = np.asarray(poly['outline'])
                if len(o) < 3:
                    continue
                x0, y0 = o.min(0)
                x1, y1 = o.max(0)
                idx = np.nonzero((P[:, 0] >= x0) & (P[:, 0] <= x1) & (P[:, 1] >= y0) & (P[:, 1] <= y1))[0]
                m = Path(o).contains_points(P[idx])
                for h in poly['holes']:
                    if len(h) >= 3:
                        m &= ~Path(np.asarray(h)).contains_points(P[idx])
                lab[L][idx[m]] = i
    return xs, n, names, {L: v.reshape(n, n) for L, v in lab.items()}


def write_qui(g, path, leads):
    xs, n, names, lab = raster(g)
    ids, rev = canon(names)
    T = g['board']['thickness_mm']
    key_idx = [names.index(x) for k in ('P', 'FB', 'OUT') for x in KEY[k] if x in names]
    near = np.zeros((n, n), bool)
    for L in lab:
        near |= np.isin(lab[L], key_idx)
    fine = ndimage.distance_transform_edt(~near) * HF <= ROI
    x0 = xs[0] - HF / 2
    y0 = xs[0] + (g['board']['cy'] - g['board']['cx']) - HF / 2
    lines = ['0 TIA parasitics']
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
                fb = fine[bi:bi + K, bj:bj + K]
                if blk.shape == (K, K) and not fb.any() and (blk == blk[0, 0]).all():
                    nm = ids[names[blk[0, 0]]]
                    quad(nm, x0 + bj * HF, y0 + bi * HF, x0 + (bj + K) * HF, y0 + (bi + K) * HF, z)
                    continue
                for di in range(blk.shape[0]):
                    for dj in range(blk.shape[1]):
                        v = blk[di, dj]
                        if v >= 0:
                            i, j = bi + di, bj + dj
                            quad(ids[names[v]], x0 + j * HF, y0 + i * HF, x0 + (j + 1) * HF, y0 + (i + 1) * HF, z)
    if leads:
        s = 1e-3
        for p in g['pmt']:
            nm = ids.get(p['net'])
            if nm is None:
                continue
            h = LEAD_W / 2
            cs = [(p['x'] - h, p['y'] - h), (p['x'] + h, p['y'] - h), (p['x'] + h, p['y'] + h), (p['x'] - h, p['y'] + h)]
            for k in range(int(LEAD_H / LEAD_DZ)):
                za, zb = -k * LEAD_DZ, -(k + 1) * LEAD_DZ
                for (xa, ya), (xb, yb) in zip(cs, cs[1:] + cs[:1]):
                    cnt += 1
                    lines.append(f'Q {nm} {xa*s:.7e} {ya*s:.7e} {za*s:.7e} {xb*s:.7e} {yb*s:.7e} {za*s:.7e} '
                                 f'{xb*s:.7e} {yb*s:.7e} {zb*s:.7e} {xa*s:.7e} {ya*s:.7e} {zb*s:.7e}')
    open(path, 'w', encoding='ascii').write('\n'.join(lines) + '\n')
    return cnt, ids, rev


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
    rows = {}
    order = []
    for line in txt[m.end():].splitlines():
        mm_ = re.match(r'^\s*(\S+?)%\S*\s+(\d+)\s+(.*)$', line)
        if mm_:
            name, k = mm_.group(1), int(mm_.group(2))
            if k not in rows:
                rows[k] = (name, [])
                order.append(k)
            rows[k][1].extend(float(v) for v in mm_.group(3).split())
    names = [rows[k][0] for k in sorted(rows)]
    C = np.array([rows[k][1] for k in sorted(rows)]) * unit      # пФ
    return names, C


def main():
    jobs = []
    for board in ('main', 'alt'):
        g = json.load(open(os.path.join(OUT, f'{board}_div.json'), encoding='utf-8'))
        for leads in (False, True):
            tag = f'{board}_{"leads" if leads else "board"}'
            q = os.path.join(OUT, tag + '.qui')
            if '--parse-only' in sys.argv:
                ids, rev = canon(sorted(g['nets']))
                cnt = '—'
            else:
                cnt, ids, rev = write_qui(g, q, leads)
            print(f'{tag}: панелей {cnt}, проводников {len(ids)}')
            jobs.append((tag, q, rev))
    res = {}
    with cf.ThreadPoolExecutor(max_workers=4) as ex:
        futs = {ex.submit(run_fastcap, q): (tag, rev) for tag, q, rev in jobs}
        for fu in cf.as_completed(futs):
            tag, rev = futs[fu]
            names, C = fu.result()
            res[tag] = {'names': names, 'nets': [rev.get(n, n) for n in names], 'C_pF_air': C.tolist()}
            print('готово:', tag)
    json.dump(res, open(os.path.join(OUT, 'cap_results.json'), 'w', encoding='utf-8'), ensure_ascii=False)

    for tag in sorted(res):
        names, C = res[tag]['names'], np.array(res[tag]['C_pF_air'])
        ix = {n: i for i, n in enumerate(names)}

        def c(a, b):
            return -C[ix[a], ix[b]] if a in ix and b in ix else float('nan')

        p = ix.get('P')
        print(f'\n### {tag}  (пФ; воздух / ×{EPS_TYP} типично / ×{EPS_MAX} максимум)')
        if p is None:
            print('  узел P не найден:', names)
            continue
        cpp = C[p, p]
        print(f'  собственная ёмкость анода P (на всё остальное): {cpp:.3f} / {cpp*EPS_TYP:.2f} / {cpp*EPS_MAX:.2f}')
        print(f'  Cin = плата + вход ADA4817 {C_ADA} пФ: {cpp*EPS_TYP + C_ADA:.2f} (типично) … {cpp*EPS_MAX + C_ADA:.2f} (макс), требование ≤ 20')
        coup = sorted(((c('P', n), n) for n in names if n != 'P'), reverse=True)
        print('  связь анода (крупнейшие):', ', '.join(f'{res[tag]["nets"][ix[n]]} {v*EPS_TYP:.3f}' for v, n in coup[:6] if v > 0))
        for a, b in (('P', 'FB'), ('P', 'OUT'), ('FB', 'GND'), ('OUT', 'GND'), ('P', 'DY8'), ('P', 'DY7')):
            v = c(a, b)
            if not math.isnan(v):
                print(f'  C({a}↔{b}) = {v:.4f} / {v*EPS_TYP:.3f} / {v*EPS_MAX:.3f}')


if __name__ == '__main__':
    main()
