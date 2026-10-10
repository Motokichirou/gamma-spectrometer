# -*- coding: utf-8 -*-
"""Сводная таблица из out/*.json: python summarize.py"""
import glob
import json
import os
import sys

import numpy as np

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
HERE = os.path.dirname(os.path.abspath(__file__))


def ms(xs, k=1.0):
    xs = np.array([x * k for x in xs if x is not None], float)
    if len(xs) == 0:
        return 'n/a'
    return '%.2f ± %.2f' % (xs.mean(), xs.std(ddof=1) / np.sqrt(len(xs)))


print('| прогон | FWHM эл., % | светимость, % | смещение медианы, % | лишних оценок |')
print('|---|---|---|---|---|')
for p in sorted(glob.glob(os.path.join(HERE, 'out', '*.json'))):
    d = json.load(open(p, encoding='utf-8'))
    rows = d['rows']
    print('| %s | %s | %s | %s | %s |' % (
        os.path.basename(p)[:-5],
        ms([r['fit']['fwhm'] for r in rows]),
        ms([r['fit']['eff'] for r in rows], 100),
        ms([r['fit']['bias'] for r in rows]),
        ms([r.get('n_spur', 0) for r in rows])))
