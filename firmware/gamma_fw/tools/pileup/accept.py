# -*- coding: utf-8 -*-
"""accept.py — приёмка разбора хвостовых наложений на записанных данных (план 2026-11-13, задача 5).

Прогоняет tools/replay (детектор прошивки бит в бит) по файлам порций без разбора и с таблицей хвоста,
при авто-пороге и фиксированном, и сводит: принято, брак по причинам, восстановлено, живое время,
FWHM / пик Cs, площади Am 59.5 / Ra 352 / Cs 662 (подгонка — как в spec_cmp.py), сдвиг пика Cs у
восстановленных относительно остальных принятых, критерии приёмки.

  python -I accept.py --replay ..\\replay\\out\\replay.exe --tail out\\t5\\tail.bin --out out\\t5 \\
         --data D:\\gamma_data\\pileup [--thr 0,18] [--files mix_1,mix_2,mix_3,cs_only,am_only] [--no-run]
         [--model --tpl D:\\gamma_data\\pileup\\tpl_cs.h --shape out\\S.npz]

replay должен быть собран с тем же эталоном, по которому строилась таблица (build.cmd путь\\tpl.h).
--model — сверка с моделью run.py (fw без разбора, unfold с разбором, тот же порог и σ, что у replay).
Результат — таблицы Markdown в stdout и <out>/accept.md."""
import argparse
import os
import re
import subprocess
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spec_cmp import gfit, GP  # noqa: E402  (подгонка гауссами с линейным фоном, как в spec_cmp.py)

FS = 3466667.0
MIX = ['mix_1', 'mix_2', 'mix_3']


def load_csv(path):
    return np.loadtxt(path, delimiter=',', skiprows=1)[:, 1]


def run_replay(a, name, thr, on):
    tag = f"{'on' if on else 'off'}_t{thr}_{name}"
    pref = os.path.join(a.out, tag)
    cmd = [a.replay, os.path.join(a.data, name + '.bin'), pref + '.csv', str(thr), '1']
    if on:
        cmd.append(a.tail)
    if not a.no_run:
        r = subprocess.run(cmd, capture_output=True)
        txt = r.stdout.decode('utf-8', 'replace')
        if r.returncode:
            raise SystemExit(f'replay {tag}: код {r.returncode}\n{txt}{r.stderr.decode("utf-8", "replace")}')
        with open(pref + '.txt', 'w', encoding='utf-8') as f:
            f.write(' '.join(cmd) + '\n' + txt)
    with open(pref + '.txt', encoding='utf-8') as f:
        txt = f.read()
    m = re.search(r'^STAT (.*)$', txt, re.M)
    st = {k: float(v) for k, v in (kv.split('=') for kv in m.group(1).split())}
    st['sigma'] = float(re.search(r'σ ([0-9.]+)', txt).group(1))
    st['pref'] = pref
    st['tag'] = tag
    return st


def peak_mu(h, lo, hi, mu0):
    """пик гаусса(ов) с ошибкой положения последнего (Cs) — по ковариации curve_fit"""
    from scipy.optimize import curve_fit
    x = np.arange(lo, hi); y = h[lo:hi]
    npk = len(mu0)

    def f(x, *p):
        r = p[0] + p[1] * (x - lo)
        for k in range(npk):
            aa, m, s = p[2 + 3 * k: 5 + 3 * k]
            r = r + aa * np.exp(-0.5 * ((x - m) / s) ** 2)
        return r
    p0 = [y[:5].mean(), (y[-5:].mean() - y[:5].mean()) / (hi - lo)]
    for m in mu0:
        p0 += [h[int(m)] - p0[0], m, 0.03 * m]
    p, cv = curve_fit(f, x, y, p0=p0, sigma=np.sqrt(np.maximum(y, 1)), maxfev=20000)
    k = npk - 1
    s = abs(p[4 + 3 * k])
    return p[3 + 3 * k], np.sqrt(cv[3 + 3 * k, 3 + 3 * k]), 235.48 * s / p[3 + 3 * k]


def gp_fit(h, path):
    with open(path, 'w') as f:
        f.write('channel,count\n')
        for i, c in enumerate(h):
            f.write(f'{i},{int(c)}\n')
    r = subprocess.run([GP, 'fit', path, '--lo', '1000', '--hi', '1500'], capture_output=True)
    return r.stdout.decode('utf-8', 'replace').strip()


