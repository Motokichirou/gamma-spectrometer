"""Усреднённая модель петли MAX1847 (для сверки с замером на переключательной модели, run_fra.py).

T(s) = B(s) · gm·Zc(s) · Gi · Zout(s)
  B     — делитель FB: 1 ГОм ‖ Cff  /  2.2 МОм ‖ Cfb (HV_CONTROL — земля по переменному току)
  gm·Zc — 400 мкА/В на Ro=3 МОм ‖ (R201 + 1/sC201)
  Gi    — dI_cw/dV_comp: энергия за цикл ∝ Ipk², Ipk = (Vcomp−Voff)/(3.3·(Rcs + Ms·Lp/Vin))
          ⇒ Gi = 2·I_cw/Ipk · dIpk/dVcomp
  Zout  — C13+C14 ‖ цепочка RC-фильтров ‖ нагрузка; источник постоянной мощности добавляет
          проводимость I/V (эффективно R_L/2).
"""
import numpy as np

gm, Ro, Acs, Rcs, Ms = 400e-6, 3e6, 3.3, 0.1, 41e3


def loop(f, Icw, Vcw, Ipk, Lp=5.6e-6, Vin=5.0, Cd=35e-9, Cf=35e-9, R201=1e6, C201=33e-9,
         Rtop=1e9, Rbot=2.2e6, Cff=10e-15, Cfb=5e-12, Cc2=0.0):
    s = 2j * np.pi * np.asarray(f, float)
    Zt = 1 / (1 / Rtop + s * Cff)
    Zb = 1 / (1 / Rbot + s * Cfb)
    B = Zb / (Zb + Zt)
    Zc = 1 / (1 / Ro + 1 / (R201 + 1 / (s * C201)) + s * Cc2)
    dIpk = 1 / (Acs * (Rcs + Ms * Lp / Vin))
    Gi = 2 * Icw / Ipk * dIpk
    Rl = 1 / (1 / 7.73e6 + 1 / 68.11e6)
    Zc_ = lambda C: 1 / (s * C)
    Z = Rl
    for _ in range(3):
        Z = 100e3 + 1 / (1 / Zc_(Cf) + 1 / Z)
    Y = 2 * s * Cd + 1 / Z + 1 / Rtop + Icw / abs(Vcw)
    return B * gm * Zc * Gi / Y


def margins(f, T):
    mag = np.abs(T)
    i = np.where((mag[:-1] >= 1) & (mag[1:] < 1))[0]
    if not len(i):
        return None, None
    i = i[0]
    # лог-интерполяция
    a = np.log(mag[i]) / (np.log(mag[i]) - np.log(mag[i + 1]))
    fc = f[i] * (f[i + 1] / f[i]) ** a
    ph = np.degrees(np.angle(T[i])) + a * (np.degrees(np.angle(T[i + 1])) - np.degrees(np.angle(T[i])))
    return fc, 180 + ph


# рабочие точки из переключательной модели (run_ripple.py): I_cw, Vcs пик
OP = {500: (72.3e-6, -500, 0.214), 800: (109.6e-6, -800, 0.370),
      1200: (162.0e-6, -1200, 0.601), 1500: (205.8e-6, -1500, 0.835)}

if __name__ == '__main__':
    f = np.logspace(-1, 5, 2000)
    for v, (I, V, Ipk) in OP.items():
        T = loop(f, I, V, Ipk)
        fc, pm = margins(f, T)
        dc = 20 * np.log10(abs(loop([0.01], I, V, Ipk)[0]))
        print(f'{v} В: T(0.01 Гц) {dc:.0f} дБ, срез {fc:.0f} Гц, запас по фазе {pm:.0f}°')
        for ff in (50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000):
            t = loop([ff], I, V, Ipk)[0]
            print(f'    {ff:6d} Гц: {20*np.log10(abs(t)):6.1f} дБ {np.degrees(np.angle(t)):7.1f}°')
