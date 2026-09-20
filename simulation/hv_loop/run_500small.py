"""Перемер петли при 500 В с инжекцией 30 мкВ (было 100 мкВ — COMP модулировался ~30 %)."""
import numpy as np
import run_fra
from spice_io import run
run_fra.AMP = 3e-5
run_fra.case_name = lambda v, f: f'fra_{v}_{int(f)}_small'
fs = [2000, 5000]
run(run_fra.plan(500, fs), par=2)
for f in fs:
    T = run_fra.measure(500, f)
    print(f'500 В {f} Гц (инж. 30 мкВ): |T| {20*np.log10(abs(T)):6.1f} дБ  фаза {np.degrees(np.angle(T)):7.1f}°', flush=True)
