/* window.jsx — ImGui app-window composition + primitives. Exports to window. */

/* ---------------- primitives ---------------- */
function TitleBar({ title }) {
  return (
    <div className="titlebar">
      <div className="titlebar__icon" />
      <div className="titlebar__title"><b>ГаммаПульт</b> &nbsp;·&nbsp; {title}</div>
      <div className="titlebar__sp" />
      <div className="titlebar__btns">
        <div className="titlebar__btn titlebar__btn--min"><svg viewBox="0 0 11 11"><line x1="1" y1="9" x2="10" y2="9" stroke="currentColor" strokeWidth="1" /></svg></div>
        <div className="titlebar__btn titlebar__btn--max"><svg viewBox="0 0 11 11"><rect x="1.5" y="1.5" width="8" height="8" fill="none" stroke="currentColor" strokeWidth="1" /></svg></div>
        <div className="titlebar__btn titlebar__btn--close"><svg viewBox="0 0 11 11"><line x1="1.5" y1="1.5" x2="9.5" y2="9.5" stroke="currentColor" strokeWidth="1.1" /><line x1="9.5" y1="1.5" x2="1.5" y2="9.5" stroke="currentColor" strokeWidth="1.1" /></svg></div>
      </div>
    </div>
  );
}

function MenuBar({ active }) {
  const items = ['Файл', 'Прибор', 'Вид', 'Калибровка', 'Самотест'];
  return (
    <div className="menubar">
      {items.map((m) => <div key={m} className={'menubar__item' + (m === active ? ' menubar__item--on' : '')}>{m}</div>)}
      <div className="menubar__item menubar__item--disabled" title="Дорожная карта — недоступно">
        Высокое напряжение
        <svg className="lock" width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1"><rect x="1.5" y="4.5" width="7" height="5" /><path d="M3 4.5V3a2 2 0 0 1 4 0v1.5" /></svg>
      </div>
      <div className="menubar__item menubar__item--disabled" title="Дорожная карта — недоступно">Термокалибровка</div>
      <div className="menubar__sp" />
      <div className="menubar__hint">shproto · 600000 8N1 · v2.3.1</div>
    </div>
  );
}

function Panel({ title, meta, tools, active, locked, children, area, flex, style }) {
  return (
    <div className={'panel' + (active ? ' panel--active' : '') + (locked ? ' panel--locked' : '') + (flex ? ' panel--flex' : '')}
      style={{ gridArea: area, ...style }}>
      <div className="panel__head">
        <span className="panel__tri">▾</span>
        <span className="panel__title">{title}</span>
        <span className="panel__sp" />
        {meta && <span className="panel__meta">{meta}</span>}
        {tools && <span className="panel__tools">{tools}</span>}
      </div>
      <div className="panel__body">{children}</div>
    </div>
  );
}

function Dot({ kind, pulse, blink }) {
  return <span className={'dot dot--' + kind + (pulse ? ' dot--pulse' : '') + (blink ? ' dot--blink' : '')} />;
}

function KV({ k, v, cls }) {
  return (
    <div className="row" style={{ gap: 6 }}>
      <span className="lbl" style={{ width: 78, flex: '0 0 78px' }}>{k}</span>
      <span className={'val ' + (cls || '')}>{v}</span>
    </div>
  );
}

