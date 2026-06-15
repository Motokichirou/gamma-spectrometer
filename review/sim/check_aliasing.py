# -*- coding: utf-8 -*-
"""check_aliasing.py — спектр импульса и восстанавливаемость пика при Fs=4МГц.
Отделяет «плохой алгоритм прошивки» от фундаментального недосэмплирования."""
import os, sys, numpy as np
sys.stdout.reconfigure(encoding='utf-8', errors='replace') if hasattr(sys.stdout,'reconfigure') else None
HERE=os.path.dirname(os.path.abspath(__file__)); import ltspice
L=ltspice.Ltspice(os.path.join(HERE,'sh_pulse.raw')); L.parse()
t=np.asarray(L.get_time()); v=np.asarray(L.get_data('V(adc_in)'))
bl=v[(t>=200e-9)&(t<=400e-9)].mean(); amp=bl-v

# равномерная сетка 1нс для спектра
tu=np.arange(0,12e-6,1e-9); au=np.interp(tu,t,amp)
A=np.abs(np.fft.rfft(au)); f=np.fft.rfftfreq(len(tu),1e-9)
def epow(fc):
    m=f<=fc; return (A[m]**2).sum()/(A**2).sum()
print("Доля спектральной энергии импульса ниже частоты:")
for fc in [0.5e6,1e6,1.5e6,2e6,3e6,5e6]:
    print(f"   < {fc/1e6:.1f} МГц : {epow(fc)*100:6.2f}%")
print(f"  Найквист при 4 Мвыб/с = 2.0 МГц. Энергия ВЫШЕ Найквиста (алиасинг): {(1-epow(2e6))*100:.2f}%")

# band-limited восстановление через FFT-передискретизацию (scipy.signal.resample)
from scipy.signal import resample
Ts=250e-9
Apk=amp.max()
def amp_at(x): return np.interp(x,t,amp)
errs=[]
for ph in np.linspace(0,Ts,40,endpoint=False):
    st=np.arange(ph,12e-6,Ts); sv=amp_at(st)   # выборки АЦП 250нс на окне 12мкс
    up=resample(sv, len(sv)*64)                  # band-limited FFT-интерполяция x64
    errs.append((Apk-up.max())/Apk)
errs=np.array(errs)
print(f"\nFFT band-limited восстановление пика из выборок 250нс (идеальный DPP):")
print(f"   RMS={errs.std()*100:.3f}%  смещение={errs.mean()*100:+.3f}%  макс|.|={np.abs(errs).max()*100:.3f}%")
print("Вывод: малый %% => 4 Мвыб/с достаточно, ошибка пик-детекта = дефект АЛГОРИТМА прошивки.")
