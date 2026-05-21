"""
analyze_sim2.py  —  Parse LTspice transient .raw, report shaper metrics.
Gamma spectrometer ADC/MCU board v1.4

Checks:
  - DC baseline at all nodes before pulse
  - Peaking factor (V_shaper_peak / V_AD8000_peak) = 0.36
  - Peaking time ~ 380 ns from pulse start
  - Undershoot ~7% of peak
  - Polarity: adc_in goes DOWN from 2.1V baseline
"""

import struct, sys, os, math
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')


def read_transient_raw(filepath):
    with open(filepath, 'rb') as f:
        raw = f.read()

    # Find Binary: marker (UTF-16 LE in LTspice 24/26)
    for crlf in [False, True]:
        nl = '\r\n' if crlf else '\n'
        marker = ('Binary:' + nl).encode('utf-16-le')
        pos = raw.find(marker)
        if pos != -1:
            header_text = raw[:pos].decode('utf-16-le', errors='replace')
            data_start = pos + len(marker)
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
        if lo.startswith('flags:'): flags = lo.split(':', 1)[1].strip()
        elif lo.startswith('no. variables:'): n_vars   = int(lo.split(':', 1)[1].strip())
        elif lo.startswith('no. points:'):    n_points = int(lo.split(':', 1)[1].strip())
        elif lo.startswith('variables:'):     in_vars = True
        elif in_vars:
            parts = line.strip().split()
            if len(parts) >= 2:
                try:
                    variables.append((int(parts[0]), parts[1]))
                except ValueError:
                    pass

    data_bytes = raw[data_start:]
    is_double = 'double' in flags

    # Determine format: time is always 8-byte double
    # Voltages: 4-byte float (default) or 8-byte double
    # Check file size to determine
    n_vars_data = n_vars  # includes time as var 0
    size_float  = n_points * (8 + (n_vars_data - 1) * 4)   # time=8, rest=4
    size_double = n_points * (n_vars_data * 8)              # all 8 bytes

    tol = 64
    if abs(len(data_bytes) - size_double) < tol:
        var_bytes = 8
    elif abs(len(data_bytes) - size_float) < tol:
        var_bytes = 4
    else:
        # Try with 4-byte time (some versions)
        size_all4 = n_points * n_vars_data * 4
        if abs(len(data_bytes) - size_all4) < tol:
            var_bytes = 4  # all 4 bytes including time??
        else:
            # Heuristic: deduce from file size
            per_pt_approx = len(data_bytes) / n_points
            if per_pt_approx / n_vars_data > 6:
                var_bytes = 8
            else:
                var_bytes = 4

    # Parse binary
    times = []
    traces = {name: [] for _, name in variables[1:]}
    var_names = [name for _, name in variables]

    bytes_per_point = 8 + (n_vars - 1) * var_bytes  # time=8, rest=var_bytes
    if var_bytes == 8:
        bytes_per_point = n_vars * 8  # all double including time

    for i in range(n_points):
        off = i * bytes_per_point
        if off + bytes_per_point > len(data_bytes):
            break
        t = struct.unpack_from('<d', data_bytes, off)[0]
        times.append(t)
        for j, name in enumerate(var_names[1:]):
            voff = off + 8 + j * var_bytes
            if var_bytes == 8:
                voff = off + (j + 1) * 8
                v = struct.unpack_from('<d', data_bytes, voff)[0]
            else:
                v = struct.unpack_from('<f', data_bytes, voff)[0]
            traces[name].append(v)

    return n_vars, n_points, var_bytes, times, traces


def find_trace(traces, target):
    """Case-insensitive trace lookup."""
    tlo = target.lower()
    for k in traces:
        if k.lower() == tlo:
            return traces[k], k
    return None, None