/* ---------------- panel bodies ---------------- */
function ConnBody({ state }) {
  const connected = state === 'acquiring' || state === 'idle';
  return (
    <>
      <div className="row" style={{ gap: 6 }}>
        <span className="lbl" style={{ width: 64, flex: '0 0 64px' }}>COM-порт</span>
        <div className={'field grow combo' + (connected || state === 'connecting' ? ' combo--disabled' : '')}>
          <span>{state === 'disconnected' ? 'COM7 — STM32 Virtual ComPort' : 'COM7 — STM32 VCP'}</span>
          <svg width="9" height="9" viewBox="0 0 9 9" fill="none" stroke="currentColor" strokeWidth="1.2"><path d="M1.5 3l3 3 3-3" /></svg>
        </div>
      </div>
      <div className="row" style={{ gap: 6 }}>
        <span className="lbl" style={{ width: 64, flex: '0 0 64px' }}>Скорость</span>
        <div className="field field--ro grow" style={{ justifyContent: 'space-between' }}><span>600000</span><span className="caption">бод · 8N1</span></div>
      </div>

      {/* action buttons per state */}
      {state === 'disconnected' && (
        <div className="row" style={{ gap: 6 }}>
          <button className="btn btn--sm btn--ghost" style={{ flex: '0 0 auto' }}>⟳ Порты</button>
          <button className="btn btn--primary grow" style={{ height: 26 }}>Подключить</button>
        </div>
      )}
      {state === 'connecting' && (
        <div className="row" style={{ gap: 6 }}>
          <button className="btn grow" style={{ height: 26 }} disabled>Подключение…</button>
          <button className="btn btn--sm" style={{ flex: '0 0 auto' }}>Отмена</button>
        </div>
      )}
      {connected && (
        <div className="row" style={{ gap: 6 }}>
          <button className="btn btn--danger grow" style={{ height: 26 }}>Отключить</button>
          <button className="btn btn--sm" style={{ flex: '0 0 auto' }} title="Сброс прибора (-rst)">-rst</button>
        </div>
      )}
      {state === 'error' && (
        <div className="row" style={{ gap: 6 }}>
          <button className="btn btn--primary grow" style={{ height: 26 }}>Переподключить</button>
          <button className="btn btn--sm" style={{ flex: '0 0 auto' }}>-rst</button>
        </div>
      )}

      <div className="sep" />
      {/* link status */}
      <div className="status-line">
        {state === 'disconnected' && (<><Dot kind="idle" /><span className="t val--dim">Нет связи · порт закрыт</span></>)}
        {state === 'connecting' && (<><Dot kind="accent" pulse /><span className="t val--accent">Подключение… запрос -inf</span></>)}
        {state === 'idle' && (<><Dot kind="ok" /><span className="t val--ok">Связь установлена · простой</span></>)}
        {state === 'acquiring' && (<><Dot kind="ok" /><span className="t val--ok">Связь установлена · набор</span></>)}
        {state === 'error' && (<><Dot kind="danger" blink /><span className="t val--danger">Ошибка связи · таймаут кадра</span></>)}
      </div>

      <div className="sep" />
      {/* device identity (-inf / -cal) */}
      <div className="col" style={{ gap: 5 }}>
        <KV k="Серийник" v={connected || state === 'error' ? 'SN-04F2A1' : '—'} cls={connected ? 'val--strong' : 'val--dim'} />
        <KV k="Прошивка" v={connected || state === 'error' ? 'shproto 2.3.1' : '—'} cls={connected ? '' : 'val--dim'} />
        <KV k="Сборка" v={connected || state === 'error' ? '2025-11-04' : '—'} cls="val--dim" />
        <KV k="Калибр. k" v={connected || state === 'error' ? '2.0473 кан/код' : '—'} cls={connected ? 'val--accent' : 'val--dim'} />
      </div>
      {state === 'error' && (
        <div style={{ marginTop: 'auto', background: 'var(--danger-bg)', border: '1px solid var(--danger)', padding: '6px 8px' }}>
          <span className="mono" style={{ fontSize: 11, color: 'var(--danger)' }}>RX timeout 1500 мс · CRC err 3 · нет ответа на -sta</span>
        </div>
      )}
    </>
  );
}

