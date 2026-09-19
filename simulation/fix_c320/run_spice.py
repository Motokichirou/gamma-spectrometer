# -*- coding: utf-8 -*-
"""
fix_c320/run_spice.py — варианты исправления находок 1.1 и 1.3 (review/REVIEW_critic_2026-09-15.md).

1.1: C320 (100 нФ на GND) сидит прямо на PB0 вместе с C314 -> ёмкостный делитель x0.5.
1.3: разделительные цепи C317/R308 и C314/(R310||R311) имеют tau ~0.1-0.2 мс -> хвосты от загрузки.

Топология = нетлист pcb/gamma_spectrometer.net:
  AMP_OUT -R309- cr_mid -(C315||R314)- PA3(-IN OPAMP1), R305 PA3-PA2, +IN = V_A (R312/R313, C321)
  PA2 -C317- R308 - PA5(-IN OPAMP2), C316||R306 PA5-PA6, +IN = V_A
  PA6 -C314- PB0(+IN OPAMP3) + R310/R311 (V_B) + C320 на GND;  OPAMP3 = PGA x4
  ветка DAC_TEST: cr_mid -C318(100p)- R307(10k) - PA4
Транзиент: TIA поведенческий (V = +499*I, tau = Rf*Cf = 2.35 нс) — он в сотни раз быстрее
шейпера и на хвосты не влияет. Шумовые деки — официальная модель ADA4817 + opamp_g474_noise.

Запуск:  python simulation/fix_c320/run_spice.py [--no-run]
Выход:   simulation/fix_c320/out/  (деки, .raw/.log, *_tran.npz, *_noise.npz, results.json)
"""
import os, re, sys, json, subprocess
import numpy as np

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.dirname(HERE)
OUT = os.path.join(HERE, 'out')
MODELS = os.path.join(SIM, 'models')
LTSPICE = r'C:\Program Files\ADI\LTspice\LTspice.exe'

VREF = 2.5
CH_PER_V = 8192 / VREF          # 12 бит -> 8192 канала (канал = половина LSB)
VOLSAT, VDDA = 0.100, 3.3       # DS12288 Табл. 80
IPK = {'Am241': 0.084e-3, 'Cs137': 0.94e-3, '3.5MeV': 4.97e-3}   # как в sim_bias_fixed.cir
T_PULSE = 200e-6
T_STOP = 30e-3

# tau = (C317*R308, C314*R_экв) — аналитически, для таблицы
VARIANTS = {
    'Vdoc': dict(desc='старая дека: C320 за несуществующим Rbias 4.65k',
                 c317='100n', c314='100n', pb0='rbias', tau=(100e-6, 465e-6)),
    'V0':   dict(desc='как в схеме сейчас: C320 прямо на PB0',
                 c317='100n', c314='100n', pb0='c320', tau=(100e-6, 175e-6)),
    'V1':   dict(desc='удалить C320',
                 c317='100n', c314='100n', pb0='none', tau=(100e-6, 87e-6)),
    'V1d':  dict(desc='V1 + включён тестовый ЦАП PA4 (ветка C318+R307)',
                 c317='100n', c314='100n', pb0='none', dac=True, tau=(100e-6, 87e-6)),
    'V2':   dict(desc='удалить C320; C317 и C314 -> 1 мкФ',
                 c317='1u', c314='1u', pb0='none', tau=(1e-3, 873e-6)),
    'V3':   dict(desc='C320 оставить; Rs 10k узел делителя->PB0; C317 -> 1 мкФ',
                 c317='1u', c314='100n', pb0='rs', tau=(1e-3, 1.0e-3)),
    'V4':   dict(desc='без C317/C314 (DC-связь); PGA с опорой VINM0=PB2 (1.62k/976 от VREF, 100n)',
                 c317=None, c314=None, pb0='dc', tau=(None, None)),
}


