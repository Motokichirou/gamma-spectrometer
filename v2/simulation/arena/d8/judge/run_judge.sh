#!/bin/sh
cd "$(dirname "$0")"
P=python
# воспроизведение A (код судьи без --adc-snr совпадает с A)
$P arena.py csp --no-refine                                > out/r_csp_norefine.txt
$P arena.py csp                                            > out/r_csp_orig.txt
$P arena.py csp --solver vp --detector lmax --redetect 2   > out/r_csp_best.txt
$P arena.py tia --solver vp --redetect 2                   > out/r_tia_best.txt
# перекрёстная проверка: лучший алгоритм каждой ветви + тепловой шум АЦП (модель B)
$P arena.py csp --solver vp --detector lmax --redetect 2 --adc-snr 78 > out/x_csp_best_adc78.txt
$P arena.py tia --solver vp --redetect 2 --q 0.33333 --cin 6e-12 --adc-snr 73 > out/x_tia_best_phys_adc73.txt
$P arena.py csp --solver vp --detector lmax --redetect 2 --adc-snr 74 > out/x_csp_best_adc74.txt
$P arena.py tia --solver vp --redetect 2 --q 0.33333 --cin 6e-12 --adc-snr 74 > out/x_tia_best_phys_adc74.txt
echo DONE > out/DONE
