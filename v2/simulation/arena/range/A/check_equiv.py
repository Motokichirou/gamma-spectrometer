# Проверка: копия csp_core.py при исходных параметрах (v_lo=0, без шума АЦП, без флагов арены)
# даёт бит в бит те же числа, что исходный v2/simulation/mc_csp.py на том же сиде.
import sys, os, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..'))   # v2/simulation
import mc_csp as O
sys.path.insert(0, HERE)
import csp_core as C
f_pp, cont = O.cs_spectrum()
for rate in (3e5, 1e6):
    O.rng = np.random.default_rng(20261009)
    ro = O.run(125e6, 50e6, rate, 4e-3, f_pp, cont)
    rc = C.run(125e6, 50e6, rate, 4e-3, f_pp, cont, seed=20261009, v_lo=0.0)
    print(rate, 'исходный:', ro['fit'], ro['twoavg']['fwhm'], ro['_']['resets'])
    print(rate, 'копия:   ', rc['fit'], rc['twoavg']['fwhm'], rc['_']['resets'])
