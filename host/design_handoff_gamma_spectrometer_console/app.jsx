/* app.jsx — boards + DesignCanvas assembly */

/* ===== helpers ===== */
function Board({ children, style }) {
  return <div className="app" style={{ display: 'block', height: '100%', overflow: 'hidden', background: 'var(--bg-app)', ...style }}>{children}</div>;
}
function BoardHead({ t, s }) {
  return (
    <div style={{ marginBottom: 16 }}>
      <div style={{ fontSize: 15, fontWeight: 600, color: 'var(--text-strong)', letterSpacing: .2 }}>{t}</div>
      {s && <div style={{ fontSize: 12, color: 'var(--text-dim)', marginTop: 3 }}>{s}</div>}
    </div>
  );
}
function Group({ t, children }) {
  return (
    <div style={{ marginBottom: 14 }}>
      <div className="tokhead">{t}</div>
      <div className="col" style={{ gap: 7 }}>{children}</div>
    </div>
  );
}
function Swatch({ c, name, hex, use }) {
  return (
    <div className="swatch">
      <div className="swatch__chip" style={{ background: c }} />
      <div style={{ minWidth: 0 }}>
        <div className="row" style={{ gap: 8 }}><span className="swatch__name">{name}</span><span className="swatch__hex">{hex}</span></div>
        {use && <div className="swatch__use">{use}</div>}
      </div>
    </div>
  );
}

/* ===== Board: Acquiring (annotated) ===== */
const PINS = [
  { n: 1, x: 250, y: 17 },
  { n: 2, x: 458, y: 47 },
  { n: 3, x: 288, y: 79 },
  { n: 4, x: 288, y: 360 },
  { n: 5, x: 372, y: 79 },
  { n: 6, x: 452, y: 250 },
  { n: 7, x: 852, y: 79 },
  { n: 8, x: 372, y: 537 },
  { n: 9, x: 250, y: 727 },
];
const LEGEND = [
  { n: 1, t: 'Идентификация прибора', d: 'Тайтлбар Windows: имя ПО, кристалл/ФЭУ/МК и режим. Кнопки свернуть/развернуть/закрыть справа.' },
  { n: 2, t: 'Меню · дорожная карта', d: 'Активные разделы + заглушки «Высокое напряжение» и «Термокалибровка» — недоступны (замок), заложено место.' },
  { n: 3, t: 'Панель «Подключение»', d: 'COM-порт, скорость 600000, индикатор связи, серийник/прошивка/калибровка из -inf / -cal.' },
  { n: 4, t: 'Панель «Статус / Телеметрия»', d: 'Время набора, cps крупно + спарклайн, % отбраковки, суммарный счёт, индикатор «ИДЁТ НАБОР».' },
  { n: 5, t: 'Панель «Спектр» — доминанта', d: 'Гистограмма 8192 канала, лог-ось, активная панель подсвечена акцентом снизу шапки.' },
  { n: 6, t: 'Курсор спектра', d: 'Янтарная пунктирная линия + отсчёт «канал · счёт · E». Подписаны фотопики Cs-137 и K-40.' },
  { n: 7, t: 'Панель «Самотест · ENC»', d: 'Прогон ENC и тест-режимы, таблица амплитуда→центроид/σ/FWHM, графики линейности и FWHM, вывод k и INL.' },
  { n: 8, t: 'Панель «Консоль · shproto»', d: 'Лог кадров RX/TX с цветовой разметкой и hex, чипы быстрых команд, строка ввода произвольной команды.' },
  { n: 9, t: 'Статусбар', d: 'Сегмент «ИДЁТ НАБОР» с акцентом, ключевая телеметрия, параметры порта, счётчик ошибок CRC.' },
];
function AcquiringAnnotated() {
  return (
    <Board style={{ background: '#0e1014', padding: 0 }}>
      <div style={{ position: 'absolute', left: 30, top: 30, width: 1200, height: 740 }}>
        <AppWindow state="acquiring" />
        {PINS.map((p) => <div key={p.n} className="pin" style={{ left: p.x, top: p.y }}>{p.n}</div>)}
      </div>
      <div style={{ position: 'absolute', left: 1262, top: 30, width: 408, bottom: 30 }}>
        <div style={{ fontSize: 13, fontWeight: 700, color: 'var(--text-strong)', textTransform: 'uppercase', letterSpacing: .6, marginBottom: 4 }}>Главное окно · состояние «идёт набор»</div>
        <div style={{ fontSize: 12, color: 'var(--text-dim)', marginBottom: 8 }}>Раскладка докнутых панелей. Окно ~1200×740, ресайзится; панели докуемые.</div>
        <div className="legend">
          {LEGEND.map((l) => (
            <div key={l.n} className="legend__row">
              <div className="legend__n">{l.n}</div>
              <div className="legend__t"><b>{l.t}</b><p>{l.d}</p></div>
            </div>
          ))}
        </div>
      </div>
    </Board>
  );
}

