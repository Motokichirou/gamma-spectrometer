# -*- coding: utf-8 -*-
"""false_trig.py — ложные срабатывания по остатку: одиночные импульсы (без чистки) прогоняются
в режиме unfold; лишние принятые события — по положению τ от первого и по A2/A1.
python -I false_trig.py raw.bin ev.npz S.npz tpl thr sigma alo ahi"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pumodel as pm
sys.stdout.reconfigure(encoding='utf-8')
raw, evp, sp, tp, thr, sig, alo, ahi = sys.argv[1:9]
thr = int(thr); sig = float(sig)
x = pm.load_portions(raw); ev = np.load(evp)['ev']
sh = pm.Shape.load(sp); tpl = pm.Tpl(tp)
W, A, P, I, R = pm.collect_singles(x, ev, thr, float(alo), float(ahi), 30, 150, quiet_from=150)
print(f'одиночных A {alo}–{ahi}: {len(A)}')
taus = []; rat = []; nfw = 0
for k in range(len(A)):
    for mode in ('fw', 'unfold'):
        e = []
        pm.detect(W[k], thr, sig ** 2, tpl, mode=mode, shape=sh, ev=e, start_armed=True, front_k=2)
        ex = [q for q in e if q[3] == 'acc' and abs(q[0] - 30) > 1]
        if mode == 'fw':
            nfw += len(ex)
        else:
            for q in ex:
                taus.append(q[0] - 30); rat.append(q[1] / A[k])
taus = np.array(taus); rat = np.array(rat)
print(f'лишних принятых: fw {nfw}, unfold {len(taus)} ({len(taus)/max(len(A),1)*100:.2f} % от импульсов)')
if len(taus):
    h, e = np.histogram(taus, bins=[6, 15, 25, 35, 45, 60, 80, 100, 120, 151])
    print('  по τ (отсчёты от максимума):', dict(zip([f'{int(a)}–{int(b)}' for a, b in zip(e[:-1], e[1:])], h)))
    print(f'  A2/A1: медиана {np.median(rat):.3f}, 90 % {np.percentile(rat, 90):.3f}')
# зависимость от A1: число лишних с A2 в фиксированном окне на один импульс
A2 = []; A1l = []
for k in range(len(A)):
    e = []
    pm.detect(W[k], thr, sig ** 2, tpl, mode='unfold', shape=sh, ev=e, start_armed=True, front_k=2)
    for q in e:
        if q[3] == 'acc' and abs(q[0] - 30) > 1 and 12 <= q[0] - 30 <= 35:
            A2.append(q[1]); A1l.append(A[k])
A2 = np.array(A2); A1l = np.array(A1l)
for lo, hi in [(300, 500), (500, 700), (700, 1200), (1200, 4000)]:
    n1 = ((A >= lo) & (A < hi)).sum()
    m = (A1l >= lo) & (A1l < hi)
    c = ((A2 >= 30) & (A2 < 70) & m).sum()
    c2 = ((A2 >= 70) & (A2 < 150) & m).sum()
    print(f'  A1 {lo}–{hi}: импульсов {n1}, лишних с A2 30–70: {c} ({c/max(n1,1)*100:.2f} %), A2 70–150: {c2} ({c2/max(n1,1)*100:.2f} %)')
