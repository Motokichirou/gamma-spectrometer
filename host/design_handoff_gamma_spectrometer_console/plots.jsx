/* plots.jsx — SVG instrument plots (ImPlot-style). Exports to window. */

/* ---- deterministic gamma-spectrum model (8192 channels) ---- */
function gauss(x, mu, amp, sig) { const d = (x - mu) / sig; return amp * Math.exp(-0.5 * d * d); }
function spectrumCounts(ch) {
  let c = 5;
  c += gauss(ch, 0, 1700, 140);          // low-energy noise wall
  c += 560 * Math.exp(-ch / 1700);        // Compton continuum
  c += gauss(ch, 250, 1150, 46);          // Pb K x-ray
  c += gauss(ch, 690, 430, 34);           // backscatter bump
  c += 300 * Math.exp(-Math.max(0, ch - 1352) / 900) * (ch < 1352 ? 1 : 0); // Compton edge shelf
  c += gauss(ch, 1352, 2680, 40);         // Cs-137 photopeak 662 keV
  c += gauss(ch, 2990, 560, 74);          // K-40 photopeak 1460 keV
  c += gauss(ch, 4180, 90, 96);           // faint high-E
  // deterministic ripple so the trace looks like real counts
  c *= 1 + 0.06 * Math.sin(ch * 0.21) + 0.04 * Math.sin(ch * 0.043 + 1.7);
  return Math.max(0.6, c);
}

const SPEC_MAX_CH = 8192;
const SPEC_CMAX = 3300;

/* Build the trace path in a 0..1000 × 0..600 viewBox. preserveAspectRatio=none
   means we can stretch freely; axis text is HTML-overlaid for sharpness. */
function buildTrace(mode) {
  const W = 1000, H = 600, N = 620;
  const ymap = (c) => {
    if (mode === 'log') {
      const lo = Math.log10(1), hi = Math.log10(12000);
      return H - ((Math.log10(c + 1) - lo) / (hi - lo)) * H;
    }
    return H - (c / SPEC_CMAX) * H;
  };
  let d = `M 0 ${H}`;
  for (let i = 0; i <= N; i++) {
    const ch = (i / N) * SPEC_MAX_CH;
    const x = (i / N) * W;
    const y = Math.max(2, ymap(spectrumCounts(ch)));
    d += ` L ${x.toFixed(1)} ${y.toFixed(1)}`;
  }
  const lineEnd = d;
  d += ` L ${W} ${H} Z`;
  return { fill: d, line: lineEnd };
}

/* channel -> x fraction (0..1) */
const chFrac = (ch) => ch / SPEC_MAX_CH;

