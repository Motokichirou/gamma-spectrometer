# -*- coding: utf-8 -*-
"""poisson_rate.py — проверка оценки скорости «принято / живое» детектора прошивки (tools/replay).

Пуассоновский поток известной скорости из реальных одиночных импульсов (singles_*.npz от mk_singles.py,
окна −30…+330 от максимума, y − база; где окон нет — белый шум σ), прогон replay без разбора и с таблицей
хвоста, сравнение принято/живое с истинной скоростью (импульсы с максимумом внутри порций).

  python -I poisson_rate.py --replay ..\\replay\\out\\replay.exe --tail out\\t5\\tail.bin --singles out\\singles_cs.npz
         --rate 19000 [--blocks 1500] [--thr 17] [--sigma 3.24] [--seed 1] [--out out\\t5\\poisson.bin]
"""
import argparse
import subprocess
import sys
import numpy as np

FS = 3466667.0
BLK = 8192
PRE = 30


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser()
    ap.add_argument('--replay', required=True)
    ap.add_argument('--tail', required=True)
    ap.add_argument('--singles', required=True)
    ap.add_argument('--rate', type=float, required=True)
    ap.add_argument('--blocks', type=int, default=1500)
    ap.add_argument('--thr', type=int, default=17)
    ap.add_argument('--sigma', type=float, default=3.24)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--out', default='poisson.bin')
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    w = np.load(a.singles)['w']
    L = w.shape[1]
    x = np.zeros((a.blocks, BLK))
    cov = np.zeros((a.blocks, BLK), dtype=bool)
    ntrue = 0
    for b in range(a.blocks):
        t = rng.exponential(FS / a.rate) - PRE
        while t < BLK:
            k = int(np.floor(t))
            i = rng.integers(len(w))
            lo, hi = max(k, 0), min(k + L, BLK)
            if hi > lo:
                x[b, lo:hi] += w[i][lo - k:hi - k]
                cov[b, lo:hi] = True
            if 0 <= k + PRE < BLK:
                ntrue += 1
            t += rng.exponential(FS / a.rate)
    x += np.where(cov, 0.0, rng.normal(0, a.sigma, x.shape))
    np.clip(np.rint(x + 610), 0, 4095).astype('<u2').tofile(a.out)
    T = a.blocks * BLK / FS
    print(f'поток {a.rate:.0f}/с, порций {a.blocks}, импульсов {ntrue}, истинная скорость {ntrue / T:.0f}/с, порог {a.thr}')
    for tl in ([], [a.tail]):
        o = subprocess.run([a.replay, a.out, a.out + '.csv', str(a.thr), '1'] + tl,
                           capture_output=True).stdout.decode('utf-8', 'replace')
        st = dict(kv.split('=') for kv in [ln for ln in o.splitlines() if ln.startswith('STAT')][0][5:].split())
        acc, live = float(st['acc']), float(st['live_s'])
        print(f"  разбор {'вкл ' if tl else 'выкл'}: принято {acc:.0f}, живое {live:.4f} с, принято/живое "
              f"{acc / live:.0f}/с ({100 * (acc / live / (ntrue / T) - 1):+.2f} % к истинной); наложения {st['pile']}, "
              f"ширина {st['width']}, восстановлено {st['rec']}")


if __name__ == '__main__':
    main()
