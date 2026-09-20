"""Генератор нетлиста LTspice: плата ВВ (MAX1847 flyback + CW ×4 ступени + RC-фильтры).

Топология — по pcb/gamma_spectrometer.net (сверено 2026-09-19):
  T201: pin1(dot)=+5V_HV, pin4=сток Q201; pin2(dot)=GND, pin3 → C209
  CW (отрицательный, 4 ступени = 8 диодов BAV23S по одному «диоду» на корпус, pin3 NC):
    толкающая колонка A: T201.3 -C209- A1 -C211- A2 -C213- A3 -C215- A4
    сглаживающая B:      GND -C210- B1 -C212- B2 -C214- B3 -C216- B4 = CW_out
    диоды (анод→катод): D205 A1→GND, D204 B1→A1, D207 A2→B1, D206 B2→A2,
                        D209 A3→B2, D208 B3→A3, D211 A4→B3, D210 B4→A4
  CW_out: C13+C14 (по 2×0.1 мкФ/1 кВ в серии) → R220 → n1 → R231 → n2 → R232 → n3 → R233 → −HV_out
  FB: CW_out ─ 1 ГОм (R207,R209–R217) ─ FB ─ 2.2 МОм (R208) ─ HV_CONTROL.  ⇒ RC-фильтр ВНЕ петли.
  HV_MONITOR: −HV_out ─ 68 МОм ─ 110 кОм ─ 2.5 В (TL431).

MAX1847 — поведенческая модель по даташиту (max1847_text.txt):
  gm = 400 мкА/В, Ro = 3 МОм, A_CS = 3.3, компенсация наклона Ms = 41 мВ/мкс (приведена к CS),
  ограничение тока V_CS = 85/100/115 мВ (мин/тип/макс), FB-порог 0 В, f по R202 = 180 к → 252 кГц.
  ДОПУЩЕНИЯ (в даташите нет): смещение ШИМ-компаратора Voff = 1 В (влияет только на рабочую точку COMP,
  не на усиление), бланкирование 100 нс, зажим COMP 0.5…3.5 В, ток EA ±200 мкА, R_on EXT 4 Ом.
"""
import math

LTSPICE = r'C:\Program Files\ADI\LTspice\LTspice.exe'

DEFAULTS = dict(
    Vin=5.0, Lp=5.6e-6, Llk=0.3e-6, N=10.2, Rpri=0.35, Rsec=40.0, Csec=3e-12,
    fsw=None, Voff=1.0, Ms=41e3, Vcl=0.100, blank=100e-9,
    Cd=35e-9,      # C13/C14: 50 нФ номинал, X7R 1 кВ при ~750 В на корпус — берём −30 %
    Cf=35e-9,      # C15–C17: то же
    Ccw=1e-9,      # C209–C216 C0G
    Rload=7.73e6,  # делитель R1307: 1500 В / 194 мкА
    Rmon=68.11e6, Rtop=1e9, Rbot=2.2e6, Cff=10e-15, Cfb=5e-12,
    R201=1e6, C201=33e-9, Cc2=0.0,
    Vctrl=2.64,
    Vz=17.9, Rz=15.0,
    tstop=30e-3, tsave=28e-3, maxstep=20e-9,
    comp_ic=None,
    extra='',
    saves=None,
    opts='',
    fbnet=None,    # eps (2.75/4.5): вставить реальную цепочку FB и матрицу ёмкостей из out/hv_caps.json
    inj=None,      # (амплитуда, частота): синус в разрыв FB → вход EA, для измерения петлевого усиления
)


def fsw_from_rfreq(r=180e3):
    t = 5.21e-7 + 1.92e-11 * r - 4.86e-19 * r * r
    return 1.0 / t


SIMNODE = {'GND': '0', 'FB': 'fb', 'CWO': 'cw', 'A1': 'a1', 'A2': 'a2', 'A3': 'a3', 'A4': 'a4',
           'SEC': 'sec', 'DRN': 'drain', 'SNB': 'snk', 'COMP': 'comp', 'HVC': 'ctrl',
           **{f'CH{i}': f'ch{i}' for i in range(1, 10)}}
VICTIM = ['FB', 'COMP'] + [f'CH{i}' for i in range(1, 10)]


