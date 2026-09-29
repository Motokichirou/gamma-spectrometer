# -*- coding: utf-8 -*-
"""panelize.py — собрать 4 круглые платы в одну заготовку с перемычками под разлом.

Что делает (см. docs/fab_pselectro.md):
  1. Сдвигает платы по X: шаг 70 -> 47.5 мм, зазор между контурами 2.5 мм.
  2. Меняет сверло стоек 2.75 -> 2.8 мм и проходное отверстие -ВВ 5.0 -> 4.5 мм.
  3. Заменяет 4 окружности Edge.Cuts на дуги с разрывами + прямые стороны перемычек.
  4. Добавляет ряды отверстий Ø0.6 под разлом (mouse bites).

Правки точечные: файл разбирается в дерево s-выражений со спанами, меняются только
нужные числа, всё остальное остаётся байт-в-байт.

Запуск:  python pcb/panelize.py [--apply]
Без --apply результат пишется рядом (*_panel.kicad_pcb) и файл проекта не трогается.
"""
import io
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'gamma_spectrometer', 'gamma_spectrometer.kicad_pcb')

# --- геометрия заготовки ---------------------------------------------------
R = 22.5                      # радиус платы
CY = 100.0                    # общая ось Y
OLD_CX = [100.0, 170.0, 240.0, 310.0]
PITCH = 47.5                  # новый шаг: зазор 2.5 мм под стандартную фрезу завода
NEW_CX = [100.0 + i * PITCH for i in range(4)]
NAMES = ['USB', 'HV', 'MCU', 'DIV']

TAB_ANG = 15.0                # угол перемычки от линии центров, градусы
TAB_W = 3.0                   # ширина перемычки по дуге, мм
TAB_HALF = math.degrees(TAB_W / 2 / R)     # половина углового раствора перемычки
BITE_D = 0.6                  # диаметр отверстия разлома
BITE_PITCH = 1.0              # шаг отверстий в ряду
BITE_N = 3                    # отверстий в ряду
BITE_OUT = 0.2                # вынос центров наружу от контура: держит медь дальше 0.3 мм

COORD_TAGS = {'at', 'start', 'end', 'mid', 'center', 'xy'}
MOVABLE = {'footprint', 'segment', 'arc', 'via', 'zone', 'gr_line', 'gr_arc',
           'gr_circle', 'gr_rect', 'gr_poly', 'gr_curve', 'gr_text', 'gr_bbox',
           'dimension', 'image', 'target', 'table'}


# --- минимальный разборщик s-выражений со спанами ---------------------------
class Node:
    __slots__ = ('head', 'start', 'end', 'kids', 'atoms')

    def __init__(self, head, start, end, kids, atoms):
        self.head, self.start, self.end, self.kids, self.atoms = head, start, end, kids, atoms


def parse(s, i):
    """Разбирает список, начинающийся с '(' в позиции i. Возвращает (Node, next_i)."""
    assert s[i] == '('
    start = i
    i += 1
    head = None
    kids = []
    atoms = []          # (start, end) непосредственных атомов
    while True:
        while i < len(s) and s[i] in ' \t\r\n':
            i += 1
        if s[i] == ')':
            return Node(head, start, i + 1, kids, atoms), i + 1
        if s[i] == '(':
            node, i = parse(s, i)
            kids.append(node)
            continue
        a0 = i
        if s[i] == '"':
            i += 1
            while s[i] != '"':
                i += 2 if s[i] == '\\' else 1
            i += 1
        else:
            while i < len(s) and s[i] not in ' \t\r\n()':
                i += 1
        if head is None:
            head = s[a0:i]
        else:
            atoms.append((a0, i))


def walk(node):
    yield node
    for k in node.kids:
        yield from walk(k)


def nums(s, node):
    """Числовые значения непосредственных атомов узла."""
    out = []
    for a, b in node.atoms:
        t = s[a:b]
        try:
            out.append((float(t), a, b))
        except ValueError:
            pass
    return out


def coords(s, node):
    """(x, y, span_x) для координатного узла."""
    v = nums(s, node)
    if len(v) < 2:
        return None
    return v[0][0], v[1][0], (v[0][1], v[0][2])