def roi_net(h, lo, hi, w=10):
    """отсчёты в [lo, hi) за вычетом линейного фона по боковым полосам w каналов"""
    b = 0.5 * (h[lo - w:lo].mean() + h[hi:hi + w].mean()) * (hi - lo)
    n = h[lo:hi].sum()
    return n - b, np.sqrt(n + b * (hi - lo) / (2 * w))


AM_ROI = (108, 140)      # Am 59.5: пик ≈ 123 кан. (am_only)
RA_ROI = (612, 682)      # Ra 352 (Pb-214): пик ≈ 647


def spec_summary(h, kind):
    """kind: mix — Cs двумя гауссами с Bi-609 (1000–1400), cs — один гаусс 1060–1320, am — только Am.
    Am гауссом — только на am_only (98–150): на смеси пик Am тонет в континууме и Pb-X, там — ROI-нетто."""
    out = {'am_roi': roi_net(h, *AM_ROI)}
    out['am'] = None
    if kind == 'am':
        try:
            am = gfit(h, 98, 150, 1, [121])[0]
            out['am'] = (am[0], am[2], am[3], am[4])
        except Exception:
            pass
        return out
    out['ra'] = None
    out['ra_roi'] = roi_net(h, *RA_ROI) if kind == 'mix' else None
    if kind == 'mix':
        try:
            ra = gfit(h, 585, 720, 1, [645])[0]
            out['ra'] = (ra[0], ra[2], ra[3], ra[4])
        except Exception:
            pass
    try:
        if kind == 'mix':
            bi, cs = gfit(h, 1000, 1400, 2, [1110, 1190])
        else:
            cs = gfit(h, 1060, 1320, 1, [1190])[0]
        out['cs'] = (cs[0], cs[2], cs[3], cs[4])
    except Exception:
        out['cs'] = None
    return out


def fmt_pk(v, area=True):
    if v is None:
        return '—'
    s = f'{v[0]:.1f} / {v[1]:.2f} %'
    return s + (f' / {v[2]:.0f} ± {v[3]:.0f}' if area else '')


