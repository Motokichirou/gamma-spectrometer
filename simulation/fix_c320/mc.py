# -*- coding: utf-8 -*-
"""
fix_c320/mc.py — Монте-Карло: пуассоновский поток импульсов -> шум с реальным спектром
-> АЦП 12 бит @4 Мвыб/с (VREF 2.5 В) -> НАСТОЯЩИЙ firmware/core/dsp.c (out/dsp_shim.dll).

Импульсный отклик каждого варианта — из LTspice (run_spice.py, 30 мс, видны хвосты
разделительных цепей). Система линейна (модель ОУ без клиппинга), поэтому поток =
суперпозиция откликов: основная часть (40 мкс) — точная интерполяция с дробной фазой,
хвост — свёртка с сэмплированным ядром. Шум — гауссов с PSD из шумовой деки варианта.

Спектр амплитуд — inverse-CDF Cs-137 из firmware/core/pulsegen.c: фотопик заменён дельтой
(амплитуда = отклик на 662 кэВ), континуум (<0.85 фотопика) — из таблицы. Метрики по окну
фотопика ±10 %: среднее и σ с итеративным отсечением 3σ (электронное уширение, без
статистики сцинтиллятора).

--phase-lock: импульсы приходят ровно на отсчёт АЦП. Смещение параболического фита становится
постоянным, фазовый разброс уходит — остаются шум, хвосты разделительных цепей и наложения.
Без флага (случайная фаза) результат доминирует фазовый разброс фита по 5 точкам (~3 % σ).

Параметры детектора под импульс боевой платы (FWHM ~2.3 сэмпла @4 Мвыб/с): min_width=2,
fit_m=2; порог 25 кодов при полном усилении (V1), для других вариантов — в той же энергии.

Запуск (после run_spice.py и build_dsp.cmd):  python simulation/fix_c320/mc.py
"""
import os, re, sys, json, ctypes, time
import numpy as np
from scipy.signal import fftconvolve

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))

FS = 4e6
T = 1 / FS
VREF = 2.5
CH_PER_V = 8192 / VREF
T_PULSE = 200e-6
FINE = 10e-9
MAIN_N = 160            # сэмплов точной части отклика (40 мкс)
CHUNK = 1 << 22
THR_REF, MINW, MAXW, FITM = 25, 2, 250, 2
VARS = ['V0', 'V1', 'V2', 'V3', 'V4']
RATES = [(1000, 15000), (10000, 40000), (30000, 60000)]

PHASE_LOCK = '--phase-lock' in sys.argv   # импульсы ровно на отсчёт: убирает фазовый разброс фита
rng = np.random.default_rng(20260915)

u16p = ctypes.POINTER(ctypes.c_uint16)
dll = ctypes.CDLL(os.path.join(OUT, 'dsp_shim.dll'))
dll.shim_init.argtypes = [ctypes.c_int] * 4
dll.shim_init.restype = None
dll.shim_process.argtypes = [u16p, ctypes.c_size_t, u16p, ctypes.c_size_t]
dll.shim_process.restype = ctypes.c_size_t
dll.shim_stats.argtypes = [ctypes.POINTER(ctypes.c_uint)] * 3
dll.shim_stats.restype = None


def load_kernel(name):
    z = np.load(os.path.join(OUT, f'{name}_tran.npz'))
    t, v = z['t'], z['adc_in'].astype(float)
    dc = float(np.interp(0.75 * T_PULSE, t, v))
    tt, h = t - T_PULSE, v - dc
    vpk = float(h.max())
    h = h / vpk                      # ядро нормировано на пик: амплитуда задаётся в simulate()
    hf = np.interp(np.arange(0, (MAIN_N + 2) * T, FINE), tt, h)
    m = np.arange(MAIN_N, int((t[-1] - T_PULSE) / T) - 1)
    ker = np.concatenate([np.zeros(MAIN_N), np.interp((m + 0.5) * T, tt, h)])
    return dict(dc=dc, hf=hf, ker=ker, vpk=vpk)