/* ===== Board: Connection states (5) ===== */
function ConnStates() {
  const cards = [
    { s: 'disconnected', t: 'Отключено', note: 'Нейтральный серый индикатор. Активна синяя кнопка «Подключить». Поля прибора — прочерки.' },
    { s: 'connecting', t: 'Идёт подключение', note: 'Пульсирующий акцент. Кнопка занята, доступна «Отмена». Идёт запрос -inf.' },
    { s: 'idle', t: 'Подключено · простой', note: 'Зелёный индикатор связи. Кнопка набора готова. Телеметрия в нуле.' },
    { s: 'acquiring', t: 'Идёт набор', note: 'Зелёная связь + пульсирующий акцент «ИДЁТ НАБОР». cps крупно и активно.' },
    { s: 'error', t: 'Ошибка связи', note: 'Красный мигающий индикатор и баннер ошибки. Замороженные последние значения. CTA «Переподключить».' },
  ];
  return (
    <Board style={{ padding: 22 }}>
      <BoardHead t="Состояния связи" s="Как меняются индикаторы, доступность кнопок и акценты. Красный — только опасность и ошибки." />
      <div className="row" style={{ gap: 14, alignItems: 'stretch' }}>
        {cards.map((c) => (
          <div key={c.s} style={{ width: 290, display: 'flex', flexDirection: 'column' }}>
            <div style={{ height: 470 }}><ConnCard state={c.s} title={c.t} /></div>
            <div style={{ marginTop: 10, fontSize: 11.5, color: 'var(--text-dim)', lineHeight: 1.5 }}>
              <b style={{ color: 'var(--text-strong)' }}>{c.t}.</b> {c.note}
            </div>
          </div>
        ))}
      </div>
    </Board>
  );
}

/* ===== Board: Palette ===== */
function PaletteBoard() {
  return (
    <Board style={{ padding: 24 }}>
      <BoardHead t="Палитра" s="Тёмная приборная тема. Один акцент (синий ImGui). Красный — строго опасность/ошибки." />
      <div className="row" style={{ alignItems: 'flex-start', gap: 40 }}>
        <div style={{ width: 300 }}>
          <Group t="Поверхности">
            <Swatch c="#0b0d11" name="bg-app" hex="#0b0d11" use="фон дока, под окнами" />
            <Swatch c="#15181e" name="bg-win" hex="#15181e" use="окно и тело панели" />
            <Swatch c="#1b1f27" name="bg-child" hex="#1b1f27" use="дочерние фреймы" />
            <Swatch c="#232a35" name="bg-head" hex="#232a35" use="шапка панели" />
            <Swatch c="#20252e" name="bg-frame" hex="#20252e" use="инпуты, комбо" />
            <Swatch c="#10131a" name="plot-bg" hex="#10131a" use="холст графика, лог" />
          </Group>
          <Group t="Линии">
            <Swatch c="#2e3540" name="border" hex="#2e3540" use="рамки панелей/виджетов" />
            <Swatch c="#242a33" name="border-soft" hex="#242a33" use="разделители, строки" />
            <Swatch c="#1f2630" name="grid" hex="#1f2630" use="сетка графиков" />
          </Group>
        </div>
        <div style={{ width: 300 }}>
          <Group t="Текст">
            <Swatch c="#e9edf3" name="text-strong" hex="#e9edf3" use="значения, заголовки" />
            <Swatch c="#c6ccd6" name="text" hex="#c6ccd6" use="базовый текст" />
            <Swatch c="#79818e" name="text-dim" hex="#79818e" use="метки, подписи" />
            <Swatch c="#555d6a" name="text-faint" hex="#555d6a" use="оси, таймстемпы" />
          </Group>
          <Group t="Акцент · ImGui синий">
            <Swatch c="#4c8df0" name="accent" hex="#4c8df0" use="активное · набор · выбор" />
            <Swatch c="#5e9bf5" name="accent-hover" hex="#5e9bf5" use="ховер" />
            <Swatch c="#3b7ae0" name="accent-active" hex="#3b7ae0" use="нажатие" />
            <Swatch c="rgba(76,141,240,.22)" name="accent-sel" hex="rgba · .22" use="выделение, подсветка" />
          </Group>
          <Group t="Семантика">
            <Swatch c="#4fb07a" name="ok" hex="#4fb07a" use="связь установлена" />
            <Swatch c="#e0a23c" name="warn" hex="#e0a23c" use="отбраковка · курсор" />
            <Swatch c="#e5484d" name="danger" hex="#e5484d" use="ОПАСНОСТЬ ВВ · ошибки" />
          </Group>
          <Group t="График">
            <Swatch c="#6fb0f0" name="trace" hex="#6fb0f0" use="линия спектра" />
            <Swatch c="#4fb07a" name="trace-2" hex="#4fb07a" use="линейность (точки)" />
            <Swatch c="#e0a23c" name="cursor" hex="#e0a23c" use="курсор графика" />
          </Group>
        </div>
      </div>
    </Board>
  );
}

