"""tpl_dac.py — эталон формы импульса тестового ЦАП (src/core/tpl_dac.h) с платы.

  python -I tpl_dac.py [--code 900] [--period 70] [--blocks 120] [--base <коды>] [--out tpl_dac.h]
                       [--port COM4] [--baud 600000]

Серия -dac <код> 65535 <период> (перезапуск каждые 10 порций), сырые порции "-osc raw", из них —
окна −8…+24 вокруг фронта (выше базы + 9, перед фронтом тихо, пик > база + 300). База — --base
или "-gst base" (замороженная в режиме ЦАП база детектора — та же, от которой он считает).

Почему не gfw.py tpl: тот выравнивает импульсы по оценке времени (центр тяжести) — у импульса ЦАП
(фронт за 1–2 отсчёта, плоская вершина, долгий спад) оценка смещена по фазе, и амплитуда подгонки
гуляла по фазе на ±0.9 % (замер 2026-11-06). Здесь все импульсы одной амплитуды, а фаза ступеньки
равномерна (добавка 0…44 такта, dactest.h) ⇒ фаза восстанавливается по рангу признака
f = (y[max+1] − y[max−1]) / y[max] (монотонен на периоде отсчёта): сортировка по f, 64 равные
группы = 64 фазы, строка эталона — среднее окно группы. Нормировка — наибольший пик по строкам
(Q14 = 16384). Разброс амплитуды по фазе после этого ±0.3 кода при σ шума 0.8.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfw  # noqa: E402


def raw_blocks(s, p, code, period, nblk):
    import numpy as np
    out = []
    for b in range(nblk):
        if b % 10 == 0:
            gfw.query(s, p, "-dac off", prefix="-ok dac")
            r = gfw.query(s, p, f"-dac {code} 65535 {period}", prefix="-ok dac")
            if not r or not r.startswith("-ok"):
                raise RuntimeError(f"-dac: {r}")
            time.sleep(0.01)
        ch = {}

        def on(c, pl):
            if c == gfw.CMD_PULSE:
                d = gfw.decode_pulse(pl)
                if d:
                    ch[d[0]] = d[2]
        s.write(gfw.build_frame(gfw.CMD_TEXT, b"-osc raw"))
        gfw.wait_text(s, p, lambda t: t.startswith("-ok osc raw") or t.startswith("-err"), 5, on)
        if len(ch) == 16:
            x = np.zeros(8192)
            for i, v in ch.items():
                x[i:i + len(v)] = v
            out.append(x)
    gfw.query(s, p, "-dac off", prefix="-ok dac")
    return np.array(out)


def windows(blocks, base, thr=9, amin=300, pre=8, post=24):
    import numpy as np
    w = []
    for x in blocks:
        i = pre + 1
        while i < len(x) - post - 16:
            if (x[i] > base + thr and x[i - 1] <= base + thr and x[i:i + 6].max() > base + amin
                    and np.all(np.abs(x[i - pre - 1:i - 1] - base) < 10)):
                w.append(x[i - pre:i + post] - base)
                i += post + 16
            else:
                i += 1
    return np.array(w)


def eq_template(y, nph=gfw.TPL_NPH, ns=gfw.TPL_NS, ipk=gfw.TPL_IPK):
    """Строки эталона (float, пик = 1) из окон y (n × W, без базы) равной амплитуды."""
    import numpy as np
    im = np.argmax(y, axis=1)
    ok = (im >= ipk + 1) & (im + ns - ipk <= y.shape[1])
    y, im = y[ok], im[ok]
    k = np.arange(len(y))
    w = np.stack([y[k, im - ipk + j] for j in range(ns)], 1)
    f = (y[k, im + 1] - y[k, im - 1]) / y[k, im]
    o = np.argsort(f)
    rows = np.array([w[g].mean(0) for g in np.array_split(o, nph)])
    return rows / rows[:, ipk].max()


def main(argv=None):
    import numpy as np
    ap = argparse.ArgumentParser()
    ap.add_argument("--code", type=int, default=900)   # V2: ≈2.46 кода АЦП/код, 1500 насыщает
    ap.add_argument("--period", type=int, default=70)
    ap.add_argument("--blocks", type=int, default=120)
    ap.add_argument("--base", type=float, default=None)
    ap.add_argument("--out", default="tpl_dac.h")
    ap.add_argument("--port", default=gfw.DEF_PORT)
    ap.add_argument("--baud", type=int, default=gfw.DEF_BAUD)
    a = ap.parse_args(argv)
    p = gfw.Parser()
    with gfw.open_port(a.port, a.baud) as s:
        s.reset_input_buffer()
        base = a.base
        if base is None:
            base = float(gfw.parse_kv(gfw.query(s, p, "-gst", prefix="-ok gst"))["base"])
        blocks = raw_blocks(s, p, a.code, a.period, a.blocks)
    y = windows(blocks, base)
    if len(y) < 1000:
        print(f"окон {len(y)} (< 1000)", file=sys.stderr)
        return 1
    q = gfw.tpl_quant(eq_template(y))
    A, ph, rel = gfw.tpl_fit(y, np.argmax(y, axis=1), q)
    dev = [A[ph == k].mean() - A.mean() for k in range(gfw.TPL_NPH) if (ph == k).sum() >= 5]
    print(f"порций {len(blocks)}, окон {len(y)}, база {base:.2f}; A {A.mean():.2f} sd {A.std():.2f}, "
          f"по фазам ±{max(abs(min(dev)), max(dev)):.2f}, невязка медиана {100 * np.median(rel):.3f} %")
    note = (f"эталон импульса тестового ЦАП (PA4 → R307/C318, ЦАП без буфера), с платы: tools/tpl_dac.py "
            f"--code {a.code} --period {a.period}, окон {len(y)}")
    open(a.out, "w", encoding="utf-8").write(gfw.tpl_header(q, "tpl_dac", note))
    print("записано:", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