function SpectrumPlot({ mode = 'log', cursorCh = 1352, cursorCount = 2607 }) {
  const t = buildTrace(mode);
  const yTicks = mode === 'log'
    ? [{ v: '10k', f: 0.07 }, { v: '1k', f: 0.27 }, { v: '100', f: 0.50 }, { v: '10', f: 0.74 }, { v: '1', f: 0.97 }]
    : [{ v: '3k', f: 0.06 }, { v: '2k', f: 0.36 }, { v: '1k', f: 0.66 }, { v: '0', f: 0.97 }];
  const xTicks = [0, 2048, 4096, 6144, 8192];
  const cx = chFrac(cursorCh) * 100;

  return (
    <div className="plot" style={{ position: 'relative', flex: 1, minHeight: 0, background: 'var(--plot-bg)', border: '1px solid var(--border)' }}>
      {/* plot canvas inset to leave room for axis labels: left 34, bottom 18, top 6, right 6 */}
      <div style={{ position: 'absolute', left: 38, right: 8, top: 8, bottom: 20 }}>
        <svg viewBox="0 0 1000 600" preserveAspectRatio="none" style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
          {/* horizontal grid */}
          {yTicks.map((tk, i) => (
            <line key={'h' + i} x1="0" x2="1000" y1={tk.f * 600} y2={tk.f * 600} stroke="var(--grid)" strokeWidth="1" vectorEffect="non-scaling-stroke" />
          ))}
          {/* vertical grid */}
          {xTicks.map((c, i) => (
            <line key={'v' + i} x1={chFrac(c) * 1000} x2={chFrac(c) * 1000} y1="0" y2="600" stroke="var(--grid)" strokeWidth="1" vectorEffect="non-scaling-stroke" />
          ))}
          {/* trace fill + line */}
          <path d={t.fill} fill="var(--trace-fill)" />
          <path d={t.line} fill="none" stroke="var(--trace)" strokeWidth="1.4" vectorEffect="non-scaling-stroke" strokeLinejoin="round" />
          {/* cursor */}
          <line x1={chFrac(cursorCh) * 1000} x2={chFrac(cursorCh) * 1000} y1="0" y2="600" stroke="var(--cursor)" strokeWidth="1" strokeDasharray="4 3" vectorEffect="non-scaling-stroke" />
        </svg>
        {/* peak labels (HTML overlay, sharp) */}
        <div className="mono" style={{ position: 'absolute', left: `${chFrac(1352) * 100}%`, top: 4, transform: 'translateX(-50%)', fontSize: 10, color: 'var(--text-dim)', whiteSpace: 'nowrap' }}>Cs-137 · 662</div>
        <div className="mono" style={{ position: 'absolute', left: `${chFrac(2990) * 100}%`, top: 32, transform: 'translateX(-50%)', fontSize: 10, color: 'var(--text-faint)', whiteSpace: 'nowrap' }}>K-40 · 1460</div>
        {/* cursor readout chip */}
        <div className="mono" style={{ position: 'absolute', left: `calc(${cx}% + 6px)`, top: 10, fontSize: 11, color: 'var(--cursor)', background: '#1a1206ee', border: '1px solid var(--cursor)', padding: '2px 6px', whiteSpace: 'nowrap', lineHeight: 1.5 }}>
          канал {cursorCh}<br />счёт {cursorCount}
        </div>
      </div>
      {/* y axis labels */}
      {yTicks.map((tk, i) => (
        <div key={'yl' + i} className="mono" style={{ position: 'absolute', left: 0, width: 34, textAlign: 'right', top: `calc(8px + ${tk.f} * (100% - 28px) - 6px)`, fontSize: 10, color: 'var(--text-dim)' }}>{tk.v}</div>
      ))}
      {/* x axis labels */}
      {xTicks.map((c, i) => (
        <div key={'xl' + i} className="mono" style={{ position: 'absolute', bottom: 4, left: `calc(38px + ${chFrac(c)} * (100% - 46px))`, transform: i === 0 ? 'none' : (i === xTicks.length - 1 ? 'translateX(-100%)' : 'translateX(-50%)'), fontSize: 10, color: 'var(--text-dim)' }}>{c}</div>
      ))}
    </div>
  );
}

function EmptyPlot() {
  const yTicks = [0.07, 0.5, 0.97];
  const xTicks = [0, 2048, 4096, 6144, 8192];
  return (
    <div className="plot" style={{ position: 'relative', flex: 1, minHeight: 0, background: 'var(--plot-bg)', border: '1px solid var(--border)' }}>
      <div style={{ position: 'absolute', left: 38, right: 8, top: 8, bottom: 20 }}>
        <svg viewBox="0 0 1000 600" preserveAspectRatio="none" style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
          {yTicks.map((f, i) => <line key={i} x1="0" x2="1000" y1={f * 600} y2={f * 600} stroke="var(--grid)" strokeWidth="1" vectorEffect="non-scaling-stroke" />)}
          {xTicks.map((c, i) => <line key={i} x1={chFrac(c) * 1000} x2={chFrac(c) * 1000} y1="0" y2="600" stroke="var(--grid)" strokeWidth="1" vectorEffect="non-scaling-stroke" />)}
        </svg>
        <div style={{ position: 'absolute', inset: 0, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 10 }}>
          <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="var(--text-faint)" strokeWidth="1.3" style={{ opacity: .6 }}>
            <path d="M3 17l4-6 3 3 4-8 3 5 4-3" /><path d="M3 21h18" strokeOpacity=".4" />
          </svg>
          <div style={{ fontSize: 12, color: 'var(--text-dim)', textAlign: 'center', maxWidth: 280, lineHeight: 1.5 }}>
            Нет данных гистограммы.<br />Подключитесь к прибору и запустите набор.
          </div>
          <div className="mono" style={{ fontSize: 11, color: 'var(--accent)' }}>→ Подключить · затем команда -sta</div>
        </div>
      </div>
    </div>
  );
}

/* ---- sparkline (cps over time) ---- */
function Sparkline({ color = 'var(--accent)', flat = false }) {
  const N = 56, W = 1000, H = 100;
  let d = '';
  for (let i = 0; i < N; i++) {
    const base = flat ? 6 : 50 + 30 * Math.sin(i * 0.5) + 10 * Math.sin(i * 1.3 + 1) + (i > 40 ? 12 : 0);
    const y = H - (base / 100) * H;
    d += (i === 0 ? 'M' : 'L') + ` ${(i / (N - 1) * W).toFixed(1)} ${y.toFixed(1)}`;
  }
  return (
    <svg viewBox="0 0 1000 100" preserveAspectRatio="none" style={{ width: '100%', height: 26, display: 'block' }}>
      <path d={d} fill="none" stroke={color} strokeWidth="1.6" vectorEffect="non-scaling-stroke" />
    </svg>
  );
}