/* ===== Board: Typography ===== */
function TypeBoard() {
  const rows = [
    { meta: 'Plex Mono · 38 / 600', s: { fontFamily: 'var(--mono)', fontSize: 38, fontWeight: 600, color: 'var(--accent)', letterSpacing: -1 }, sample: '1 842', use: 'крупная телеметрия (cps)' },
    { meta: 'Plex Mono · 18 / 500', s: { fontFamily: 'var(--mono)', fontSize: 18, color: 'var(--text-strong)' }, sample: '00:12:47', use: 'значения времени/счёта' },
    { meta: 'Plex Mono · 13', s: { fontFamily: 'var(--mono)', fontSize: 13, color: 'var(--text)' }, sample: 'k = 2.0473 кан/код', use: 'значения, поля ввода' },
    { meta: 'Plex Mono · 11', s: { fontFamily: 'var(--mono)', fontSize: 11, color: 'var(--text-dim)' }, sample: '12:41:09  RX  STATUS cps=1842', use: 'лог, оси, статусбар' },
    { meta: 'Segoe UI · 13', s: { fontFamily: 'var(--prop)', fontSize: 13, color: 'var(--text)' }, sample: 'Подключить прибор', use: 'базовый UI, кнопки' },
    { meta: 'Segoe UI · 12', s: { fontFamily: 'var(--prop)', fontSize: 12, color: 'var(--text-dim)' }, sample: 'COM-порт · Скорость', use: 'метки (.lbl)' },
    { meta: 'Segoe UI · 11 / 600 ↑', s: { fontFamily: 'var(--prop)', fontSize: 11, fontWeight: 600, color: 'var(--text-strong)', textTransform: 'uppercase', letterSpacing: .4 }, sample: 'Спектр', use: 'заголовки панелей, TH таблиц' },
  ];
  return (
    <Board style={{ padding: 24 }}>
      <BoardHead t="Типографика" s="Пропорциональный Segoe UI — подписи и заголовки. Моноширинный IBM Plex Mono — все числа, телеметрия и лог." />
      <div style={{ display: 'flex', gap: 12, marginBottom: 16 }}>
        <div style={{ flex: 1, border: '1px solid var(--border)', background: 'var(--bg-win)', padding: 14 }}>
          <div className="tokhead">Пропорциональный</div>
          <div style={{ fontFamily: 'var(--prop)', fontSize: 26, color: 'var(--text-strong)' }}>Segoe UI</div>
          <div style={{ fontFamily: 'var(--prop)', fontSize: 12, color: 'var(--text-dim)', marginTop: 4 }}>системный шрифт Windows · подписи, меню, кнопки</div>
        </div>
        <div style={{ flex: 1, border: '1px solid var(--border)', background: 'var(--bg-win)', padding: 14 }}>
          <div className="tokhead">Моноширинный</div>
          <div style={{ fontFamily: 'var(--mono)', fontSize: 26, color: 'var(--text-strong)' }}>IBM Plex Mono</div>
          <div style={{ fontFamily: 'var(--mono)', fontSize: 12, color: 'var(--text-dim)', marginTop: 4 }}>0123456789 · табличные цифры · лог и телеметрия</div>
        </div>
      </div>
      {rows.map((r, i) => (
        <div key={i} className="specrow">
          <div className="specrow__meta">{r.meta}</div>
          <div style={{ ...r.s, flex: 1 }}>{r.sample}</div>
          <div style={{ fontSize: 11, color: 'var(--text-faint)', width: 180, flex: '0 0 180px', alignSelf: 'center' }}>{r.use}</div>
        </div>
      ))}
    </Board>
  );
}

