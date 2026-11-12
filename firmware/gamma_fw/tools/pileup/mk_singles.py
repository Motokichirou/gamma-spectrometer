# -*- coding: utf-8 -*-
"""mk_singles.py — чистые одиночные (без неотловленных соседей: остаток после A·S ≤ 6σ + 2 % A).
python -I mk_singles.py raw.bin ev.npz S.npz thr sigma alo ahi out.npz [post]"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pumodel as pm
sys.stdout.reconfigure(encoding='utf-8')
raw, evp, sp, thr, sig, alo, ahi, outp = sys.argv[1:9]
post = int(sys.argv[9]) if len(sys.argv) > 9 else 330
thr = int(thr); sig = float(sig)
x = pm.load_portions(raw)
ev = np.load(evp)['ev']
sh = pm.Shape.load(sp)
W, A, P, I, R = pm.collect_singles(x, ev, thr, float(alo), float(ahi), 30, post)
n = np.arange(-30, post + 1)
res = W - A[:, None] * sh(n[None, :] - sh.off[P][:, None])
keep = np.abs(res[:, n >= 6]).max(1) <= 6 * sig + 0.02 * A
print(f'{outp}: одиночных {len(A)}, чистых {keep.sum()} ({keep.mean()*100:.1f} %), A ср {A[keep].mean():.1f}')
np.savez_compressed(outp, w=W[keep], A=A[keep], ph=P[keep])
