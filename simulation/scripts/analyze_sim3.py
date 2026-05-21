"""
analyze_sim3.py  —  Parse LTspice .noise .raw, report integrated RMS noise.
Gamma spectrometer ADC/MCU board v1.5

Checks:
  - σ_total at V(adc_in) vs budget (target < 150 µV rms for PGA x4)
  - Noise spectral density at key frequencies
  - Dominant noise source identification
"""

import struct, sys, os, math
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')


def read_noise_raw(filepath):
    with open(filepath, 'rb') as f:
        raw = f.read()

    # Find Binary: marker (UTF-16 LE)
    for crlf in [False, True]:
        nl = '\r\n' if crlf else '\n'
        marker = ('Binary:' + nl).encode('utf-16-le')
        pos = raw.find(marker)
        if pos != -1:
            header_text = raw[:pos].decode('utf-16-le', errors='replace')
            data_start  = pos + len(marker)
            break
    else:
        raise ValueError("Binary: marker not found")

    # Parse header
    n_vars, n_points = 0, 0
    flags = ''
    variables = []
    in_vars = False
    for line in header_text.splitlines():
        lo = line.strip().lower()
        if lo.startswith('flags:'):          flags    = lo.split(':', 1)[1].strip()
        elif lo.startswith('no. variables:'): n_vars   = int(lo.split(':', 1)[1].strip())
        elif lo.startswith('no. points:'):    n_points = int(lo.split(':', 1)[1].strip())
        elif lo.startswith('variables:'):     in_vars  = True
        elif in_vars:
            parts = line.strip().split()
            if len(parts) >= 2:
                try:
                    variables.append((int(parts[0]), parts[1]))
                except ValueError:
                    pass

    # Noise .raw: freq = 8-byte double, vars = 4-byte float (real magnitude)
    # Same layout as transient .raw (not AC complex format)
    data_bytes = raw[data_start:]
    bytes_per_point = 8 + (n_vars - 1) * 4

    freqs  = []
    traces = {name: [] for _, name in variables[1:]}
    var_names = [name for _, name in variables]

    for i in range(n_points):
        off = i * bytes_per_point
        if off + bytes_per_point > len(data_bytes):
            break
        freq = struct.unpack_from('<d', data_bytes, off)[0]
        freqs.append(freq)
        for j, name in enumerate(var_names[1:]):
            voff = off + 8 + j * 4
            v = struct.unpack_from('<f', data_bytes, voff)[0]
            traces[name].append(v)

    return n_vars, n_points, freqs, traces


def find_trace(traces, target):
    tlo = target.lower()
    for k in traces:
        if k.lower() == tlo:
            return traces[k], k
    return None, None


def analyze_noise(freqs, traces):
    print()
    print("=" * 58)
    print("  Sim3 — Noise analysis at V(adc_in)")
    print("=" * 58)

    # Find V(onoise) — output noise spectral density
    onoise, found = find_trace(traces, 'V(onoise)')
    if onoise is None:
        # Try without V() wrapper
        for k in traces:
            if 'onoise' in k.lower():
                onoise, found = traces[k], k
                break
    if onoise is None:
        print("\n  V(onoise) not found in traces.")
        print(f"  Available: {list(traces.keys())[:10]}")
        return

    print(f"\n  Trace: {found}  ({len(freqs)} points)")
    print(f"  Freq range: {freqs[0]:.1f} Hz ... {freqs[-1]/1e6:.2f} MHz")

    mag = [abs(float(v)) for v in onoise]

    # Spectral density at key frequencies
    def idx_f(f):
        return min(range(len(freqs)), key=lambda i: abs(freqs[i] - f))

    print("\n  NOISE SPECTRAL DENSITY:")
    for f_hz in [1e3, 10e3, 100e3, 350e3, 700e3, 1e6, 2e6]:
        idx = idx_f(f_hz)
        print(f"    {freqs[idx]/1e3:8.1f} kHz:  {mag[idx]*1e9:.1f} nV/rtHz")

    # Integrate: σ² = ∫ S(f) df  where S(f) = mag²
    # Trapezoidal integration over log-spaced frequency points
    sigma2 = 0.0
    for i in range(1, len(freqs)):
        df = freqs[i] - freqs[i-1]
        s_avg = (mag[i]**2 + mag[i-1]**2) / 2
        sigma2 += s_avg * df
    sigma_total = math.sqrt(sigma2) * 1e6  # µV rms

    # Integration over noise bandwidth only (1 Hz to 2 MHz = Nyquist)
    sigma2_nyq = 0.0
    for i in range(1, len(freqs)):
        if freqs[i] <= 2e6:
            df = freqs[i] - freqs[i-1]
            s_avg = (mag[i]**2 + mag[i-1]**2) / 2
            sigma2_nyq += s_avg * df
    sigma_nyq = math.sqrt(sigma2_nyq) * 1e6

    # Integration over shaper noise bandwidth (10 kHz to 2 MHz)
    sigma2_bw = 0.0
    for i in range(1, len(freqs)):
        if 10e3 <= freqs[i] <= 2e6:
            df = freqs[i] - freqs[i-1]
            s_avg = (mag[i]**2 + mag[i-1]**2) / 2
            sigma2_bw += s_avg * df
    sigma_bw = math.sqrt(sigma2_bw) * 1e6

    # ADC resolution
    vref = 2.5
    n_channels = 8192
    v_per_channel = vref / n_channels
    sigma_channels = sigma_nyq / (v_per_channel * 1e6)
    sigma_fwhm = sigma_channels * 2.355

    print(f"\n  INTEGRATED RMS NOISE:")
    print(f"    Full range (1Hz-2MHz):     {sigma_total:.1f} µV rms")
    print(f"    To Nyquist (1Hz-2MHz):     {sigma_nyq:.1f} µV rms")
    print(f"    Shaper BW (10kHz-2MHz):    {sigma_bw:.1f} µV rms")

    target = 150.0
    ok = sigma_nyq <= target
    print(f"\n    Target: < {target:.0f} µV rms (PGA x4 budget)")
    print(f"    [{'PASS' if ok else 'FAIL'}]  σ = {sigma_nyq:.1f} µV rms")

    print(f"\n  ADC IMPACT (VREF=2.5V, 8192 channels):")
    print(f"    V/channel:   {v_per_channel*1000:.3f} mV")
    print(f"    σ_baseline:  {sigma_channels:.3f} channels rms")
    print(f"    FWHM_noise:  {sigma_fwhm:.3f} channels")
    print(f"    5σ threshold: {sigma_channels*5:.2f} channels  "
          f"({sigma_channels*5 * vref/n_channels * 1000:.2f} mV)")

    print()
    print("=" * 58)


if __name__ == '__main__':
    raw_path = sys.argv[1] if len(sys.argv) > 1 else 'simulation/sim3_noise.raw'
    if not os.path.exists(raw_path):
        print(f"Not found: {raw_path}")
        sys.exit(1)

    print(f"Reading {raw_path}  ({os.path.getsize(raw_path)} bytes)")
    n_vars, n_points, freqs, traces = read_noise_raw(raw_path)
    print(f"  Vars: {n_vars}  Points: {n_points}")
    print(f"  Traces: {list(traces.keys())}")
    analyze_noise(freqs, traces)