def load_psd(name):
    z = np.load(os.path.join(OUT, f'{name}_noise.npz'))
    return z['f'], z['onoise'] ** 2


def cs_spectrum():
    src = open(os.path.join(ROOT, 'firmware', 'core', 'pulsegen.c'), encoding='utf-8').read()
    body = re.search(r'invcdf\[INVCDF_LEN\]\s*=\s*\{(.*?)\};', src, re.S).group(1)
    body = re.sub(r'/\*.*?\*/|//[^\n]*', '', body, flags=re.S)
    vals = np.array([int(x) for x in re.findall(r'\d+', body)], float)
    assert len(vals) == 1024, len(vals)
    # фотопик = максимум плотности узлов (бины 10 кодов) выше половины 95-го перцентиля;
    # от максимума таблицы отсчитывать нельзя — последний узел одиночный (4072 при пике ~880)
    lo = 0.5 * np.percentile(vals, 95)
    edges = np.arange(lo, vals.max() + 10, 10)
    hist, _ = np.histogram(vals, edges)
    j = int(np.argmax(hist))
    sel = vals[(vals >= edges[j] - 20) & (vals < edges[j + 1] + 20)]
    P = float(np.median(sel))
    rel = vals / P
    return P, int(vals.max()), float(np.mean(np.abs(rel - 1) < 0.1)), rel[rel < 0.85]


def amplitudes(n, f_pp, cont):
    a = np.ones(n)
    k = rng.random(n) >= f_pp
    a[k] = np.interp(rng.random(k.sum()) * (len(cont) - 1), np.arange(len(cont)), cont)
    return a, ~k


def colored_noise(N, fpsd, spsd):
    X = np.fft.rfft(rng.standard_normal(N))
    f = np.fft.rfftfreq(N, T)
    S = np.interp(np.log10(np.maximum(f, fpsd[0])), np.log10(fpsd), spsd)
    X *= np.sqrt(S * FS / 2)
    return np.fft.irfft(X, N)


def simulate(K, psd, rate, npl, amps, thr):
    p = (np.cumsum(rng.exponential(1 / rate, npl)) + 10e-3) / T
    if PHASE_LOCK:
        p = np.round(p) + 1e-9
    A = amps * K['vpk']
    hf, ker = K['hf'], K['ker']
    M = len(ker)
    nsamp = int(p[-1]) + MAIN_N + 4
    dll.shim_init(int(thr), MINW, MAXW, FITM)
    out = np.zeros(npl + 1024, np.uint16)
    nout = 0
    offs = np.arange(MAIN_N)
    fidx = np.arange(len(hf))
    for n0 in range(0, nsamp, CHUNK):
        N = min(CHUNK, nsamp - n0)
        sig = np.full(N, K['dc'])
        i0, i1 = np.searchsorted(p, n0 - MAIN_N), np.searchsorted(p, n0 + N)
        if i1 > i0:
            ps = p[i0:i1]
            KK = np.ceil(ps).astype(np.int64)[:, None] + offs[None, :]
            vals = (A[i0:i1, None] * np.interp((KK - ps[:, None]) * (T / FINE), fidx, hf)).ravel()
            loc = (KK - n0).ravel()
            ok = (loc >= 0) & (loc < N)
            sig += np.bincount(loc[ok], weights=vals[ok], minlength=N)[:N]
        j0 = np.searchsorted(p, n0 - M)
        if i1 > j0:
            idx = np.ceil(p[j0:i1]).astype(np.int64) - (n0 - M)
            ok = (idx >= 0) & (idx < M + N)
            imp = np.bincount(idx[ok], weights=A[j0:i1][ok], minlength=M + N)[:M + N]
            sig += fftconvolve(imp, ker)[M:M + N]
        if psd is not None:
            sig += colored_noise(N, *psd)
        codes = np.clip(np.rint(sig / VREF * 4096), 0, 4095).astype(np.uint16)
        cap = len(out) - nout
        n = dll.shim_process(codes.ctypes.data_as(u16p), N, out[nout:].ctypes.data_as(u16p), cap)
        nout += min(n, cap)
    acc, rej, bl = ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint()
    dll.shim_stats(ctypes.byref(acc), ctypes.byref(rej), ctypes.byref(bl))
    return out[:nout].astype(float), acc.value, rej.value


