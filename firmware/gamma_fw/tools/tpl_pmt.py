"""tpl_pmt.py — эталон формы импульса (64 фазы × 7, Q14, как src/core/tpl_3467k.h) с реального
тракта ФЭУ + сцинтиллятор по сырым порциям АЦП ("-osc raw").

  python -I tpl_pmt.py collect <порций> [--out D:/gamma_data/tpl_pmt/raw_<время>.npz] [--port COM4]
        -stt (запомнить, шёл ли набор), -sto, затем <порций> раз "-osc raw" (8192 отсчёта ADC3,
        кадры 0x02 по 512); сырые порции → .npz (blocks: n × 8192 uint16), запись каждые 500
        порций и в конце. Если набор шёл — в конце -sta. ВВ не трогает.
  python -I tpl_pmt.py build <raw.npz> [<raw2.npz> …] [--out-h file.h] [--load] [--port COM4]
        разбор на ПК (см. ниже), печать сводки; --load — 64 × "-tplw" и "-tplok" (в силе сразу,
        во флеш НЕ пишет — это отдельная "-save" при остановленном наборе).

Разбор:
  * база — медиана порции; σ шума — 1.4826·MAD;
  * импульс — пересечение порога база + thr·σ (thr = 10) снизу вверх; ОДИНОЧНЫЙ — в ±40 отсчётов
    от пересечения нет другого пересечения порога, перед импульсом (−40…−3) все отсчёты в ±порога;
  * максимум — в 10 отсчётах после пересечения; амплитуда (наибольший отсчёт над базой) 100…1200
    кодов, сырой отсчёт < 4090 (без насыщения); широкое окно −4…+24 от максимума;
  * фаза — ранг признака f = (y[max+1] − y[max−1]) / y[max] (монотонен по фазе, как в tpl_dac.py):
    сортировка, 64 равные группы (фаза прихода равномерна);
  * нормировка импульса — √(Σy² − W·σ²) по широкому окну (для формы с полосой ниже fs/2 от фазы
    почти не зависит), среднее по группе → 64 широких строки;
  * строки чередуются на сетку 1/64 отсчёта (отсчёт j строки p → узел 64·j + p) — непрерывная форма;
    её пик (полином 4-й степени у максимума) = 1.0, строки эталона — как gen_template.py
    (gfw.tpl_rows_from_shape): строка p — отсчёты в k + p/64 от пика, окно от максимума − 1.
  Печать: число импульсов, невязка каждого импульса rms(w − A·h)/A (подгонка как в детекторе:
  одна амплитуда, перебор 64 фаз) к новому эталону и к tpl_3467k.h — среднее и σ, %; ширина
  (отсчётов выше 50 % пика, у отдельных импульсов и у непрерывной формы), сравнение формы.
"""
import argparse
import datetime
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfw  # noqa: E402

DEF_DIR = "D:/gamma_data/tpl_pmt"
BLK = 8192
PRE, POST = 4, 24          # широкое окно: −PRE … +POST от максимума
ISO = 40                   # одиночность: ± отсчётов от пересечения порога


# ---------------- сбор ----------------
def raw_block(s, p, timeout=5.0):
    """Одна сырая порция: массив 8192 кодов или None (потерян кусок / -err)."""
    import numpy as np
    ch, tot = {}, [0]

    def on(c, pl):
        if c == gfw.CMD_PULSE:
            d = gfw.decode_pulse(pl)
            if d:
                ch[d[0]] = d[2]
                tot[0] = d[1]
    s.write(gfw.build_frame(gfw.CMD_TEXT, b"-osc raw"))
    r = gfw.wait_text(s, p, lambda t: t.startswith("-ok osc raw") or t.startswith("-err"), timeout, on)
    if r is None or r.startswith("-err"):
        return None, r
    n = tot[0]
    if n != BLK or sum(len(v) for v in ch.values()) != n:
        return None, "неполная порция"
    x = np.zeros(n, dtype=np.uint16)
    for i, v in ch.items():
        x[i:i + len(v)] = v
    return x, r


