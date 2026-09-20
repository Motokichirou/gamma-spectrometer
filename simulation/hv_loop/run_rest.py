import time
import numpy as np
from run_fra import plan, measure
from spice_io import run
sets = {1200: [20000], 500: [500, 2000, 5000, 20000], 1500: [500, 2000, 5000, 20000]}
paths = sum((plan(v, fs) for v, fs in sets.items()), [])
t = time.time(); run(paths, par=8); print('FRA LTspice %.0f с' % (time.time() - t), flush=True)
for v, fs in sets.items():
    for f in fs:
        T = measure(v, f)
        print(f'{v} В {f:6d} Гц: |T| {20*np.log10(abs(T)):6.1f} дБ  фаза {np.degrees(np.angle(T)):7.1f}°', flush=True)
