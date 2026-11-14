# -*- coding: utf-8 -*-
"""synpairs.py — сдвиг амплитуды восстановленных импульсов на детекторе прошивки (tools/replay с ev.csv).

Синтетический поток из реальных одиночных импульсов (singles_*.npz от mk_singles.py, окна −30…+330 от
максимума, y − база): тройки слотов «пара (i, j со сдвигом Δ)», «j отдельно», «i отдельно», между окнами —
белый шум. Каждый j встречается и вторым в паре, и отдельно, с одним и тем же шумом, поэтому разность
A_пара − A_отдельно — это сдвиг от вычета хвоста, без статистики формы пика. Пары и одиночные — в одном
файле, база у них общая.

  python -I synpairs.py --replay ..\\replay\\out\\replay.exe --tail out\\t5\\tail.bin --s1 out\\singles_cs.npz
         --s2 out\\singles_cs.npz --out out\\t5\\syn [--n 12000] [--dmin 14] [--dmax 60] [--thr 17] [--sigma 3.24]
"""
import argparse
import os
import subprocess
import sys
import numpy as np

BLK = 8192
SLOT = 470
PRE = 30
BASE = 610


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser()
    ap.add_argument('--replay', required=True)
    ap.add_argument('--tail', nargs='+', required=True, help='одна или несколько таблиц (сравнение)')
    ap.add_argument('--s1', required=True)
    ap.add_argument('--s2', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--n', type=int, default=12000)
    ap.add_argument('--dmin', type=int, default=14)
    ap.add_argument('--dmax', type=int, default=60)
    ap.add_argument('--thr', type=int, default=17)
    ap.add_argument('--sigma', type=float, default=3.24)
    ap.add_argument('--seed', type=int, default=5)
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    z1, z2 = np.load(a.s1), np.load(a.s2)
    w1, w2 = z1['w'], z2['w']
    L = w1.shape[1]
    per = BLK // SLOT // 3                     # троек на порцию
    nblk = (a.n + per - 1) // per
    x = rng.normal(0.0, a.sigma, (nblk, BLK))
    truth = []                                 # blk, pos_i_pair, pos_j_pair, pos_j_ctl, pos_i_ctl, Δ
    g = 0
    for b in range(nblk):
        for k in range(per):
            if g >= a.n:
                break
            i = rng.integers(len(w1)); j = rng.integers(len(w2))
            dd = int(rng.integers(a.dmin, a.dmax + 1))
            s0 = 3 * k * SLOT + 2
            x[b, s0:s0 + L] = w1[i]
            seg = x[b, s0 + dd:s0 + dd + L]
            ov = L - dd                         # w_j поверх хвоста w_i, дальше — w_j поверх шума
            seg[:ov] += w2[j][:ov]
            seg[ov:] = w2[j][ov:]
            s1 = s0 + SLOT
            x[b, s1:s1 + L] = w2[j]
            s2 = s1 + SLOT
            x[b, s2:s2 + L] = w1[i]
            truth.append((b, s0 + PRE, s0 + dd + PRE, s1 + PRE, s2 + PRE, dd))
            g += 1
    truth = np.array(truth)
    raw = os.path.join(a.out, 'syn.bin')
    os.makedirs(a.out, exist_ok=True)
    np.clip(np.rint(x + BASE), 0, 4095).astype('<u2').tofile(raw)
    print(f'поток: {len(truth)} троек, {nblk} порций, Δ {a.dmin}…{a.dmax}, шум σ {a.sigma}, порог {a.thr}')
    for tail in a.tail:
        tag = os.path.splitext(os.path.basename(tail))[0]
        evp = os.path.join(a.out, f'ev_{tag}.csv')
        r = subprocess.run([a.replay, raw, os.path.join(a.out, f'spec_{tag}.csv'), str(a.thr), '1', tail, evp],
                           capture_output=True)
        print(f'== {tag}:', [ln for ln in r.stdout.decode('utf-8', 'replace').splitlines() if ln.startswith('STAT')][0])
        ev = np.loadtxt(evp, delimiter=',', skiprows=1, ndmin=2)
        key = {}
        for blk, pos, ch, rec in ev:
            key[(int(blk), int(pos))] = (ch / 2.0, int(rec))

        def find(b, p):
            for dp in (0, -1, 1, -2, 2):
                v = key.get((b, p + dp))
                if v is not None:
                    return v
            return None
        rows = []
        for b, pi, pj, cj, ci, dd in truth:
            ej, cjv, eiv = find(b, pj), find(b, cj), find(b, pi)
            civ = find(b, ci)
            rows.append((dd,
                         ej[0] if ej else np.nan, ej[1] if ej else -1, cjv[0] if cjv else np.nan,
                         eiv[0] if eiv else np.nan, civ[0] if civ else np.nan))
        R = np.array(rows, dtype=float)
        print('  Δ        | 2-й принят | из них rec | сдвиг 2-го, коды | % от A | σ разности | 1-й: сдвиг %')
        bins = [(a.dmin, 19), (20, 29), (30, 39), (40, 49), (50, a.dmax), (a.dmin, a.dmax)]
        for lo, hi in bins:
            m = (R[:, 0] >= lo) & (R[:, 0] <= hi)
            ok = m & np.isfinite(R[:, 1]) & np.isfinite(R[:, 3])
            dif = R[ok, 1] - R[ok, 3]
            pct = 100 * dif.mean() / R[ok, 3].mean() if ok.any() else np.nan
            epct = 100 * dif.std() / np.sqrt(max(ok.sum(), 1)) / R[ok, 3].mean() if ok.any() else np.nan
            o1 = m & np.isfinite(R[:, 4]) & np.isfinite(R[:, 5])
            d1 = 100 * (R[o1, 4] - R[o1, 5]).mean() / R[o1, 5].mean() if o1.any() else np.nan
            print(f'  {lo:2d}…{hi:2d}    | {ok.sum():5d}/{m.sum():5d} | {(R[ok, 2] == 1).sum():5d} | '
                  f'{dif.mean():+7.3f} ± {dif.std() / np.sqrt(max(ok.sum(), 1)):.3f} | {pct:+.3f} ± {epct:.3f} | '
                  f'{dif.std():6.2f} | {d1:+.3f}')


if __name__ == '__main__':
    main()
