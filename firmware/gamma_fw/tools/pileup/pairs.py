# -*- coding: utf-8 -*-
"""pairs.py — синтетические пары из реальных одиночных импульсов (с их шумом).
Импульс 1 (окно −30…+N от максимума) + импульс 2, сдвинутый на Δ отсчётов (максимум к максимуму).
Истинные амплитуды — подгонки одиночных. Прогон модели детектора в режимах fw и unfold.
python -I pairs.py --tpl tpl_cs.h --shape out/S.npz --s1 out/singles_cs.npz --s2 out/singles_am.npz --tag csam --thr 17 --sigma 4.08
Выход: out/pairs_<tag>.npz и png."""
import argparse
import os
import sys
import numpy as np
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pumodel as pm

PRE = 30
G = {}


def init(a):
    G['a'] = a
    G['tpl'] = pm.Tpl(a.tpl)
    G['sh'] = pm.Shape.load(a.shape)
    z1 = np.load(a.s1); z2 = np.load(a.s2)
    G['w1'], G['a1'] = z1['w'], z1['A']
    G['w2'], G['a2'] = z2['w'], z2['A']


MODES = None


def modes(a):
    m = [('fw', dict(mode='fw'))]
    for fk in a.fronts:
        m.append((f'uf_k{fk}', dict(mode='unfold', front_k=fk, eta=a.eta)))
    return m


def work(d):
    a = G['a']; tpl = G['tpl']; sh = G['sh']
    rng = np.random.default_rng(1000 + d)
    w1, a1, w2, a2 = G['w1'], G['a1'], G['w2'], G['a2']
    L1 = w1.shape[1]
    out = {}
    ms = modes(a)
    for name, _ in ms:
        out[name] = []
    for k in range(a.n):
        i = rng.integers(len(a1)); j = rng.integers(len(a2))
        if j == i and a.s1 == a.s2:
            j = (j + 1) % len(a2)
        y = w1[i][:d + 191].copy()
        y[d: d + 181] += w2[j][:181]
        ip1, ip2 = PRE, PRE + d
        for name, kw in ms:
            ev = []
            pm.detect(y, a.thr, a.sigma ** 2, tpl, shape=sh, ev=ev, start_armed=True, tail_len=a.tail, **kw)
            r1 = r2 = np.nan; extra = 0; f1 = f2 = 0
            for (ip, A, ph, kind, pw) in ev:
                if abs(ip - ip1) <= 1:
                    f1 = 1 if kind == 'acc' else 2
                    if kind == 'acc':
                        r1 = A - a1[i]
                elif abs(ip - ip2) <= 2:
                    f2 = 1 if kind == 'acc' else 2
                    if kind == 'acc':
                        r2 = A - a2[j]
                elif kind == 'acc':
                    extra += 1
            out[name].append((a1[i], a2[j], f1, f2, r1, r2, extra))
    return d, {k: np.array(v) for k, v in out.items()}


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser()
    ap.add_argument('--tpl', required=True); ap.add_argument('--shape', required=True)
    ap.add_argument('--s1', required=True); ap.add_argument('--s2', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--thr', type=int, default=17); ap.add_argument('--sigma', type=float, default=4.08)
    ap.add_argument('--eta', type=float, default=0.02)
    ap.add_argument('--tail', type=int, default=64)
    ap.add_argument('--n', type=int, default=400)
    ap.add_argument('--fronts', type=int, nargs='+', default=[4])
    ap.add_argument('--dmin', type=int, default=4); ap.add_argument('--dmax', type=int, default=150)
    a = ap.parse_args()
    ds = list(range(a.dmin, a.dmax + 1))
    res = {}
    with Pool(12, initializer=init, initargs=(a,)) as p:
        for d, o in p.imap_unordered(work, ds):
            res[d] = o
    names = [n for n, _ in modes(a)]
    save = {}
    for nm in names:
        save[nm] = np.stack([res[d][nm] for d in ds])        # Δ × n × 7
    np.savez_compressed(f'out/pairs_{a.tag}.npz', d=np.array(ds), names=np.array(names), thr=a.thr,
                        **save)
    print('сохранено', f'out/pairs_{a.tag}.npz')


if __name__ == '__main__':
    main()
