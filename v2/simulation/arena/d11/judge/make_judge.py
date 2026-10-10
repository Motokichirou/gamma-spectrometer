# -*- coding: utf-8 -*-
"""Судья D11: общий стенд. Берёт mc_csp_b.py (B) и добавляет VP+ЛМ стороны A
(функции _vp_fit/_sep_ok/_vp_lm скопированы из A/mc_csp_vp.py без изменений).
Все варианты: одна запись, один набор кластеров, сопоставление по t_det (и по своему моменту)."""
import os, re
H = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(H, '..', 'B', 'mc_csp_b.py'), encoding='utf-8').read()
a = open(os.path.join(H, '..', 'A', 'mc_csp_vp.py'), encoding='utf-8').read()
i0 = a.index('def _vp_fit'); i1 = a.index('def _grid_sweep')
vp_code = a[i0:i1]

def rep(s, old, new):
    assert s.count(old) == 1, old
    return s.replace(old, new)

src = rep(src, 'solve_cluster.accepted = 0\n\n\n', 'solve_cluster.accepted = 0\n\n\n' + vp_code + '''
def solve_vp(seg, tl, tq, ta):
    T = tl[1] - tl[0]
    th, Qm, rn = _vp_fit(seg, tl, tq, ta)
    if th is None:
        return None
    th, tq, Qm, rn = _vp_lm(seg, tl, tq, ta, T, th, Qm, rn)
    return th[:-1], tq, rn


''')
src = rep(src, "VARIANTS = ['det', 'lm', 'lmfix', 'B_const', 'B_lut', 'B_tm', 'B_tm+lm', 'oracle']",
          "VARIANTS = ['det', 'lmfix', 'lmfix20', 'B_const', 'B_tm', 'B_tm+lm20', 'vp', 'off_vp', 'B_tm+vp', 'oracle']")
src = rep(src, "'B_const': (t_const, False, -1.0), 'B_tm+lm': (t_tm, True, +1.0)}",
          "'B_const': (t_const, False, -1.0), 'B_tm+lm': (t_tm, True, +1.0),\n"
          "            'lmfix20': (t_det, True, +1.0), 'B_tm+lm20': (t_tm, True, +1.0),\n"
          "            'vp': (t_det, 'vp', 0), 'off_vp': (t_const, 'vp', 0), 'B_tm+vp': (t_tm, 'vp', 0)}")
src = rep(src, "            res = solve_cluster(seg, tl, tq, ta, refine=refine, sign=sign, maxit=LM_MAXIT)",
          "            if refine == 'vp':\n"
          "                res = solve_vp(seg, tl, tq, ta)\n"
          "            else:\n"
          "                res = solve_cluster(seg, tl, tq, ta, refine=refine, sign=sign,\n"
          "                                    maxit=(20 if v.endswith('20') else LM_MAXIT))")
src = rep(src, "    name = 'mc_csp_b%s", "    name = 'judge_%s' % '_'.join(sys.argv[1:]).replace('-', '') + '.json'\n    _unused = 'mc_csp_b%s")
src = src.replace("os.path.join(ROOT, 'v2', 'simulation', 'arena', 'd11', 'B', 'out')", "os.path.join(HERE, 'out')")
src = src.replace("'%5.2f%% %+5.1f±%4.2f' % (r[v]['fwhm'], r[v]['dt_bias_ns'], r[v]['dt_sig_ns'])",
                  "'%5.2f/%5.2f' % (r[v]['fwhm'], r[v]['fwhm_own'])")
open(os.path.join(H, 'mc_csp_judge.py'), 'w', encoding='utf-8').write(src)
print('ok')
