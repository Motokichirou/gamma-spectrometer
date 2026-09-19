# -*- coding: utf-8 -*-
"""
tia_parasitics/fasthenry_run.py — индуктивности петель у TIA на плате делителя (FastHenry 3.0wr).

Вход: out/<main|alt>_div.json (extract_div.py). Выход: out/<board>_fh.inp, out/<board>_Zc.mat, out/ind_results.json.

Порты (FastHenry: импеданс между парой узлов, остальные порты разомкнуты):
  LP5  — U402 вывод 7 (+VS) ↔ EPAD: петля +5 В через C413/C414 и землю;
  LN5  — U402 вывод 4 (−VS) ↔ EPAD: петля −5 В через C411/C412;
  LFB  — U402 вывод 1 (FB) ↔ вывод 2 (−IN): петля обратной связи через R417/C415;
  A    — коллектор Q403 ↔ GND-пад C414: путь импульсного тока Dy8 по земле к развязке TIA;
  B    — U402 вывод 3 (+IN, опорная земля входа) ↔ COAX4 (оплётка, куда отсчитывается AMP_OUT).
Z_BA = V_B / I_A — ложное напряжение на входе TIA от тока Q403 (правило №10 шпаргалки).

Модель: медь всех цепей в прямоугольнике вокруг U402/Q403/COAX4/развязки (+MARGIN мм) — сетка сегментов
шагом H (ширина H, толщина 35 мкм); виа и сквозные пады — вертикальные сегменты; конденсаторы и резистор
(R417, C411–C415) — перемычки между их падами (на ВЧ ≈ короткое замыкание, ESL корпуса приближённо).
"""
import os, re, sys, json, math, subprocess
import numpy as np
from matplotlib.path import Path

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
FASTHENRY = r'C:\Users\motok\tools\fastfield\bin\fasthenry.exe'
H = 0.4                  # шаг сетки заливок, мм
WG = 0.28                # ширина сегмента сетки: < H, чтобы не залезать на соседние дорожки
T_CU = 0.035
MARGIN = 3.0
BODIES = ('R417', 'C411', 'C412', 'C413', 'C414', 'C415')


def raster_box(g, box):
    x0, x1, y0, y1 = box
    xs = np.arange(x0 + H / 2, x1, H)
    ys = np.arange(y0 + H / 2, y1, H)
    X, Y = np.meshgrid(xs, ys)
    P = np.column_stack([X.ravel(), Y.ravel()])
    names = sorted(g['nets'])
    lab = {L: np.full(len(P), -1, np.int32) for L in ('F.Cu', 'B.Cu')}
    for i, net in enumerate(names):
        for L, polys in g['nets'][net].items():
            for poly in polys:
                o = np.asarray(poly['outline'])
                if len(o) < 3:
                    continue
                a0, b0 = o.min(0)
                a1, b1 = o.max(0)
                idx = np.nonzero((P[:, 0] >= a0) & (P[:, 0] <= a1) & (P[:, 1] >= b0) & (P[:, 1] <= b1))[0]
                if not len(idx):
                    continue
                m = Path(o).contains_points(P[idx])
                for h in poly['holes']:
                    if len(h) >= 3:
                        m &= ~Path(np.asarray(h)).contains_points(P[idx])
                lab[L][idx[m]] = i
    return xs, ys, names, {L: v.reshape(len(ys), len(xs)) for L, v in lab.items()}


def pad(g, ref, num=None, net=None, pick=None):
    c = [p for p in g['pads'] if p['ref'] == ref and (num is None or p['num'] == num) and (net is None or p['net'] == net)]
    if pick == 'big':
        c = sorted(c, key=lambda p: -p['sx'] * p['sy'])
    return c[0] if c else None