function TelemetryBody({ state }) {
  const live = state === 'acquiring';
  const frozen = state === 'error';
  return (
    <>
      <KV k="Время набора" v={live ? '00:12:47' : frozen ? '00:03:11' : '00:00:00'} cls="val--strong" />
      <div className="sep" />
      <div>
        <div className="lbl" style={{ marginBottom: 3 }}>Скорость счёта</div>
        <div className="row between" style={{ alignItems: 'flex-end' }}>
          <div className={'big' + (live ? ' big--accent' : '')} style={frozen ? { color: 'var(--text-dim)' } : {}}>
            {live ? '1 842' : frozen ? '1 790' : '0'}<span className="u">cps</span>
          </div>
        </div>
        <div style={{ marginTop: 6 }}><Sparkline color={live ? 'var(--accent)' : 'var(--text-faint)'} flat={!live && !frozen} /></div>
      </div>
      <div className="sep" />
      <div className="row between">
        <span className="lbl">Отбраковано</span>
        <span className={'val ' + (live ? 'val--warn' : 'val--dim')}>{live ? '0.42 %' : frozen ? '0.51 %' : '— %'}</span>
      </div>
      <div className="meter"><div className={'meter__fill meter__fill--warn'} style={{ width: live ? '4%' : '0%' }} /></div>
      <div className="sep" />
      <div className="row between">
        <span className="lbl">Суммарный счёт</span>
        <span className="val val--strong">{live ? '1 412 933' : frozen ? '341 070' : '0'}</span>
      </div>
      <div className="status-line" style={{ marginTop: 2 }}>
        {live && (<><Dot kind="accent" pulse /><span className="t val--accent" style={{ letterSpacing: .4 }}>ИДЁТ НАБОР</span></>)}
        {state === 'idle' && (<><Dot kind="idle" /><span className="t val--dim">Набор остановлен</span></>)}
        {frozen && (<><Dot kind="danger" blink /><span className="t val--danger">Набор прерван</span></>)}
        {(state === 'disconnected' || state === 'connecting') && (<><Dot kind="idle" /><span className="t val--dim">Набор остановлен</span></>)}
      </div>
    </>
  );
}

function SpectrumTools({ mode }) {
  return (
    <>
      <div className="seg">
        <div className={'seg__opt' + (mode === 'lin' ? ' seg__opt--on' : '')}>Лин</div>
        <div className={'seg__opt' + (mode === 'log' ? ' seg__opt--on' : '')}>Лог</div>
      </div>
    </>
  );
}

function SpectrumBody({ active }) {
  if (!active) return <EmptyPlot />;
  return (
    <>
      <div className="row between" style={{ flex: '0 0 auto', flexWrap: 'nowrap', overflow: 'hidden', gap: 12 }}>
        <div className="row" style={{ gap: 14, flexWrap: 'nowrap' }}>
          <span className="mono" style={{ fontSize: 11, color: 'var(--text-dim)', whiteSpace: 'nowrap' }}>Σ <b style={{ color: 'var(--text-strong)' }}>1 412 933</b></span>
          <span className="mono" style={{ fontSize: 11, color: 'var(--text-dim)', whiteSpace: 'nowrap' }}>пик <b style={{ color: 'var(--accent)' }}>1352</b></span>
        </div>
        <span className="mono" style={{ fontSize: 11, color: 'var(--cursor)', whiteSpace: 'nowrap' }}>⌖ 1352 · 2607 · 662 кэв</span>
      </div>
      <SpectrumPlot mode="log" cursorCh={1352} cursorCount={2607} />
    </>
  );
}

