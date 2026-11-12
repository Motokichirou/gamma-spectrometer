# -*- coding: utf-8 -*-
"""run.py — прогон модели по файлу(ам) порций: спектр, статистика, события.
python -I run.py --tpl tpl_cs.h --mode fw|unfold [--shape S.npz] --thr 17 --sigma 4.08 --out pref raw1.bin [raw2.bin ...]
Спектр — pref.csv (channel,count), события — pref_ev.npz."""
import argparse
import os
import sys
import numpy as np
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pumodel as pm

G = {}


def init(args):
    G['tpl'] = pm.Tpl(args.tpl)
    G['shape'] = pm.Shape.load(args.shape) if args.shape else None
    G['a'] = args


def work(job):
    path, lo, hi = job
    a = G['a']
    x = np.fromfile(path, dtype=np.uint16, count=(hi - lo) * pm.BLK, offset=lo * pm.BLK * 2).reshape(-1, pm.BLK)
    st = pm.Stats()
    evs = []
    for k in range(x.shape[0]):
        b, v = pm.portion_base(x[k], a.thr)
        y = x[k].astype(np.float64) - b
        ev = []
        if a.rebase and a.mode == 'unfold':
            # база, согласованная с вычетом хвостов: среднее (y − pred) по спокойным отсчётам
            po = []
            pm.detect(y, a.thr, a.sigma ** 2, G['tpl'], mode=a.mode, shape=G['shape'], eta=a.eta, ev=[],
                      tail_len=a.tail, front_k=a.front, pred_out=po)
            pr = po[0][:y.size]
            q = (y - pr) <= a.thr
            # спокойные: ниже порога по остатку и не ближе 40 отсчётов после отсчёта выше порога
            hi = np.flatnonzero(~q)
            m = np.zeros(y.size + 48, bool)
            for d_ in range(-2, 41):
                idx = hi + d_
                m[idx[(idx >= 0)]] = True
            qq = ~m[:y.size]
            if qq.sum() > 100:
                y = y - (y[qq] - pr[qq]).mean()
        s = pm.detect(y, a.thr, a.sigma ** 2, G['tpl'], mode=a.mode, shape=G['shape'], eta=a.eta, ev=ev, tail_len=a.tail, front_k=a.front)
        st.add(s)
        for e in ev:
            evs.append((lo + k, e[0], e[1], e[2], {'acc': 0, 'rej_pile': 1, 'rej_shape': 2, 'rej_width': 3}[e[3]], e[4]))
    return st, evs


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser()
    ap.add_argument('--tpl', required=True)
    ap.add_argument('--shape')
    ap.add_argument('--mode', default='fw')
    ap.add_argument('--thr', type=int, required=True)
    ap.add_argument('--sigma', type=float, required=True)
    ap.add_argument('--eta', type=float, default=0.02)
    ap.add_argument('--tail', type=int, default=64)
    ap.add_argument('--front', type=int, default=4)
    ap.add_argument('--rebase', action='store_true')
    ap.add_argument('--gain', type=float, default=1.0)
    ap.add_argument('--max', type=int, default=0, help='порций с файла (0 — все)')
    ap.add_argument('--out', required=True)
    ap.add_argument('files', nargs='+')
    a = ap.parse_args()
    jobs = []
    for f in a.files:
        n = os.path.getsize(f) // (pm.BLK * 2)
        if a.max:
            n = min(n, a.max)
        for lo in range(0, n, 100):
            jobs.append((f, lo, min(lo + 100, n)))
    st = pm.Stats()
    allev = []
    with Pool(12, initializer=init, initargs=(a,)) as p:
        for fi, (s, e) in enumerate(p.imap(work, jobs)):
            st.add(s)
            fidx = [j[0] for j in jobs].index(jobs[fi][0])
            allev += [(a.files.index(jobs[fi][0]),) + t for t in e]
    print(a.out, a.mode, 'thr', a.thr, ':', st)
    ev = np.array(allev, dtype=np.float64).reshape(-1, 7)   # file, portion, ip, A, phase, kind, pred
    np.savez_compressed(a.out + '_ev.npz', ev=ev)
    acc = ev[ev[:, 5] == 0]
    ch = np.clip(np.floor(2 * acc[:, 3] * a.gain + 0.5), 0, 8191).astype(int)
    h = np.bincount(ch, minlength=8192)
    with open(a.out + '.csv', 'w') as f:
        f.write('channel,count\n')
        for i, c in enumerate(h):
            f.write(f'{i},{c}\n')


if __name__ == '__main__':
    main()
