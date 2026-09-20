"""Запуск LTspice и разбор .raw/.log (копия приёмов из fix_c320/run_spice.py)."""
import os
import re
import subprocess

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
LTSPICE = r'C:\Program Files\ADI\LTspice\LTspice.exe'


def write(name, lines):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + '.cir')
    with open(path, 'w', encoding='ascii') as f:
        f.write('\n'.join(lines + ['.end']) + '\n')
    return path


BELOW_NORMAL = 0x00004000       # чтобы расчёт не мешал работе за ПК


def _free_cpu0(proc):
    """LTspice не занимает CPU0 — одно ядро остаётся системе."""
    try:
        import ctypes
        n = os.cpu_count() or 4
        mask = ((1 << n) - 1) & ~1
        h = ctypes.windll.kernel32.OpenProcess(0x0200 | 0x0400, False, proc.pid)  # SET_INFO | QUERY_INFO
        if h:
            ctypes.windll.kernel32.SetProcessAffinityMask(ctypes.c_void_p(h), ctypes.c_size_t(mask))
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(h))
    except Exception:
        pass


def run(paths, par=4):
    for i in range(0, len(paths), par):
        ps = [subprocess.Popen([LTSPICE, '-b', p], cwd=OUT, creationflags=BELOW_NORMAL)
              for p in paths[i:i + par]]
        for q in ps:
            _free_cpu0(q)
        for q in ps:
            q.wait()


def read_raw(path):
    b = open(path, 'rb').read()
    for nl in ('\n', '\r\n'):
        mk = ('Binary:' + nl).encode('utf-16-le')
        pos = b.find(mk)
        if pos >= 0:
            break
    hdr = b[:pos].decode('utf-16-le')
    npts = int(re.search(r'No\. Points:\s*(\d+)', hdr).group(1))
    names = re.findall(r'^\s*\d+\s+(\S+)\s+\S+\s*$', re.split(r'^Variables:', hdr, flags=re.M)[1], re.M)
    dbl = 'double' in re.search(r'^Flags:(.*)$', hdr, re.M).group(1)
    ft = '<f8' if dbl else '<f4'
    dt = np.dtype([('x', '<f8')] + [(f'v{i}', ft) for i in range(len(names) - 1)])
    a = np.frombuffer(b[pos + len(mk):], dtype=dt, count=npts)
    d = {names[0].lower(): np.abs(a['x'])}
    for i, n in enumerate(names[1:]):
        d[n.lower()] = a[f'v{i}'].astype(float)
    return d


def read_log(path):
    b = open(path, 'rb').read()
    txt = b.decode('utf-16-le', errors='replace') if b[1:2] == b'\x00' else b.decode('latin-1')
    m = {}
    for g in re.finditer(r'^\s*(\w+):\s*[^=\n]*=\s*([-+]?[\d.]+(?:[eE][-+]?\d+)?)', txt, re.M):
        m[g.group(1).lower()] = float(g.group(2))
    return m, txt


def uniform(t, y, dt):
    """Пересэмплинг на равномерную сетку (шаг LTspice переменный)."""
    tu = np.arange(t[0], t[-1], dt)
    return tu, np.interp(tu, t, y)