def fmt_roi(v):
    return '—' if v is None else f'{v[0]:.0f} ± {v[1]:.0f}'


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser()
    ap.add_argument('--replay', required=True)
    ap.add_argument('--tail', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--data', default=r'D:\gamma_data\pileup')
    ap.add_argument('--thr', default='0,18')
    ap.add_argument('--files', default='mix_1,mix_2,mix_3,cs_only,am_only')
    ap.add_argument('--no-run', action='store_true')
    ap.add_argument('--model', action='store_true')
    ap.add_argument('--tpl')
    ap.add_argument('--shape')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    files = a.files.split(',')
    thrs = [int(t) for t in a.thr.split(',')]
    L = []
    p = L.append
    R = {}
    for thr in thrs:
        for on in (0, 1):
            for name in files:
                R[(thr, on, name)] = run_replay(a, name, thr, on)

    p('## Прогоны replay (детектор прошивки, tools/replay)\n')
    p('| порог | разбор | файл | thr факт | принято | наложения (фронт) | форма | ширина | бюджет | брак % | '
      'восстановлено | мёртвых % | живое, с | принято/живое, имп/с |')
    p('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    for thr in thrs:
        for on in (0, 1):
            for name in files:
                s = R[(thr, on, name)]
                trig = s['acc'] + s['pile'] + s['shape'] + s['width'] + s['budget']
                p(f"| {'авто' if thr == 0 else thr} | {'вкл' if on else 'выкл'} | {name} | {s['thr']:.0f} | "
                  f"{s['acc']:.0f} | {s['pile']:.0f} ({s['front']:.0f}) | {s['shape']:.0f} | {s['width']:.0f} | "
                  f"{s['budget']:.0f} | {100 * (trig - s['acc']) / trig:.2f} | {s['rec']:.0f} | "
                  f"{100 * s['dead'] / s['total']:.2f} | {s['live_s']:.4f} | {s['acc'] / s['live_s']:.1f} |")
    p('')

    p('## Спектры (пик / FWHM / площадь; Cs на смеси — 2 гаусса с Bi-609 в 1000–1400, '
      'как spec_cmp.py; cs_only — один гаусс 1060–1320)\n')
    p('| порог | разбор | набор | всего | Am 59.5 гаусс | Am ROI-нетто 108–140 | Ra 352 гаусс | Ra ROI-нетто 612–682 | '
      'Cs 662 | gammapult fit 1000–1500 |')
    p('|---|---|---|---|---|---|---|---|---|---|')
    SP = {}
    groups = [('mix 1–3', MIX, 'mix')] if all(m in files for m in MIX) else []
    groups += [(n, [n], 'cs' if n.startswith('cs') else 'am' if n.startswith('am') else 'mix')
               for n in files if n not in MIX]
    for thr in thrs:
        for on in (0, 1):
            for gname, names, kind in groups:
                h = sum(load_csv(R[(thr, on, n)]['pref'] + '.csv') for n in names)
                sm = spec_summary(h, kind)
                SP[(thr, on, gname)] = (h, sm)
                gp = gp_fit(h, os.path.join(a.out, f'_gp_{thr}_{on}_{gname.replace(" ", "")}.csv')) \
                    if kind != 'am' else '—'
                p(f"| {'авто' if thr == 0 else thr} | {'вкл' if on else 'выкл'} | {gname} | {int(h.sum())} | "
                  f"{fmt_pk(sm['am'])} | {fmt_roi(sm['am_roi'])} | {fmt_pk(sm.get('ra'))} | "
                  f"{fmt_roi(sm.get('ra_roi'))} | {fmt_pk(sm.get('cs'))} | {gp} |")
    p('')
    p('Отношение вкл / выкл (рост числа событий):\n')
    p('| порог | набор | всего | Am ROI | Ra гаусс | Ra ROI | Cs гаусс |')
    p('|---|---|---|---|---|---|---|')
    for thr in thrs:
        for gname, names, kind in groups:
            (h0, s0), (h1, s1) = SP[(thr, 0, gname)], SP[(thr, 1, gname)]

            def rt(k, i):
                a0, a1 = s0.get(k), s1.get(k)
                return f'{a1[i] / a0[i]:.3f}' if a0 is not None and a1 is not None and a0[i] > 0 else '—'
            p(f"| {'авто' if thr == 0 else thr} | {gname} | {h1.sum() / h0.sum():.3f} | {rt('am_roi', 0)} | "
              f"{rt('ra', 2)} | {rt('ra_roi', 0)} | {rt('cs', 2)} |")
    p('')

    # восстановленные против остальных принятых
    p('## Сдвиг пика Cs: восстановленные (с вычетом хвоста) против остальных принятых, разбор вкл\n')
    p('| порог | набор | восстановлено в спектре | пик rec | пик com | сдвиг % | FWHM rec / com % |')
    p('|---|---|---|---|---|---|---|')
    SH = {}
    for thr in thrs:
        for gname, names, kind in groups:
            if kind == 'am':
                continue
            hr = sum(load_csv(R[(thr, 1, n)]['pref'] + '_rec.csv') for n in names)
            hc = sum(load_csv(R[(thr, 1, n)]['pref'] + '_com.csv') for n in names)
            try:
                if kind == 'mix':
                    mr, er, fr = peak_mu(hr, 1000, 1400, [1110, 1190])
                    mc, ec, fc = peak_mu(hc, 1000, 1400, [1110, 1190])
                else:
                    mr, er, fr = peak_mu(hr, 1060, 1320, [1190])
                    mc, ec, fc = peak_mu(hc, 1060, 1320, [1190])
                sh = 100 * (mr / mc - 1); esh = 100 * np.hypot(er / mc, ec * mr / mc ** 2)
                SH[(thr, gname)] = (sh, esh)
                p(f"| {'авто' if thr == 0 else thr} | {gname} | {int(hr.sum())} | {mr:.2f} ± {er:.2f} | "
                  f"{mc:.2f} ± {ec:.2f} | {sh:+.3f} ± {esh:.3f} | {fr:.2f} / {fc:.2f} |")
            except Exception as e:
                p(f"| {'авто' if thr == 0 else thr} | {gname} | {int(hr.sum())} | подгонка не сошлась: {e} | | | |")
    p('')

    # критерии
    p('## Критерии приёмки\n')
    p('| порог | критерий | цель | факт | итог |')
    p('|---|---|---|---|---|')
    for thr in thrs:
        tl = 'авто' if thr == 0 else str(thr)
        mixs = [R[(thr, 1, n)] for n in MIX if n in files]
        if mixs:
            rj = [100 * (s['pile'] + s['shape'] + s['width'] + s['budget']) /
                  (s['acc'] + s['pile'] + s['shape'] + s['width'] + s['budget']) for s in mixs]
            p(f"| {tl} | брак на смеси (по файлам) | ≤ 5 % | {' / '.join(f'{x:.2f}' for x in rj)} % | "
              f"{'да' if max(rj) <= 5 else 'НЕТ'} |")
            bud = sum(s['budget'] for s in mixs)
            p(f"| {tl} | rej_budget на смеси | 0 | {bud:.0f} | {'да' if bud == 0 else 'НЕТ'} |")
        for gname in [g[0] for g in groups if g[2] != 'am']:
            cs = SP[(thr, 1, gname)][1].get('cs')
            if cs is not None:
                p(f"| {tl} | FWHM Cs, {gname} (2 гаусса / 1 гаусс) | ≤ 6.8 % | {cs[1]:.2f} % | "
                  f"{'да' if cs[1] <= 6.8 else 'НЕТ'} |")
            if (thr, gname) in SH:
                sh, esh = SH[(thr, gname)]
                p(f"| {tl} | сдвиг пика Cs rec/com, {gname} | ≤ 0.1 % | {sh:+.3f} ± {esh:.3f} % | "
                  f"{'да' if abs(sh) <= 0.1 else 'НЕТ'} |")
        for name in files:
            r0 = R[(thr, 0, name)]['acc'] / R[(thr, 0, name)]['live_s']
            r1 = R[(thr, 1, name)]['acc'] / R[(thr, 1, name)]['live_s']
            d = 100 * (r1 / r0 - 1)
            p(f"| {tl} | принято/живое вкл против выкл, {name} | ≤ 3 % | {r0:.0f} → {r1:.0f} имп/с ({d:+.2f} %) | "
              f"{'да' if abs(d) <= 3 else 'НЕТ'} |")
    p('')

    if a.model:
        p('## Сверка с моделью run.py (тот же порог и σ, что у replay; unfold — --front 2 --eta 1/32)\n')
        p('| порог | файл | replay выкл / модель fw | расх. % | replay вкл / модель unfold | расх. % |')
        p('|---|---|---|---|---|---|')
        here = os.path.dirname(os.path.abspath(__file__))
        for thr in thrs:
            for name in files:
                if name == 'am_only':
                    continue          # 31 000 порций — долго; сверка по mix и cs_only
                row = []
                for on, mode in ((0, 'fw'), (1, 'unfold')):
                    s = R[(thr, on, name)]
                    pref = os.path.join(a.out, f'model_{mode}_t{thr}_{name}')
                    if not a.no_run or not os.path.exists(pref + '.txt'):
                        cmd = [sys.executable, '-I', os.path.join(here, 'run.py'), '--tpl', a.tpl,
                               '--mode', mode, '--thr', str(int(s['thr'])), '--sigma', f"{s['sigma']:.3f}",
                               '--out', pref, os.path.join(a.data, name + '.bin')]
                        if mode == 'unfold':
                            cmd += ['--shape', a.shape, '--front', '2', '--eta', '0.03125']
                        r = subprocess.run(cmd, capture_output=True)
                        with open(pref + '.txt', 'w', encoding='utf-8') as f:
                            f.write(' '.join(cmd) + '\n' + r.stdout.decode('utf-8', 'replace') +
                                    r.stderr.decode('utf-8', 'replace'))
                    with open(pref + '.txt', encoding='utf-8') as f:
                        mm = re.search(r'принято (\d+)', f.read())
                    macc = int(mm.group(1))
                    row.append((s['acc'], macc, 100 * (s['acc'] / macc - 1)))
                p(f"| {'авто' if thr == 0 else thr} | {name} | {row[0][0]:.0f} / {row[0][1]} | {row[0][2]:+.2f} | "
                  f"{row[1][0]:.0f} / {row[1][1]} | {row[1][2]:+.2f} |")
        p('')

    txt = '\n'.join(L)
    print(txt)
    with open(os.path.join(a.out, 'accept.md'), 'w', encoding='utf-8') as f:
        f.write(txt + '\n')


if __name__ == '__main__':
    main()