def peak_stats(ch, ref):
    w = ch[(ch > 0.9 * ref) & (ch < 1.1 * ref)]
    n = len(w)
    mu, sd = float(np.mean(w)), float(np.std(w))
    for _ in range(10):                   # итеративное отсечение 3σ (наложения/континуум)
        if sd == 0.0:                     # все значения совпали (фаза привязана, шум мал)
            break
        w = w[np.abs(w - mu) <= 3 * sd]
        mu, sd = float(np.mean(w)), float(np.std(w))
    return mu, sd, n


def main():
    P, vmax, f_pp, cont = cs_spectrum()
    print('Режим:', 'фаза привязана к отсчёту' if PHASE_LOCK else 'случайная фаза')
    print(f'Спектр pulsegen.c: фотопик = код {P}, максимум {vmax} (×{vmax / P:.2f}), '
          f'доля фотопика {f_pp * 100:.1f} %')
    kern = {n: load_kernel(n) for n in VARS}
    res = {}
    for name in VARS:
        K = kern[name]
        fpsd, spsd = load_psd(name)
        s_int = np.sqrt(np.trapezoid(spsd, fpsd))
        chk = colored_noise(1 << 20, fpsd, spsd).std()
        thr = max(1, round(THR_REF * K['vpk'] / kern['V1']['vpk']))
        t0 = time.time()
        ch0, _, _ = simulate(K, None, 100.0, 400, np.ones(400), thr)
        ref = float(np.median(ch0))
        r = dict(thr=thr, ch_analog=K['vpk'] * CH_PER_V, ch_clean=ref,
                 noise_uV=s_int * 1e6, noise_gen_uV=chk * 1e6)
        print(f'\n{name}: порог {thr} код., аналоговый пик {r["ch_analog"]:.1f} кан., '
              f'DSP без шума {ref:.1f} кан., шум {s_int * 1e6:.1f} мкВ (генератор {chk * 1e6:.1f})')
        for rate, npl in RATES:
            a, is_pp = amplitudes(npl, f_pp, cont)
            ch, acc, rej = simulate(K, (fpsd, spsd), rate, npl, a, thr)
            med, sig, n = peak_stats(ch, ref)
            r[str(rate)] = dict(median=med, shift_pct=(med / ref - 1) * 100, sigma_ch=sig,
                                fwhm_el_pct=2.3548 * sig / ref * 100, n_peak=n,
                                eff_pct=n / is_pp.sum() * 100, acc=acc, rej=rej)
            q = r[str(rate)]
            print(f'  {rate / 1e3:>4.0f} кимп/с: медиана {med:7.1f} ({q["shift_pct"]:+.2f} %), '
                  f'σ {sig:.2f} кан., FWHM_эл {q["fwhm_el_pct"]:.2f} %, '
                  f'в пике {q["eff_pct"]:.0f} % ({time.time() - t0:.0f} с)')
        res[name] = r
    with open(os.path.join(OUT, 'mc_results_phaselock.json' if PHASE_LOCK else 'mc_results.json'), 'w', encoding='utf-8') as f:
        json.dump(res, f, ensure_ascii=False, indent=1)

    print('\n| Вар | канал Cs (DSP) | порог | ' +
          ' | '.join(f'{r // 1000}к: сдвиг / FWHM_эл / в пике' for r, _ in RATES) + ' |')
    print('|' + '---|' * (3 + len(RATES)))
    for n, r in res.items():
        cells = [f"{r[str(k)]['shift_pct']:+.2f} % / {r[str(k)]['fwhm_el_pct']:.2f} % / "
                 f"{r[str(k)]['eff_pct']:.0f} %" for k, _ in RATES]
        print(f"| {n} | {r['ch_clean']:.0f} | {r['thr']} | " + ' | '.join(cells) + ' |')


if __name__ == '__main__':
    main()
