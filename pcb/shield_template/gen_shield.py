"""Шаблон экрана между платами ВВ и МК (печать 1:1 и вырезка из пермаллоя).

Геометрия берётся из межплатной шины (`pcb/gamma_spectrometer/gamma_spectrometer.kicad_pcb`):
  - цанг-пины Ø2.5 на R20: 106 119 132 145 171 184 197 223 236 250 317 343 356° (пины платы МК —
    именно они проходят зазор ВВ↔МК);
  - стойки M2.5 на R19: 90 210 330° (экран зажимается между «половинными» стойками по 6 мм);
  - провод ПВВ-1: от пина −ВВ платы ВВ (R20, 288°) к отверстию H_HV301 платы МК (R18, 288°).
Углы — вид как в KiCad (со стороны F.Cu), против часовой от оси +X.

Экран: круг R17 (≈1.75 мм до края цанг), три лепестка к стойкам (скругление R2.2, отверстие Ø2.8),
вырез под ПВВ-1 радиусом 5 мм. Запуск: python gen_shield.py → shield_template.kicad_pcb.
"""
import math, os

R_DISK = 17.0
STANDOFFS = (90, 210, 330); R_SO = 19.0; TAB_R = 2.2; TAB_W = 2.2; HOLE_SO = 2.8
PINS = (106, 119, 132, 145, 171, 184, 197, 223, 236, 250, 317, 343, 356); R_PIN = 20.0; PIN_D = 2.5
PIN_CLR = 1.0                       # зазор от края цанги
HV_ANG = 288; HV_R = 19.0; HV_NOTCH = 5.0
CX, CY = 100.0, 100.0               # центр на листе (мм)

def ray_far_tab(th, phi):
    d = math.radians(th - phi)
    far = 0.0
    s, c = R_SO * math.sin(d), R_SO * math.cos(d)
    if abs(s) <= TAB_R and c > 0:
        far = max(far, c + math.sqrt(TAB_R**2 - s**2))
    if math.cos(d) > 0:
        rr = R_SO / math.cos(d)
        if abs(math.sin(d)) > 1e-9:
            rr = min(rr, TAB_W / abs(math.sin(d)))
        far = max(far, rr)
    return far

def ray_entry(th, phi, D, rad):
    d = math.radians(th - phi)
    s, c = D * math.sin(d), D * math.cos(d)
    if abs(s) < rad and c > 0:
        return c - math.sqrt(rad**2 - s**2)
    return None

def outline(step=0.25):
    pts = []
    n = int(round(360 / step))
    for i in range(n):
        th = i * step
        r = R_DISK
        for so in STANDOFFS:
            r = max(r, ray_far_tab(th, so))
        e = ray_entry(th, HV_ANG, HV_R, HV_NOTCH)
        if e is not None: r = min(r, e)
        for p in PINS:
            e = ray_entry(th, p, R_PIN, PIN_D / 2 + PIN_CLR)
            if e is not None: r = min(r, e)
        a = math.radians(th)
        pts.append((CX + r * math.cos(a), CY - r * math.sin(a)))
    return pts

def pol(r, ang):
    a = math.radians(ang); return CX + r * math.cos(a), CY - r * math.sin(a)

def gr_circle(x, y, rad, layer, w=0.1):
    return f'\t(gr_circle (center {x:.4f} {y:.4f}) (end {x+rad:.4f} {y:.4f}) (stroke (width {w}) (type solid)) (fill no) (layer "{layer}"))\n'

def gr_line(x1, y1, x2, y2, layer, w=0.15):
    return f'\t(gr_line (start {x1:.4f} {y1:.4f}) (end {x2:.4f} {y2:.4f}) (stroke (width {w}) (type solid)) (layer "{layer}"))\n'

def gr_text(t, x, y, layer, h=1.5, ang=0):
    return (f'\t(gr_text "{t}" (at {x:.3f} {y:.3f} {ang}) (layer "{layer}")\n'
            f'\t\t(effects (font (size {h} {h}) (thickness {h*0.15:.3f}))))\n')

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    src = open(os.path.join(here, '..', 'gamma_spectrometer', 'gamma_spectrometer.kicad_pcb'), encoding='utf-8').read()
    head = src[:src.index('\n\t(footprint')]
    body = ''
    pts = outline()
    pts_s = ' '.join(f'(xy {x:.4f} {y:.4f})' for x, y in pts)
    body += f'\t(gr_poly (pts {pts_s}) (stroke (width 0.1) (type solid)) (fill no) (layer "Edge.Cuts"))\n'
    for so in STANDOFFS:
        x, y = pol(R_SO, so); body += gr_circle(x, y, HOLE_SO / 2, 'Edge.Cuts')
    # ориентиры (не резать): цанги, отверстие ПВВ, контур платы Ø45, центр
    for p in PINS:
        x, y = pol(R_PIN, p); body += gr_circle(x, y, PIN_D / 2, 'Dwgs.User', 0.1)
    x, y = pol(18.0, HV_ANG); body += gr_circle(x, y, 2.25, 'Dwgs.User', 0.1)
    x, y = pol(20.0, HV_ANG); body += gr_circle(x, y, 1.25, 'Dwgs.User', 0.1)
    body += gr_circle(CX, CY, 22.5, 'Dwgs.User', 0.1)
    body += gr_line(CX - 2, CY, CX + 2, CY, 'Dwgs.User', 0.1) + gr_line(CX, CY - 2, CX, CY + 2, 'Dwgs.User', 0.1)
    x, y = pol(24.5, HV_ANG); body += gr_text('ПВВ-1', x, y + 1.5, 'Dwgs.User', 1.2)
    # проверка масштаба: отрезок 50 мм
    y0 = CY + 35
    body += gr_line(CX - 25, y0, CX + 25, y0, 'Dwgs.User', 0.2)
    for k in range(6):
        xx = CX - 25 + 10 * k; body += gr_line(xx, y0 - 1.5, xx, y0 + 1.5, 'Dwgs.User', 0.2)
    body += gr_text('50 мм — проверить линейкой (печать 100 %)', CX, y0 + 4, 'Dwgs.User', 1.5)
    body += gr_text('Экран ВВ/МК, вид как в KiCad (F.Cu). Резать по сплошному контуру.', CX, CY - 32, 'Dwgs.User', 1.5)
    body += gr_text('Кружки — цанги/ПВВ/контур платы (ориентиры). Не совпали пины — перевернуть лист.', CX, CY - 29, 'Dwgs.User', 1.2)
    out = os.path.join(here, 'shield_template.kicad_pcb')
    open(out, 'w', encoding='utf-8', newline='\n').write(head + '\n' + body + ')\n')
    # отчёт по зазорам
    mind = min(math.hypot(x - px, y - py) - PIN_D / 2 for x, y in pts for px, py in (pol(R_PIN, p) for p in PINS))
    print(out, '| вершин', len(pts), '| мин. зазор контур–цанга %.2f мм' % mind)

if __name__ == '__main__':
    main()
