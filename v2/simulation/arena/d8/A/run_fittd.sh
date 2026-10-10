#!/bin/sh
# Свободная td у каждого события, обе ветви, тот же набор систематик.
set -e
cd "$(dirname "$0")"
P=python
for S in none static:+4 static:-4 drift:4; do
  T=$(echo $S | tr ':+' '_p')
  $P arena.py tia --solver vp --redetect 2 --fit-td --sys $S                 --json out/td_tia_$T.json
  $P arena.py csp --solver vp --detector lmax --redetect 2 --fit-td --sys $S --json out/td_csp_$T.json
done
$P arena.py tia --solver vp --redetect 2 --fit-td --slow 0.05                 --json out/td_tia_slow5.json
$P arena.py csp --solver vp --detector lmax --redetect 2 --fit-td --slow 0.05 --json out/td_csp_slow5.json
echo done