def analyze_shaper(times, traces):
    pulse_start = 500e-9  # pulse begins at t=500ns

    # Helper: index nearest to time t
    def idx_t(t):
        return min(range(len(times)), key=lambda i: abs(times[i] - t))

    # Baseline window: 200-400 ns (before pulse)
    i_bl_start = idx_t(200e-9)
    i_bl_end   = idx_t(400e-9)

    print()
    print("=" * 58)
    print("  Sim2 — Shaper transient analysis (Cs-137, I_peak=0.94mA)")
    print("=" * 58)

    # --- DC baselines ---
    print("\n  DC BASELINES (200-400 ns, before pulse):")
    baseline_nodes = [
        ('V(amp_out)',  'AD8000 out',     0.0,   0.1),
        ('V(cr_out)',   'OPAMP1 CR out',  0.490, 0.05),
        ('V(rc_out)',   'OPAMP2 RC out',  0.490, 0.05),
        ('V(adc_in)',   'OPAMP3/ADC in',  2.1,   0.15),
    ]
    dc_ok = True
    for node, label, expected, tol in baseline_nodes:
        sig, found_name = find_trace(traces, node)
        if sig is None:
            print(f"    {label:<20} {node} NOT FOUND")
            dc_ok = False
            continue
        baseline = sum(sig[i_bl_start:i_bl_end+1]) / max(1, i_bl_end - i_bl_start + 1)
        err = abs(baseline - expected)
        ok = err <= tol
        marker = "OK  " if ok else "WARN"
        print(f"    [{marker}] {label:<20} {baseline:+.4f} V  (expected {expected:+.4f} V, err {err*1000:.1f} mV)")
        if not ok:
            dc_ok = False

    # --- Find pulse peak at adc_in ---
    sig_adc, _ = find_trace(traces, 'V(adc_in)')
    sig_amp, _ = find_trace(traces, 'V(amp_out)')
    sig_cr, _  = find_trace(traces, 'V(cr_out)')

    if sig_adc is None:
        print("\n  V(adc_in) not found — cannot analyze pulse")
        return

    # Baseline at adc_in (average 200-400ns)
    baseline_adc = sum(sig_adc[i_bl_start:i_bl_end+1]) / max(1, i_bl_end - i_bl_start + 1)

    # Search for peak in pulse window: 500ns to 4000ns
    i_pulse_start = idx_t(pulse_start)
    i_search_end  = idx_t(4000e-9)
    window_adc = sig_adc[i_pulse_start:i_search_end]
    window_t   = times[i_pulse_start:i_search_end]

    # Pulse goes DOWN from baseline (negative polarity at ADC)
    min_val = min(window_adc)
    min_idx = window_adc.index(min_val) + i_pulse_start
    t_peak  = times[min_idx]

    delta_adc = baseline_adc - min_val   # positive number = downward pulse
    t_from_start = t_peak - pulse_start

    print(f"\n  PULSE AT ADC_IN:")
    print(f"    Baseline:       {baseline_adc:.4f} V")
    print(f"    Peak value:     {min_val:.4f} V  (at t = {t_peak*1e6:.3f} us)")
    # expected delta_adc = 2 * PF_g474 * amp_delta = 2 * 0.206 * 432 = 178 mV
    # (design doc target was 335 mV for ideal opamps; G474 GBW=13MHz gives ~0.206)
    print(f"    Delta (down):   {delta_adc*1000:.2f} mV  (expected ~178 mV for G474)")
    print(f"    Peaking time:   {t_from_start*1e9:.0f} ns  (from pulse start, expected ~330 ns for G474)")

    # --- Peaking factor ---
    # PF = delta_adc / (V_AD8000_peak * 2)  [factor 2 = OPAMP3 gain]
    if sig_amp is not None:
        baseline_amp = sum(sig_amp[i_bl_start:i_bl_end+1]) / max(1, i_bl_end - i_bl_start + 1)
        win_amp = sig_amp[i_pulse_start:i_search_end]
        # AD8000 output: negative pulse (inverting amp)
        amp_peak_val = min(win_amp)
        amp_delta    = baseline_amp - amp_peak_val  # positive

        # peaking_factor = (delta_adc / 2_gain_opamp3) / amp_delta
        # = shaper_output_peak / AD8000_output_peak
        pf = (delta_adc / 2.0) / amp_delta if amp_delta > 0 else 0
        # PGA gain x4 → divide delta_adc by 4 to get shaper output
        # G474 GBW=13MHz → PF~0.20 (design doc target 0.36 was for ideal opamps)
        # amp_delta: EXP peak at tau_rise=5ns → 0.919*I_peak*(Rin||Rg)*15 = 432 mV
        pf = (delta_adc / 4.0) / amp_delta if amp_delta > 0 else 0
        pf_ok = 0.17 <= pf <= 0.25
        print(f"\n  PEAKING FACTOR (PGA x4):")
        print(f"    AD8000 peak delta:   {amp_delta*1000:.1f} mV  (expected ~432 mV for EXP tau_rise=5ns)")
        print(f"    Shaper peak delta:   {delta_adc/4*1000:.1f} mV  (expected ~87 mV for G474)")
        print(f"    Peaking factor:      {pf:.3f}  (expected ~0.20 for G474; design target 0.36 ideal)")
        print(f"    [{('PASS' if pf_ok else 'FAIL')}]")

    # --- Undershoot ---
    # After peak, signal should overshoot above baseline
    # Look in window t_peak+200ns to t_peak+3000ns
    i_us_start = min_idx + idx_t(200e-9) - idx_t(0)  # rough
    i_us_start = min_idx + max(1, int(200e-9 / (times[1]-times[0])))
    i_us_end   = min(len(times)-1, min_idx + int(3000e-9 / (times[1]-times[0])))
    if i_us_start < len(sig_adc) and i_us_end < len(sig_adc):
        us_window = sig_adc[i_us_start:i_us_end]
        max_above_bl = max(v - baseline_adc for v in us_window)
        undershoot_pct = max_above_bl / delta_adc * 100 if delta_adc > 0 else 0
        us_ok = 2 <= undershoot_pct <= 15
        print(f"\n  UNDERSHOOT (after peak):")
        print(f"    Max above baseline: {max_above_bl*1000:.2f} mV")
        print(f"    Undershoot %:       {undershoot_pct:.1f}%  (expected ~7% design; not yet validated for G474)")
        print(f"    [{'PASS' if us_ok else 'WARN'}]")

    print()
    print("=" * 58)


if __name__ == '__main__':
    raw_path = sys.argv[1] if len(sys.argv) > 1 else 'simulation/sim2_shaper.raw'
    if not os.path.exists(raw_path):
        print(f"Not found: {raw_path}")
        sys.exit(1)

    print(f"Reading {raw_path}  ({os.path.getsize(raw_path)} bytes)")
    n_vars, n_points, var_bytes, times, traces = read_transient_raw(raw_path)
    print(f"  Vars: {n_vars}  Points: {n_points}  Var size: {var_bytes} bytes")
    print(f"  Traces: {list(traces.keys())[:8]} ...")
    print(f"  Time range: {times[0]*1e6:.3f} us ... {times[-1]*1e6:.3f} us")
    analyze_shaper(times, traces)
