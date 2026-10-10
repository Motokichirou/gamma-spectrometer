# -*- coding: utf-8 -*-
"""OPA818 TIA: сравнение с моделью стенда bench/tia.py (один полюс, e_n 4 нВ/√Гц, Cin 6 пФ)."""
import sys
import numpy as np
import rawread
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')

TD, Q, RF, KT4 = 230e-9, 7.2e-12, 5000.0, 4 * 1.380649e-23 * 300
CASES = [(0.8e-12, 125e6), (1.25e-12, 65e6)]   # Cf -> fs, при котором полоса ≈ fs/2.5

def bench_shape(t, f_tia):
    tt = 1 / (2 * np.pi * f_tia)
    tp = np.maximum(t, 0)
    return np.where(t > 0, (np.exp(-tp / TD) - np.exp(-tp / tt)) / (TD - tt), 0.0)

def bench_sigma(fs, cin=6e-12, en=4e-9):
    f_tia = fs / 2.5
    f = np.linspace(1e3, fs / 2, 200001)
    cf = 1 / (2 * np.pi * RF * f_tia); fz = 1 / (2 * np.pi * RF * (cin + cf))
    pole = np.abs(1 / (1 + 1j * f / f_tia)) ** 2
    psd = (KT4 * RF + en ** 2 * np.abs(1 + 1j * f / fz) ** 2) * pole
    return np.sqrt(np.trapezoid(psd, f))

def main():
    n, S = rawread.read('tran_noise.raw'); jo = [x.lower() for x in n].index('v(o2)')
    nn, SN = rawread.read('noise.raw'); jn = [x.lower() for x in nn].index('v(onoise)')
    print('Cf, пФ | fs | пик, мВ (стенд) | эквив. полюс, МГц (стенд) | ошибка формы, % пика | шум до fs/2, мкВ (стенд) | шум до 1 ГГц, мкВ')
    for k, (cf, fs) in enumerate(CASES):
        a = S[k]; t = a[:, 0] - 100e-9; v = a[:, jo] - a[0, jo]
        # стенд: Q·Rf на делителе 1k/(1k+22), один полюс
        g = RF * 1000 / 1022
        ft_best, e_best = None, 9
        for ft in np.linspace(10e6, 200e6, 1901):
            m = Q * g * bench_shape(t, ft)
            e = np.max(np.abs(m - v)) / v.max()
            if e < e_best: ft_best, e_best = ft, e
        vb = Q * g * bench_shape(t, fs / 2.5)
        b = SN[k]; f = np.real(b[:, 0]); d = np.real(b[:, jn])
        s_half = np.sqrt(np.trapezoid(d[f <= fs / 2] ** 2, f[f <= fs / 2]))
        s_all = np.sqrt(np.trapezoid(d ** 2, f))
        print('%.2f | %d | %.1f (%.1f) | %.1f (%.1f) | %.2f | %.1f (%.1f) | %.1f' % (
            cf * 1e12, fs / 1e6, v.max() * 1e3, vb.max() * 1e3, ft_best / 1e6, fs / 2.5e6,
            e_best * 100, s_half * 1e6, bench_sigma(fs) * 1e6, s_all * 1e6))
        np.savetxt('pulse_cf%.2fp.txt' % (cf * 1e12), np.c_[t, v], header='t_s v_out_V (OPA818, Cs 6p, Q 7.2pC)')


if __name__ == '__main__':
    main()
