"""
analyze_sim1.py  —  LTspice .raw parser + AD8000 AC analysis
Gamma spectrometer ADC/MCU board v1.4

Handles LTspice 24/26 format: UTF-16 LE header, double-precision binary.

Usage:
    python simulation/analyze_sim1.py
    python simulation/analyze_sim1.py simulation/sim1_ad8000.raw
"""

import struct
import math
import sys
import os

# Force UTF-8 output — avoids cp1251 crash on Russian/special chars
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')


def read_raw(filepath):
    """Parse LTspice .raw file (AC or transient), return header dict + data."""
    with open(filepath, 'rb') as f:
        raw = f.read()

    # LTspice 24/26: header is UTF-16 LE, "Binary:" marker also UTF-16 LE
    marker_utf16 = 'Binary:\n'.encode('utf-16-le')
    marker_pos = raw.find(marker_utf16)

    if marker_pos != -1:
        header_bytes = raw[:marker_pos]
        data_start = marker_pos + len(marker_utf16)
        header_text = header_bytes.decode('utf-16-le', errors='replace')
    else:
        # Fallback: ASCII header (LTspice XVII)
        marker_ascii = b'Binary:\n'
        marker_pos = raw.find(marker_ascii)
        if marker_pos == -1:
            raise ValueError("Binary: marker not found — not a valid .raw file")
        header_bytes = raw[:marker_pos]
        data_start = marker_pos + len(marker_ascii)
        header_text = header_bytes.decode('latin-1', errors='replace')

    # Parse header lines
    info = {
        'flags': '',
        'n_vars': 0,
        'n_points': 0,
        'variables': [],   # list of (index, name, type)
    }

    in_vars = False
    for line in header_text.splitlines():
        line = line.strip()
        if not line:
            continue
        lo = line.lower()
        if lo.startswith('flags:'):
            info['flags'] = lo.split(':', 1)[1].strip()
        elif lo.startswith('no. variables:'):
            info['n_vars'] = int(lo.split(':', 1)[1].strip())
        elif lo.startswith('no. points:'):
            info['n_points'] = int(lo.split(':', 1)[1].strip())
        elif lo.startswith('variables:'):
            in_vars = True
        elif in_vars:
            parts = line.split()
            if len(parts) >= 3:
                try:
                    idx = int(parts[0])
                    info['variables'].append((idx, parts[1], parts[2]))
                except ValueError:
                    pass

    n_vars   = info['n_vars']
    n_points = info['n_points']
    is_complex = 'complex' in info['flags']

    data_bytes = raw[data_start:]

    # Determine precision: check bytes per point
    # complex float:  freq=8B + (n_vars-1)*8B   => n_vars*8
    # complex double: freq=8B + (n_vars-1)*16B  OR all vars = n_vars*16B
    expected_float_total  = n_points * n_vars * 8
    expected_double_total = n_points * n_vars * 16

    tol = 32  # allow small header alignment padding
    if abs(len(data_bytes) - expected_double_total) < tol:
        bytes_per_var = 16
        is_double = True
    else:
        bytes_per_var = 8
        is_double = False

    bytes_per_point = n_vars * bytes_per_var

    if is_complex:
        freqs = []
        # dict: varname -> list of complex values
        var_names = [v[1] for v in info['variables']]
        data = {name: [] for name in var_names[1:]}  # skip 'frequency'

        for i in range(n_points):
            offset = i * bytes_per_point
            if offset + bytes_per_point > len(data_bytes):
                break
            # Frequency (always first variable)
            if is_double:
                freq_re, freq_im = struct.unpack_from('<dd', data_bytes, offset)
            else:
                freq_re = struct.unpack_from('<d', data_bytes, offset)[0]
            freqs.append(freq_re)

            for j, name in enumerate(var_names[1:]):
                var_offset = offset + bytes_per_var * (j + 1)
                if is_double:
                    re, im = struct.unpack_from('<dd', data_bytes, var_offset)
                else:
                    re, im = struct.unpack_from('<ff', data_bytes, var_offset)
                data[name].append(complex(re, im))

        return info, freqs, data

    return info, None, None


