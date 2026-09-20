# -*- coding: utf-8 -*-
"""
hv_loop/extract_hv.py — медь платы ВВ по цепям (ТОЛЬКО ЧТЕНИЕ .kicad_pcb).

Запуск интерпретатором KiCad:
  "C:\\Program Files\\KiCad\\9.0\\bin\\python.exe" simulation/hv_loop/extract_hv.py

Цель: паразитные ёмкости между узлом FB (2.2 МОм, вход усилителя ошибки MAX1847), цепочкой
делителя 1 ГОм (R207, R209–R217) и «агрессорами» — колонкой A умножителя (a1…a4, размах
до ±190 В на 252 кГц), стоком Q201 и снаббером. Модель показала: уже 1 фФ связи кладёт COMP в упор.

Выход: out/hv_geom.json (объединённые полигоны меди по цепям и слоям в пределах платы ВВ)
+ сводка минимальных расстояний между FB-цепями и агрессорами.
"""
import json, math, os
import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
OUT = os.path.join(HERE, 'out')
BOARD = os.path.join(ROOT, 'pcb', 'gamma_spectrometer', 'gamma_spectrometer.kicad_pcb')
CX, CY, RB = 170.0, 100.0, 22.5
mm = pcbnew.ToMM
ERR = pcbnew.FromMM(0.005)
LAYERS = {'F.Cu': pcbnew.F_Cu, 'B.Cu': pcbnew.B_Cu}

# цепи петли обратной связи и агрессоры (имена — из pcb/gamma_spectrometer.net)
FB_NETS = ['Net-(U201-FB)', 'Net-(R207-Pad2)', 'Net-(R209-Pad2)', 'Net-(R210-Pad2)', 'Net-(R211-Pad2)',
           'Net-(R212-Pad2)', 'Net-(R213-Pad2)', 'Net-(R214-Pad2)', 'Net-(R215-Pad2)', 'Net-(R216-Pad2)',
           'Net-(U201-COMP)', 'HV_CONTROL']
AGGR = ['Net-(C209-Pad2)', 'Net-(C210-Pad2)', 'Net-(C211-Pad2)', 'Net-(C212-Pad2)', 'Net-(C213-Pad2)',
        'Net-(C214-Pad2)', 'Net-(C215-Pad2)', 'Net-(C216-Pad2)', 'Net-(D201-A)', 'Net-(D201-K)',
        'Net-(C209-Pad1)', 'Net-(C13a201-Pad1)']


def on_hv(p, margin=0.3):
    return math.hypot(mm(p.x) - CX, mm(p.y) - CY) <= RB + margin


def chain(c):
    return [(round(mm(c.CPoint(k).x), 4), round(mm(c.CPoint(k).y), 4)) for k in range(c.PointCount())]


def to_list(ps):
    return [{'outline': chain(ps.Outline(i)), 'holes': [chain(ps.Hole(i, h)) for h in range(ps.HoleCount(i))]}
            for i in range(ps.OutlineCount())]


def add(acc, key, ps):
    if key not in acc:
        acc[key] = pcbnew.SHAPE_POLY_SET()
    acc[key].BooleanAdd(ps)