def shaper(v, oa):
    L = ['VDD VDD 0 3.3', f'VREF VREF 0 {VREF}',
         'R312 VREF va 8.2k', 'R313 va 0 14.7k', 'C321 va 0 100n',
         'R309 amp_out cr_mid 10k', 'C315 cr_mid cr_in 100p', 'R314 cr_mid cr_in 10k',
         'R305 cr_out cr_in 10k', f'XU1 va cr_in VDD 0 cr_out {oa}']
    if v.get('dac'):
        L += ['C318 cr_mid dac1 100p', 'R307 dac1 pa4 10k', 'VPA4 pa4 0 1.0']
    if v['c317']:
        L += [f"C317 cr_out rc_node {v['c317']}", 'R308 rc_node rc_in 1k']
    else:
        L += ['R308 cr_out rc_in 1k']
    L += ['C316 rc_out rc_in 510p', 'R306 rc_out rc_in 1k', f'XU2 va rc_in VDD 0 rc_out {oa}']
    pb0, ref = v['pb0'], '0'
    if pb0 != 'dc':
        L += ['R310 VREF vb 22k', 'R311 vb 0 909']
    if pb0 == 'rbias':
        L += ['C320 vb 0 100n', f"C314 rc_out pb0 {v['c314']}", 'Rbias vb pb0 4.65k']
    elif pb0 == 'c320':
        L += ['C320 vb 0 100n', f"C314 rc_out vb {v['c314']}", 'Rpb0 vb pb0 1m']
    elif pb0 == 'none':
        L += [f"C314 rc_out vb {v['c314']}", 'Rpb0 vb pb0 1m']
    elif pb0 == 'rs':
        L += ['C320 vb 0 100n', 'Rs vb pb0 10k', f"C314 rc_out pb0 {v['c314']}"]
    elif pb0 == 'dc':
        L += ['Rpb0 rc_out pb0 1m', 'Rbt VREF vbi 1.62k', 'Rbb vbi 0 976', 'Cbi vbi 0 100n']
        ref = 'vbi'
    L += ['Rpga1 adc_in pga_inm 30k', f'Rpga2 pga_inm {ref} 10k',
          f'XU3 pb0 pga_inm VDD 0 adc_in {oa}']
    return L


def tran_deck(name, v):
    L = [f'* fix_c320 {name} TRAN', '.inc ' + os.path.join(MODELS, 'opamp_g474.sub'),
         f"I_pmt 0 tia EXP(0 {IPK['Cs137']} {T_PULSE} 5n {T_PULSE + 5e-9} 230n)",
         'Rf tia 0 499', 'Cf tia 0 4.7p', 'E_tia amp_out 0 tia 0 1']
    L += shaper(v, 'opamp_g474')
    L += ['.save V(adc_in) V(cr_out) V(rc_out) V(pb0)', '.options plotwinsize=0',
          f'.tran 0 {T_STOP} 0 20n']
    for n in ('adc_in', 'cr_out', 'rc_out', 'pb0'):
        L += [f'.meas TRAN {n}_dc FIND V({n}) AT={0.75 * T_PULSE}',
              f'.meas TRAN {n}_max MAX V({n}) FROM={T_PULSE} TO={T_PULSE + 30e-6}',
              f'.meas TRAN {n}_min MIN V({n}) FROM={T_PULSE} TO={T_STOP}']
    return L + ['.end']


def noise_deck(name, v):
    L = [f'* fix_c320 {name} NOISE',
         '.inc ' + os.path.join(MODELS, 'ada4817.cir'),
         '.inc ' + os.path.join(MODELS, 'opamp_g474_noise.sub'),
         'VCC VCC 0 5', 'VEE VEE 0 -5',
         'I_pmt anode 0 DC 0 AC 1', 'Cin anode 0 10p',
         'R417 fb anode 499', 'C415 fb anode 4.7p',
         'X_tia 0 anode VCC VEE amp_out fb VCC ADA4817']
    L += shaper(v, 'opamp_g474_noise')
    L += ['.noise V(adc_in) I_pmt dec 50 10 2Meg',
          '.meas NOISE vn_2m INTEG V(onoise)']
    return L + ['.end']


def run_ltspice(decks, par=4):
    for i in range(0, len(decks), par):
        ps = [subprocess.Popen([LTSPICE, '-b', d], cwd=OUT) for d in decks[i:i + par]]
        for p in ps:
            p.wait()


def read_raw(path):
    b = open(path, 'rb').read()
    for nl in ('\n', '\r\n'):
        mk = ('Binary:' + nl).encode('utf-16-le')
        pos = b.find(mk)
        if pos >= 0:
            break
    hdr = b[:pos].decode('utf-16-le')
    npts = int(re.search(r'No\. Points:\s*(\d+)', hdr).group(1))
    names = re.findall(r'^\s*\d+\s+(\S+)\s+\S+\s*$', re.split(r'^Variables:', hdr, flags=re.M)[1], re.M)
    dt = np.dtype([('x', '<f8')] + [(f'v{i}', '<f4') for i in range(len(names) - 1)])
    a = np.frombuffer(b[pos + len(mk):], dtype=dt, count=npts)
    d = {names[0]: np.abs(a['x'])}
    for i, n in enumerate(names[1:]):
        d[n] = a[f'v{i}'].astype(float)
    return d


def read_meas(path):
    b = open(path, 'rb').read()
    txt = b.decode('utf-16-le', errors='replace') if b[1:2] == b'\x00' else b.decode('latin-1')
    m = {}
    for g in re.finditer(r'^\s*(\w+):\s*[^=\n]*=\s*([-+]?[\d.]+(?:[eE][-+]?\d+)?)', txt, re.M):
        m[g.group(1).lower()] = float(g.group(2))
    return m


