#!/bin/sh
cd "$(dirname "$0")"
P=python
$P arena.py csp --solver vp --detector lmax --redetect 2 > out/p_csp_best.txt
$P arena.py tia --solver vp --redetect 2 --q 0.33333 --cin 6e-12 > out/p_tia_best_phys.txt
$P arena.py csp --solver vp --detector lmax --redetect 2 --adc-snr 78 --nav 8 > out/x_csp_best_adc78_nav8.txt
$P arena.py csp --solver vp --detector lmax --redetect 2 --adc-snr 78 --vfs 2.0 --vramp 1.6 > out/x_csp_best_adc78_span2.txt
$P arena.py csp --solver vp --detector lmax --redetect 2 --adc-snr 78 --redetect 4 > out/x_csp_best_adc78_rd4.txt
$P arena.py tia --solver vp --redetect 2 --q 0.33333 --cin 6e-12 --adc-snr 73 --sys drift:4 --fit-td > out/x_tia_phys_adc73_drift_fittd.txt
$P arena.py csp --solver vp --detector lmax --redetect 2 --adc-snr 78 --sys drift:4 > out/x_csp_adc78_drift.txt
$P arena.py tia --solver vp --redetect 2 --q 0.33333 --cin 6e-12 --adc-snr 73 --sys drift:4 > out/x_tia_phys_adc73_drift.txt
echo DONE > out/DONE2