/* ===== Board: Spacing ===== */
function SpacingBoard() {
  const steps = [4, 8, 12, 16, 20];
  return (
    <Board style={{ padding: 24 }}>
      <BoardHead t="Сетка отступов" s="Базовый шаг 4 px. Прямые углы (radius 0), рамки 1 px — приборная, не вебовая эстетика." />
      <div className="tokhead">Шкала отступов</div>
      <div className="row" style={{ gap: 18, alignItems: 'flex-end', marginBottom: 22 }}>
        {steps.map((v) => (
          <div key={v} style={{ textAlign: 'center' }}>
            <div style={{ width: v, height: v, background: 'var(--accent)', margin: '0 auto 8px' }} />
            <div className="mono" style={{ fontSize: 11, color: 'var(--text-dim)' }}>{v}px</div>
          </div>
        ))}
      </div>
      <div className="tokhead">Правила раскладки</div>
      <div className="col" style={{ gap: 8 }}>
        {[
          ['Отступ панели', '9 px со всех сторон тела панели'],
          ['Интервал виджетов', '8 px по вертикали · 6–8 px по горизонтали'],
          ['Паддинг поля/кнопки', '4 px верт · 8–11 px гориз · высота 26 px'],
          ['Гаттер дока', '5 px между докнутыми панелями'],
          ['Высоты строк', 'тайтлбар 34 · меню 27 · шапка 26 · статусбар 26'],
          ['Скругление', '0 px — прямые углы у всех элементов'],
          ['Рамки', '1 px сплошные, цвет border #2e3540'],
        ].map(([k, v]) => (
          <div key={k} className="row" style={{ gap: 12 }}>
            <span style={{ width: 168, flex: '0 0 168px', fontSize: 12, color: 'var(--text)' }}>{k}</span>
            <span className="mono" style={{ fontSize: 12, color: 'var(--text-dim)' }}>{v}</span>
          </div>
        ))}
      </div>
    </Board>
  );
}

/* ===== Board: Buttons & inputs ===== */
function BtnCell({ children, label }) {
  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'flex-start', gap: 7 }}>
      {children}
      <span style={{ fontSize: 10.5, color: 'var(--text-faint)', fontFamily: 'var(--mono)' }}>{label}</span>
    </div>
  );
}
function ButtonsBoard() {
  return (
    <Board style={{ padding: 24 }}>
      <BoardHead t="Кнопки · поля · команды" s="Стандартные виджеты ImGui: высота 26 px, прямые углы, явные состояния." />
      <div className="tokhead">Кнопки</div>
      <div className="row" style={{ gap: 16, flexWrap: 'wrap', marginBottom: 20 }}>
        <BtnCell label="default"><button className="btn">Обновить</button></BtnCell>
        <BtnCell label=":hover"><button className="btn" style={{ background: 'var(--bg-frame-hov)', borderColor: 'var(--border-strong)' }}>Обновить</button></BtnCell>
        <BtnCell label=":active"><button className="btn" style={{ background: 'var(--bg-frame-act)' }}>Обновить</button></BtnCell>
        <BtnCell label="primary"><button className="btn btn--primary">Подключить</button></BtnCell>
        <BtnCell label="primary:hover"><button className="btn btn--primary" style={{ background: 'var(--accent-hov)' }}>Подключить</button></BtnCell>
        <BtnCell label="danger"><button className="btn btn--danger">Отключить</button></BtnCell>
        <BtnCell label="disabled"><button className="btn btn--disabled">Старт набор</button></BtnCell>
        <BtnCell label="ghost · sm"><button className="btn btn--ghost btn--sm">⟳ Порты</button></BtnCell>
      </div>
      <div className="tokhead">Поля и выпадающие</div>
      <div className="row" style={{ gap: 16, alignItems: 'flex-start', flexWrap: 'wrap', marginBottom: 20 }}>
        <BtnCell label="combo"><div className="field combo" style={{ width: 210 }}><span>COM7 — STM32 VCP</span><svg width="9" height="9" viewBox="0 0 9 9" fill="none" stroke="currentColor" strokeWidth="1.2"><path d="M1.5 3l3 3 3-3" /></svg></div></BtnCell>
        <BtnCell label="field · focus"><div className="field field--focus" style={{ width: 150 }}><span>-inf</span><span className="caret" /></div></BtnCell>
        <BtnCell label="field · readonly"><div className="field field--ro" style={{ width: 110 }}><span>600000</span></div></BtnCell>
      </div>
      <div className="tokhead">Переключатель шкалы спектра</div>
      <div className="row" style={{ gap: 16, marginBottom: 20 }}>
        <BtnCell label="лог активна"><div className="seg"><div className="seg__opt">Лин</div><div className="seg__opt seg__opt--on">Лог</div></div></BtnCell>
        <BtnCell label="лин активна"><div className="seg"><div className="seg__opt seg__opt--on">Лин</div><div className="seg__opt">Лог</div></div></BtnCell>
      </div>
      <div className="tokhead">Чипы быстрых команд</div>
      <div className="row" style={{ gap: 6 }}>
        {['-sta', '-sto', '-rst', '-inf', '-cal', '-tst'].map((c) => <span key={c} className="chip">{c}</span>)}
        <span className="chip chip--mut">-help</span>
      </div>
    </Board>
  );
}

