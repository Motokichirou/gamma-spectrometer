# -*- coding: utf-8 -*-
"""
ground_mcu/extract_geom.py — геометрия земли платы МК из gamma_spectrometer.kicad_pcb (ТОЛЬКО ЧТЕНИЕ).

Запуск интерпретатором KiCad (нужен модуль pcbnew):
  "C:\\Program Files\\KiCad\\9.0\\bin\\python.exe" simulation/ground_mcu/extract_geom.py
Выход: simulation/ground_mcu/out/mcu_gnd_geom.json

Берётся то, что сохранено в файле: залитые полигоны GND (как их залил редактор), GND-пады,
GND-дорожки, GND-виа, сквозные GND-пады (цанги, стойки, коаксиал) — выходы земли в стек.
"""
import json, math, os
import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.join(HERE, '..', '..', 'pcb', 'gamma_spectrometer', 'gamma_spectrometer.kicad_pcb')
OUT = os.path.join(HERE, 'out')
CX, CY, R = 240.0, 100.0, 22.5          # контур платы МК
mm = pcbnew.ToMM
LAYERS = {'F.Cu': pcbnew.F_Cu, 'B.Cu': pcbnew.B_Cu}


def on_mcu(x, y, margin=0.5):
    return math.hypot(x - CX, y - CY) <= R + margin


def chain(c):
    return [(round(mm(c.CPoint(k).x), 4), round(mm(c.CPoint(k).y), 4)) for k in range(c.PointCount())]


def polyset(ps):
    out = []
    for i in range(ps.OutlineCount()):
        out.append({'outline': chain(ps.Outline(i)),
                    'holes': [chain(ps.Hole(i, h)) for h in range(ps.HoleCount(i))]})
    return out


def main():
    b = pcbnew.LoadBoard(os.path.abspath(PCB))
    geom = {'board': {'cx': CX, 'cy': CY, 'r': R}, 'zones': {'F.Cu': [], 'B.Cu': []},
            'pads': [], 'vias': [], 'tracks': []}
    for z in b.Zones():
        if z.GetNetname() != 'GND':
            continue
        for ln, lid in LAYERS.items():
            if not z.IsOnLayer(lid):
                continue
            for p in polyset(z.GetFilledPolysList(lid)):
                xs = [q[0] for q in p['outline']]
                ys = [q[1] for q in p['outline']]
                if on_mcu((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2):
                    geom['zones'][ln].append(p)
    for fp in b.GetFootprints():
        fpos = fp.GetPosition()
        if not on_mcu(mm(fpos.x), mm(fpos.y), 2.0):
            continue
        for pad in fp.Pads():
            q = pad.GetPosition()
            rec = {'ref': fp.GetReference(), 'fpid': fp.GetFPIDAsString().split(':')[-1],
                   'num': pad.GetNumber(), 'net': pad.GetNetname(),
                   'x': mm(q.x), 'y': mm(q.y),
                   'tht': pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH,
                   'drill': mm(pad.GetDrillSize().x), 'poly': {}}
            for ln, lid in LAYERS.items():
                if pad.IsOnLayer(lid):
                    ps = pcbnew.SHAPE_POLY_SET()
                    pad.TransformShapeToPolygon(ps, lid, 0, pcbnew.FromMM(0.005), pcbnew.ERROR_INSIDE)
                    rec['poly'][ln] = polyset(ps)
            geom['pads'].append(rec)
    for t in b.GetTracks():
        net = t.GetNetname()
        if net != 'GND':
            continue
        if t.GetClass() == 'PCB_VIA':
            p = t.GetPosition()
            if on_mcu(mm(p.x), mm(p.y)):
                geom['vias'].append({'x': mm(p.x), 'y': mm(p.y), 'drill': mm(t.GetDrillValue()),
                                     'dia': mm(t.GetWidth(pcbnew.F_Cu))})
        elif t.GetClass() == 'PCB_TRACK':
            s, e = t.GetStart(), t.GetEnd()
            if on_mcu(mm(s.x), mm(s.y)):
                geom['tracks'].append({'x1': mm(s.x), 'y1': mm(s.y), 'x2': mm(e.x), 'y2': mm(e.y),
                                       'w': mm(t.GetWidth()), 'layer': b.GetLayerName(t.GetLayer())})
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'mcu_gnd_geom.json'), 'w', encoding='utf-8') as f:
        json.dump(geom, f)
    print('zones F/B:', len(geom['zones']['F.Cu']), len(geom['zones']['B.Cu']),
          '| pads:', len(geom['pads']), '(GND', sum(p['net'] == 'GND' for p in geom['pads']), ')',
          '| vias:', len(geom['vias']), '| tracks:', len(geom['tracks']))


main()