function SelfTestBody({ enabled }) {
  if (!enabled) {
    return (
      <div className="empty">
        <svg className="empty__ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.2">
          <path d="M3 17l5-7 3 3 4-7 3 5 3-2" /><rect x="2.5" y="3" width="19" height="18" strokeOpacity=".3" />
        </svg>
        <div className="empty__t">Самотест ENC недоступен.<br />Подключитесь к прибору, затем запустите прогон.</div>
        <div className="row" style={{ gap: 6 }}>
          <button className="btn btn--sm btn--disabled" disabled>▶ Прогон ENC</button>
          <button className="btn btn--sm btn--disabled" disabled>Тест-режим</button>
        </div>
        <div className="empty__cta">требуется команда -tst</div>
      </div>
    );
  }
  const rows = [
    { a: 500, c: 1024, s: 18.2, f: 9.8 },
    { a: 1000, c: 2047, s: 17.9, f: 7.6 },
    { a: 1500, c: 3072, s: 19.4, f: 6.9, sel: true },
    { a: 2000, c: 4098, s: 21.0, f: 6.4 },
    { a: 3000, c: 6140, s: 24.8, f: 6.1 },
    { a: 3800, c: 7779, s: 28.1, f: 6.0 },
  ];
  return (
    <>
      <div className="row" style={{ gap: 6 }}>
        <button className={'btn btn--sm' + (enabled ? ' btn--primary' : ' btn--disabled')} disabled={!enabled}>▶ Прогон ENC</button>
        <div className={'field combo btn--sm grow' + (enabled ? '' : ' combo--disabled')} style={{ height: 22, fontSize: 12 }}>
          <span>Тест-режим: меандр</span>
          <svg width="9" height="9" viewBox="0 0 9 9" fill="none" stroke="currentColor" strokeWidth="1.2"><path d="M1.5 3l3 3 3-3" /></svg>
        </div>
      </div>
      <table className="tbl">
        <thead><tr><th>Ампл. код</th><th>Центроид</th><th>σ</th><th>FWHM</th></tr></thead>
        <tbody>
          {rows.map((r) => (
            <tr key={r.a} className={r.sel ? 'is-sel' : ''}>
              <td>{r.a}</td><td>{r.c}</td><td>{r.s.toFixed(1)}</td><td>{r.f.toFixed(1)} %</td>
            </tr>
          ))}
        </tbody>
      </table>
      <div className="row between" style={{ fontFamily: 'var(--mono)', fontSize: 12 }}>
        <span className="val--dim">k = <b className="val--accent">2.0473</b> кан/код</span>
        <span className="val--dim">INL = <b style={{ color: 'var(--ok)' }}>0.18</b> %FS</span>
        <span className="val--dim">R² = <b style={{ color: 'var(--text-strong)' }}>0.9998</b></span>
      </div>
      <LinearityPlot />
      <FwhmPlot />
      {!enabled && (
        <div className="caption" style={{ textAlign: 'center', marginTop: 2 }}>Доступно после подключения к прибору</div>
      )}
    </>
  );
}

const LOG_ACQ = [
  { d: 'sys', dir: '', ts: '12:41:02', t: 'Порт COM7 открыт @ 600000 8N1' },
  { d: 'tx', dir: 'TX', ts: '12:41:02', t: '-inf', hex: '02 01 00 7E' },
  { d: 'rx', dir: 'RX', ts: '12:41:02', t: 'INFO sn=04F2A1 fw=2.3.1', hex: '8A 12 …' },
  { d: 'tx', dir: 'TX', ts: '12:41:05', t: '-cal', hex: '02 03 00 5C' },
  { d: 'rx', dir: 'RX', ts: '12:41:05', t: 'CAL k=2.0473 inl=0.18', hex: '8C 0A …' },
  { d: 'tx', dir: 'TX', ts: '12:41:09', t: '-sta', hex: '02 10 00 3B' },
  { d: 'rx', dir: 'RX', ts: '12:41:09', t: 'ACK start acquire', hex: '90 01 …' },
  { d: 'rx', dir: 'RX', ts: '12:53:56', t: 'STATUS t=767s cps=1842 drop=0.42%', hex: '94 28 …' },
];
const LOG_OFF = [
  { d: 'sys', dir: '', ts: '12:39:40', t: 'Порт закрыт. Выберите COM-порт и нажмите «Подключить».' },
  { d: 'sys', dir: '', ts: '12:39:40', t: 'Доступные команды: -sta -sto -rst -inf -cal -tst' },
];
function ConsoleBody({ state }) {
  const log = state === 'acquiring' ? LOG_ACQ : LOG_OFF;
  const off = state === 'disconnected' || state === 'connecting';
  return (
    <>
      <div className="log">
        {log.map((l, i) => (
          <div key={i} className={'log__line log__' + l.d}>
            <span className="log__ts">{l.ts}</span>
            <span className="log__dir">{l.dir || '··'}</span>
            <span>{l.t}</span>
            {l.hex && <span className="log__hex">  [{l.hex}]</span>}
          </div>
        ))}
      </div>
      <div className="cmdbar">
        <div className="chip-row row" style={{ gap: 4 }}>
          {['-sta', '-sto', '-rst', '-inf', '-cal', '-tst'].map((c) => <span key={c} className="chip">{c}</span>)}
        </div>
      </div>
      <div className="cmdbar">
        <div className="field grow" style={{ background: 'var(--plot-bg)' }}>
          <span className="prompt" style={{ marginRight: 6 }}>›</span>
          <span className="mono" style={{ color: 'var(--text)' }}>{state === 'acquiring' ? '-sto' : off ? '' : '-inf'}</span>
          <span className="caret" />
        </div>
        <button className={'btn btn--sm' + (off ? '' : ' btn--primary')} style={{ height: 26 }}>Отправить</button>
      </div>
    </>
  );
}

