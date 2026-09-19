# -*- coding: utf-8 -*-
"""
ground_mcu/spice_transfer.py — как смещение местной земли попадает на вход АЦП.

Опорный узел 0 = VSSA (относительно него меряет АЦП и работают внутренние ОУ G474).
По очереди подаётся AC 1 В на одну из «земель»:
  gA — низ делителя A (R313, C321)      -> V_A на +IN OPAMP1/OPAMP2
  gB — низ делителя B (R311)             -> смещение +IN OPAMP3 (PB0)
  gR — земля опоры REF35 (U301, C303)    -> VREF и оба делителя
  gT — земля выхода TIA (оплётка COAX3)  -> AMP_OUT
Топология — вариант V2 из simulation/fix_c320 (C320 удалён, C317 = C314 = 1 мкФ).
Выход: out/transfer.npz (f, H[4]) + таблица |H| на характерных частотах.
"""
import os, re, sys, subprocess
import numpy as np

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
MODELS = os.path.abspath(os.path.join(HERE, '..', 'models'))
LTSPICE = r'C:\Program Files\ADI\LTspice\LTspice.exe'
SRC = ['gA', 'gB', 'gR', 'gT']
NAMES = {'gA': 'низ делителя A (R313/C321)', 'gB': 'низ делителя B (R311)',
         'gR': 'земля опоры U301', 'gT': 'земля TIA / оплётка COAX3'}


def deck(active):
    ac = {s: ('AC 1' if s == active else '0') for s in SRC}
    return '\n'.join([
        f'* ground transfer, active = {active}',
        '.inc ' + os.path.join(MODELS, 'opamp_g474.sub'),
        'VDD VDD 0 3.3',
        f"VgA gA 0 {ac['gA']}", f"VgB gB 0 {ac['gB']}", f"VgR gR 0 {ac['gR']}", f"VgT gT 0 {ac['gT']}",
        'VREF VREF gR 2.5', 'C303 VREF gR 1u',
        'R312 VREF va 8.2k', 'R313 va gA 14.7k', 'C321 va gA 100n',
        'Rtia amp_out gT 1m',
        'R309 amp_out cr_mid 10k', 'C315 cr_mid cr_in 100p', 'R314 cr_mid cr_in 10k',
        'R305 cr_out cr_in 10k', 'XU1 va cr_in VDD 0 cr_out opamp_g474',
        'C317 cr_out rc_node 1u', 'R308 rc_node rc_in 1k',
        'C316 rc_out rc_in 510p', 'R306 rc_out rc_in 1k', 'XU2 va rc_in VDD 0 rc_out opamp_g474',
        'R310 VREF vb 22k', 'R311 vb gB 909', 'C314 rc_out vb 1u',
        'Rpga1 adc_in pga_inm 30k', 'Rpga2 pga_inm 0 10k', 'XU3 vb pga_inm VDD 0 adc_in opamp_g474',
        '.save V(adc_in)', '.ac dec 50 10 10Meg', '.end', ''])


def read_ac_raw(path):
    b = open(path, 'rb').read()
    for nl in ('\n', '\r\n'):
        mk = ('Binary:' + nl).encode('utf-16-le')
        pos = b.find(mk)
        if pos >= 0:
            break
    hdr = b[:pos].decode('utf-16-le')
    npts = int(re.search(r'No\. Points:\s*(\d+)', hdr).group(1))
    names = re.findall(r'^\s*\d+\s+(\S+)\s+\S+\s*$', re.split(r'^Variables:', hdr, flags=re.M)[1], re.M)
    a = np.frombuffer(b[pos + len(mk):], dtype='<f8', count=npts * 2 * len(names)).reshape(npts, len(names), 2)
    c = a[..., 0] + 1j * a[..., 1]
    return np.abs(c[:, 0]), {n: c[:, i] for i, n in enumerate(names)}


def main():
    os.makedirs(OUT, exist_ok=True)
    paths = []
    for s in SRC:
        p = os.path.join(OUT, f'gnd_{s}.cir')
        open(p, 'w', encoding='ascii').write(deck(s))
        paths.append(p)
    if '--no-run' not in sys.argv:
        for p in [subprocess.Popen([LTSPICE, '-b', q], cwd=OUT) for q in paths]:
            p.wait()
    H = []
    for s in SRC:
        f, d = read_ac_raw(os.path.join(OUT, f'gnd_{s}.raw'))
        H.append(next(v for k, v in d.items() if k.lower() == 'v(adc_in)'))
    H = np.array(H)
    np.savez(os.path.join(OUT, 'transfer.npz'), f=f, H=H, src=SRC)
    fq = [100, 1e3, 1e4, 5e4, 1.6e5, 5e5, 2e6]
    print('| земля | ' + ' | '.join(f'{x:g} Гц' for x in fq) + ' |')
    print('|' + '---|' * (len(fq) + 1))
    for i, s in enumerate(SRC):
        vals = [abs(np.interp(np.log10(x), np.log10(f), H[i].real) + 1j * np.interp(np.log10(x), np.log10(f), H[i].imag)) for x in fq]
        print(f'| {s}: {NAMES[s]} | ' + ' | '.join(f'{v:.3f}' for v in vals) + ' |')
    print('\n|H| = В на входе АЦП на 1 В смещения этой земли относительно VSSA.')


if __name__ == '__main__':
    main()
