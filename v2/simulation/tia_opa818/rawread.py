# -*- coding: utf-8 -*-
"""Чтение бинарного .raw LTspice (tran / ac / noise), со степами."""
import numpy as np


def read(path):
    b = open(path, 'rb').read()
    key = 'Binary:\n'.encode('utf-16-le')
    i = b.index(key)
    hdr = b[:i].decode('utf-16-le')
    data = b[i + len(key):]
    lines = hdr.split('\n')
    flags = next(l for l in lines if l.startswith('Flags:')).lower()
    nvar = int(next(l for l in lines if l.startswith('No. Variables:')).split(':')[1])
    npts = int(next(l for l in lines if l.startswith('No. Points:')).split(':')[1])
    k = lines.index('Variables:')
    names = [lines[k + 1 + j].split('\t')[2] for j in range(nvar)]
    if 'complex' in flags:
        a = np.frombuffer(data, dtype=np.complex128, count=npts * nvar).reshape(npts, nvar)
    elif 'double' in flags:
        a = np.frombuffer(data, dtype=np.float64, count=npts * nvar).reshape(npts, nvar)
    else:  # tran: время double, остальное float32
        rec = np.dtype([('t', '<f8')] + [('v%d' % j, '<f4') for j in range(1, nvar)])
        r = np.frombuffer(data, dtype=rec, count=npts)
        a = np.empty((npts, nvar))
        a[:, 0] = np.abs(r['t'])
        for j in range(1, nvar):
            a[:, j] = r['v%d' % j]
    # разбиение по степам: ось (время/частота) начинается заново
    x = np.real(a[:, 0])
    cuts = [0] + [j for j in range(1, npts) if x[j] < x[j - 1]] + [npts]
    steps = [a[cuts[s]:cuts[s + 1]] for s in range(len(cuts) - 1)]
    return names, steps