def build(g, tag):
    T = g['board']['thickness_mm']
    zL = {'F.Cu': T, 'B.Cu': 0.0}
    u = g['u402']
    pins = {n: pad(g, 'U402', n) for n in ('1', '2', '3', '4', '7')}
    epad = sorted([p for p in g['pads'] if p['ref'] == 'U402' and p['num'] not in '12345678'],
                  key=lambda p: -p['sx'] * p['sy'])[0]
    q403c = pad(g, 'Q403', net='GND')
    coax4 = pad(g, 'COAX4')
    c414g = pad(g, 'C414', net='GND')
    pts = [(u[0], u[1])] + [(p['x'], p['y']) for p in (q403c, coax4) if p] + \
          [(p['x'], p['y']) for p in g['pads'] if p['ref'] in BODIES]
    pts = np.array(pts)
    cx, cy, rb = g['board']['cx'], g['board']['cy'], g['board']['r']
    box = (cx - rb, cx + rb, cy - rb, cy + rb)          # вся плата делителя
    inb = lambda x, y, m=0.0: box[0] - m <= x <= box[1] + m and box[2] - m <= y <= box[3] + m
    L = ['* TIA loops ' + tag, '.units mm', f'.default sigma=5.8e4 nhinc=1 nwinc=1 h={T_CU}']
    nodes, edges, equiv = {}, [], []

    def nk(Ln, x, y, z=None):
        z = zL[Ln] if z is None else z
        key = (Ln, round(x, 3), round(y, 3), round(z, 3))
        if key not in nodes:
            nodes[key] = f'N{len(nodes)}'
            L.append(f'{nodes[key]} x={x:.4f} y={y:.4f} z={z:.4f}')
        return nodes[key]

    def edge(a, b, w, h=T_CU):
        if a != b:
            edges.append((a, b))
            L.append(f'E{len(edges)} {a} {b} w={w:.4f} h={h:.4f}')

    def eq(a, b):
        if a != b:
            equiv.append((a, b))

    # заливки: сетка H по слоям
    xs = np.arange(box[0] + H / 2, box[1], H)
    ys = np.arange(box[2] + H / 2, box[3], H)
    X, Y = np.meshgrid(xs, ys)
    P = np.column_stack([X.ravel(), Y.ravel()])
    zlab, zvert = {}, {}
    for Ln in zL:
        lab = np.full(len(P), -1, np.int32)
        znets = sorted(n for n in g.get('zones', {}) if Ln in g['zones'][n])
        for k, net in enumerate(znets):
            verts = []
            for poly in g['zones'][net][Ln]:
                o = np.asarray(poly['outline'])
                if len(o) < 3:
                    continue
                verts.append(o)
                m = Path(o).contains_points(P)
                for h in poly['holes']:
                    if len(h) >= 3:
                        m &= ~Path(np.asarray(h)).contains_points(P)
                        verts.append(np.asarray(h))
                lab[m] = k
            zvert[(net, Ln)] = np.vstack(verts) if verts else np.zeros((0, 2))
        A = lab.reshape(len(ys), len(xs))
        hm = (A[:, :-1] >= 0) & (A[:, :-1] == A[:, 1:])
        for i, j in zip(*np.nonzero(hm)):
            edge(nk(Ln, xs[j], ys[i]), nk(Ln, xs[j + 1], ys[i]), WG)
        vm = (A[:-1, :] >= 0) & (A[:-1, :] == A[1:, :])
        for i, j in zip(*np.nonzero(vm)):
            edge(nk(Ln, xs[j], ys[i]), nk(Ln, xs[j], ys[i + 1]), WG)
        zlab[Ln] = (A, znets)

    def zone_cell(Ln, net, x, y, maxd=3.0, inside_only=False):
        A, znets = zlab[Ln]
        if net not in znets:
            return None
        k = znets.index(net)
        j = int(round((x - xs[0]) / H))
        i = int(round((y - ys[0]) / H))
        if inside_only:
            ok = 0 <= i < len(ys) and 0 <= j < len(xs) and A[i, j] == k
            return nk(Ln, xs[j], ys[i]) if ok else None
        ii, jj = np.nonzero(A == k)
        if not len(ii):
            return None
        d = np.hypot(xs[jj] - x, ys[ii] - y)
        m = int(np.argmin(d))
        return nk(Ln, xs[jj[m]], ys[ii[m]]) if d[m] <= maxd else None

    def touches_zone(Ln, net, x, y, r):
        if zone_cell(Ln, net, x, y, inside_only=True):      # пад/виа внутри заливки (сплошное соединение)
            return True
        v = zvert.get((net, Ln))                              # или спица термобарьера касается края
        return v is not None and len(v) > 0 and np.min(np.hypot(v[:, 0] - x, v[:, 1] - y)) <= r

    # пады и виа
    anchors, padnode = [], {}
    for p in g['pads']:
        if not inb(p['x'], p['y'], 2):
            continue
        r = max(p['sx'], p['sy']) / 2
        for Ln in p['layers']:
            n = nk(Ln, p['x'], p['y'])
            padnode.setdefault(id(p), {})[Ln] = n
            anchors.append((Ln, p['x'], p['y'], r + 0.05, p['net'], n))
            if touches_zone(Ln, p['net'], p['x'], p['y'], r + 0.15):
                c = zone_cell(Ln, p['net'], p['x'], p['y'])
                if c:
                    eq(n, c)
        if p['tht'] and len(p['layers']) == 2:
            edge(padnode[id(p)]['F.Cu'], padnode[id(p)]['B.Cu'], 0.5, 0.5)
    for v in g['vias']:
        if not inb(v['x'], v['y'], 2):
            continue
        nf, nb = nk('F.Cu', v['x'], v['y']), nk('B.Cu', v['x'], v['y'])
        edge(nf, nb, v['drill'], v['drill'])
        for Ln, n in (('F.Cu', nf), ('B.Cu', nb)):
            for aL, ax, ay, r, an, pn_ in list(anchors):
                if aL == Ln and an == v['net'] and math.hypot(v['x'] - ax, v['y'] - ay) <= r:
                    eq(n, pn_)
            if touches_zone(Ln, v['net'], v['x'], v['y'], v['dia'] / 2 + 0.15):
                c = zone_cell(Ln, v['net'], v['x'], v['y'])
                if c:
                    eq(n, c)
            anchors.append((Ln, v['x'], v['y'], v['dia'] / 2 + 0.05, v['net'], n))

    def snap(Ln, x, y, net):
        for aL, ax, ay, r, an, n in anchors:
            if aL == Ln and an == net and math.hypot(x - ax, y - ay) <= r:
                return n
        return nk(Ln, x, y)

    # дорожки (внутри своей заливки не моделируются: их медь уже в сетке)
    ntr = 0
    for t in g['tracks']:
        if t['layer'] not in zL or not (inb(t['x1'], t['y1'], 1) or inb(t['x2'], t['y2'], 1)):
            continue
        c1 = zone_cell(t['layer'], t['net'], t['x1'], t['y1'], inside_only=True)
        c2 = zone_cell(t['layer'], t['net'], t['x2'], t['y2'], inside_only=True)
        a, b = snap(t['layer'], t['x1'], t['y1'], t['net']), snap(t['layer'], t['x2'], t['y2'], t['net'])
        if c1 and c2:
            eq(a, c1)
            eq(b, c2)
            continue
        ln_ = math.hypot(t['x2'] - t['x1'], t['y2'] - t['y1'])
        if ln_ < 0.12:
            eq(a, b)
        else:
            ns = max(1, int(math.ceil(ln_ / H)))            # заранее режем: FastHenry сам не дробит
            prev = a
            for k in range(1, ns + 1):
                if k == ns:
                    nxt = b
                else:
                    fr = k / ns
                    nxt = nk(t['layer'], t['x1'] + fr * (t['x2'] - t['x1']), t['y1'] + fr * (t['y2'] - t['y1']))
                edge(prev, nxt, t['w'])
                prev = nxt
            ntr += 1
        for cc, nn in ((c1, a), (c2, b)):
            if cc:
                eq(nn, cc)
        anchors += [(t['layer'], t['x1'], t['y1'], t['w'] / 2, t['net'], a),
                    (t['layer'], t['x2'], t['y2'], t['w'] / 2, t['net'], b)]
    # корпуса R/C: вверх на 0.4 мм, перемычка поверху, вниз
    for ref in BODIES:
        pp = [p for p in g['pads'] if p['ref'] == ref]
        if len(pp) == 2:
            la, lb = pp[0]['layers'][0], pp[1]['layers'][0]
            za = zL[la] + (0.4 if la == 'F.Cu' else -0.4)
            ua = nk(la, pp[0]['x'], pp[0]['y'], za)
            ub = nk(lb, pp[1]['x'], pp[1]['y'], za)
            edge(padnode[id(pp[0])][la], ua, 0.5, 0.5)
            edge(ua, ub, 0.8, 0.5)
            edge(ub, padnode[id(pp[1])][lb], 0.5, 0.5)

    def pn(p):
        return padnode[id(p)]['F.Cu' if 'F.Cu' in p['layers'] else 'B.Cu']

    par = {}

    def f(x):
        while par.setdefault(x, x) != x:
            par[x] = par[par[x]]
            x = par[x]
        return x
    for a2, b2 in edges + equiv:
        par[f(a2)] = f(b2)
    for a2, b2 in equiv:
        L.append(f'.equiv {a2} {b2}')
    ports = {'LP5': (pins['7'], epad), 'LN5': (pins['4'], epad), 'LFB': (pins['1'], pins['2']),
             'A': (q403c, c414g), 'B': (pins['3'], coax4)}
    order = []
    for k, (pa, pb) in ports.items():
        if pa is None or pb is None:
            print(f'  {tag}: порт {k}: нет пада')
            continue
        a2, b2 = pn(pa), pn(pb)
        if f(a2) == f(b2) and a2 != b2:
            L.append(f'.external {a2} {b2} {k}')
            order.append(k)
        else:
            print(f'  {tag}: порт {k}: узлы {a2}/{b2} не связаны — пропущен')
            if '--debug' in sys.argv:
                xyz = {v: k2 for k2, v in nodes.items()}
                for q in g['pads']:
                    if q['ref'] in ('U402',) + BODIES and id(q) in padnode:
                        for Lq, nq in padnode[id(q)].items():
                            print(f"      {q['ref']}.{q['num']:4} {q['net']:10} {Lq[0]} {nq:7} кусок {f(nq)}")
                print(f"      порт {k}: {a2} -> кусок {f(a2)}, {b2} -> кусок {f(b2)}")
                near = [(v, round(k2[1], 2), round(k2[2], 2), k2[0][0], f(v)) for k2, v in nodes.items()
                        if abs(k2[1] - pa['x']) < 3 and abs(k2[2] - pa['y']) < 1 and k2[3] == zL[k2[0]]][:25]
                print('      узлы рядом с первым падом:', near)
    L += ['.freq fmin=1e6 fmax=1e8 ndec=1', '.end']
    info = dict(box=[round(float(v), 2) for v in box], segments=len(edges), equiv=len(equiv), tracks=ntr,
                ports=order,
                dist_Q403_U402=round(math.hypot(q403c['x'] - u[0], q403c['y'] - u[1]), 1) if q403c else None)
    return '\n'.join(L) + '\n', order, info