def analyze(name, v):
    r = dict(desc=v['desc'], tau317=v['tau'][0], tau314=v['tau'][1])
    m = read_meas(os.path.join(OUT, f'{name}_tran.log'))
    d = read_raw(os.path.join(OUT, f'{name}_tran.raw'))
    t = d['time']
    np.savez_compressed(os.path.join(OUT, f'{name}_tran.npz'), t=t,
                        adc_in=d['V(adc_in)'], cr_out=d['V(cr_out)'], rc_out=d['V(rc_out)'])
    k = IPK['3.5MeV'] / IPK['Cs137']
    dc = m['adc_in_dc']
    pk = m['adc_in_max'] - dc
    r.update(adc_dc=dc, pb0_dc=m['pb0_dc'], v_cs=pk, ch_cs=pk * CH_PER_V,
             adc_top35=dc + pk * k, adc_low35=dc + (m['adc_in_min'] - dc) * k)
    for st in ('cr_out', 'rc_out'):
        sdc = m[f'{st}_dc']
        r[f'{st}_dc'] = sdc
        r[f'{st}_35'] = (sdc + (m[f'{st}_min'] - sdc) * k, sdc + (m[f'{st}_max'] - sdc) * k)
    tt = t - T_PULSE
    h = d['V(adc_in)'] - dc
    post = tt > 0
    r['undershoot_pct'] = float(h[post].min() / pk * 100)
    late = tt > 30e-6
    r['tail_pct_after30u'] = float(np.abs(h[late]).max() / pk * 100)
    main = (tt >= 0) & (tt <= 30e-6)
    r['area_main'] = float(np.trapezoid(h[main], t[main]))
    r['area_total'] = float(np.trapezoid(h[post], t[post]))
    # средний сдвиг базовой линии от хвостов при 30 кимп/с (доля амплитуды Cs-137)
    r['bl_shift30k_pct'] = float(-30e3 * (r['area_main'] - r['area_total']) / pk * 100)

    mn = read_meas(os.path.join(OUT, f'{name}_noise.log'))
    dn = read_raw(os.path.join(OUT, f'{name}_noise.raw'))
    np.savez_compressed(os.path.join(OUT, f'{name}_noise.npz'),
                        f=dn['frequency'], onoise=next(x for k, x in dn.items() if k.lower() == 'v(onoise)'))
    vn = mn['vn_2m']
    r.update(vn_uV=vn * 1e6, sigma_ch=vn * CH_PER_V, fwhm_el_pct=2.3548 * vn / pk * 100)
    return r


def fmt_tau(x):
    return '—' if x is None else (f'{x * 1e6:.0f} мкс' if x < 1e-3 else f'{x * 1e3:.2f} мс')


def report(res):
    ref = res['Vdoc']['v_cs']
    print('\n| Вар | τ C317 | τ C314 | Cs-137 на АЦП | канал | к Vdoc | baseline | верх @3.5 МэВ (до 2.5 В) '
          '| OPAMP1 @3.5 МэВ | OPAMP2 @3.5 МэВ | выброс | хвост >30 мкс | шум | σ, кан | FWHM_эл | сдвиг BL @30к |')
    print('|' + '---|' * 16)
    for n, r in res.items():
        c1, c2 = r['cr_out_35'], r['rc_out_35']
        print(f"| {n} | {fmt_tau(r['tau317'])} | {fmt_tau(r['tau314'])} | {r['v_cs'] * 1e3:.1f} мВ | "
              f"{r['ch_cs']:.0f} | ×{r['v_cs'] / ref:.3f} | {r['adc_dc']:.3f} В | "
              f"{r['adc_top35']:.3f} В (+{VREF - r['adc_top35']:.3f}) | {c1[0]:.2f}…{c1[1]:.2f} | "
              f"{c2[0]:.2f}…{c2[1]:.2f} | {r['undershoot_pct']:.2f} % | {r['tail_pct_after30u']:.3f} % | "
              f"{r['vn_uV']:.1f} мкВ | {r['sigma_ch']:.2f} | {r['fwhm_el_pct']:.3f} % | {r['bl_shift30k_pct']:+.2f} % |")
    print(f'\nОкно выхода ОУ: {VOLSAT:.2f}…{VDDA - VOLSAT:.2f} В; вход АЦП 0…{VREF} В.')


def main():
    os.makedirs(OUT, exist_ok=True)
    decks = []
    for name, v in VARIANTS.items():
        for kind, fn in (('tran', tran_deck), ('noise', noise_deck)):
            p = os.path.join(OUT, f'{name}_{kind}.cir')
            with open(p, 'w', encoding='ascii') as f:
                f.write('\n'.join(fn(name, v)) + '\n')
            decks.append(p)
    if '--no-run' not in sys.argv:
        run_ltspice(decks)
    res = {n: analyze(n, v) for n, v in VARIANTS.items()}
    with open(os.path.join(OUT, 'results.json'), 'w', encoding='utf-8') as f:
        json.dump(res, f, ensure_ascii=False, indent=1)
    report(res)


if __name__ == '__main__':
    main()