def extract(path):
    b = pcbnew.LoadBoard(path)
    acc, pads, vias, tracks = {}, [], [], []
    for t in b.GetTracks():
        if not on_hv(t.GetPosition() if t.GetClass() == 'PCB_VIA' else t.GetStart()):
            continue
        for ln, lid in LAYERS.items():
            if t.GetClass() == 'PCB_VIA' or t.GetLayer() == lid:
                ps = pcbnew.SHAPE_POLY_SET()
                t.TransformShapeToPolygon(ps, lid, 0, ERR, pcbnew.ERROR_INSIDE)
                if ps.OutlineCount():
                    add(acc, (t.GetNetname(), ln), ps)
        if t.GetClass() == 'PCB_VIA':
            q = t.GetPosition()
            vias.append({'net': t.GetNetname(), 'x': mm(q.x), 'y': mm(q.y), 'drill': mm(t.GetDrillValue()),
                         'dia': mm(t.GetWidth(pcbnew.F_Cu))})
        else:
            pts = [t.GetStart(), t.GetMid(), t.GetEnd()] if t.GetClass() == 'PCB_ARC' else [t.GetStart(), t.GetEnd()]
            for a, c in zip(pts, pts[1:]):
                tracks.append({'net': t.GetNetname(), 'layer': b.GetLayerName(t.GetLayer()), 'w': mm(t.GetWidth()),
                               'x1': mm(a.x), 'y1': mm(a.y), 'x2': mm(c.x), 'y2': mm(c.y)})
    for fp in b.GetFootprints():
        if not on_hv(fp.GetPosition(), 2.0):
            continue
        for pad in fp.Pads():
            if not pad.GetNetname():
                continue
            q = pad.GetPosition()
            pads.append({'ref': fp.GetReference(), 'num': pad.GetNumber(), 'net': pad.GetNetname(),
                         'x': mm(q.x), 'y': mm(q.y), 'tht': pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH,
                         'layers': [ln for ln, lid in LAYERS.items() if pad.IsOnLayer(lid)],
                         'sx': mm(pad.GetSize(pcbnew.F_Cu).x), 'sy': mm(pad.GetSize(pcbnew.F_Cu).y)})
            for ln, lid in LAYERS.items():
                if pad.IsOnLayer(lid):
                    ps = pcbnew.SHAPE_POLY_SET()
                    pad.TransformShapeToPolygon(ps, lid, 0, ERR, pcbnew.ERROR_INSIDE)
                    if ps.OutlineCount():
                        add(acc, (pad.GetNetname(), ln), ps)
    zones = {}
    for z in b.Zones():
        if z.GetIsRuleArea() or not z.GetNetname():
            continue
        for ln, lid in LAYERS.items():
            if not z.IsOnLayer(lid):
                continue
            fp = z.GetFilledPolysList(lid)
            keep = pcbnew.SHAPE_POLY_SET()
            for i in range(fp.OutlineCount()):
                if on_hv(fp.Outline(i).BBox().Centre()):
                    keep.AddOutline(fp.Outline(i))
                    for h in range(fp.HoleCount(i)):
                        keep.AddHole(fp.Hole(i, h), keep.OutlineCount() - 1)
            if keep.OutlineCount():
                add(acc, (z.GetNetname(), ln), keep)
                zs = pcbnew.SHAPE_POLY_SET(keep)
                zs.Simplify()
                zones.setdefault(z.GetNetname(), {}).setdefault(ln, []).extend(to_list(zs))
    nets = {}
    for (net, ln), ps in acc.items():
        ps.Simplify()
        nets.setdefault(net, {})[ln] = to_list(ps)
    return {'board': {'cx': CX, 'cy': CY, 'r': RB,
                      'thickness_mm': mm(b.GetDesignSettings().GetBoardThickness())},
            'nets': nets, 'pads': pads, 'vias': vias, 'tracks': tracks, 'zones': zones}


def seg_dist(a, b):
    """минимальное расстояние между двумя отрезками (2D)"""
    def d_pt(p, q, r):
        vx, vy = r[0] - q[0], r[1] - q[1]
        L = vx * vx + vy * vy
        t = 0.0 if L == 0 else max(0.0, min(1.0, ((p[0] - q[0]) * vx + (p[1] - q[1]) * vy) / L))
        return math.hypot(p[0] - q[0] - t * vx, p[1] - q[1] - t * vy)
    return min(d_pt(a[0], b[0], b[1]), d_pt(a[1], b[0], b[1]), d_pt(b[0], a[0], a[1]), d_pt(b[1], a[0], a[1]))


def report(g):
    """минимальные расстояния медь-медь (по осевым линиям дорожек и центрам падов) FB ↔ агрессоры"""
    items = {}
    for t in g['tracks']:
        items.setdefault(t['net'], []).append((t['layer'], ((t['x1'], t['y1']), (t['x2'], t['y2'])), t['w']))
    for p in g['pads']:
        for ln in p['layers']:
            items.setdefault(p['net'], []).append((ln, ((p['x'], p['y']), (p['x'], p['y'])), max(p['sx'], p['sy'])))
    print(f'\nцепей на плате ВВ: {len(g["nets"])}, дорожек {len(g["tracks"])}, падов {len(g["pads"])}, '
          f'виа {len(g["vias"])}, зон {list(g["zones"])}')
    print('\nминимальные зазоры (край-край, по слоям) FB-цепи ↔ агрессоры:')
    for fn in FB_NETS:
        if fn not in items:
            continue
        best = []
        for an in AGGR:
            if an not in items:
                continue
            d = min((seg_dist(s1, s2) - (w1 + w2) / 2, ln1 if ln1 == ln2 else f'{ln1}/{ln2}')
                    for ln1, s1, w1 in items[fn] for ln2, s2, w2 in items[an])
            best.append((d[0], an, d[1]))
        best.sort()
        s = ', '.join(f'{an} {d:.2f} мм [{ln}]' for d, an, ln in best[:3])
        print(f'  {fn:22s}: {s}')


def main():
    os.makedirs(OUT, exist_ok=True)
    g = extract(BOARD)
    with open(os.path.join(OUT, 'hv_geom.json'), 'w', encoding='utf-8') as f:
        json.dump(g, f)
    report(g)


main()