def parse_zc(path):
    lines = open(path, encoding='ascii', errors='replace').read().splitlines()
    out = {}
    num = r'([-+]?(?:nan|[\d.]+(?:e[-+]?\d+)?))'
    for li, line in enumerate(lines):
        m = re.match(r'Impedance matrix for frequency = ([\d.eE+-]+) (\d+) x \d+', line)
        if not m:
            continue
        f, n = float(m.group(1)), int(m.group(2))
        M = []
        for r in lines[li + 1:li + 1 + n]:
            v = re.findall(num + r'\s*([+-]\s*(?:nan|[\d.]+(?:e[-+]?\d+)?))j', r)
            M.append([complex(float(x), float(y.replace(' ', ''))) for x, y in v])
        out[f] = np.array(M)
    return out


def main():
    res = {}
    for board in ('main', 'alt'):
        g = json.load(open(os.path.join(OUT, f'{board}_div.json'), encoding='utf-8'))
        inp, order, info = build(g, board)
        fin = os.path.join(OUT, f'{board}_fh.inp')
        open(fin, 'w', encoding='ascii').write(inp)
        print(f'{board}: {info}')
        if '--parse-only' not in sys.argv:
            with open(os.path.join(OUT, f'{board}_fh.log'), 'w', encoding='ascii', errors='replace') as f:
                subprocess.run([FASTHENRY, os.path.basename(fin)], cwd=OUT, stdout=f, stderr=subprocess.STDOUT)
            os.replace(os.path.join(OUT, 'Zc.mat'), os.path.join(OUT, f'{board}_Zc.mat'))
            nwarn = open(os.path.join(OUT, f'{board}_fh.log'), errors='replace').read().count('Severe warning')
            print(f'  {board}: предупреждений FastHenry Severe warning: {nwarn}')
        Z = parse_zc(os.path.join(OUT, f'{board}_Zc.mat'))
        res[board] = {'info': info, 'ports': order,
                      'Z': {str(f): [[[z.real, z.imag] for z in row] for row in M] for f, M in Z.items()}}
        print(f'\n### {board}: порты {order}')
        for f, M in sorted(Z.items()):
            w = 2 * math.pi * f
            parts = [f'{k}: R {M[i, i].real*1e3:.1f} мОм, L {M[i, i].imag/w*1e9:.2f} нГн' for i, k in enumerate(order)]
            print(f'  {f/1e6:g} МГц | ' + ' | '.join(parts))
            if 'A' in order and 'B' in order:
                a, b = order.index('A'), order.index('B')
                zba = M[b, a]
                print(f'           Z_BA (ток Q403 → опора TIA): R {zba.real*1e3:.3f} мОм, M {zba.imag/w*1e12:.1f} пГн')
    json.dump(res, open(os.path.join(OUT, 'ind_results.json'), 'w', encoding='utf-8'), ensure_ascii=False)


if __name__ == '__main__':
    main()