/* ---- self-test mini plots ---- */
const LIN_PTS = [
  { a: 500, c: 1024 }, { a: 1000, c: 2047 }, { a: 1500, c: 3072 },
  { a: 2000, c: 4098 }, { a: 3000, c: 6140 }, { a: 3800, c: 7779 },
];
function LinearityPlot() {
  const W = 1000, H = 100;
  const ax = (a) => (a / 4000) * W;
  const cy = (c) => H - (c / 8192) * H;
  const fit = `M ${ax(0)} ${cy(0).toFixed(1)} L ${ax(4000)} ${cy(8190).toFixed(1)}`;
  return (
    <div style={{ position: 'relative', height: 96, background: 'var(--plot-bg)', border: '1px solid var(--border)', padding: '6px 6px 14px 26px' }}>
      <svg viewBox="0 0 1000 100" preserveAspectRatio="none" style={{ position: 'absolute', left: 26, right: 6, top: 6, bottom: 14, width: 'calc(100% - 32px)', height: 'calc(100% - 20px)' }}>
        {[0.25, 0.5, 0.75].map((f, i) => <line key={i} x1="0" x2="1000" y1={f * 100} y2={f * 100} stroke="var(--grid)" strokeWidth="1" vectorEffect="non-scaling-stroke" />)}
        <path d={fit} stroke="var(--accent)" strokeWidth="1.2" strokeDasharray="5 3" vectorEffect="non-scaling-stroke" fill="none" />
        {LIN_PTS.map((p, i) => <rect key={i} x={ax(p.a) - 4} y={cy(p.c) - 4} width="8" height="8" fill="var(--trace2)" vectorEffect="non-scaling-stroke" />)}
      </svg>
      <div className="mono" style={{ position: 'absolute', left: 3, top: 3, fontSize: 9, color: 'var(--text-dim)' }}>кан</div>
      <div className="mono" style={{ position: 'absolute', right: 6, bottom: 1, fontSize: 9, color: 'var(--text-dim)' }}>код</div>
      <div className="mono" style={{ position: 'absolute', left: 26, top: 1, fontSize: 9.5, color: 'var(--text-faint)' }}>Линейность</div>
    </div>
  );
}
const FWHM_PTS = [
  { a: 500, f: 9.8 }, { a: 1000, f: 7.6 }, { a: 1500, f: 6.9 },
  { a: 2000, f: 6.4 }, { a: 3000, f: 6.1 }, { a: 3800, f: 6.0 },
];
function FwhmPlot() {
  const ax = (a) => (a / 4000) * 1000;
  const fy = (f) => 100 - ((f - 5) / 6) * 100;
  let d = '';
  FWHM_PTS.forEach((p, i) => { d += (i === 0 ? 'M' : 'L') + ` ${ax(p.a).toFixed(1)} ${fy(p.f).toFixed(1)}`; });
  return (
    <div style={{ position: 'relative', height: 96, background: 'var(--plot-bg)', border: '1px solid var(--border)', padding: '6px 6px 14px 26px' }}>
      <svg viewBox="0 0 1000 100" preserveAspectRatio="none" style={{ position: 'absolute', left: 26, right: 6, top: 6, bottom: 14, width: 'calc(100% - 32px)', height: 'calc(100% - 20px)' }}>
        {[0.25, 0.5, 0.75].map((f, i) => <line key={i} x1="0" x2="1000" y1={f * 100} y2={f * 100} stroke="var(--grid)" strokeWidth="1" vectorEffect="non-scaling-stroke" />)}
        <path d={d} stroke="var(--warn)" strokeWidth="1.4" vectorEffect="non-scaling-stroke" fill="none" />
        {FWHM_PTS.map((p, i) => <circle key={i} cx={ax(p.a)} cy={fy(p.f)} r="3.5" fill="var(--warn)" vectorEffect="non-scaling-stroke" />)}
      </svg>
      <div className="mono" style={{ position: 'absolute', left: 3, top: 3, fontSize: 9, color: 'var(--text-dim)' }}>%</div>
      <div className="mono" style={{ position: 'absolute', right: 6, bottom: 1, fontSize: 9, color: 'var(--text-dim)' }}>код</div>
      <div className="mono" style={{ position: 'absolute', left: 26, top: 1, fontSize: 9.5, color: 'var(--text-faint)' }}>FWHM</div>
    </div>
  );
}

Object.assign(window, { SpectrumPlot, EmptyPlot, Sparkline, LinearityPlot, FwhmPlot });
