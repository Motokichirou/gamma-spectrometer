# -*- coding: utf-8 -*-
"""Контроль судьи: позиция A (рампа 1.55 В) и позиция B (CR τ, G) на ОДНОМ стенде стороны A
(refine=True, те же сиды, тот же шум АЦП, одна и та же обработка сбросов для всех)."""
import sys, os, argparse, numpy as np
from multiprocessing import Pool
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import csp_judge as C
if hasattr(sys.stdout,'reconfigure'): sys.stdout.reconfigure(encoding='utf-8')
def nz(vfs, snr): return vfs/(2*np.sqrt(2))*10**(-snr/20)
def job(a):
    name, p, seed, rate, ra, qr = a
    f_pp, cont = C.cs_spectrum()
    r = C.run(125e6, 50e6, rate, 4e-3, f_pp, cont, refine=True, seed=seed, reset_aware=ra, quiet_reset=qr, **p)
    return name, seed, r
if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('--seed0', type=int, default=20261009)
    ap.add_argument('--seeds', type=int, default=5); ap.add_argument('--rate', type=float, default=1e6)
    ap.add_argument('--snr', type=float, default=77); ap.add_argument('--ra', action='store_true'); ap.add_argument('--qr', action='store_true')
    ap.add_argument('--bits', type=int, default=16)
    a = ap.parse_args(); sn = nz(2.0, a.snr)
    V = {
     'A_ideal4.4': dict(v_ramp=4.0, v_fs=4.4, adc_noise=0.0),
     'Adiv_real':  dict(v_ramp=4.0, v_fs=2.0, k_div=1/2.2, v_lo=-0.05/2.2, adc_noise=sn, bits=a.bits),
     'A_ramp1.55': dict(v_ramp=1.55, v_fs=2.0, adc_noise=sn, bits=a.bits),
     'B_cr2_G2.5': dict(v_ramp=4.0, v_fs=2.0, v_lo=-0.25, adc_noise=sn, cr_tau=2e-6, cr_gain=2.5, bits=a.bits),
     'B_cr1_G4':   dict(v_ramp=4.0, v_fs=2.0, v_lo=-0.2, adc_noise=sn, cr_tau=1e-6, cr_gain=4.0, bits=a.bits),
    }
    seeds = [a.seed0 + k for k in range(a.seeds)]
    jobs = [(n, p, s, a.rate, a.ra, a.qr) for n, p in V.items() for s in seeds]
    with Pool(min(len(jobs), os.cpu_count() or 4)) as pool: res = pool.map(job, jobs)
    print('rate %.0e, SNR %g dBFS (%.0f мкВ на 2 В), bits %d, сиды %d..%d, ra=%s qr=%s' % (a.rate, a.snr, sn*1e6, a.bits, seeds[0], seeds[-1], a.ra, a.qr))
    pf = {}
    for n in V:
        rr = [r for (m, s, r) in res if m == n]
        if any(r is None for r in rr): print(n, 'детектор пуст'); continue
        fw = np.array([r['fit']['fwhm'] for r in rr]); ef = np.array([100*r['fit']['eff'] for r in rr])
        pf[n] = fw
        print('%-12s FWHM %6.2f ± %4.2f  свет %5.1f ± %3.1f  сбросы %5.1f  мёртв %.2f%%  clip %d' % (n, fw.mean(), fw.std(ddof=1)/np.sqrt(len(fw)), ef.mean(), ef.std(ddof=1)/np.sqrt(len(ef)),
              np.mean([r['_']['resets'] for r in rr]), np.mean([r['_']['deadtime_pct'] for r in rr]), sum(r['_']['n_clip'] for r in rr)))
    if 'A_ramp1.55' in pf and 'B_cr2_G2.5' in pf:
        d = pf['B_cr2_G2.5'] - pf['A_ramp1.55']; print('B_cr2 − A_ramp парно: %+.2f ± %.2f п.п.' % (d.mean(), d.std(ddof=1)/np.sqrt(len(d))))
