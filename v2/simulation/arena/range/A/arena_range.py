# -*- coding: utf-8 -*-
"""Арена «диапазон АЦП», сторона A: меньшая рампа + чаще сбросы против делителя ÷2.2.

Все варианты гоняются на ОДНИХ И ТЕХ ЖЕ сидах: поток событий и аналоговый шум
предусилителя совпадают бит в бит (сброс и АЦП не трогают генератор событий).
Собственный шум АЦП берётся из отдельного генератора с тем же сидом во всех вариантах.

Варианты (fs = 125 Мвыб/с, 16 бит, Cf = 100 пФ, окно сброса 1 мкс — как в mc_csp.py):
  REF  рампа 4.0 В, АЦП 4.4 В, делителя нет  — нынешнее ТЗ (такого АЦП на 125 М нет)
  B    рампа 4.0 В, делитель ÷2.2, АЦП 2.0 В  — противник
  A    рампа 1.55 В, АЦП 2.0 В, делителя нет  — моя позиция (1.55+0.38+0.05 = 1.98 В)
Модели шума АЦП:
  ideal  — только квантование (как в mc_csp.py)
  real   — + собственный шум АЦП SNR = 77 dBFS при 2 В п-п ⇒ 100 мкВ rms на входе АЦП;
           для гипотетического АЦП 4.4 В тот же SNR в dBFS ⇒ 220 мкВ. Применено ко ВСЕМ.

python arena_range.py [--seeds 5] [--dur 4e-3] [--rate 1e6] [--noise ideal,real] [--blank 1e-6]
"""
import argparse, json, os, sys, time
from multiprocessing import Pool
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import csp_core as C

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

SNR_DBFS = 77.0
def adc_noise(v_fs):  # rms на входе АЦП, синус полной шкалы v_fs п-п
    return v_fs / (2 * np.sqrt(2)) * 10 ** (-SNR_DBFS / 20)

VARIANTS = {
    'REF': dict(v_ramp=4.0, v_fs=4.4, k_div=1.0, v_lo=-0.05),
    'B':   dict(v_ramp=4.0, v_fs=2.0, k_div=1 / 2.2, v_lo=-0.05 / 2.2),
    'A':   dict(v_ramp=1.55, v_fs=2.0, k_div=1.0, v_lo=-0.05),
}

