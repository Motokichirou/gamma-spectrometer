# -*- coding: utf-8 -*-
"""Сводка по 5 зёрнам: среднее ± ст.ошибка FWHM, светимость, sigma момента.

python summarize.py csp  [каталог=out]     # ветка ЗЧУ, mc_csp_vp.py
python summarize.py tia  [каталог=out]     # ветка TIA, mc_pileup_vp.py
"""
import json, os, sys
import numpy as np
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
HERE = os.path.dirname(os.path.abspath(__file__))
br = sys.argv[1] if len(sys.argv) > 1 else 'csp'
d = os.path.join(HERE, sys.argv[2] if len(sys.argv) > 2 else 'out')
if br == 'csp':
    SEEDS, prefix, tag = [20261009, 1, 2, 3, 4], 'mc_csp', 'fit'
    methods = ['lm', 'lm_fix', 'off', 'vp', 'grid', 'off_vp', 'oracle']
else:
    SEEDS, prefix, tag = [20261008, 1, 2, 3, 4], 'mc_pileup', 'fit2'
    methods = ['none', 'off', 'vp', 'off_vp', 'oracle']
print('%-8s %5s %8s | %-16s %-7s %-10s %s' % ('метод', 'fs', 'имп/с', 'FWHM, %', 'свет,%', 'sig_t,отсч', 'по зёрнам'))
for m in methods:
    D = {}
    for s in SEEDS:
        f = os.path.join(d, '%s_%s_s%d.json' % (prefix, m, s))
        if not os.path.exists(f):
            continue
        for r in json.load(open(f, encoding='utf-8')):
            D.setdefault((r['_']['fs_MHz'], r['_']['rate']), []).append(r[tag])
    for k in sorted(D):
        v = [x['fwhm'] for x in D[k] if x['fwhm'] is not None]
        print('%-8s %5.0f %8.0f | %6.2f ± %5.2f   %5.1f   %6.3f     %s  (n=%d)' % (
            m, k[0], k[1], np.mean(v), np.std(v, ddof=1) / np.sqrt(len(v)),
            100 * np.mean([x['eff'] for x in D[k]]),
            np.nanmean([x.get('dt_sig', np.nan) for x in D[k]]),
            ' '.join('%.2f' % x for x in v), len(v)))
    if D:
        print()
