# -*- coding: utf-8 -*-
"""
fix_c320/gen_template.py — эталон формы импульса для подгонки в firmware/core/dsp.c.

Источник формы — отклик шейпера из LTspice (out/V2_tran.npz, вариант V2: C320 удалён,
C314/C317 = 1 мкФ). Импульс боевой платы короткий: FWHM 640 нс = 2.56 отсчёта при 4 Мвыб/с,
поэтому парабола по 5 точкам занижает амплитуду на 12–15 % и даёт разброс по фазе ~3 %.

Таблица: NPH строк (суб-отсчётные фазы) × NS отсчётов, значения в Q14 (16384 = пик 1.0).
Строки выровнены так же, как их видит dsp.c: максимум строки стоит на позиции IPK —
детектор берёт окно относительно найденного максимума buf[imax].

Амплитуда из подгонки = пик АНАЛОГОВОГО импульса в кодах АЦП (строки нормированы на
непрерывный пик 1.0), то есть на 13 % больше, чем давала парабола. Энергокалибровка
это поглощает, но старые калибровки после смены алгоритма недействительны.

Запуск: python simulation/fix_c320/gen_template.py [вариант] [--fs F] [--nph N] [--out путь] [--name имя]
  без аргументов — прежнее поведение: V2, 4 Мвыб/с, 32 фазы → firmware/core/dsp_template.h.
  Для gamma_fw (3.4667 Мвыб/с, 64 фазы):
    python -I simulation/fix_c320/gen_template.py --fs 3466666.67 --nph 64 \
        --out firmware/gamma_fw/src/core/tpl_3467k.h --name tpl_3467k
"""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
DST = os.path.abspath(os.path.join(HERE, '..', '..', 'firmware', 'core', 'dsp_template.h'))

FS = 4e6
T = 1 / FS
T_PULSE = 200e-6          # момент импульса в деках run_spice.py
NPH = 32                  # суб-отсчётных фаз
NS = 7                    # отсчётов в окне
IPK = 1                   # позиция максимума в окне (1 отсчёт до пика)
Q = 14                    # формат Q14
FINE = 1e-9


def shape(name):
    """Непрерывная форма, нормированная на пик 1.0, на мелкой сетке."""
    z = np.load(os.path.join(OUT, f'{name}_tran.npz'))
    t, v = z['t'], z['adc_in'].astype(float)
    dc = float(np.interp(0.75 * T_PULSE, t, v))
    tt, h = t - T_PULSE, v - dc
    h = h / h.max()
    g = np.arange(-2e-6, 20e-6, FINE)
    return g, np.interp(g, tt, h)


def rows(g, s):
    """Для каждой фазы: отсчёты АЦП, выровненные по максимуму (как в dsp.c)."""
    out = []
    for p in range(NPH):
        d = p / NPH                                   # сдвиг сетки АЦП относительно импульса
        k = np.arange(-4, 24)
        y = np.interp((k + d) * T + g[int(np.argmax(s))], g, s)
        imax = int(np.argmax(y))
        w = y[imax - IPK: imax - IPK + NS]
        assert len(w) == NS and int(np.argmax(w)) == IPK, (p, w)
        out.append(w)
    return np.array(out)


def main():
    global FS, T, NPH
    # консоль cp1251 не печатает «×», «Σ» — без этого скрипт падал до записи файла
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description='эталон формы импульса (Q14) из отклика LTspice')
    ap.add_argument('variant', nargs='?', default='V2', help='вариант схемы: out/<вариант>_tran.npz')
    ap.add_argument('--fs', type=float, default=FS, help='частота выборки, выб/с (4e6)')
    ap.add_argument('--nph', type=int, default=NPH, help='число суб-отсчётных фаз (32)')
    ap.add_argument('--out', default=DST, help='выходной .h (firmware/core/dsp_template.h)')
    ap.add_argument('--name', default='dsp_template',
                    help='имя массива; от него же стража и макросы')
    a = ap.parse_args()
    FS, T, NPH = a.fs, 1 / a.fs, a.nph
    name, arr = a.variant, a.name
    if arr == 'dsp_template':                     # прежние имена — байт в байт
        guard, pfx, user = 'DSP_TEMPLATE_H', 'DSP_TPL', 'dsp.c'
    else:
        guard, pfx, user = arr.upper() + '_H', arr.upper(), 'детекторе'
    fname = os.path.basename(a.out)
    g, s = shape(name)
    R = rows(g, s)
    q = np.rint(R * (1 << Q)).astype(int)
    assert q.max() <= (1 << Q) and q.min() > -(1 << 15), (q.max(), q.min())
    # контроль: ошибка квантования и разброс энергии строк
    err = np.abs(q / (1 << Q) - R).max()
    e = (R ** 2).sum(1)
    print(f'{name}: строк {NPH} × {NS} отсчётов, максимум в позиции {IPK}')
    print(f'  ошибка квантования Q{Q}: {err:.2e} (пик = 1.0)')
    print(f'  Σs² по фазам: {e.min():.3f} … {e.max():.3f}')
    print(f'  строка фазы 0: ' + ' '.join(f'{x:+.3f}' for x in R[0]))

    lines = [
        f'/* {fname} — эталон формы импульса для подгонки в {user} (СГЕНЕРИРОВАНО).',
        ' *',
        f' * Генератор: simulation/fix_c320/gen_template.py, источник формы — {name}_tran.npz',
        f' * (LTspice, шейпер боевой платы). Частота выборки {FS / 1e6:g} Мвыб/с.',
        ' *',
        f' * {NPH} суб-отсчётных фаз × {NS} отсчётов, формат Q{Q} (16384 = пик аналогового импульса).',
        f' * Максимум каждой строки стоит на позиции {IPK} — окно берётся относительно buf[imax].',
        ' * Амплитуда подгонки = пик аналогового импульса в кодах АЦП.',
        ' *',
        ' * Руками не править: перегенерировать скриптом. Позже эталон можно набрать на реальной',
        ' * плате (усреднение ~10 000 импульсов, как в BecqMoni) и подставить той же таблицей.',
        ' */',
        f'#ifndef {guard}',
        f'#define {guard}',
        '',
        '#include <stdint.h>',
        '',
        f'#define {pfx}_NPH  {NPH}u',
        f'#define {pfx}_NS   {NS}u',
        f'#define {pfx}_IPK  {IPK}u',
        f'#define {pfx}_Q    {Q}u',
        '',
        f'static const int16_t {arr}[{pfx}_NPH][{pfx}_NS] = {{',
    ]
    for p in range(NPH):
        lines.append('    { ' + ', '.join(f'{v:6d}' for v in q[p]) + ' },')
    lines += ['};', '', f'#endif /* {guard} */', '']
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
    print('записано:', a.out)


if __name__ == '__main__':
    main()