# --- сборка новой геометрии контура ----------------------------------------
def pt(cx, ang, r=R):
    a = math.radians(ang)
    return cx + r * math.cos(a), CY + r * math.sin(a)


def tab_angles(bi):
    """Углы перемычек данной платы: справа (к следующей) и слева (к предыдущей)."""
    out = []
    if bi < 3:
        out += [TAB_ANG, -TAB_ANG]                      # к правому соседу
    if bi > 0:
        out += [180.0 - TAB_ANG, 180.0 + TAB_ANG]       # к левому соседу
    return out


def edge_geometry():
    """Возвращает (список дуг, список отрезков, список отверстий разлома)."""
    arcs, lines, holes = [], [], []
    for bi, cx in enumerate(NEW_CX):
        cuts = []
        for t in tab_angles(bi):
            cuts.append(((t - TAB_HALF) % 360, (t + TAB_HALF) % 360))
        cuts.sort()
        for k in range(len(cuts)):
            a0 = cuts[k][1]
            a1 = cuts[(k + 1) % len(cuts)][0]
            span = (a1 - a0) % 360
            if span < 0.01:
                continue
            arcs.append((pt(cx, a0), pt(cx, a0 + span / 2), pt(cx, a1)))
    # стороны перемычек: горизонтальные отрезки между соседними кругами
    for j in range(3):
        cxa, cxb = NEW_CX[j], NEW_CX[j + 1]
        for sign in (1, -1):
            for da in (-TAB_HALF, +TAB_HALF):
                a = sign * (TAB_ANG + da)
                xa, ya = pt(cxa, a)
                xb = cxb - R * math.cos(math.radians(a))
                lines.append(((xa, ya), (xb, ya)))
        # отверстия разлома: по ряду на каждом конце каждой перемычки
        for sign in (1, -1):
            for cx, base in ((cxa, sign * TAB_ANG), (cxb, 180.0 - sign * TAB_ANG)):
                step = math.degrees(BITE_PITCH / (R + BITE_OUT))
                for k in range(-(BITE_N // 2), BITE_N // 2 + 1):
                    holes.append(pt(cx, base + k * step, R + BITE_OUT))
    return arcs, lines, holes


def tab_quads():
    """Четырёхугольники перемычек — под зоны запрета заливки.

    Контуры зон GND нарисованы крупнее платы (их обрезал круглый Edge.Cuts),
    поэтому после появления перемычек перезаливка залила бы медь прямо в них
    и на отверстия разлома. Запрещаем заливку в области каждой перемычки,
    заходя на 0.45 мм внутрь обеих плат — это меньше, чем отступ меди (0.5 мм),
    поэтому существующая заливка не страдает.
    """
    a1 = TAB_ANG - TAB_HALF
    a2 = TAB_ANG + TAB_HALF
    m = 0.45          # запас: зона запрета не доходит до меди (та отступает 0.5 мм от края)
    out = []
    for j in range(3):
        cxa, cxb = NEW_CX[j], NEW_CX[j + 1]
        for sign in (1, -1):
            c1, s1 = math.cos(math.radians(a1)), math.sin(math.radians(a1)) * sign
            c2, s2 = math.cos(math.radians(a2)), math.sin(math.radians(a2)) * sign
            out.append([
                (cxa + R * c1 - m, CY + R * s1 - m * sign),
                (cxb - R * c1 + m, CY + R * s1 - m * sign),
                (cxb - R * c2 + m, CY + R * s2 + m * sign),
                (cxa + R * c2 - m, CY + R * s2 + m * sign),
            ])
    return out


def render_keepouts(quads):
    out = []
    for i, q in enumerate(quads):
        pts = ' '.join('(xy %s %s)' % (fmt(x), fmt(y)) for x, y in q)
        out.append('\t(zone\n\t\t(net 0)\n\t\t(net_name "")\n'
                   '\t\t(layers "F.Cu" "B.Cu")\n'
                   '\t\t(uuid "keep0000-0000-4000-8000-%012d")\n'
                   '\t\t(name "TAB_KEEPOUT_%d")\n'
                   '\t\t(hatch edge 0.5)\n'
                   '\t\t(connect_pads\n\t\t\t(clearance 0)\n\t\t)\n'
                   '\t\t(min_thickness 0.25)\n\t\t(filled_areas_thickness no)\n'
                   '\t\t(keepout\n\t\t\t(tracks allowed)\n\t\t\t(vias allowed)\n'
                   '\t\t\t(pads allowed)\n\t\t\t(copperpour not_allowed)\n'
                   '\t\t\t(footprints allowed)\n\t\t)\n'
                   '\t\t(placement\n\t\t\t(enabled no)\n\t\t\t(sheetname "")\n\t\t)\n'
                   '\t\t(polygon\n\t\t\t(pts\n\t\t\t\t%s\n\t\t\t)\n\t\t)\n\t)\n'
                   % (i, i + 1, pts))
    return ''.join(out)


def fmt(v):
    return ('%.6f' % v).rstrip('0').rstrip('.')


def render_edges(arcs, lines):
    out = []
    for (sx, sy), (mx, my), (ex, ey) in arcs:
        out.append('\t(gr_arc\n\t\t(start %s %s)\n\t\t(mid %s %s)\n\t\t(end %s %s)\n'
                   '\t\t(stroke\n\t\t\t(width 0.05)\n\t\t\t(type solid)\n\t\t)\n'
                   '\t\t(layer "Edge.Cuts")\n\t)\n'
                   % (fmt(sx), fmt(sy), fmt(mx), fmt(my), fmt(ex), fmt(ey)))
    for (sx, sy), (ex, ey) in lines:
        out.append('\t(gr_line\n\t\t(start %s %s)\n\t\t(end %s %s)\n'
                   '\t\t(stroke\n\t\t\t(width 0.05)\n\t\t\t(type solid)\n\t\t)\n'
                   '\t\t(layer "Edge.Cuts")\n\t)\n'
                   % (fmt(sx), fmt(sy), fmt(ex), fmt(ey)))
    return ''.join(out)


def render_bites(holes):
    pads = []
    for i, (x, y) in enumerate(holes):
        pads.append('\t\t(pad "" np_thru_hole circle\n\t\t\t(at %s %s)\n\t\t\t(size %s %s)\n'
                    '\t\t\t(drill %s)\n\t\t\t(layers "F&B.Cu" "*.Mask")\n'
                    '\t\t\t(uuid "b17e0000-0000-4000-8000-%012d")\n\t\t)\n'
                    % (fmt(x), fmt(y), fmt(BITE_D), fmt(BITE_D), fmt(BITE_D), i))
    return ('\t(footprint "PANEL_MOUSEBITES"\n'
            '\t\t(layer "F.Cu")\n'
            '\t\t(uuid "b17e0000-0000-4000-8000-000000000000")\n'
            '\t\t(at 0 0)\n'
            '\t\t(descr "Ryady otverstij pod razlom zagotovki (mouse bites). '
            'Koordinaty padov absolyutnye, footprint stoit v (0,0).")\n'
            '\t\t(attr exclude_from_pos_files exclude_from_bom allow_missing_courtyard)\n'
            '\t\t(property "Reference" "PANEL1"\n\t\t\t(at 0 0 0)\n\t\t\t(layer "F.Fab")\n'
            '\t\t\t(hide yes)\n\t\t\t(uuid "b17e0000-0000-4000-8000-00000000ffff")\n'
            '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1 1)\n\t\t\t\t\t(thickness 0.15)\n'
            '\t\t\t\t)\n\t\t\t)\n\t\t)\n'
            '\t\t(property "Value" "PANEL_MOUSEBITES"\n\t\t\t(at 0 0 0)\n\t\t\t(layer "F.Fab")\n'
            '\t\t\t(hide yes)\n\t\t\t(uuid "b17e0000-0000-4000-8000-00000000fffe")\n'
            '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1 1)\n\t\t\t\t\t(thickness 0.15)\n'
            '\t\t\t\t)\n\t\t\t)\n\t\t)\n'
            + ''.join(pads) + '\t\t(embedded_fonts no)\n\t)\n')


# --- основная работа --------------------------------------------------------
def main():
    s = io.open(SRC, encoding='utf-8').read()
    root, _ = parse(s, s.index('('))
    edits = []          # (start, end, new_text)
    stats = dict(moved={}, skipped=0, circles=0, standoff=0, hole=0)

    def board_of(x, y):
        best, bi = 1e9, None
        for i, cx in enumerate(OLD_CX):
            d = math.hypot(x - cx, y - CY)
            if d < best:
                best, bi = d, i
        return bi if best <= R + 6 else None

    for item in root.kids:
        if item.head not in MOVABLE:
            continue
        if item.head == 'gr_circle':
            c = [k for k in item.kids if k.head == 'center']
            if c and any('Edge.Cuts' in s[k.start:k.end] for k in item.kids):
                edits.append((item.start, item.end, ''))     # старый контур убираем
                stats['circles'] += 1
                continue
        # опорная точка
        if item.head == 'footprint':
            at = next((k for k in item.kids if k.head == 'at'), None)
            if not at:
                continue
            c = coords(s, at)
            rep = (c[0], c[1])
            targets = [at]
        else:
            pts = [coords(s, n) for n in walk(item) if n.head in COORD_TAGS]
            pts = [p for p in pts if p]
            if not pts:
                continue
            rep = (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts))
            targets = [n for n in walk(item) if n.head in COORD_TAGS]
        bi = board_of(*rep)
        if bi is None:
            stats['skipped'] += 1
            continue
        dx = NEW_CX[bi] - OLD_CX[bi]
        stats['moved'][NAMES[bi]] = stats['moved'].get(NAMES[bi], 0) + 1
        if dx == 0:
            continue
        for n in targets:
            c = coords(s, n)
            if not c:
                continue
            a, b = c[2]
            edits.append((a, b, fmt(c[0] + dx)))

    # сверло стоек 2.75 -> 2.8 и проходное отверстие 5.0 -> 4.5
    for item in root.kids:
        if item.head != 'footprint':
            continue
        name = s[item.start:item.start + 200]
        is_standoff = 'STANDOFF_M2.5_GND_x3_TRIANGLE' in name
        is_hole = 'HOLE_HV_WIRE' in name
        if not (is_standoff or is_hole):
            continue
        for pad in [n for n in walk(item) if n.head == 'pad']:
            for n in pad.kids:
                v = nums(s, n)
                if n.head == 'drill' and v and abs(v[0][0] - (2.75 if is_standoff else 5.0)) < 1e-9:
                    edits.append((v[0][1], v[0][2], '2.8' if is_standoff else '4.5'))
                    stats['standoff' if is_standoff else 'hole'] += 1
                if is_hole and n.head == 'size' and v and abs(v[0][0] - 5.0) < 1e-9:
                    edits.append((v[0][1], v[0][2], '4.5'))
                    edits.append((v[1][1], v[1][2], '4.5'))

    # футпринт проходного отверстия переименован вслед за диаметром
    n_ren = s.count('HOLE_HV_WIRE_D5.0')

    # применяем правки справа налево
    edits.sort(key=lambda e: -e[0])
    out = s
    for a, b, t in edits:
        out = out[:a] + t + out[b:]

    # дописываем новый контур и отверстия разлома
    arcs, lines, holes = edge_geometry()
    tail = out.rstrip()
    assert tail.endswith(')')
    out = out.replace('HOLE_HV_WIRE_D5.0', 'HOLE_HV_WIRE_D4.5')
    stats['renamed'] = n_ren

    quads = tab_quads()
    out = (tail[:-1] + render_edges(arcs, lines) + render_bites(holes)
           + render_keepouts(quads) + ')\n')

    dst = SRC if '--apply' in sys.argv else SRC.replace('.kicad_pcb', '_panel.kicad_pcb')
    io.open(dst, 'w', encoding='utf-8', newline='').write(out)
    print('записано: %s' % dst)
    print('перенесено элементов по платам: %s' % stats['moved'])
    print('контуров-окружностей убрано: %d, вне плат пропущено: %d' % (stats['circles'], stats['skipped']))
    print('сверл стоек исправлено: %d, проходное отверстие: %d, переименований футпринта: %d'
          % (stats['standoff'], stats['hole'], stats.get('renamed', 0)))
    print('новый контур: дуг %d, отрезков перемычек %d, отверстий разлома %d, зон запрета %d'
          % (len(arcs), len(lines), len(holes), len(quads)))
    print('габарит заготовки: %.1f x %.1f мм' % (NEW_CX[-1] + R - (NEW_CX[0] - R), 2 * R))


if __name__ == '__main__':
    main()
