#!/bin/sh
# Полный набор прогонов стороны A. 125 Мвыб/с, 1 Мимп/с, зёрна 1..5, 4 мс каждое.
# Запуск из этого каталога:  sh run_all.sh   (≈ 20–30 мин на 12 ядрах)
set -e
cd "$(dirname "$0")"
P=python
# 1. исходные алгоритмы (воспроизведение стендов)
$P arena.py tia                          --json out/tia_orig.json
$P arena.py csp                          --json out/csp_orig.json
# 2. доработка ЗЧУ по шагам
$P arena.py csp --lm-fix                 --json out/csp_lmfix.json
$P arena.py csp --solver vp --detector lmax              --json out/csp_vp.json
$P arena.py csp --solver vp --detector lmax --redetect 2 --json out/csp_vp_rd.json
# 3. тот же алгоритм — в ветку TIA
$P arena.py tia --solver vp                              --json out/tia_vp.json
$P arena.py tia --solver vp --redetect 2                 --json out/tia_vp_rd.json
# 4. систематика td, одинаковая для обеих, лучшие алгоритмы обеих
for S in static:+4 static:-4 drift:4; do
  T=$(echo $S | tr ':+' '_p')
  $P arena.py tia --solver vp --redetect 2 --sys $S                  --json out/tia_vp_rd_$T.json
  $P arena.py csp --solver vp --detector lmax --redetect 2 --sys $S  --json out/csp_vp_rd_$T.json
  $P arena.py tia --sys $S                                           --json out/tia_orig_$T.json
done
# 5. медленная компонента 5 % / 1 мкс (баллистический дефицит), обе
$P arena.py tia --solver vp --redetect 2 --slow 0.05                 --json out/tia_vp_rd_slow5.json
$P arena.py csp --solver vp --detector lmax --redetect 2 --slow 0.05 --json out/csp_vp_rd_slow5.json
# 6. рабочая точка TIA по D4 (заряд 7.2 пКл) и шумовое усиление e_n
$P arena.py tia --solver vp --redetect 2 --q 0.33333 --cin 6e-12     --json out/tia_vp_rd_q3cin.json