/* ===== Board: Indicators ===== */
function IndicatorsBoard() {
  const dots = [
    ['ok', false, false, 'связь / норма'],
    ['accent', true, false, 'активно / набор (пульс)'],
    ['warn', false, false, 'предупреждение'],
    ['danger', false, true, 'ошибка / ВВ (мигание)'],
    ['idle', false, false, 'неактивно'],
  ];
  const lines = [
    ['idle', false, false, 'val--dim', 'Нет связи · порт закрыт'],
    ['accent', true, false, 'val--accent', 'Подключение… запрос -inf'],
    ['ok', false, false, 'val--ok', 'Связь установлена · простой'],
    ['accent', true, false, 'val--accent', 'ИДЁТ НАБОР'],
    ['danger', false, true, 'val--danger', 'Ошибка связи · таймаут кадра'],
  ];
  return (
    <Board style={{ padding: 24 }}>
      <BoardHead t="Индикаторы и связь" s="Светодиодные точки с гало. Пульс — активность, мигание — тревога." />
      <div className="tokhead">Точки состояния</div>
      <div className="row" style={{ gap: 26, marginBottom: 22, flexWrap: 'wrap' }}>
        {dots.map(([k, p, b, lab]) => (
          <div key={lab} style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 9 }}>
            <Dot kind={k} pulse={p} blink={b} />
            <span style={{ fontSize: 10.5, color: 'var(--text-faint)', fontFamily: 'var(--mono)' }}>{k}</span>
            <span style={{ fontSize: 11, color: 'var(--text-dim)', maxWidth: 92, textAlign: 'center' }}>{lab}</span>
          </div>
        ))}
      </div>
      <div className="tokhead">Строки статуса связи</div>
      <div className="col" style={{ gap: 11 }}>
        {lines.map(([k, p, b, cls, txt], i) => (
          <div key={i} className="status-line">
            <Dot kind={k} pulse={p} blink={b} />
            <span className={'t ' + cls}>{txt}</span>
          </div>
        ))}
      </div>
      <div className="tokhead" style={{ marginTop: 22 }}>Индикатор отбраковки (meter)</div>
      <div style={{ width: 220 }}>
        <div className="row between" style={{ marginBottom: 4 }}><span className="lbl">Отбраковано</span><span className="val val--warn">0.42 %</span></div>
        <div className="meter"><div className="meter__fill meter__fill--warn" style={{ width: '4%' }} /></div>
      </div>
    </Board>
  );
}

