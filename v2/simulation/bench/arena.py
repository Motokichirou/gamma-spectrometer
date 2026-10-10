# -*- coding: utf-8 -*-
"""Прогон одного варианта по зёрнам. Печатает среднее ± ст.ошибка по зёрнам.

  python arena.py tia  [--q 0.3333] [--rf 5000] [--cin 6e-12] [--sys none|static:+4|drift:4] [--slow 0.05]
  python arena.py csp  [--solver cluster|block] [--sys ...] [--slow ...] [--no-refine]
  общие: --seeds 1,2,3,4,5  --dur 4e-3  --rate 1e6  --fs 125e6  --json out.json
"""
import argparse
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')


def job(args):
    branch, seed, kw = args
    if branch == 'tia':
        import tia
        return tia.run(seed, **kw)
    import csp
    return csp.run(seed, **kw)


def ms(xs):
    xs = np.array([x for x in xs if x is not None], float)
    if len(xs) == 0:
        return 'n/a'
    if len(xs) == 1:
        return '%.2f' % xs[0]
    return '%.2f ± %.2f' % (xs.mean(), xs.std(ddof=1) / np.sqrt(len(xs)))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('branch', choices=['tia', 'csp'])
    p.add_argument('--seeds', default='1,2,3,4,5')
    p.add_argument('--dur', type=float, default=4e-3)
    p.add_argument('--rate', type=float, default=1e6)
    p.add_argument('--fs', type=float, default=125e6)
    p.add_argument('--sys', default='none')
    p.add_argument('--slow', type=float, default=0.0)
    p.add_argument('--q', type=float, default=1.0)
    p.add_argument('--rf', type=float, default=5000.0)
    p.add_argument('--cin', type=float, default=0.0)
    p.add_argument('--solver', default='cluster')
    p.add_argument('--no-refine', action='store_true')
    p.add_argument('--detector', default='group')
    p.add_argument('--cf', type=float, default=100e-12)
    p.add_argument('--en-scale', type=float, default=1.0)
    p.add_argument('--nav', type=int, default=3)
    p.add_argument('--bits', type=int, default=16)
    p.add_argument('--oracle', action='store_true')
    p.add_argument('--lm-fix', action='store_true')
    p.add_argument('--redetect', type=int, default=0)
    p.add_argument('--k-slow', type=int, default=16)
    p.add_argument('--near-min', type=int, default=4)
    p.add_argument('--fit-td', action='store_true')
    p.add_argument('--adc-snr', type=float, default=None)
    p.add_argument('--vfs', type=float, default=4.4)
    p.add_argument('--vramp', type=float, default=4.0)
    p.add_argument('--json')
    a = p.parse_args()
    seeds = [int(s) for s in a.seeds.split(',')]
    kw = dict(adc_snr=a.adc_snr, fs=a.fs, rate=a.rate, dur=a.dur, sysmode=a.sys, slow=a.slow)
    if a.branch == 'tia':
        kw.update(q_scale=a.q, rf=a.rf, cin=a.cin, solver=('vp' if a.solver == 'vp' else 'block'),
                  redetect=a.redetect, k_slow=a.k_slow, near_min=a.near_min, fit_td=a.fit_td)
    else:
        kw.update(solver=a.solver, refine=not a.no_refine, detector=a.detector, cf=a.cf, en_scale=a.en_scale, nav=a.nav, adc_bits=a.bits, oracle=a.oracle, lm_fix=a.lm_fix, redetect=a.redetect, k_slow=a.k_slow, near_min=a.near_min, fit_td=a.fit_td, v_fs=a.vfs, v_ramp=a.vramp)
    with ProcessPoolExecutor(max_workers=min(len(seeds), 10)) as ex:
        rows = list(ex.map(job, [(a.branch, s, kw) for s in seeds]))
    print('%s %s' % (a.branch, ' '.join('%s=%s' % kv for kv in sorted(kw.items()))))
    tags = ['fit', 'solo'] + (['twoavg'] if a.branch == 'csp' else [])
    for t in tags:
        fw = ms([r[t]['fwhm'] for r in rows])
        bias = ms([r[t]['bias'] for r in rows])
        eff = ms([100 * r[t]['eff'] for r in rows]) if 'eff' in rows[0][t] else '-'
        if 'pk3' in rows[0][t]:
            print('  %-7s доля фотопика с |ошибкой|<3%%: %s %%' % (t, ms([100 * r[t]['pk3'] for r in rows])))
        print('  %-7s FWHM %-15s %%  светимость %-15s %%  смещение медианы %s %%' % (t, fw, eff, bias))
    print('  событий %s, найдено %s' % (ms([r['n_ev'] for r in rows]), ms([r['n_det'] for r in rows])))
    print('  оценок %s, из них без истинного события %s' % (ms([r['n_est'] for r in rows]), ms([r['n_spur'] for r in rows])))
    if 'thr_rel' in rows[0]:
        print('  порог детектора %s %% фотопика, сбросов %s, мёртвое время %s %%' % (ms([100*r['thr_rel'] for r in rows]), ms([r['resets'] for r in rows]), ms([r['dead'] for r in rows])))
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump(dict(args=vars(a), rows=rows), f, ensure_ascii=False, indent=1)


if __name__ == '__main__':
    main()