/* ---------------- full window ---------------- */
function AppWindow({ state }) {
  const acquiring = state === 'acquiring';
  return (
    <div className="app">
      <div className="win">
        <TitleBar title={acquiring ? 'NaI(Tl) · R1307 · STM32G474 — набор' : 'NaI(Tl) · R1307 · STM32G474'} />
        <MenuBar active="Прибор" />
        <div className="dock">
          <div className="area-left">
            <Panel title="Подключение" area=""><ConnBody state={state} /></Panel>
            <Panel title="Статус / Телеметрия" flex><TelemetryBody state={state} /></Panel>
          </div>
          <Panel title="Спектр" area="spectrum" active={acquiring} meta="8192 кан" tools={acquiring ? <SpectrumTools mode="log" /> : <SpectrumTools mode="log" />}>
            <SpectrumBody active={acquiring} />
          </Panel>
          <Panel title="Консоль · shproto" area="console" meta={acquiring ? 'кадров 4128 · CRC err 0' : 'idle'}>
            <ConsoleBody state={state} />
          </Panel>
          <Panel title="Самотест · ENC" area="selftest"><SelfTestBody enabled={acquiring} /></Panel>
        </div>
        {/* statusbar */}
        <div className="statusbar">
          {acquiring ? (
            <>
              <div className="statusbar__seg statusbar__seg--accent"><Dot kind="accent" pulse /><span>ИДЁТ НАБОР</span></div>
              <div className="statusbar__seg">t 00:12:47</div>
              <div className="statusbar__seg">1842 cps</div>
              <div className="statusbar__seg">Σ 1 412 933</div>
              <div className="statusbar__seg">drop 0.42%</div>
              <div className="statusbar__sp" />
              <div className="statusbar__seg">COM7</div>
              <div className="statusbar__seg">600000 8N1</div>
              <div className="statusbar__seg">CRC err 0</div>
            </>
          ) : (
            <>
              <div className="statusbar__seg"><Dot kind="idle" /><span>Отключено</span></div>
              <div className="statusbar__seg">Выберите COM-порт → Подключить</div>
              <div className="statusbar__sp" />
              <div className="statusbar__seg">COM7 доступен</div>
              <div className="statusbar__seg">порт закрыт</div>
            </>
          )}
        </div>
      </div>
    </div>
  );
}

/* ---------------- compact connection-state card ---------------- */
function ConnCard({ state, title }) {
  return (
    <div className="app" style={{ height: '100%' }}>
      <div className="win" style={{ height: '100%' }}>
        <div className="titlebar" style={{ height: 28, flex: '0 0 28px' }}>
          <div className="titlebar__icon" style={{ width: 13, height: 13 }} />
          <div className="titlebar__title" style={{ fontSize: 11 }}>{title}</div>
        </div>
        <div style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column', gap: 5, padding: 5, background: 'var(--bg-app)' }}>
          <Panel title="Подключение"><ConnBody state={state} /></Panel>
          <Panel title="Телеметрия" flex><TelemetryBody state={state} /></Panel>
        </div>
      </div>
    </div>
  );
}

Object.assign(window, { AppWindow, ConnCard, Panel, Dot, KV, TitleBar, MenuBar });