def analyze_ac_gain(freqs, data):
    """Print key metrics for AD8000 AC sweep. Returns True if all pass."""

    # Find V(vout) — try exact then case-insensitive
    target = 'V(vout)'
    varname = None
    for k in data:
        if k == target:
            varname = k
            break
    if varname is None:
        for k in data:
            if k.lower() == target.lower():
                varname = k
                break
    if varname is None:
        print(f"  ERROR: '{target}' not found. Available: {list(data.keys())}")
        return False

    vals = data[varname]
    mags = [abs(v) for v in vals]
    mags_db = [20 * math.log10(m) if m > 1e-15 else -300.0 for m in mags]

    def idx_nearest(f_target):
        return min(range(len(freqs)), key=lambda i: abs(freqs[i] - f_target))

    i_1M   = idx_nearest(1e6)
    i_10M  = idx_nearest(10e6)
    i_100M = idx_nearest(100e6)
    i_200M = idx_nearest(200e6)

    # Midband: gain at 1 MHz (well below rolloff)
    midband_db = mags_db[i_1M]
    expected_db = 20 * math.log10(15)  # = 23.52 dB

    # Peaking: maximum gain above midband
    peak_db  = max(mags_db)
    i_peak   = mags_db.index(peak_db)
    peaking  = peak_db - midband_db

    # -3 dB frequency (search from midband outward)
    threshold = midband_db - 3.0
    f_3db = None
    for i in range(i_1M, len(freqs) - 1):
        if mags_db[i] >= threshold > mags_db[i + 1]:
            f1, f2 = freqs[i], freqs[i + 1]
            d1, d2 = mags_db[i], mags_db[i + 1]
            t = (threshold - d1) / (d2 - d1)
            f_3db = f1 * (f2 / f1) ** t
            break

    print("\n" + "=" * 55)
    print("  AD8000 AC Analysis — sim1_ad8000")
    print("=" * 55)
    print(f"  Points:          {len(freqs)}")
    print(f"  Freq range:      {freqs[0]/1e3:.0f} kHz … {freqs[-1]/1e6:.0f} MHz")
    print()
    print(f"  Gain @ 1 MHz:    {mags_db[i_1M]:.2f} dB  ({mags[i_1M]:.3f} V/V)")
    print(f"  Gain @ 10 MHz:   {mags_db[i_10M]:.2f} dB")
    print(f"  Gain @ 100 MHz:  {mags_db[i_100M]:.2f} dB")
    print(f"  Gain @ 200 MHz:  {mags_db[i_200M]:.2f} dB")
    print(f"  Peak gain:       {peak_db:.2f} dB @ {freqs[i_peak]/1e6:.1f} MHz")
    print(f"  Peaking:         {peaking:+.2f} dB  (above midband)")
    if f_3db:
        print(f"  -3 dB freq:      {f_3db/1e6:.1f} MHz")
    else:
        print(f"  -3 dB freq:      > {freqs[-1]/1e6:.0f} MHz (not reached)")

    print()
    print("  VERDICT:")
    all_pass = True

    # Check 1: midband gain
    gain_err = abs(mags_db[i_1M] - expected_db)
    if gain_err < 0.5:
        print(f"  [PASS] Gain @ 1 MHz: {mags_db[i_1M]:.2f} dB  (expected {expected_db:.1f} dB)")
    else:
        print(f"  [FAIL] Gain @ 1 MHz: {mags_db[i_1M]:.2f} dB  (expected {expected_db:.1f} dB)")
        all_pass = False

    # Check 2: peaking
    if peaking <= 0.5:
        print(f"  [PASS] Peaking {peaking:+.2f} dB  (target ≤ 0.5 dB)")
    elif peaking <= 1.0:
        print(f"  [WARN] Peaking {peaking:+.2f} dB  (target ≤ 0.5 dB) — try Cfb = 2p")
    else:
        print(f"  [FAIL] Peaking {peaking:+.2f} dB  (target ≤ 0.5 dB) — increase Cfb")
        all_pass = False

    # Check 3: bandwidth
    if f_3db and f_3db >= 100e6:
        print(f"  [PASS] BW = {f_3db/1e6:.0f} MHz  (required ≥ 100 MHz)")
    elif f_3db:
        print(f"  [WARN] BW = {f_3db/1e6:.0f} MHz  (required ≥ 100 MHz)")
    else:
        print(f"  [PASS] BW > {freqs[-1]/1e6:.0f} MHz  (required ≥ 100 MHz)")

    print("=" * 55)
    return all_pass


