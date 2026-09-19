# -*- coding: utf-8 -*-
"""
tia_parasitics/extract_div.py — медь платы делителя по цепям (ТОЛЬКО ЧТЕНИЕ .kicad_pcb).

Запуск интерпретатором KiCad:
  "C:\\Program Files\\KiCad\\9.0\\bin\\python.exe" simulation/tia_parasitics/extract_div.py
Выход: out/<имя>_div.json для основной платы и для gamma_alt.

Для каждой цепи и слоя — объединение полигонов меди (дорожки, дуги, пады, пятаки виа, залитые зоны)
в пределах контура делителя (центр 310,100, R 22.5). Плюс выводы ФЭУ J_PMT401 и положение U402.
"""
import json, math, os
import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
OUT = os.path.join(HERE, 'out')
BOARDS = {'main': os.path.join(ROOT, 'pcb', 'gamma_spectrometer', 'gamma_spectrometer.kicad_pcb'),
          'alt': os.path.join(ROOT, 'pcb', 'gamma_alt', 'gamma_alt.kicad_pcb')}
CX, CY, RB = 310.0, 100.0, 22.5
mm = pcbnew.ToMM
ERR = pcbnew.FromMM(0.005)
LAYERS = {'F.Cu': pcbnew.F_Cu, 'B.Cu': pcbnew.B_Cu}


def on_div(p, margin=0.3):
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
    acc = {}
    for t in b.GetTracks():
        if not on_div(t.GetPosition() if t.GetClass() == 'PCB_VIA' else t.GetStart()):
            continue
        for ln, lid in LAYERS.items():
            if t.GetClass() == 'PCB_VIA' or t.GetLayer() == lid:
                ps = pcbnew.SHAPE_POLY_SET()
                t.TransformShapeToPolygon(ps, lid, 0, ERR, pcbnew.ERROR_INSIDE)
                if ps.OutlineCount():
                    add(acc, (t.GetNetname(), ln), ps)
    pmt, u402, padlist, vias, tracks = [], None, [], [], []
    for t in b.GetTracks():
        if t.GetClass() in ('PCB_TRACK', 'PCB_ARC') and on_div(t.GetStart()):
            pts = [t.GetStart(), t.GetMid(), t.GetEnd()] if t.GetClass() == 'PCB_ARC' else [t.GetStart(), t.GetEnd()]
            for a, c in zip(pts, pts[1:]):
                tracks.append({'net': t.GetNetname(), 'layer': b.GetLayerName(t.GetLayer()), 'w': mm(t.GetWidth()),
                               'x1': mm(a.x), 'y1': mm(a.y), 'x2': mm(c.x), 'y2': mm(c.y)})
    for t in b.GetTracks():
        if t.GetClass() == 'PCB_VIA' and on_div(t.GetPosition()):
            q = t.GetPosition()
            vias.append({'net': t.GetNetname(), 'x': mm(q.x), 'y': mm(q.y), 'drill': mm(t.GetDrillValue()),
                         'dia': mm(t.GetWidth(pcbnew.F_Cu))})
    for fp in b.GetFootprints():
        if not on_div(fp.GetPosition(), 2.0):
            continue
        if fp.GetReference() == 'U402':
            u402 = (mm(fp.GetPosition().x), mm(fp.GetPosition().y))
        for pad in fp.Pads():
            if not pad.GetNetname():
                continue
            q = pad.GetPosition()
            padlist.append({'ref': fp.GetReference(), 'num': pad.GetNumber(), 'net': pad.GetNetname(),
                            'x': mm(q.x), 'y': mm(q.y), 'tht': pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH,
                            'layers': [ln for ln, lid in LAYERS.items() if pad.IsOnLayer(lid)],
                            'sx': mm(pad.GetSize(pcbnew.F_Cu).x), 'sy': mm(pad.GetSize(pcbnew.F_Cu).y)})
            if fp.GetReference() == 'J_PMT401':
                q = pad.GetPosition()
                pmt.append({'num': pad.GetNumber(), 'net': pad.GetNetname(), 'x': mm(q.x), 'y': mm(q.y)})
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
                c = fp.Outline(i).BBox().Centre()
                if on_div(c):
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
            'nets': nets, 'pmt': pmt, 'u402': u402, 'pads': padlist, 'vias': vias,
            'tracks': tracks, 'zones': zones}


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, path in BOARDS.items():
        g = extract(path)
        with open(os.path.join(OUT, f'{name}_div.json'), 'w', encoding='utf-8') as f:
            json.dump(g, f)
        print(f'{name}: цепей {len(g["nets"])}, выводов ФЭУ {len(g["pmt"])}, U402 {g["u402"]}, '
              f'толщина {g["board"]["thickness_mm"]:.2f} мм')
        for n in ('P', '/Divider_Board/P', 'FeedBack', 'AMP_OUT'):
            if n in g['nets']:
                print('   ', n, {ln: len(v) for ln, v in g['nets'][n].items()})


main()
