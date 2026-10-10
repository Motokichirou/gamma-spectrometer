# -*- coding: utf-8 -*-
import sys
import numpy as np
import rawread
from analyze import bench_sigma, bench_shape, Q, RF
sys.stdout.reconfigure(encoding='utf-8')
CASES = [(0.5, 50, 125), (0.8, 50, 125), (0.8, 26, 65), (1.25, 26, 65)]
n, S = rawread.read('filt.raw'); jo = [x.lower() for x in n].index('v(o2)')
nn, SN = rawread.read('filt_noise.raw'); jn = [x.lower() for x in nn].index('v(onoise)')
print('Cf пФ | RC МГц | fs | пик мВ | σ до 1 ГГц мкВ | пик/σ | стенд: пик мВ, σ мкВ, пик/σ')
for k, (cf, frc, fs) in enumerate(CASES):
    a = S[k]; v = a[:, jo] - a[0, jo]
    b = SN[k]; f = np.real(b[:, 0]); d = np.real(b[:, jn])
    s = np.sqrt(np.trapezoid(d ** 2, f))
    vb = Q * RF * 1000 / 1100 * bench_shape(a[:, 0] - 100e-9, fs * 1e6 / 2.5).max(); sb = bench_sigma(fs * 1e6)
    # полоса по шумовой АЧХ нельзя; считаем по AC отдельно не будем — оценим по переходному: эквивалент
    print('%.2f | %d | %d | %.1f | %.1f | %.0f | %.1f, %.1f, %.0f' % (
        cf, frc, fs, v.max() * 1e3, s * 1e6, v.max() / s, vb * 1e3, sb * 1e6, vb / sb))
    np.savetxt('pulse_k%d.txt' % (k + 1), np.c_[a[:, 0] - 100e-9, v], header='t_s v_V Cf=%.2fp RC=%dMHz' % (cf, frc))
