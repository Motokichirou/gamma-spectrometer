#!/bin/sh
# Основная таблица ветки TIA: лучший алгоритм (блочный VP + повторный поиск),
# рабочая точка D4 (q=1/3, Cin=6 пФ), тепловой шум АЦП 73 дБFS. 5 зёрен x 4 мс.
cd "$(dirname "$0")"
for fs in 40e6 65e6 125e6; do
  for r in 3e5 1e6; do
    python arena.py tia --solver vp --redetect 2 --q 0.33333 --cin 6e-12 --adc-snr 73 \
      --fs $fs --rate $r --json out/tia_fs${fs}_r${r}.json > out/tia_fs${fs}_r${r}.txt
  done
done
echo DONE > out/DONE_main