/* ===== Board: Log & table ===== */
function LogTableBoard() {
  const log = [
    { d: 'sys', dir: '··', ts: '12:41:02', t: 'Порт COM7 открыт @ 600000 8N1' },
    { d: 'tx', dir: 'TX', ts: '12:41:02', t: '-inf', hex: '02 01 00 7E' },
    { d: 'rx', dir: 'RX', ts: '12:41:02', t: 'INFO sn=04F2A1 fw=2.3.1', hex: '8A 12 …' },
    { d: 'err', dir: 'ERR', ts: '12:41:14', t: 'CRC mismatch · кадр отброшен', hex: '?? 3F' },
  ];
  const rows = [
    { a: 1000, c: 2047, s: 17.9, f: 7.6, cls: '' },
    { a: 1500, c: 3072, s: 19.4, f: 6.9, cls: 'is-sel' },
    { a: 2000, c: 4098, s: 21.0, f: 6.4, cls: '' },
    { a: 3800, c: 7779, s: 31.4, f: 11.8, cls: 'is-bad' },
  ];
  return (
    <Board style={{ padding: 24 }}>
      <BoardHead t="Лог · таблица · значения" s="Моноширинные данные. Цветовая разметка направления кадра и аномалий." />
      <div className="tokhead">Строки лога shproto</div>
      <div className="log" style={{ flex: 'none', height: 'auto', marginBottom: 20, padding: '8px 10px' }}>
        {log.map((l, i) => (
          <div key={i} className={'log__line log__' + l.d}>
            <span className="log__ts">{l.ts}</span><span className="log__dir">{l.dir}</span><span>{l.t}</span>
            {l.hex && <span className="log__hex">  [{l.hex}]</span>}
          </div>
        ))}
      </div>
      <div className="row" style={{ gap: 30, alignItems: 'flex-start', flexWrap: 'wrap' }}>
        <div style={{ width: 320 }}>
          <div className="tokhead">Строки таблицы самотеста</div>
          <table className="tbl">
            <thead><tr><th>Ампл.</th><th>Центроид</th><th>σ</th><th>FWHM</th></tr></thead>
            <tbody>
              {rows.map((r) => <tr key={r.a} className={r.cls}><td>{r.a}</td><td>{r.c}</td><td>{r.s.toFixed(1)}</td><td>{r.f.toFixed(1)} %</td></tr>)}
            </tbody>
          </table>
          <div className="row" style={{ gap: 14, marginTop: 7, fontSize: 10.5, color: 'var(--text-faint)' }}>
            <span>обычная</span><span style={{ color: 'var(--accent)' }}>выбранная</span><span style={{ color: 'var(--warn)' }}>вне допуска</span>
          </div>
        </div>
        <div>
          <div className="tokhead">Крупное значение</div>
          <div className="big big--accent">1 842<span className="u">cps</span></div>
          <div className="lbl" style={{ marginTop: 6 }}>Скорость счёта · набор</div>
        </div>
      </div>
    </Board>
  );
}

/* ===== assemble canvas ===== */
function App() {
  return (
    <DesignCanvas>
      <DCSection id="main" title="Главное окно" subtitle="Докинг-раскладка · окно ~1200×740, ресайзится">
        <DCArtboard id="acq" label="Идёт набор · аннотировано" width={1700} height={800}><AcquiringAnnotated /></DCArtboard>
        <DCArtboard id="off" label="Отключено · пустые состояния" width={1200} height={740}><AppWindow state="disconnected" /></DCArtboard>
      </DCSection>
      <DCSection id="states" title="Состояния интерфейса" subtitle="Отключено · подключение · простой · набор · ошибка">
        <DCArtboard id="conn" label="Состояния связи" width={1580} height={620}><ConnStates /></DCArtboard>
      </DCSection>
      <DCSection id="tokens" title="Токены" subtitle="Палитра · типографика · отступы">
        <DCArtboard id="pal" label="Палитра" width={720} height={680}><PaletteBoard /></DCArtboard>
        <DCArtboard id="type" label="Типографика" width={720} height={620}><TypeBoard /></DCArtboard>
        <DCArtboard id="space" label="Сетка отступов" width={560} height={560}><SpacingBoard /></DCArtboard>
      </DCSection>
      <DCSection id="components" title="Состояния компонентов" subtitle="Кнопки · индикаторы · переключатели · лог · таблица">
        <DCArtboard id="btns" label="Кнопки · поля · команды" width={640} height={560}><ButtonsBoard /></DCArtboard>
        <DCArtboard id="ind" label="Индикаторы и связь" width={520} height={560}><IndicatorsBoard /></DCArtboard>
        <DCArtboard id="logtbl" label="Лог · таблица · значения" width={680} height={560}><LogTableBoard /></DCArtboard>
      </DCSection>
    </DesignCanvas>
  );
}
ReactDOM.createRoot(document.getElementById('root')).render(<App />);
