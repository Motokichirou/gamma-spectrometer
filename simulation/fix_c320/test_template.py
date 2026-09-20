# -*- coding: utf-8 -*-
"""
fix_c320/test_template.py — проверка подгонки по эталону формы в НАСТОЯЩЕМ firmware/core/dsp.c.

Одиночные импульсы со случайной суб-отсчётной фазой подаются в детектор через DLL
(build_dsp.cmd) и сравниваются с истинным пиком аналогового импульса. Проверяется:
  1) смещение и разброс без шума (парабола против эталона);
  2) линейность по амплитуде от порога до верха шкалы;
  3) вклад шума (σ = 94 мкВ по sim3) в разброс амплитуды.

Запуск: python simulation/fix_c320/test_template.py
"""
import ctypes
import os
import subprocess
import sys

import numpy as np

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')

FS = 4e6
T = 1 / FS
VREF = 2.5
LSB = VREF / 4096
T_PULSE = 200e-6
FINE = 1e-9
BASE = 0.41                      # рабочая точка АЦП, В (DC шейпера)
NOISE_V = 94.4e-6                # σ шума тракта (sim3)
rng = np.random.default_rng(20260921)

u16p = ctypes.POINTER(ctypes.c_uint16)


def build():
    r = subprocess.run([os.path.join(HERE, 'build_dsp.cmd')], capture_output=True, shell=True)
    r.stdout, r.stderr = (x.decode('cp866', 'replace') for x in (r.stdout, r.stderr))
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-2000:])
        raise SystemExit('сборка DLL не удалась')


def load_dll():
    dll = ctypes.CDLL(os.path.join(OUT, 'dsp_shim.dll'))
    dll.shim_init.argtypes = [ctypes.c_int] * 4
    dll.shim_process.argtypes = [u16p, ctypes.c_size_t, u16p, ctypes.c_size_t]
    dll.shim_process.restype = ctypes.c_size_t
    dll.shim_set_template.argtypes = [ctypes.c_int]      # 1 = эталон, 0 = парабола
    dll.shim_set_template.restype = ctypes.c_int
    return dll


VPK_CS = 0.0         # пик Cs-137 на входе АЦП, В (из ядра LTspice)


def shape(name='V2'):
    """Форма (нормирована на пик 1.0) и масштаб: сколько вольт на входе АЦП даёт Cs-137.

    ⚠ 469 мВ из CLAUDE.md — это выход TIA ДО шейпера; на входе АЦП после шейпера ~354 мВ,
    поэтому запас по шкале — до ~3.9 МэВ, а не до 2.95 МэВ."""
    global VPK_CS
    z = np.load(os.path.join(OUT, f'{name}_tran.npz'))
    t, v = z['t'], z['adc_in'].astype(float)
    dc = float(np.interp(0.75 * T_PULSE, t, v))
    tt, h = t - T_PULSE, v - dc
    VPK_CS = float(h.max())
    g = np.arange(-2e-6, 40e-6, FINE)
    return g, np.interp(g, tt, h) / VPK_CS


def volts(e_kev):
    """Энергия -> амплитуда на входе АЦП (линейный тракт)."""
    return VPK_CS * e_kev / 662.0


KPRE = 4          # отсчётов до вершины в сгенерированном импульсе


def stream(g, s, amps_v, phases, gap=64, noise_v=0.0):
    """Поток отсчётов АЦП: импульсы с заданной амплитудой (В) и суб-отсчётной фазой.

    Фаза ph — сдвиг сетки АЦП относительно импульса: вершина попадает между отсчётами."""
    n = gap * (len(amps_v) + 2)
    sig = np.full(n, BASE)
    kk = np.arange(0, 48)
    t0 = g[int(np.argmax(s))]                      # положение вершины на мелкой сетке
    for i, (a, ph) in enumerate(zip(amps_v, phases)):
        k0 = gap * (i + 1)
        y = a * np.interp((kk - KPRE - ph) * T + t0, g, s)
        m = min(len(y), n - k0)
        sig[k0:k0 + m] += y[:m]
    if noise_v:
        sig += rng.normal(0, noise_v, n)
    return np.clip(np.rint(sig / VREF * 4096), 0, 4095).astype(np.uint16)


