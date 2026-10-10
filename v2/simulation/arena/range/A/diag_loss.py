# Диагностика: где теряются события фотопика относительно сбросов (вариант A против REF)
import sys, numpy as np
sys.path.insert(0, '.')
import csp_core as C
f_pp, cont = C.cs_spectrum()
for name, vr in (('REF', 4.0), ('A', 1.55)):
    r = C.run(125e6, 50e6, 1e6, 2e-3, f_pp, cont, seed=20261009, v_ramp=vr, v_fs=2.0 if vr < 2 else 4.4, reset_aware="ra" in sys.argv, quiet_reset="qr" in sys.argv)
    d = r['_dbg']; t = d['t_ev']; pp = np.where(d['is_pp'])[0]
    lost = np.setdiff1d(pp, d['matched'])
    rs = d['resets']
    def dist(tt):  # время от события до ближайшего сброса (знак: + после сброса)
        k = np.searchsorted(rs, tt)
        prev = np.where(k > 0, tt - rs[np.maximum(k - 1, 0)], np.inf)
        nxt = np.where(k < len(rs), rs[np.minimum(k, len(rs) - 1)] - tt, np.inf)
        return np.where(prev < nxt, prev, -nxt)
    dl = dist(t[lost]) * 1e6
    print(name, 'pp', len(pp), 'lost', len(lost), 'resets', len(rs), 'skip', r['_']['n_skip_blank'], r['_']['n_skip_close'], r['_']['n_skip_big'])
    print('  lost within [-3,+3] us of reset:', np.sum(np.abs(dl) < 3), ' hist(us):', np.histogram(dl, [-1e9, -20, -5, -2, -1, 0, 1, 2, 5, 20, 1e9])[0])
