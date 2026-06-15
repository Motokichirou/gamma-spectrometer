# -*- coding: utf-8 -*-
"""
detector_calcs.py — независимая перепроверка чисел проекта (REVIEW).
Считает: физику детектора, токи/мощность делителя, ПОЛНОЕ усиление ФЭУ под
нагрузкой (активный vs пассивный делитель — из .meas логов LTspice),
pile-up при 30/300 кимп/с, баллистический дефицит, ошибку семплирования АЦП.
Запуск:  python review/sim/detector_calcs.py
"""
import os, re, sys, math
import numpy as np

sys.stdout.reconfigure(encoding='utf-8', errors='replace') if hasattr(sys.stdout,'reconfigure') else None
HERE = os.path.dirname(os.path.abspath(__file__))

def hdr(t): print("\n"+"="*70+"\n  "+t+"\n"+"="*70)

# ---------------------------------------------------------------------------
def parse_meas(logpath):
    """Парсит stepped .meas таблицы LTspice -> {meas_name: {step:int -> value:float}}."""
    out = {}
    if not os.path.exists(logpath): return out
    txt = open(logpath, 'r', errors='replace').read()
    blocks = re.split(r'Measurement:\s*', txt)
    for b in blocks[1:]:
        name = b.splitlines()[0].strip()
        d = {}
        for line in b.splitlines()[1:]:
            m = re.match(r'\s*(\d+)\s+([-\d.eE+]+)', line)
            if m:
                d[int(m.group(1))] = float(m.group(2))
        # also single-value form "name: expr=val at t"
        if not d:
            m = re.search(r'=\s*([-\d.eE+]+)', b)
            if m: d[1] = float(m.group(1))
        out[name] = d
    return out

# ===========================================================================
hdr("1. ФИЗИКА ДЕТЕКТОРА — самосогласованность с симуляцией")
e = 1.602e-19
Y = 38.0            # фотонов/кэВ (NaI(Tl), типично 38..43)
G = 2.7e5           # усиление ФЭУ (из CLAUDE.md)
tau_s = 230e-9      # время высвечивания NaI (быстрая компонента)
# калибруем эффективную QE*сбор так, чтобы совпасть с I_peak(Cs)=0.94мА из sim2
Qcs_target = 0.94e-3 * tau_s     # заряд из I_peak*tau
Npe_cs = Qcs_target/(G*e)
QEeff = Npe_cs/(662*Y)
print(f"  Назад из I_peak(Cs)=0.94мА:  Q={Qcs_target*1e9:.3f} нКл, Npe={Npe_cs:.0f}, QE*сбор={QEeff:.3f}")
print(f"  (самосогласовано: QE*сбор≈0.20 — реалистично для бищелочного фотокатода)\n")

energies = {'Am-241':59.5,'Cs-137':662,'Co-60':1330,'Tl-208':2614,'потолок':3500}
print(f"  {'Источник':<10}{'E,кэВ':>8}{'Npe':>8}{'Q,нКл':>9}{'Ipeak,мА':>10}")
charges = {}
for nm,E in energies.items():
    Npe = E*Y*QEeff
    Q = Npe*G*e
    Ipk = Q/tau_s
    charges[nm] = Q
    print(f"  {nm:<10}{E:>8.0f}{Npe:>8.0f}{Q*1e9:>9.3f}{Ipk*1e3:>10.3f}")
print(f"\n  ПРОВЕРКА заявления divider_board.md «макс. заряд анодного импульса ~12 нКл»:")
print(f"    реальный макс (3.5 МэВ) = {charges['потолок']*1e9:.2f} нКл — заявленные 12 нКл завышены в ~{12/(charges['потолок']*1e9):.0f}x")

# ===========================================================================
hdr("2. ДЕЛИТЕЛЬ — токи, мощность, отношение к среднему анодному")
Rchain = 750e3*2 + 680e3*5 + 820e3 + 1e6 + 499e3*2
Vhv = 1500.0
Idiv = Vhv/Rchain
Pdiv = Vhv*Idiv
print(f"  R_chain={Rchain/1e6:.3f} МОм, I_div={Idiv*1e6:.1f} мкА, P={Pdiv*1e3:.0f} мВт  (док: 7.72М/194мкА/290мВт)")
R = 30000.0
print(f"\n  Средний анодный ток при 30 кимп/с и отношение к I_div={Idiv*1e6:.0f}мкА:")
print(f"  {'сценарий':<22}{'Q/имп':>9}{'I_anode_avg':>13}{'I_div/I_an':>11}")
for nm in ['Cs-137','Tl-208','потолок']:
    Iavg = R*charges[nm]
    print(f"  {('моно '+nm):<22}{charges[nm]*1e9:>8.2f}н{Iavg*1e6:>11.1f}мкА{Idiv/Iavg:>11.1f}")