def fb_chain(eps):
    """Цепочка 1 ГОм десятью резисторами + паразитные ёмкости из FastCap (out/hv_caps.json)."""
    import json, os
    import numpy as np
    d = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out', 'hv_caps.json')))
    names, C = d['names'], np.array(d['C_pF_air']) * eps      # пФ
    ix = {n: i for i, n in enumerate(names)}
    L, k = [], 0
    seq = ['cw'] + [f'ch{i}' for i in range(1, 10)] + ['fb']
    for a, b in zip(seq, seq[1:]):
        k += 1
        L.append(f'Rch{k} {a} {b} 100Meg')
    for i, a in enumerate(names):
        if a not in SIMNODE:
            continue
        for b in names[i + 1:]:
            if b not in SIMNODE:
                continue
            cm = -C[ix[a], ix[b]]
            if cm > 1e-5:
                L.append(f'Cp_{a}_{b} {SIMNODE[a]} {SIMNODE[b]} {cm:.4g}p')
        if a in VICTIM:      # остаток «на бесконечность» сверх всех взаимных — тоже на землю
            cg = C[ix[a], ix[a]] - sum(-C[ix[a], ix[b]] for b in names if b != a)
            if cg > 1e-5:
                L.append(f'Cg_{a} {SIMNODE[a]} 0 {cg:.4g}p')
    return L