def job(args):
    name, nm, seed, rate, dur, blank, extra, ra, qr, snr = args
    global SNR_DBFS
    SNR_DBFS = snr
    p = dict(VARIANTS[name]); p.update(extra)
    p['adc_noise'] = adc_noise(p['v_fs']) if nm == 'real' else 0.0
    f_pp, cont = C.cs_spectrum()
    fs = 125e6
    r = C.run(fs, fs / 2.5, rate, dur, f_pp, cont, refine=True, t_blank=blank, seed=seed, reset_aware=ra, quiet_reset=qr, **p)
    return name, nm, seed, r

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seeds', type=int, default=5)
    ap.add_argument('--dur', type=float, default=4e-3)
    ap.add_argument('--rate', type=float, default=1e6)
    ap.add_argument('--noise', default='ideal,real')
    ap.add_argument('--blank', type=float, default=1e-6)
    ap.add_argument('--variants', default='REF,B,A')
    ap.add_argument('--ramp', type=float, default=None, help='переопределить рампу варианта A')
    ap.add_argument('--tag', default='')
    ap.add_argument('--quiet-reset', action='store_true', help='тихий сброс (ко ВСЕМ вариантам)')
    ap.add_argument('--reset-aware', action='store_true', help='обрезать окна фита по сбросу (ко ВСЕМ вариантам)')
    ap.add_argument('--snr', type=float, default=77.0, help='SNR АЦП, dBFS (ко всем вариантам)')
    a = ap.parse_args()
    global SNR_DBFS
    SNR_DBFS = a.snr
    extraA = {} if a.ramp is None else {'v_ramp': a.ramp}
    seeds = [20261009 + k for k in range(a.seeds)]
    jobs = [(v, nm, s, a.rate, a.dur, a.blank, extraA if v == 'A' else {}, a.reset_aware, a.quiet_reset, a.snr)
            for nm in a.noise.split(',') for v in a.variants.split(',') for s in seeds]
    t0 = time.time()
    with Pool(min(len(jobs), os.cpu_count() or 4)) as pool:
        res = pool.map(job, jobs)
    print('fs 125 Мвыб/с, %.0f имп/с, %g мс × %d сидов, окно сброса %.2f мкс, кластеры у сброса: %s, %.0f с'
          % (a.rate, a.dur * 1e3, a.seeds, a.blank * 1e6, 'обрезаются' if a.reset_aware else 'выбрасываются (как mc_csp.py)', time.time() - t0))
    print('SNR АЦП %.1f dBFS ⇒ шум АЦП %.0f мкВ rms при 2 В п-п (real)' % (a.snr, adc_noise(2.0) * 1e6))
    print('тихий сброс: %s' % ('да (мягкий порог −0.1 В, ко всем)' if a.quiet_reset else 'нет (как mc_csp.py)'))
    print('%-4s %-6s | %-15s %-15s %-15s | %6s %7s %6s %6s %6s %5s' %
          ('вар', 'шумАЦП', 'фит FWHM, %', 'светим., %', '2 средн., %', 'сбросы', 'мёртв%',
           'σ,мкВ', 'LSB', 'σ/LSB', 'clip'))
    summ = {}
    for nm in a.noise.split(','):
        for v in a.variants.split(','):
            rr = [r for (n_, m_, s_, r) in res if n_ == v and m_ == nm and r]
            fw = np.array([r['fit']['fwhm'] for r in rr])
            ef = np.array([100 * r['fit']['eff'] for r in rr])
            ta = np.array([r['twoavg']['fwhm'] or np.nan for r in rr])
            u = rr[0]['_']
            sig_in = np.hypot(u['sigma_out_uV'], u['adc_noise_uV'] / u['k_div'])  # к выходу ЗЧУ
            row = dict(fwhm=fw.mean(), fwhm_sem=fw.std(ddof=1) / np.sqrt(len(fw)),
                       eff=ef.mean(), eff_sem=ef.std(ddof=1) / np.sqrt(len(ef)),
                       twoavg=np.nanmean(ta), resets=np.mean([r['_']['resets'] for r in rr]),
                       dead=np.mean([r['_']['deadtime_pct'] for r in rr]),
                       clip=int(sum(r['_']['n_clip'] for r in rr)),
                       sigma_in_uV=sig_in, lsb_in_uV=u['lsb_uV'] / u['k_div'],
                       noise_over_lsb=u['noise_over_lsb'],
                       ymax=max(r['_']['y_an_max'] for r in rr),
                       per_seed_fwhm=fw.tolist(), per_seed_eff=ef.tolist())
            summ['%s/%s' % (v, nm)] = row
            print('%-4s %-6s | %5.2f ± %4.2f    %5.2f ± %4.2f    %6.2f          | %6.1f %6.2f %6.1f %6.1f %5.2f %5d'
                  % (v, nm, row['fwhm'], row['fwhm_sem'], row['eff'], row['eff_sem'], row['twoavg'],
                     row['resets'], row['dead'], sig_in, row['lsb_in_uV'], row['noise_over_lsb'], row['clip']))
    print('σ и LSB приведены к выходу ЗЧУ; σ/LSB — на входе АЦП (критерий дизера ≳0.4)')
    # парные разности по сидам (общие случайные числа)
    for nm in a.noise.split(','):
        if 'A/' + nm in summ and 'B/' + nm in summ:
            d = np.array(summ['A/' + nm]['per_seed_fwhm']) - np.array(summ['B/' + nm]['per_seed_fwhm'])
            e = np.array(summ['A/' + nm]['per_seed_eff']) - np.array(summ['B/' + nm]['per_seed_eff'])
            print('A − B [%s]: ΔFWHM = %+.2f ± %.2f п.п., Δсветимость = %+.2f ± %.2f п.п. (парно по сидам)'
                  % (nm, d.mean(), d.std(ddof=1) / np.sqrt(len(d)), e.mean(), e.std(ddof=1) / np.sqrt(len(e))))
    os.makedirs(os.path.join(HERE, 'out'), exist_ok=True)
    fn = os.path.join(HERE, 'out', 'arena_range%s_%g%s.json' % (a.tag, a.rate, ('_ra' if a.reset_aware else '') + ('_qr' if a.quiet_reset else '') + ('_snr%g' % a.snr if a.snr != 77 else '')))
    json.dump(summ, open(fn, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print('->', fn)

if __name__ == '__main__':
    main()
