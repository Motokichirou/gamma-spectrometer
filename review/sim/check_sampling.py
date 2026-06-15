# -*- coding: utf-8 -*-
"""check_sampling.py — верификация формы импульса и ошибки семплирования."""
import os, sys, numpy as np
sys.stdout.reconfigure(encoding='utf-8', errors='replace') if hasattr(sys.stdout,'reconfigure') else None
HERE=os.path.dirname(os.path.abspath(__file__))
import ltspice
from scipy.interpolate import CubicSpline

L=ltspice.Ltspice(os.path.join(HERE,'sh_pulse.raw')); L.parse()
t=np.asarray(L.get_time()); v=np.asarray(L.get_data('V(adc_in)'))
bl=v[(t>=200e-9)&(t<=400e-9)].mean()
amp=bl-v
ipk=np.argmax(amp); Apk=amp[ipk]; tpk=t[ipk]
print(f"baseline={bl:.4f}В  пик amp={Apk*1e3:.1f}мВ при t={tpk*1e6:.3f}мкс (старт 0.5мкс)")
print("\nФорма amp(t) (мВ, + = вниз от базовой):")
for tt in [0.5,0.7,0.86,1.0,1.2,1.5,2.0,2.5,3.0,4.0,6.0,10.0,15.0]:
    a=np.interp(tt*1e-6,t,amp)
    print(f"   t={tt:5.1f}мкс  amp={a*1e3:+7.2f}мВ  ({a/Apk*100:+6.1f}% пика)")
# макс. выброс выше базовой (undershoot) после пика
post=(t>tpk)&(t<tpk+8e-6)
mino=amp[post].min()
tmin=t[post][np.argmin(amp[post])]
print(f"\nМакс undershoot (ниже базовой) = {mino/Apk*100:.1f}% пика при t={tmin*1e6:.2f}мкс")

# ---- ошибка семплирования: bare / parabola / cubic-spline reconstruction ----
Ts=250e-9
def amp_at(x): return np.interp(x,t,amp)
ph=np.linspace(0,Ts,80,endpoint=False)
e_bare=[]; e_par=[]; e_spl=[]
for p in ph:
    st=np.arange(t[0]+p,t[-1]-Ts,Ts)
    sv=amp_at(st)
    j=int(np.argmax(sv))
    e_bare.append((Apk-sv[j])/Apk)
    if 0<j<len(sv)-1:
        y0,y1,y2=sv[j-1],sv[j],sv[j+1]; den=y0-2*y1+y2
        yp=y1-(y0-y2)**2/(8*den) if den!=0 else y1
        e_par.append((Apk-yp)/Apk)
        # кубический сплайн по ~7 точкам вокруг пика, поиск максимума
        lo=max(0,j-3); hi=min(len(sv),j+4)
        cs=CubicSpline(st[lo:hi],sv[lo:hi])
        xx=np.linspace(st[lo],st[hi-1],400); yy=cs(xx)
        e_spl.append((Apk-yy.max())/Apk)
for nm,arr in [("голый max-сэмпл",e_bare),("парабола 3 точки",e_par),("кубич.сплайн 7 точек",e_spl)]:
    a=np.array(arr)
    print(f"  {nm:<22} RMS={a.std()*100:5.2f}%  смещение={a.mean()*100:+5.2f}%  макс|.|={np.abs(a).max()*100:5.2f}%")