def do_collect(a):
    import numpy as np
    out = a.out or os.path.join(DEF_DIR, "raw_%s.npz" % datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    p = gfw.Parser()
    blocks, bad = [], 0
    t0 = time.time()
    with gfw.open_port(a.port, a.baud) as s:
        s.reset_input_buffer()
        was = gfw.query(s, p, "-stt", prefix="-ok")
        collecting = bool(was) and "collecting" in was
        print("набор до сбора:", "шёл" if collecting else "стоял", flush=True)
        print("< -sto", gfw.query(s, p, "-sto", prefix="-ok"), flush=True)
        try:
            while len(blocks) < a.n:
                x, r = raw_block(s, p)
                if x is None:
                    bad += 1
                    if r and "acq" in r:
                        gfw.query(s, p, "-sto", prefix="-ok")
                    if bad > 50 and bad > len(blocks):
                        print("слишком много сбоев:", r, file=sys.stderr)
                        break
                    continue
                blocks.append(x)
                k = len(blocks)
                if k % 500 == 0 or k == a.n:
                    np.savez(out, blocks=np.array(blocks))
                    el = time.time() - t0
                    print(f"  порций {k}/{a.n}, сбоев {bad}, {el:.0f} с ({el / k * 1000:.0f} мс/порцию)", flush=True)
        finally:
            if blocks:
                np.savez(out, blocks=np.array(blocks))
            if collecting:
                print("< -sta", gfw.query(s, p, "-sta", prefix="-ok"), flush=True)
    print(f"записано {len(blocks)} порций: {out}")
    return 0 if blocks else 1


# ---------------- разбор ----------------
def find_singles(blocks, thr=10.0, amin=100.0, amax=1200.0, clip=4090):
    """Широкие окна одиночных импульсов: (окна n × W без базы, σ шума средняя, сводка)."""
    import numpy as np
    W = PRE + POST + 1
    wins, sig, st = [], [], dict(cross=0, single=0, amp=0)
    for x in blocks:
        x = x.astype(float)
        b = np.median(x)
        sd = 1.4826 * np.median(np.abs(x - b))
        sig.append(sd)
        y = x - b
        th = thr * sd
        up = np.flatnonzero((y[1:] > th) & (y[:-1] <= th)) + 1
        st["cross"] += len(up)
        for k, c in enumerate(up):
            if c - ISO < 0 or c + ISO >= len(y):
                continue
            if (k > 0 and up[k - 1] >= c - ISO) or (k + 1 < len(up) and up[k + 1] <= c + ISO):
                continue
            if np.any(np.abs(y[c - ISO:c - 2]) > th):
                continue
            st["single"] += 1
            im = c + int(np.argmax(y[c:c + 10]))
            A = y[im]
            if not (amin <= A <= amax) or x[im - PRE:im + POST + 1].max() >= clip:
                continue
            st["amp"] += 1
            wins.append(y[im - PRE:im + POST + 1])
    return np.array(wins).reshape(-1, W), float(np.mean(sig)), st


def interleave(rows, ipk):
    """Строки (64 × W, строка p — отсчёты в j + τ_p, τ растёт с p на 1/64) → (сетка x, форма S),
    x — время от пика непрерывной формы в отсчётах, S нормирована на пик = 1."""
    import numpy as np
    nph, W = rows.shape
    S = rows.T.reshape(-1)                          # узел 64·j + p
    # медиана по 5 узлам (5/64 отсчёта): одиночный выброс узла — фазовая группа на отсчёте максимума,
    # где смешались импульсы с максимумом на соседнем отсчёте (S 0.74 вместо 0.99, 2026-11-07); форму не искажает
    Sp = np.r_[S[:2], S, S[-2:]]
    S = np.median(np.stack([Sp[i:i + len(S)] for i in range(5)]), axis=0)
    g = np.arange(len(S), dtype=float)
    ip = int(np.argmax(S))
    m = np.abs(g - ip) <= nph // 2
    cf = np.polyfit(g[m] - ip, S[m], 4)
    xf = np.linspace(-nph / 2, nph / 2, 4001)
    pf = np.polyval(cf, xf)
    g0, peak = ip + xf[int(np.argmax(pf))], float(pf.max())
    return (g - g0) / nph, S / peak


def fit_tau(wins, x, S, step=1.0 / 128):
    """Время максимального отсчёта относительно пика формы S (τ, отсчёты) по всему широкому окну:
    перебор τ ∈ [−1, 1] с шагом step, амплитуда по МНК, уточнение параболой."""
    import numpy as np
    W = wins.shape[1]
    taus = np.arange(-1.0, 1.0 + step / 2, step)
    j = np.arange(W) - PRE
    H = np.array([np.interp(j + t, x, S, left=0.0, right=S[-1]) for t in taus])   # T × W
    a = (wins @ H.T) / (H * H).sum(1)[None, :]                                     # n × T
    r = (wins * wins).sum(1)[:, None] - a * (wins @ H.T)                           # n × T
    b = np.clip(np.argmin(r, axis=1), 1, len(taus) - 2)
    k = np.arange(len(wins))
    rm, r0, rp = r[k, b - 1], r[k, b], r[k, b + 1]
    den = rm - 2 * r0 + rp
    fr = np.where(den > 0, 0.5 * (rm - rp) / np.where(den > 0, den, 1.0), 0.0)
    return taus[b] + np.clip(fr, -1, 1) * step


def build_rows(wins, sigma, nph=gfw.TPL_NPH, refine=3):
    """(строки эталона 64 × 7 float, сетка x, форма S, фаза-группа каждого окна).
    Начало — ранг признака f; refine итераций — ранг времени τ, подогнанного к текущей форме по
    всему окну (у импульса с фронтом короче отсчёта y[max−1] ≈ 0 у половины фаз, и f по ним
    различает фазы только по спаду — грубо). Группы всегда равные (фаза прихода равномерна)."""
    import numpy as np
    f = (wins[:, PRE + 1] - wins[:, PRE - 1]) / wins[:, PRE]
    key = -f                                         # f убывает ⇒ τ (отсчёт − пик) растёт
    W = wins.shape[1]
    E = np.sqrt(np.maximum((wins * wins).sum(1) - W * sigma ** 2, 1e-9))
    wn = wins / E[:, None]
    grp = np.empty(len(wins), dtype=int)
    for it in range(refine + 1):
        o = np.argsort(key, kind="stable")
        rows = np.zeros((nph, W))
        for gi, idx in enumerate(np.array_split(o, nph)):
            rows[gi] = wn[idx].mean(0)
            grp[idx] = gi
        x, S = interleave(rows, PRE)
        if it < refine:
            # окно привязано к максимальному отсчёту ⇒ τ лежит на отрезке длиной ~1 отсчёт
            # внутри [−1, 1] без перехода через край — сортировка по τ без «заворота»
            key = fit_tau(wins, x, S)
    fn = lambda t: np.interp(t, x, S, left=0.0, right=S[-1])
    return gfw.tpl_rows_from_shape(fn), x, S, grp


def fit_stats(wins, q):
    """Подгонка как в детекторе по окну от максимума − 1: (A, фаза, rms невязки / A)."""
    import numpy as np
    im = np.full(len(wins), PRE)
    return gfw.tpl_fit(wins, im, q)


def width50(x, S):
    """Ширина непрерывной формы на половине высоты, отсчётов."""
    import numpy as np
    a = S >= 0.5
    i = np.flatnonzero(a)
    return float(x[i[-1]] - x[i[0]])


def old_shape(old):
    import numpy as np
    """Непрерывная форма старого эталона из его 7-отсчётных строк (строка p — отсчёты k + p/64)."""
    # строка p: отсчёт j ↔ время (j − 1) + τ_p, τ_p ≡ p/64 (mod 1), максимум ближе к пику —
    # перестановка строк по τ даёт тот же порядок, что у build_rows: начиная с первой «перескочившей»
    h = np.asarray(old, float) / gfw.TPL_QMAX
    f = (h[:, 2] - h[:, 0]) / h[:, 1]
    p0 = int(np.argmax(f))                           # наименьшее τ
    rows = np.roll(h, -p0, axis=0)
    return interleave(rows, 1)


def photopeak(A, lo=0.6):
    """Положение фотопика (самый высокий максимум сглаженной гистограммы выше квантиля lo):
    центроид и σ по ±8 % от максимума, итерации."""
    import numpy as np
    A = np.asarray(A, float)
    if len(A) < 50:
        return float("nan"), float("nan"), 0
    q = np.quantile(A, lo)
    h, ed = np.histogram(A, bins=np.arange(q, A.max() + 2, 1.0))
    if len(h) < 5:
        return float("nan"), float("nan"), 0
    hs = np.convolve(h, np.exp(-0.5 * (np.arange(-6, 7) / 2.5) ** 2), "same")
    c = 0.5 * (ed[int(np.argmax(hs))] + ed[int(np.argmax(hs)) + 1])
    for _ in range(5):
        m = np.abs(A - c) <= 0.08 * c
        c = float(A[m].mean())
    m = np.abs(A - c) <= 0.08 * c
    return c, float(A[m].std()), int(m.sum())


def centroid_near(A, c, w=0.08, it=6):
    import numpy as np
    for _ in range(it):
        m = np.abs(A - c) <= w * c
        if m.sum() < 10:
            return float("nan")
        c = float(A[m].mean())
    return c


def pp_scale(rows, wins, halfw=4, smooth=2):
    """Масштаб строк по фотопику: импульсы одной энергии должны давать одну амплитуду при любой
    фазе, выбранной подгонкой. Для каждой фазы p — центроид фотопика у импульсов с фазой в
    p ± halfw; строка умножается на P_p / P (подгонка с умноженной строкой даёт A / c), выбор фазы
    от масштаба строки не зависит (невязка при свободной амплитуде). Возврат: строки, множители."""
    import numpy as np
    nph = len(rows)
    A, ph, _ = fit_stats(wins, gfw.tpl_quant(rows))
    P0 = photopeak(A)[0]
    c = np.ones(nph)
    for p in range(nph):
        d = np.abs(((ph - p + nph // 2) % nph) - nph // 2)
        v = centroid_near(A[d <= halfw], P0)
        c[p] = v / P0 if np.isfinite(v) else np.nan
    c = np.where(np.isfinite(c), c, np.nanmean(c))
    k = np.ones(2 * smooth + 1) / (2 * smooth + 1)
    c = np.convolve(np.r_[c[-smooth:], c, c[:smooth]], k, "valid")
    c /= np.exp(np.mean(np.log(c)))
    out = rows * c[:, None]
    mx = np.abs(out).max()
    if mx > 1.0:
        out /= mx
    return out, c


def peak_by_phase(A, ph, name, nb=8):
    import numpy as np
    c0, s0, n0 = photopeak(A)
    cs = []
    for k in range(nb):
        m = (ph * nb // gfw.TPL_NPH) == k
        cs.append(photopeak(A[m])[0] if m.sum() > 100 else float("nan"))
    cs = np.array(cs)
    print(f"фотопик ({name}): {c0:.1f} кода, σ {s0:.2f} ({100 * s0 / c0:.2f} %, в окне {n0}); по фазам "
          + " ".join(f"{v:.1f}" for v in cs)
          + f"  размах {100 * (np.nanmax(cs) - np.nanmin(cs)) / c0:.2f} %")


def do_build(a):
    import numpy as np
    blocks = np.concatenate([np.load(fn)["blocks"] for fn in a.npz])
    test = None
    if a.split:                                      # эталон — по чётным порциям, проверка — по нечётным
        test = find_singles(blocks[1::2], thr=a.thr)[0]
        blocks = blocks[0::2]
    wins, sigma, st = find_singles(blocks, thr=a.thr)
    print(f"порций {len(blocks)} ({len(blocks) * BLK / 3.46667e6:.2f} с записи), σ шума {sigma:.2f} кода, "
          f"порог {a.thr:g}σ; пересечений {st['cross']}, одиночных {st['single']}, "
          f"в 100…1200 кодов {st['amp']}")
    if len(wins) < 64 * 10:
        print("мало импульсов", file=sys.stderr)
        return 1
    rows, x, S, grp = build_rows(wins, sigma, refine=a.refine)
    old = np.array(gfw.load_tpl_h())
    if a.pp:
        q0 = gfw.tpl_quant(rows)
        A0, ph0, _ = fit_stats(wins, q0)
        peak_by_phase(A0, ph0, "новый эталон без поправки по фотопику")
        ctot = np.ones(len(rows))
        for _ in range(a.pp_iters):                  # окно центроида ±8 % — большой сдвиг за раз не берёт
            r1, cpp = pp_scale(rows, wins)
            ctot *= np.abs(r1).max(1) / np.maximum(np.abs(rows).max(1), 1e-12)
            rows = r1
        print("поправка масштаба строк по фотопику: от %.3f до %.3f (отн. разброс)" % (ctot.min() / ctot.max(), 1.0))
    q = gfw.tpl_quant(rows)
    An, phn, rn = fit_stats(wins, q)
    Ao, pho, ro = fit_stats(wins, old)
    print(f"импульсов в эталоне: {len(wins)}, амплитуда (макс. отсчёт) средн. {wins[:, PRE].mean():.0f} кодов")
    print(f"невязка rms(w − A·h)/A, 7 отсчётов:  новый эталон  средн. {100 * rn.mean():.3f} %  σ {100 * rn.std():.3f} %  "
          f"медиана {100 * np.median(rn):.3f} %")
    print(f"                                     tpl_3467k     средн. {100 * ro.mean():.3f} %  σ {100 * ro.std():.3f} %  "
          f"медиана {100 * np.median(ro):.3f} %")
    for lo, hi in ((100, 300), (300, 700), (700, 1200)):
        m = (wins[:, PRE] >= lo) & (wins[:, PRE] < hi)
        if m.any():
            print(f"   амплитуда {lo:4d}…{hi:4d}: n {int(m.sum()):5d}  новый {100 * rn[m].mean():.3f} %  "
                  f"старый {100 * ro[m].mean():.3f} %")
    print(f"отношение A(старый)/A(новый): средн. {np.mean(Ao / An):.4f} σ {np.std(Ao / An):.4f}")
    n50 = (wins >= 0.5 * An[:, None]).sum(1)
    xo, So = old_shape(old)
    print(f"ширина: отсчётов выше 50 % у импульса — средн. {n50.mean():.2f} (σ {n50.std():.2f}); "
          f"FWHM формы новый {width50(x, S):.3f}, tpl_3467k {width50(xo, So):.3f} отсчёта "
          f"({width50(x, S) / 3.46667:.3f} / {width50(xo, So) / 3.46667:.3f} мкс)")
    peak_by_phase(An, phn, "новый эталон")
    peak_by_phase(Ao, pho, "tpl_3467k")
    print("форма (время от пика, отсчёты): новый | tpl_3467k | разность, % пика")
    for t in (-1.5, -1.0, -0.5, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0, 12.0, 16.0, 20.0):
        sn = np.interp(t, x, S)
        so = np.interp(t, xo, So) if xo[0] <= t <= xo[-1] else float("nan")
        print(f"   {t:+5.1f}: {sn:+.4f} | {so:+.4f} | {100 * (sn - so):+.2f}")
    i0 = int(np.argmin(S))
    print(f"наименьшее значение формы (выброс вниз): новый {S[i0]:+.4f} при {x[i0]:+.2f}, "
          f"tpl_3467k {So.min():+.4f} (в окне −1…+5)")
    if test is not None and len(test):
        At, pt, rt = fit_stats(test, q)
        Ato, pto, rto = fit_stats(test, old)
        print(f"ПРОВЕРКА на нечётных порциях ({len(test)} импульсов, в эталон не входили): невязка новый "
              f"{100 * rt.mean():.3f} % (σ {100 * rt.std():.3f}), tpl_3467k {100 * rto.mean():.3f} % (σ {100 * rto.std():.3f})")
        peak_by_phase(At, pt, "проверка, новый")
        peak_by_phase(Ato, pto, "проверка, tpl_3467k")
    dr = 100 * np.abs(q - old).max(1) / gfw.TPL_QMAX
    print(f"отличие строк от tpl_3467k: медиана {np.median(dr):.2f} %, наибольшее {dr.max():.2f} % пика")
    for pr in (0, 16, 32, 48):
        print(f"   фаза {pr:2d}: новый {[int(v) for v in q[pr]]}  старый {[int(v) for v in old[pr]]}")
    if a.out_h:
        note = f"эталон с реального тракта (tools/tpl_pmt.py, импульсов {len(wins)})"
        open(a.out_h, "w", encoding="utf-8", newline="\n").write(gfw.tpl_header(q, "tpl_pmt", note))
        print("записано:", a.out_h)
    if a.save_rows:
        np.savez(a.save_rows, q=q, x=x, S=S, wins=wins)
        print("записано:", a.save_rows)
    if not a.load:
        return 0
    p = gfw.Parser()
    with gfw.open_port(a.port, a.baud) as s:
        s.reset_input_buffer()
        for c in gfw.tpl_commands(q):
            r = gfw.query(s, p, c, prefix="-ok")
            if r != "-ok":
                print(c, "→", r or "нет ответа", file=sys.stderr)
                return 1
        r = gfw.query(s, p, "-tplok", prefix="-ok tpl")
        print("<", r, "(во флеш не записано — нужна -save при остановленном наборе)")
        return 0 if r == "-ok tpl" else 1


def main(argv=None):
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ap = argparse.ArgumentParser(description="эталон формы с реального тракта по сырым порциям")
    sub = ap.add_subparsers(dest="op", required=True)
    sp = sub.add_parser("collect")
    sp.add_argument("n", type=int)
    sp.add_argument("--out", default="")
    sp2 = sub.add_parser("build")
    sp2.add_argument("npz", nargs="+")
    sp2.add_argument("--thr", type=float, default=10.0)
    sp2.add_argument("--refine", type=int, default=3)
    sp2.add_argument("--split", action="store_true", help="эталон по чётным порциям, проверка по нечётным")
    sp2.add_argument("--pp-iters", type=int, default=4)
    sp2.add_argument("--no-pp", dest="pp", action="store_false", help="без поправки масштаба строк по фотопику")
    sp2.add_argument("--out-h", default="")
    sp2.add_argument("--save-rows", default="")
    sp2.add_argument("--load", action="store_true")
    for x in (sp, sp2):
        x.add_argument("--port", default=gfw.DEF_PORT)
        x.add_argument("--baud", type=int, default=gfw.DEF_BAUD)
    a = ap.parse_args(argv)
    return {"collect": do_collect, "build": do_build}[a.op](a)


if __name__ == "__main__":
    sys.exit(main())