def deck(name, **kw):
    p = dict(DEFAULTS)
    p.update(kw)
    if p['fsw'] is None:
        p['fsw'] = fsw_from_rfreq()
    T = 1.0 / p['fsw']
    Vcw = -p['Vctrl'] * p['Rtop'] / p['Rbot']          # целевое CW_out (без смещений)
    Rl = 1.0 / (1.0 / p['Rload'] + 1.0 / p['Rmon'])
    I = -Vcw / (Rl + 300e3)                              # ток через RC-фильтр
    Vpp = -Vcw / 4.0 * 1.03
    # оценка COMP: P_in ≈ P_out/0.7
    Pout = Vcw ** 2 / (Rl + 300e3) + Vcw ** 2 / p['Rtop']
    Ipk = math.sqrt(2 * Pout / 0.7 / (p['Lp'] * p['fsw']))
    ton = p['Lp'] * Ipk / p['Vin']
    comp0 = p['comp_ic'] if p['comp_ic'] is not None else \
        p['Voff'] + 3.3 * (0.1 * Ipk + p['Ms'] * ton)
    Lsec = p['Lp'] * p['N'] ** 2
    L = [f'* {name}: HV board MAX1847 flyback + CW x4',
         f'.param T={T:.6e}',
         f'Vin in0 0 {p["Vin"]}',
         'Rsrc in0 vhv 0.2',
         'C206 vhv 0 25u Rser=5m',
         'C207 vhv 0 100n',
         'C204 vhv 0 100n',
         # трансформатор: Llk вынесена отдельно, K=1
         f'Lpri vhv p1 {p["Lp"]} Rser={p["Rpri"]}',
         f'Llk p1 drain {p["Llk"]}',
         f'Lsec 0 sec {Lsec} Rser={p["Rsec"]}',
         'K1 Lpri Lsec 1',
         f'Csec sec 0 {p["Csec"]}',
         # Q201 IRLML0060
         'M1 drain gate src src IRLML0060',
         '.model IRLML0060 VDMOS(Rd=30m Rs=10m Rg=1.5 Vto=1.8 Kp=10 lambda=0.01 Cgdmax=40p Cgdmin=6p '
         'Cgs=220p Cjo=70p m=0.5 Is=2p Rb=20m Vds=60 Ron=92m Qg=2.5n mfg=approx)',
         'R205 src 0 0.1',
         'C205 src 0 100p',
         # снаббер: D201 BAS516 (A=сток), C208, D202+D203 BZT52C18 (K к snk)
         'D201 drain snk BAS516',
         'C208 snk 0 10n Rser=20m',
         'D202 z1 snk BZT18',
         'D203 0 z1 BZT18',
         '.model BAS516 D(Is=2.5n N=1.75 Rs=0.5 Cjo=1.5p M=0.3 tt=5n Bv=100 Ibv=0.1u)',
         f'.model BZT18 D(Is=1e-14 N=1 Rs={p["Rz"]} Cjo=35p M=0.4 Bv={p["Vz"]} Ibv=5m)',
         # CW
         f'C209 sec a1 {p["Ccw"]} IC={Vpp - 51:.1f}',
         f'C211 a1 a2 {p["Ccw"]} IC={Vpp:.1f}',
         f'C213 a2 a3 {p["Ccw"]} IC={Vpp:.1f}',
         f'C215 a3 a4 {p["Ccw"]} IC={Vpp:.1f}',
         f'C210 b1 0 {p["Ccw"]} IC={-Vpp:.1f}',
         f'C212 b2 b1 {p["Ccw"]} IC={-Vpp:.1f}',
         f'C214 b3 b2 {p["Ccw"]} IC={-Vpp:.1f}',
         f'C216 b4 b3 {p["Ccw"]} IC={-Vpp:.1f}',
         'D205 a1 0 BAV23S2', 'D204 b1 a1 BAV23S2', 'D207 a2 b1 BAV23S2', 'D206 b2 a2 BAV23S2',
         'D209 a3 b2 BAV23S2', 'D208 b3 a3 BAV23S2', 'D211 a4 b3 BAV23S2', 'D210 b4 a4 BAV23S2',
         'Vsense b4 cw 0',  # ток умножителя в выходной узел — для расчёта пульсаций
         '.model BAV23S2 D(Is=5e-11 N=3.6 Rs=2 Cjo=1p M=0.3 tt=50n Bv=500 Ibv=100n)',
         # выход и фильтры
         f'C13 cw 0 {p["Cd"]} IC={Vcw:.1f}',
         f'C14 cw 0 {p["Cd"]} IC={Vcw:.1f}',
         'R220 cw n1 100k',
         f'C15 n1 0 {p["Cf"]} IC={Vcw + 100e3 * I:.1f}',
         'R231 n1 n2 100k',
         f'C16 n2 0 {p["Cf"]} IC={Vcw + 200e3 * I:.1f}',
         'R232 n2 n3 100k',
         f'C17 n3 0 {p["Cf"]} IC={Vcw + 300e3 * I:.1f}',
         'R233 n3 hvout 10',
         f'C18 hvout 0 110p IC={Vcw + 300e3 * I:.1f}',
         f'Rload hvout 0 {p["Rload"]}',
         f'Rmon hvout vref25 {p["Rmon"]}',
         'Vref25 vref25 0 2.5',
         # FB
         *(fb_chain(p['fbnet']) if p['fbnet'] else
           [f'Rtop cw fb {p["Rtop"]}', f'Cff cw fb {p["Cff"]}']),
         f'R208 fb ctrl {p["Rbot"]}',
         f'Cfb fb 0 {p["Cfb"]}',
         f'Vctrl ctrl 0 {p["Vctrl"]}',
         # усилитель ошибки
         f'Bea 0 comp I=limit(400u*V({"fbx" if p["inj"] else "fb"}),-200u,200u)',
         'Rea comp 0 3Meg',
         f'R201 comp cz {p["R201"]}',
         f'C201 cz 0 {p["C201"]} IC={comp0:.4f}',
         'Dcl1 comp vcmax DI', 'Vcmax vcmax 0 3.5', 'Dcl2 vcmin comp DI', 'Vcmin vcmin 0 0.5',
         '.model DI D(Ron=1 Roff=1G Vfwd=0)',
         # ШИМ: такт, пила компенсации наклона (в вольтах на CS), сброс
         'Vclk clk 0 PULSE(0 1 0 1n 1n 30n {T})',
         f'Vrmp rmp 0 PULSE(0 {{{p["Ms"]}*0.98*T}} 0 {{0.98*T}} {{0.01*T}} 1n {{T}})',
         f'Brst rst 0 V=if(V(rmp)>{p["Ms"] * p["blank"]:.6g} & (3.3*(V(src)+V(rmp))>V(comp)-{p["Voff"]} | '
         f'V(src)>{p["Vcl"]} | V(rmp)>{{{p["Ms"]}*0.9*T}}),1,0)',
         'Vone one 0 1',
         'A1 one 0 clk 0 rst qb q 0 DFLOP Vhigh=1 Vlow=0 Trise=3n Tfall=3n',
         'Bdrv ext0 0 V=V(q)*4.25',
         'Rext ext0 ext 4',
         'R206 ext gate 10',
         ]
    if p['fbnet']:
        p['Cfb'] = 3e-12            # в сети уже есть ёмкость меди FB; здесь только вывод MAX1847
    if p['inj']:
        L.append(f'Vinj fbx fb SINE(0 {p["inj"][0]} {p["inj"][1]})')
    if p['Cc2']:
        L.append(f'Cc2 comp 0 {p["Cc2"]}')
    L.append(f'.ic V(comp)={comp0:.4f} V(cz)={comp0:.4f}')
    if p['extra']:
        L.append(p['extra'])
    sv = p['saves'] or ('V(cw) V(n1) V(n2) V(n3) V(hvout) V(drain) V(snk) V(comp) V(src) V(sec) V(fb) V(vhv) '
                        'I(Lpri) I(D202) I(Vsense) V(a1) V(q) V(fbx) V(cz)')
    L += ['.options plotwinsize=0 numdgt=7 ' + p['opts'],
          '.save ' + sv,
          f'.tran 0 {p["tstop"]} {p["tsave"]} {p["maxstep"]} uic']
    return L, p


def meas_windows(L, t0, t1, n, node='cw'):
    """Средние V(node) по окнам — тренд установления без сохранения всей волны."""
    w = (t1 - t0) / n
    for i in range(n):
        a, b = t0 + i * w, t0 + (i + 1) * w
        L.append(f'.meas tran {node}_w{i:02d} AVG V({node}) FROM={a:.6g} TO={b:.6g}')