Iavg_mix = R*charges['Cs-137']*0.45   # реалистичный смешанный спектр ~средн.300кэВ
print(f"  {'смешанный ~300кэВ':<22}{charges['Cs-137']*0.45*1e9:>8.2f}н{Iavg_mix*1e6:>11.1f}мкА{Idiv/Iavg_mix:>11.1f}")
print("  Правило: I_div >= 100x I_anode для 0.1% линейности; >=20x для ~1%.")

# ===========================================================================
hdr("3. УСИЛЕНИЕ ФЭУ ПОД НАГРУЗКОЙ — активный vs пассивный делитель (LTspice .meas)")
p_dyn = 0.70   # показатель delta ~ V^p для NaI-ФЭУ
def pmt_gain(volts):
    """volts: dict электрод->V. Множительные зазоры: K->Dy1, Dy1->Dy2,...,Dy7->Dy8.
       Dy8->анод = коллекционный (насыщен), в усиление НЕ входит."""
    seq = ['cath','dy1','dy2','dy3','dy4','dy5','dy6','dy7','dy8']
    g = 1.0
    for i in range(1,len(seq)):
        gap = abs(volts[seq[i]] - volts[seq[i-1]])
        g *= gap**p_dyn
    return g

def load_dividermeas(logname):
    m = parse_meas(os.path.join(HERE, logname))
    steps = sorted({s for d in m.values() for s in d})
    res = {}
    keymap = {'v_cath':'cath','v_dy1':'dy1','v_dy2':'dy2','v_dy3':'dy3','v_dy4':'dy4',
              'v_dy5':'dy5','v_dy6':'dy6','v_dy7':'dy7','v_dy8':'dy8'}
    for st in steps:
        v = {}
        for k,node in keymap.items():
            if k in m and st in m[k]: v[node]=m[k][st]
        if len(v)==9: res[st]=v
    return res

for tag,logn in [('АКТИВНЫЙ (буферы Q401-403)','divider_load.log'),
                 ('ПАССИВНЫЙ (без буферов)','divider_passive.log')]:
    res = load_dividermeas(logn)
    if not res: print(f"  {tag}: лог не найден/пуст"); continue
    sts = sorted(res)
    g0 = pmt_gain(res[sts[0]])      # k=0 (без нагрузки)
    g1 = pmt_gain(res[sts[-1]])     # k=1 (полная 30кимп/с @2.6МэВ)
    dg = (g1/g0-1)*100
    print(f"  {tag}:")
    print(f"     зазор Dy7->Dy8:  {abs(res[sts[0]]['dy8']-res[sts[0]]['dy7']):.2f}В -> {abs(res[sts[-1]]['dy8']-res[sts[-1]]['dy7']):.2f}В")
    print(f"     зазор K->Dy1:    {abs(res[sts[0]]['dy1']-res[sts[0]]['cath']):.2f}В -> {abs(res[sts[-1]]['dy1']-res[sts[-1]]['cath']):.2f}В")
    print(f"     СДВИГ УСИЛЕНИЯ ФЭУ (k=0 -> 30кимп/с@2.6МэВ): {dg:+.2f}%")
print("  => сдвиг усиления линеен по току; для реалистич. смеш. спектра делить на ~6.")

# ===========================================================================
hdr("4. PILE-UP при 30 кимп/с (и 300 кимп/с из CLAUDE.md)")
# длительность импульса берём из формы (см. раздел 6); пока параметрически
for Rr in [30000.0, 300000.0]:
    print(f"\n  Скорость {Rr:.0f} имп/с (средний интервал {1e6/Rr:.1f} мкс):")
    for tau_r,label in [(0.75e-6,'FWHM~0.75мкс'),(1.5e-6,'осн.импульс~1.5мкс'),(3.0e-6,'+undershoot~3мкс')]:
        P1 = 1-math.exp(-Rr*tau_r)
        P2 = 1-math.exp(-2*Rr*tau_r)
        sumrate = Rr*P1
        print(f"    tau_r={label:<20} P(наложение±)={P2*100:5.1f}%  сумм.события≈{sumrate:7.0f}/с")