def run(dll, codes, use_tpl, thr=25, minw=2, maxw=250, fitm=2):
    dll.shim_init(int(thr), minw, maxw, fitm)
    dll.shim_set_template(1 if use_tpl else 0)
    out = np.zeros(len(codes) // 8 + 64, np.uint16)
    n = dll.shim_process(codes.ctypes.data_as(u16p), len(codes),
                         out.ctypes.data_as(u16p), len(out))
    return out[:n].astype(float) / 2.0        # половины LSB -> коды АЦП


def case(dll, g, s, amp_v, npulse, noise_v=0.0, use_tpl=True):
    ph = rng.random(npulse)                    # фаза импульса относительно сетки АЦП
    a = np.full(npulse, amp_v)
    codes = stream(g, s, a, ph, noise_v=noise_v)
    est = run(dll, codes, use_tpl)
    true_codes = amp_v / LSB
    return est, true_codes, len(est)


def main():
    build()
    dll = load_dll()
    g, s = shape()
    print('пик Cs-137 на входе АЦП: %.1f мВ (%.0f кан.);  1 код АЦП = %.0f мкВ\n'
          % (VPK_CS * 1e3, VPK_CS / VREF * 8192, LSB * 1e6))

    print('1) одиночные импульсы, случайная фаза, без шума (500 импульсов):')
    for tag, use in (('парабола (5 точек)', False), ('эталон формы', True)):
        est, tc, n = case(dll, g, s, volts(662), 500, use_tpl=use)
        print(f'   {tag:20s}: найдено {n}/500, среднее {est.mean():7.1f} код. '
              f'(истина {tc:.1f}) смещение {est.mean() / tc * 100 - 100:+6.2f} %, '
              f'σ {est.std() / tc * 100:5.2f} %, размах {(est.max() - est.min()) / tc * 100:5.2f} %')

    head = VREF - BASE
    print(f'\n2) линейность по эталону, без шума (запас АЦП {head:.2f} В над базой {BASE} В, '
          f'потолок ~{head / VPK_CS * 0.662:.2f} МэВ):')
    for e_kev in (59.5, 662, 1330, 2600, 3500, 4500):
        amp = volts(e_kev)
        est, tc, n = case(dll, g, s, amp, 200)
        note = '  <-- выше потолка АЦП, клиппинг' if amp > VREF - BASE else ''
        print(f'   {e_kev:>6.1f} кэВ ({amp:.3f} В): найдено {n}/200, смещение '
              f'{est.mean() / tc * 100 - 100:+6.2f} %, σ {est.std() / tc * 100:5.2f} %{note}')

    print('\n3) с шумом σ = 94.4 мкВ (0.15 кода):')
    for tag, use in (('парабола', False), ('эталон', True)):
        est, tc, n = case(dll, g, s, volts(662), 500, noise_v=NOISE_V, use_tpl=use)
        print(f'   {tag:10s}: смещение {est.mean() / tc * 100 - 100:+6.2f} %, '
              f'σ {est.std() / tc * 100:5.2f} % ({est.std():.2f} код.)')

    # критерии
    est, tc, n = case(dll, g, s, volts(662), 500)
    bias, sd = abs(est.mean() / tc - 1) * 100, est.std() / tc * 100
    print(f'\nкритерий: |смещение| < 1 % и σ < 0.5 % без шума → {bias:.2f} %, {sd:.2f} %',
          'OK' if (bias < 1.0 and sd < 0.5) else 'ПРОВАЛ')
    return 0 if (bias < 1.0 and sd < 0.5) else 1


if __name__ == '__main__':
    raise SystemExit(main())