def read_raw_stepped(filepath, points_per_step):
    """Read a stepped .raw file, return list of (freqs, data) per step."""
    info, freqs_all, data_all = read_raw(filepath)
    n_total = len(freqs_all)
    n_steps = n_total // points_per_step

    steps = []
    var_names = [v[1] for v in info['variables']][1:]  # skip 'frequency'
    for s in range(n_steps):
        start = s * points_per_step
        end   = start + points_per_step
        freqs_s = freqs_all[start:end]
        data_s  = {k: data_all[k][start:end] for k in var_names}
        steps.append((freqs_s, data_s))
    return info, steps


if __name__ == '__main__':
    raw_path = sys.argv[1] if len(sys.argv) > 1 else 'simulation/sim1_ad8000.raw'

    if not os.path.exists(raw_path):
        print(f"File not found: {raw_path}")
        print("Run LTspice batch first:")
        print('  & "C:\\Program Files\\ADI\\LTspice\\LTspice.exe" -b simulation\\sim1_ad8000.cir')
        sys.exit(1)

    print(f"Reading: {raw_path}  ({os.path.getsize(raw_path)} bytes)")
    info, freqs, data = read_raw(raw_path)

    if freqs is None:
        print("Not an AC simulation or parse failed.")
        sys.exit(1)

    is_stepped = 'stepped' in info['flags']
    print(f"  Vars: {info['n_vars']}  Points: {info['n_points']}  Stepped: {is_stepped}")

    if is_stepped:
        # Parametric sweep: Cfb_val list 1f 0.5p 1p 2p  (4 steps, 401 pts each)
        POINTS_PER_STEP = 401
        STEP_LABELS = ['Cfb=none (1f)', 'Cfb=0.5p', 'Cfb=1p (nominal)', 'Cfb=2p']

        info2, steps = read_raw_stepped(raw_path, POINTS_PER_STEP)
        print(f"  Steps detected: {len(steps)}\n")

        all_pass = True
        # Table header
        print(f"  {'Step':<22} {'G@1MHz':>8} {'Peak':>8} {'Peaking':>9} {'BW-3dB':>9}")
        print(f"  {'-'*22} {'-'*8} {'-'*8} {'-'*9} {'-'*9}")

        for i, (label, (freqs_s, data_s)) in enumerate(zip(STEP_LABELS, steps)):
            vals = data_s.get('V(vout)', [])
            if not vals:
                continue
            mags = [abs(v) for v in vals]
            mags_db = [20 * math.log10(m) if m > 1e-15 else -300.0 for m in mags]

            def idx_near(f):
                return min(range(len(freqs_s)), key=lambda i: abs(freqs_s[i] - f))

            mb = mags_db[idx_near(1e6)]
            peak_db = max(mags_db)
            peaking = peak_db - mb
            thr = mb - 3.0
            f3db = None
            for j in range(idx_near(1e6), len(freqs_s) - 1):
                if mags_db[j] >= thr > mags_db[j+1]:
                    f1, f2 = freqs_s[j], freqs_s[j+1]
                    t = (thr - mags_db[j]) / (mags_db[j+1] - mags_db[j])
                    f3db = f1 * (f2/f1)**t
                    break

            bw_str = f"{f3db/1e6:.0f} MHz" if f3db else ">1GHz"
            flag = "  OK" if peaking <= 0.5 else ("WARN" if peaking <= 1.0 else "FAIL")
            print(f"  {label:<22} {mb:>7.2f}dB {peak_db:>7.2f}dB {peaking:>+8.2f}dB {bw_str:>9}  [{flag}]")

        print()
        print("  Target: Gain 23.5 dB, Peaking <= +0.5 dB at Cfb=1p with Cpar=1p")

    else:
        ok = analyze_ac_gain(freqs, data)
        sys.exit(0 if ok else 1)