# ===========================================================================
hdr("5. БАЛЛИСТИЧЕСКИЙ ДЕФИЦИТ (LTspice sh_ballistic.log)")
mb = parse_meas(os.path.join(HERE,'sh_ballistic.log'))
taus = [1e-9,30e-9,60e-9,115e-9,230e-9,350e-9,460e-9,700e-9,1000e-9]
if 'adc_min' in mb and 'bl_adc' in mb:
    bl = mb['bl_adc'][1]
    deltas = {}
    for i,td in enumerate(taus, start=1):
        if i in mb['adc_min']:
            deltas[td] = bl - mb['adc_min'][i]
    d0 = deltas[1e-9]
    d230 = deltas[230e-9]
    print(f"  Отклик при фикс. заряде Cs-137:  дельта(τ→0)={d0*1e3:.1f}мВ, дельта(τ=230нс)={d230*1e3:.1f}мВ")
    print(f"  БАЛЛИСТИЧЕСКИЙ ДЕФИЦИТ при NaI(230нс) = {(1-d230/d0)*100:.1f}%  (теряется доля заряда)")
    # чувствительность к времени высвечивания около 230нс
    dl = deltas[115e-9]; dh = deltas[350e-9]
    slope = (dh-dl)/((350-115)*1e-9)
    sens = slope/d230   # 1/с
    print(f"  Чувствительность амплитуды к τ_scint около 230нс: {sens*1e-9*100:+.2f}% на 1 нс")
    print(f"  => дрейф τ_scint NaI с темп. (~+0.3..1 нс/°C) даёт ДОП. ~{abs(sens*1e-9*100)*0.5:.2f}%/°C,")
    print(f"     НЕ учитываемые однопараметрической (свет.выход) термокомпенсацией Pt1000.")

# ===========================================================================
hdr("6. ФОРМА ИМПУЛЬСА + ОШИБКА СЕМПЛИРОВАНИЯ АЦП 4 Мвыб/с (sh_pulse.raw)")
try:
    import ltspice
    L = ltspice.Ltspice(os.path.join(HERE,'sh_pulse.raw')); L.parse()
    t = L.get_time()
    try: v = L.get_data('V(adc_in)')
    except Exception: v = L.get_data('v(adc_in)')
    t = np.asarray(t); v = np.asarray(v)
    bl = v[(t>=200e-9)&(t<=400e-9)].mean()
    amp = bl - v                      # положит. = вниз от базовой
    ipk = np.argmax(amp); Apk = amp[ipk]; tpk = t[ipk]
    # FWHM
    half = Apk/2
    above = amp>=half
    idx = np.where(above)[0]
    fwhm = t[idx[-1]]-t[idx[0]] if len(idx)>1 else float('nan')
    # undershoot (макс выброс выше базовой ПОСЛЕ пика)
    post = (t>tpk+200e-9)&(t<tpk+5e-6)
    und = max(0.0, (-(amp[post])).max())   # amp<0 => выше базовой
    # возврат к 1%
    after = np.where((t>tpk)&(np.abs(amp)<0.01*Apk))[0]
    tret = t[after[0]]-500e-9 if len(after) else float('nan')
    print(f"  Пик: A={Apk*1e3:.1f}мВ при t-старт={ (tpk-500e-9)*1e9:.0f}нс; FWHM={fwhm*1e9:.0f}нс; undershoot={und/Apk*100:.1f}%")
    print(f"  Возврат к 1% базовой: ~{tret*1e6:.2f}мкс от старта импульса")
    # ----- ошибка семплирования: max-выборка vs истина, по фазе -----
    Ts = 250e-9
    # интерполятор формы
    def amp_at(tt): return np.interp(tt, t, amp)
    phases = np.linspace(0, Ts, 60, endpoint=False)
    err_max, err_par = [], []
    win = np.arange(tpk-3*Ts, tpk+3*Ts, 1e-9)
    for ph in phases:
        st = np.arange(t[0]+ph, t[-1], Ts)
        sv = amp_at(st)
        j = np.argmax(sv)
        # «голый» пик-детект (как FindPeakNegative в прошивке)
        err_max.append((Apk - sv[j])/Apk)
        # 3-точечная параболическая интерполяция
        if 0<j<len(sv)-1:
            y0,y1,y2 = sv[j-1],sv[j],sv[j+1]
            denom=(y0-2*y1+y2)
            d = 0.5*(y0-y2)/denom if denom!=0 else 0
            ypk = y1 - 0.25*(y0-y2)*d
            err_par.append((Apk-ypk)/Apk)
    err_max=np.array(err_max); err_par=np.array(err_par)
    print(f"\n  Ошибка амплитуды от семплирования 250нс (форма недосэмплирована):")
    print(f"    голый пик-детект (прошивка): RMS={err_max.std()*100:.2f}%  среднее={-err_max.mean()*100:.2f}%  макс={err_max.max()*100:.2f}%")
    print(f"    с парабол. интерполяцией:     RMS={err_par.std()*100:.3f}%  макс={abs(err_par).max()*100:.3f}%")
    fwhm_intr=6.3
    fwhm_with = math.sqrt(fwhm_intr**2 + (err_max.std()*100*2.355)**2)
    print(f"    => без интерполяции вклад в FWHM ≈ {err_max.std()*100*2.355:.2f}% ; полная FWHM {fwhm_intr}% -> {fwhm_with:.2f}%")
except Exception as ex:
    print(f"  (не удалось разобрать sh_pulse.raw: {ex})")

print("\n[готово]")
