#!/bin/sh
# Такты детектора на эмуляторе Cortex-M4: ДО (tests/detector_ref.c) и ПОСЛЕ (src/core/detector.c).
# Запуск из любого места: sh firmware/gamma_fw/tools/cyc/run.sh [--equiv] [--only new|ref] [--rate N] ...
# Первый запуск ставит unicorn 2.1.1, capstone 5.0.1 и numpy в tools/cyc/.venv (в .gitignore).
set -e
cd "$(dirname "$0")"
PY=.venv/Scripts/python.exe
[ -x "$PY" ] || PY=.venv/bin/python
if [ ! -x "$PY" ]; then
  python -I -m venv .venv
  PY=.venv/Scripts/python.exe; [ -x "$PY" ] || PY=.venv/bin/python
  "$PY" -I -m pip install -q unicorn==2.1.1 capstone==5.0.1 numpy
fi
exec "$PY" -I cyc.py "$@"
