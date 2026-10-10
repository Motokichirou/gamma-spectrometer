import sys, numpy as np
sys.path.insert(0, '.')
import csp_core as C
f_pp, cont = C.cs_spectrum()
R = {}
for name, vr in (('REF', 4.0), ('A', 1.55)):
    R[name] = C.run(125e6, 50e6, 1e6, 2e-3, f_pp, cont, seed=20261009, v_ramp=vr, v_fs=2.0 if vr < 2 else 4.4, reset_aware=True, quiet_reset=True)
d = R['A']['_dbg']; t = d['t_ev']; pp = np.where(d['is_pp'])[0]
lostA = set(np.setdiff1d(pp, d['matched'])); lostR = set(np.setdiff1d(pp, R['REF']['_dbg']['matched']))
rs = d['resets']
for i in sorted(lostA - lostR):
    k = np.searchsorted(rs, t[i]); 
    dp = (t[i] - rs[k-1])*1e6 if k>0 else None; dn = (rs[k]-t[i])*1e6 if k < len(rs) else None
    nd = np.min(np.abs(d['t_det'] - t[i]))*1e9; ne = np.min(np.abs(d['est_t'] - t[i]))*1e9 if len(d['est_t']) else -1
    print('ev %d t=%.3f us  after prev reset %s  before next %s  nearest det %.0f ns  nearest est %.0f ns' % (i, t[i]*1e6, dp, dn, nd, ne))
