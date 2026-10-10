# -*- coding: utf-8 -*-
import sys
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
"""Сводка по зёрнам: среднее ± СКО (и СКО среднего) FWHM по каждому варианту."""
import json, sys, numpy as np
from collections import defaultdict
rows = json.load(open(sys.argv[1], encoding='utf-8'))
keys = [k for k in rows[0] if k not in ('_', 'trap')]
d = defaultdict(lambda: defaultdict(list))
for r in rows:
    key = (r['_']['fs_MHz'], r['_']['rate'])
    for k in keys:
        d[key][k].append(r[k]['fwhm'])
    if 'trap' in r:
        d[key]['trap'].append(r['trap']['fwhm'])
    d[key]['eff_det'].append(r[keys[0]]['eff'])
for key in sorted(d):
    n = len(d[key][keys[0]])
    print('fs=%g МГц, %g имп/с, зёрен %d' % (key[0], key[1], n))
    for k, v in d[key].items():
        v = np.array(v)
        print('   %-9s %7.2f ± %5.2f  (σ_ср %4.2f)' % (k, v.mean(), v.std(ddof=1) if n > 1 else 0, (v.std(ddof=1) / np.sqrt(n)) if n > 1 else 0))
