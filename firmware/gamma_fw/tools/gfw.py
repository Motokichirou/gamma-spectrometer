"""gfw.py — хост для прошивки gamma_fw: shproto-клиент и перепрошивка через системный загрузчик.

  python -I gfw.py cmd "<текст>" [--port COM4] [--baud 600000] [--wait 1.5]
        текстовая команда кадром 0x03, печать текстовых ответов (до первого ответа + 0.3 с тишины;
        для "-osc <n>" — до "-ok osc done", не дольше --wait); записи 0x02 — строкой-сводкой
  python -I gfw.py listen <секунд> [--port COM4] [--baud 600000]
        печать текстовых кадров 0x03 и статусов 0x04, счёт кадров спектра 0x01
  python -I gfw.py gst [--watch <с>] [--period 1] [--settle <В> [--tol 5]] [--port COM4]
        состояние -gst: без --watch — ключ=значение построчно; с --watch — строка раз в period
        (hv, уставка, рампа, выход, добавка, ШИМ, авария, +3.3V_D, VREF+, T); --settle — выйти,
        как только выход ВВ 3 опроса подряд в ±tol от заданного (код 0), иначе по таймауту — 1
  python -I gfw.py flash <bin> [--port COM4] [--baud 600000] [--no-boot]
        '!boot' на рабочей скорости (до 3 раз, пока прошивка не ответит '-ok boot'), закрыть порт,
        0.5 с, STM32_Programmer_CLI по UART 115200 8E1 (запись, сверка, старт), затем listen 5 с.
        --no-boot — МК уже в загрузчике (BOOT0 на +3.3 В или прошлая прошивка не дошла).
  python -I gfw.py osc <n> [--out pulses.npz] [--dac <код> [--dac-period 400]] [--port COM4]
        записать n окон импульсов (-osc пачками по 64, вне набора) → .npz: win (n×32 кода АЦП),
        base, amp, ph, pk, ch, pos, pre = 8 (имя на .npy заменяется на .npz). --dac — на время
        записи тестовые импульсы ЦАП (-dac <код> 65535 <период>, в конце -dac off). Печать:
        невязка формы против эталона прошивки (src/core/tpl_3467k.h; с --dac — tpl_dac.h, которым
        детектор подгоняет импульсы ЦАП) — медиана по импульсам rms(y − A·h)/A, %, подгонка как
        в детекторе (одна амплитуда, 64 фазы).
  python -I gfw.py tpl <pulses.npz> [--dry] [--out-h file.h] [--port COM4]
        эталон 64 фазы × 7 (Q14) из записанных окон: выравнивание по фазе (итерации подгонкой
        общей формы), усреднение на сетке 1/64 отсчёта, нормировка на непрерывный пик, строки
        как в gen_template.py. Печать: отличие от эталона прошивки, невязка данных на старом и
        новом. Без --dry — 64 × "-tplw <ф> <7 чисел>" и "-tplok" (в силе сразу, во флеш — по
        -save). --out-h — записать .h того же вида, что tpl_3467k.h.
  python -I gfw.py lin <код> [<код> …] [--n 10000] [--period 100] [--port COM4]
        линейность тракта по тестовым импульсам ЦАП: на каждую ступеньку -sto, -rst, -sta,
        -dac <код> <n> <период>, ждать "-ok dac done" и полный спектр после неё, -sto; пик —
        среднее и σ канала; по двум и более точкам — прямая, R² и наибольшее отклонение
        (% от канала последней точки). В конце набор остановлен, спектр — последней ступеньки.

Кадр shproto: FF FE [cmd][payload][CRC16 LSB][MSB] A5; FE/FD/A5 внутри — FD + ~байт;
CRC16 MODBUS по cmd + payload (как src/core/shproto.c).
Модуль импортируется тестами (tests/test_gfw.py): pyserial грузится только при работе с портом.
"""
import argparse
import os
import struct
import subprocess
import sys
import time

KEEPALIVE, START, ESC, FINISH = 0xFF, 0xFE, 0xFD, 0xA5
CMD_HIST, CMD_PULSE, CMD_TEXT, CMD_STAT = 0x01, 0x02, 0x03, 0x04
SPEC_MASK = 0x7FFFFFF

CLI = r"C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe"
DEF_PORT, DEF_BAUD, BOOT_BAUD = "COM4", 600000, 115200
CLI_TIMEOUT_S = 90


# ---------------- кадры ----------------
def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def _stuff(b):
    return bytes((ESC, ~b & 0xFF)) if b in (START, ESC, FINISH) else bytes((b,))


def build_frame(cmd, payload=b""):
    body = bytes((cmd,)) + bytes(payload)
    c = crc16(body)
    out = bytearray((KEEPALIVE, START))
    for b in body + bytes((c & 0xFF, c >> 8)):
        out += _stuff(b)
    out.append(FINISH)
    return bytes(out)


class Parser:
    """Приём кадров: как shproto_rx_byte, но без предела 256 байт (спектр — 2050)."""

    def __init__(self, max_payload=65536):
        self.max = max_payload
        self.state = 0          # 0 — ждём START, 1 — ждём cmd, 2 — данные
        self.esc = False
        self.cmd = 0
        self.buf = bytearray()
        self.bad = 0            # отброшенные кадры (CRC, переполнение)

    def feed(self, data):
        frames = []
        for b in data:
            if b == START:
                self.state, self.esc, self.buf = 1, False, bytearray()
                continue
            if self.state == 0:
                continue
            if b == FINISH and not self.esc:
                if self.state == 2 and len(self.buf) >= 2:
                    pl, rc = bytes(self.buf[:-2]), self.buf[-2] | (self.buf[-1] << 8)
                    if crc16(bytes((self.cmd,)) + pl) == rc:
                        frames.append((self.cmd, pl))
                    else:
                        self.bad += 1
                else:
                    self.bad += 1
                self.state = 0
                continue
            if b == ESC and not self.esc:
                self.esc = True
                continue
            if self.esc:
                b, self.esc = ~b & 0xFF, False
            if self.state == 1:
                self.cmd, self.state = b, 2
            elif len(self.buf) < self.max + 2:
                self.buf.append(b)
            else:
                self.state, self.bad = 0, self.bad + 1
        return frames


def decode_stat(pl):
    """0x04: elapsed u32 (с), cpu u16 (0.1 %), cps u32, invalid u32 — LE, 14 байт."""
    if len(pl) < 14:
        return None
    e, cpu, cps, inv = struct.unpack_from("<IHII", pl)
    return {"elapsed_s": e, "cpu_pct": cpu / 10.0, "cps": cps, "invalid": inv}


def decode_hist(pl):
    """0x01: offset u16 + каналы u32 (маска 27 бит)."""
    if len(pl) < 2 or (len(pl) - 2) % 4:
        return None
    off = pl[0] | (pl[1] << 8)
    n = (len(pl) - 2) // 4
    return off, [v & SPEC_MASK for v in struct.unpack_from("<%dI" % n, pl, 2)]


def decode_pulse(pl):
    """0x02 (осциллограф): index u16, total u16, отсчёты u16 — LE; запись режется по 512."""
    if len(pl) < 4 or len(pl) % 2:
        return None
    idx, total = struct.unpack_from("<HH", pl)
    n = (len(pl) - 4) // 2
    return idx, total, list(struct.unpack_from("<%dH" % n, pl, 4))


class PulseAsm:
    """Сборка записей 0x02 из кусков: feed → готовая запись (список отсчётов) или None."""

    def __init__(self):
        self.buf, self.total = [], 0

    def feed(self, pl):
        d = decode_pulse(pl)
        if d is None:
            return None
        idx, total, s = d
        if idx == 0:
            self.buf, self.total = [], total
        if idx != len(self.buf) or total != self.total:   # потерян кусок — запись отбросить
            self.buf, self.total = [], 0
            return None
        self.buf += s
        if self.total and len(self.buf) >= self.total:
            rec, self.buf, self.total = self.buf[:self.total], [], 0
            return rec
        return None


def pulse_summary(rec):
    n = len(rec)
    m = sum(rec) / n
    sd = (sum((v - m) ** 2 for v in rec) / n) ** 0.5
    return "[0x02] %d отсчётов: среднее %.2f σ %.2f мин %d макс %d" % (n, m, sd, min(rec), max(rec))


class SpecAsm:
    """Сборка спектра из 16 кадров 0x01 (по 512 каналов); готов по 0x04, если пришли все куски."""

    def __init__(self, nch=8192, chunk=512):
        self.nch, self.chunk = nch, chunk
        self.cur, self.got = [0] * nch, set()

    def feed_hist(self, pl):
        d = decode_hist(pl)
        if d is None:
            return
        off, vals = d
        if off + len(vals) <= self.nch:
            self.cur[off:off + len(vals)] = vals
            self.got.add(off)

    def feed_stat(self):
        """Кадр 0x04: полный спектр (список) или None; счёт кусков — заново."""
        full = len(self.got) == self.nch // self.chunk
        self.got = set()
        return list(self.cur) if full else None


def peak_stats(spec, lo=4):
    """Пик спектра: (среднее, σ, отсчётов) по окну ±max(5, 4σ) вокруг максимума (итерации);
    каналы ниже lo не смотрятся (порог, шум). None — пусто."""
    best = max(range(lo, len(spec)), key=lambda i: spec[i])
    if spec[best] == 0:
        return None
    m, sd, n = float(best), 2.0, 0
    for _ in range(6):
        w = max(5, int(4 * sd + 0.5))
        a, b = max(lo, int(m) - w), min(len(spec), int(m) + w + 1)
        n = sum(spec[a:b])
        if n == 0:
            return None
        m = sum(i * spec[i] for i in range(a, b)) / n
        sd = max(0.3, (sum((i - m) ** 2 * spec[i] for i in range(a, b)) / n) ** 0.5)
    return m, sd, n


def lin_fit(x, y):
    """Прямая y = a·x + b по МНК: (a, b, R², наибольшее |отклонение| в % от y последней точки)."""
    n = len(x)
    mx, my = sum(x) / n, sum(y) / n
    sxx = sum((v - mx) ** 2 for v in x)
    sxy = sum((u - mx) * (v - my) for u, v in zip(x, y))
    a = sxy / sxx
    b = my - a * mx
    res = [v - (a * u + b) for u, v in zip(x, y)]
    syy = sum((v - my) ** 2 for v in y)
    r2 = 1.0 - sum(r * r for r in res) / syy if syy else 1.0
    return a, b, r2, 100.0 * max(abs(r) for r in res) / abs(y[-1])


# ---------------- эталон формы ----------------
TPL_NPH, TPL_NS, TPL_IPK, TPL_Q = 64, 7, 1, 14
TPL_QMAX = 1 << TPL_Q
TPL_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "core", "tpl_pmt.h")   # эталон прошивки по умолчанию
TPL_DAC_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "core", "tpl_dac.h")


def parse_osc_line(text):
    """'osc i=3 pos=… ch=… amp=… ph=… pk=… base=…' → dict чисел; не та строка — None."""
    t = text.split()
    if not t or t[0] != "osc":
        return None
    d = {}
    for tok in t[1:]:
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                d[k] = float(v) if "." in v else int(v)
            except ValueError:
                return None
    return d if all(k in d for k in ("i", "amp", "ph", "pk", "base")) else None


def load_tpl_h(path=TPL_H):
    """Таблица эталона из .h (как tpl_3467k.h): список 64 строк по 7 целых."""
    import re
    txt = open(path, encoding="utf-8").read()
    rows = [[int(v) for v in m.group(1).split(",")]
            for m in re.finditer(r"\{\s*(-?\d+(?:\s*,\s*-?\d+){6})\s*\}", txt)]
    if len(rows) != TPL_NPH:
        raise ValueError(f"{path}: строк {len(rows)}, ждали {TPL_NPH}")
    return rows


def tpl_rows_from_shape(fn, nph=TPL_NPH, ns=TPL_NS, ipk=TPL_IPK):
    """Строки эталона из непрерывной формы fn(x) (x — время от пика в отсчётах, пик 1.0 в x = 0):
    как rows() в simulation/fix_c320/gen_template.py — отсчёты в (k + p/nph), окно от максимума − ipk."""
    import numpy as np
    out = []
    k = np.arange(-4, 24)
    for p in range(nph):
        y = fn(k + p / nph)
        im = int(np.argmax(y))
        w = y[im - ipk: im - ipk + ns]
        if len(w) != ns or int(np.argmax(w)) != ipk:
            raise ValueError(f"фаза {p}: максимум не на позиции {ipk}")
        out.append(w)
    return np.array(out)


def tpl_quant(rows):
    """Q14 с ограничением |v| ≤ 16384 (как проверяет -tplw)."""
    import numpy as np
    return np.clip(np.rint(np.asarray(rows) * TPL_QMAX), -TPL_QMAX, TPL_QMAX).astype(int)


def tpl_commands(q):
    return ["-tplw %d %s" % (p, " ".join(str(int(v)) for v in row)) for p, row in enumerate(q)]


def tpl_header(q, name="tpl_real", note="эталон с реального тракта (gfw.py tpl)"):
    g = name.upper()
    lines = [f"/* {name}.h — {note} (СГЕНЕРИРОВАНО). Формат как tpl_3467k.h:",
             f" * {TPL_NPH} фаз × {TPL_NS} отсчётов, Q{TPL_Q}, максимум строки на позиции {TPL_IPK}. */",
             f"#ifndef {g}_H", f"#define {g}_H", "", "#include <stdint.h>", "",
             f"static const int16_t {name}[{TPL_NPH}][{TPL_NS}] = {{"]
    lines += ["    { " + ", ".join(f"{int(v):6d}" for v in row) + " }," for row in q]
    lines += ["};", "", f"#endif /* {g}_H */", ""]
    return "\n".join(lines)


def prep_windows(win, base, pre, clip=4090, amin=20.0):
    """Окна → (y = окно − база, номер максимума, маска годных): максимум рядом с триггером,
    без упора в потолок АЦП, амплитуда ≥ amin, место под окно эталона и параболу."""
    import numpy as np
    win = np.asarray(win, dtype=float)
    y = win - np.asarray(base, dtype=float)[:, None]
    im = np.argmax(y, axis=1)
    ok = (im >= max(pre - 1, 2)) & (im <= pre + 12) & (im + TPL_NS < y.shape[1])   # V2: пик позже триггера
    ok &= win.max(axis=1) < clip
    ok &= y[np.arange(len(y)), im] >= amin
    return y, im, ok


def tpl_fit(y, im, q):
    """Подгонка как в детекторе (одна амплитуда, перебор 64 фаз, окно от максимума − 1):
    (A, фаза, rms невязки / A) на каждый импульс."""
    import numpy as np
    h = np.asarray(q, dtype=float) / TPL_QMAX                         # nph × ns
    idx = im[:, None] - TPL_IPK + np.arange(TPL_NS)[None, :]
    w = np.take_along_axis(y, idx, axis=1)                            # n × ns
    a = (w @ h.T) / (h * h).sum(1)[None, :]                           # n × nph
    r = ((w[:, None, :] - a[:, :, None] * h[None, :, :]) ** 2).sum(2)
    ph = np.argmin(r, axis=1)
    k = np.arange(len(y))
    A = a[k, ph]
    return A, ph, np.sqrt(r[k, ph] / TPL_NS) / np.maximum(A, 1e-9)


def build_template(win, base, pre=8, iters=4, nbin=64):
    """Эталон с тракта: (строки float nph × ns, сведения). win — сырые окна (n × W), base — база
    каждого окна, pre — номер отсчёта триггера в окне.
    1) время пика каждого импульса — парабола по трём отсчётам у максимума;
    2) общая форма — среднее нормированных отсчётов на сетке 1/nbin отсчёта (пустые ячейки —
       интерполяция), сглаживание по 5 ячейкам;
    3) уточнение: на каждый импульс — сдвиг δ (сетка 1/nbin в ±0.5) и амплитуда по МНК к общей
       форме (8 отсчётов от максимума − 2); повтор 2–3;
    4) нормировка: непрерывный пик формы = 1.0 в x = 0; строки как в gen_template.py."""
    import numpy as np
    y, im, ok = prep_windows(win, base, pre)
    y, im = y[ok], im[ok]
    n, W = y.shape
    if n < 50:
        raise ValueError(f"годных окон {n} (< 50)")
    # Масштаб каждого импульса — √Σy² по всему окну: для формы с полосой почти до fs/2 эта
    # «энергия» от фазы не зависит (Парсеваль), поэтому общая форма не искажается. Максимальный
    # отсчёт не годится (нормированный на себя, он всегда 1.0 и сплющивает вершину), сумма
    # по окну от максимума — тоже (окно прыгает на отсчёт с фазой). Масштаб в итерациях не
    # меняется: при подгонке амплитуды форма сходилась бы к своему же искажению.
    A = np.sqrt(np.maximum((y * y).sum(1), 1e-12))
    # время — центр тяжести y² по главному лепестку (отсчёты подряд выше нуля вокруг максимума):
    # крайние отсчёты лепестка весят мало, поэтому оценка непрерывна по фазе
    j = np.arange(W)[None, :]
    pos = y > 0
    lob = np.zeros_like(pos)
    lob[np.arange(n), im] = True
    for _ in range(W):
        grow = pos & (np.roll(lob, 1, axis=1) | np.roll(lob, -1, axis=1))
        grow[:, 0] &= lob[:, 1]
        grow[:, -1] &= lob[:, -2]
        if not (grow & ~lob).any():
            break
        lob |= grow
    w2 = np.where(lob, y * y, 0.0)
    t = (w2 * j).sum(1) / w2.sum(1)
    lo, hi = -pre - 2.0, W - pre + 2.0
    grid = np.arange(lo, hi, 1.0 / nbin)

    def shape_of(t, A):
        x = (j - t[:, None]).ravel()
        v = (y / A[:, None]).ravel()
        b = np.clip(np.rint((x - lo) * nbin).astype(int), 0, len(grid) - 1)
        s = np.bincount(b, v, len(grid))
        c = np.bincount(b, None, len(grid))
        m = c > 0
        f = np.interp(grid, grid[m], s[m] / c[m])
        return np.convolve(np.pad(f, 2, mode="edge"), np.ones(5) / 5.0, mode="valid")

    dl = np.arange(-nbin // 2, nbin // 2 + 1) / nbin
    sel = np.clip(im[:, None] - 2 + np.arange(8)[None, :], 0, W - 1)
    ysel = np.take_along_axis(y, sel, axis=1)
    def resid(S, d):
        """невязка МНК (свободная амплитуда) при сдвиге d (скаляр или по импульсу)"""
        h = np.interp(sel - (t + d)[:, None], grid, S)
        a = (ysel * h).sum(1) / np.maximum((h * h).sum(1), 1e-12)
        return ((ysel - a[:, None] * h) ** 2).sum(1)

    for _ in range(iters):
        S = shape_of(t, A)
        best_r = np.full(n, np.inf)
        best_d = np.zeros(n)
        for d in dl:
            r = resid(S, d)
            b = r < best_r
            best_r = np.where(b, r, best_r)
            best_d = np.where(b, d, best_d)
        # уточнение между узлами сетки: парабола по трём соседним сдвигам
        st = 1.0 / nbin
        rm, rp = resid(S, best_d - st), resid(S, best_d + st)
        den = rm - 2 * best_r + rp
        frac = np.where(den > 0, 0.5 * (rm - rp) / np.where(den > 0, den, 1.0), 0.0)
        t = t + best_d + np.clip(frac, -0.5, 0.5) * st
    S = shape_of(t, A)
    # пик формы: вершина плоская на десятки ячеек, шум ячеек сдвигает argmax на ±0.1 отсчёта —
    # полином 4-й степени по ±0.5 отсчёта вокруг максимума, максимум — на мелкой сетке
    ip = int(np.argmax(S))
    m = np.abs(grid - grid[ip]) <= 0.5
    cf = np.polyfit(grid[m] - grid[ip], S[m], 4)
    xf = np.linspace(-0.5, 0.5, 2001)
    pf = np.polyval(cf, xf)
    x0, peak = grid[ip] + xf[int(np.argmax(pf))], float(pf.max())
    rows = tpl_rows_from_shape(lambda x: np.interp(x + x0, grid, S) / peak)
    info = {"n": int(n), "rejected": int((~ok).sum()), "amp_mean": float(np.mean(A * peak))}
    return rows, info


def text_of(pl):
    return pl.decode("ascii", "replace").rstrip("\r\n")


GST_WATCH = ("hv", "hv_set", "hv_ramp", "hv_out", "hv_trim", "hv_pwm", "hv_trip", "vdd", "vref", "t")


def parse_kv(text):
    """Ответ '-ok gst k=v k=v … [hv_off=свободный текст]' → dict; не '-ok' → None.
    hv_off — всегда последний и может содержать пробелы (причина выключения)."""
    s = text.strip()
    if not s.startswith("-ok"):
        return None
    d = {}
    i = s.find(" hv_off=")
    if i >= 0:
        d["hv_off"], s = s[i + len(" hv_off="):], s[:i]
    for tok in s.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            d[k] = v
    return d


def gst_row(d, keys=GST_WATCH):
    return " ".join(f"{k}={d.get(k, '?')}" for k in keys)


def hv_settled(outs, target, tol, n=3):
    """Последние n значений hv_out в ±tol от target."""
    if len(outs) < n:
        return False
    try:
        return all(abs(float(v) - target) <= tol for v in outs[-n:])
    except ValueError:
        return False


def flash_cmdline(port, binpath, cli=CLI):
    return [cli, "-c", f"port={port}", f"br={BOOT_BAUD}", "P=EVEN", "db=8", "sb=1",
            "-w", binpath, "0x08000000", "-v", "-g", "0x08000000"]


def cli_summary(text):
    keys = ("rror", "verified", "Start operation", "Size", "Download", "Activating", "Application")
    return "\n".join(l for l in text.splitlines() if any(k in l for k in keys))


# ---------------- порт ----------------
def open_port(port, baud, timeout=0.05):
    import serial  # pyserial — только при работе с портом
    return serial.Serial(port, baud, timeout=timeout)


def _print_frames(frames, st):
    for cmd, pl in frames:
        if cmd == CMD_TEXT:
            st["text"] += 1
            print(text_of(pl), flush=True)
        elif cmd == CMD_STAT:
            st["stat"] += 1
            d = decode_stat(pl)
            print("[0x04] t=%(elapsed_s)d с cpu=%(cpu_pct).1f %% cps=%(cps)d invalid=%(invalid)d" % d
                  if d else "[0x04] короткий кадр", flush=True)
        elif cmd == CMD_HIST:
            st["hist"] += 1
        elif cmd == CMD_PULSE:
            st["pulse"] = st.get("pulse", 0) + 1
            rec = st.setdefault("asm", PulseAsm()).feed(pl)
            if rec:
                st["recs"] = st.get("recs", 0) + 1
                print(pulse_summary(rec), flush=True)
        else:
            st["other"] += 1


def osc_waits_done(text):
    """-osc и -osc <n> отвечают сразу, а окна шлют по готовности; -osc raw — всё до ответа."""
    t = text.split()
    return bool(t) and t[0] == "-osc" and (len(t) == 1 or t[1] != "raw")


def dac_waits_done(text):
    """-dac <код> <n> [период] отвечает сразу, а "-ok dac done <n>" шлёт по окончании серии."""
    t = text.split()
    return len(t) >= 3 and t[0] == "-dac"


def is_done_text(t):
    return t.startswith("-ok osc done") or t.startswith("-ok dac done") or t.startswith("-err")


def do_cmd(a):
    st = dict(text=0, stat=0, hist=0, other=0)
    p = Parser()
    with open_port(a.port, a.baud) as s:
        s.reset_input_buffer()
        s.write(build_frame(CMD_TEXT, a.text.encode("ascii")))
        t0 = time.time()
        last_text = None
        # -osc <n> и -dac <код> <n>: окончание приходит позже ответа — ждать "-ok osc done" /
        # "-ok dac done" (или -err), не дольше --wait
        need_done = osc_waits_done(a.text) or dac_waits_done(a.text)
        while time.time() - t0 < a.wait:
            fr = p.feed(s.read(4096))
            texts = [text_of(pl) for c, pl in fr if c == CMD_TEXT]
            if texts:
                last_text = time.time()
            if any(is_done_text(t) for t in texts):
                need_done = False
            _print_frames(fr, st)
            if last_text and not need_done and time.time() - last_text > 0.3:
                break
    if st["hist"] or st["stat"]:
        print(f"(попутно: 0x01 ×{st['hist']}, 0x04 ×{st['stat']})")
    if st.get("pulse"):
        print(f"(0x02: кадров {st['pulse']}, записей {st.get('recs', 0)})")
    if not st["text"]:
        print("нет текстового ответа", file=sys.stderr)
        return 1
    return 0


def listen(port, baud, secs):
    st = dict(text=0, stat=0, hist=0, other=0)
    p = Parser()
    with open_port(port, baud) as s:
        t0 = time.time()
        while time.time() - t0 < secs:
            _print_frames(p.feed(s.read(4096)), st)
    print(f"итого за {secs:g} с: 0x03 ×{st['text']}, 0x01 ×{st['hist']}, 0x04 ×{st['stat']}, "
          f"0x02 ×{st.get('pulse', 0)} (записей {st.get('recs', 0)}), прочих ×{st['other']}, битых ×{p.bad}")
    return st


def query(s, p, text, timeout=1.5, prefix=None):
    """Команда кадром 0x03, ждать текстовый ответ (с prefix или '-err'); None — не дождались."""
    s.write(build_frame(CMD_TEXT, text.encode("ascii")))
    t0 = time.time()
    while time.time() - t0 < timeout:
        for c, pl in p.feed(s.read(4096)):
            if c == CMD_TEXT:
                t = text_of(pl)
                if prefix is None or t.startswith(prefix) or t.startswith("-err"):
                    return t
    return None


def do_gst(a):
    p = Parser()
    outs = []
    with open_port(a.port, a.baud) as s:
        s.reset_input_buffer()
        t0 = time.time()
        while True:
            r = query(s, p, "-gst", prefix="-ok gst")
            d = parse_kv(r) if r else None
            if not a.watch:
                if d is None:
                    print(r or "нет ответа на -gst", file=sys.stderr)
                    return 1
                for k, v in d.items():
                    print(f"{k} = {v}")
                return 0
            el = time.time() - t0
            print(f"{el:6.1f} с  " + (gst_row(d) if d else (r or "нет ответа")), flush=True)
            if d and a.settle is not None:
                outs.append(d.get("hv_out", "nan"))
                if hv_settled(outs, a.settle, a.tol):
                    print(f"выход {a.settle:g} ± {a.tol:g} В за {el:.1f} с")
                    return 0
            if el >= a.watch:
                if a.settle is not None:
                    print(f"не устоялся за {a.watch:g} с", file=sys.stderr)
                    return 1
                return 0
            time.sleep(max(0.0, a.period - (time.time() - t0 - el)))


def wait_text(s, p, pred, timeout, on_frame=None):
    """Ждать текстовый кадр, для которого pred(text) истинно; все кадры — в on_frame. None — таймаут."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        for c, pl in p.feed(s.read(4096)):
            if on_frame:
                on_frame(c, pl)
            if c == CMD_TEXT and pred(text_of(pl)):
                return text_of(pl)
    return None


class OscBatch:
    """Приём пачки -osc: записи 0x02 и строки 'osc i=…' (идут парами: запись, строка)."""

    def __init__(self):
        self.asm, self.recs, self.infos = PulseAsm(), [], []

    def __call__(self, c, pl):
        if c == CMD_PULSE:
            r = self.asm.feed(pl)
            if r is not None:
                self.recs.append(r)
        elif c == CMD_TEXT:
            d = parse_osc_line(text_of(pl))
            if d is not None:
                self.infos.append(d)

    def pairs(self):
        """[(окно, сведения)] или None, если число записей и строк разошлось (потерян кадр)."""
        if len(self.recs) != len(self.infos):
            return None
        return list(zip(self.recs, self.infos))


def osc_collect(s, p, n, batch=64, timeout=15.0):
    """Записать n окон: -osc пачками по batch (потолок прошивки — 64); список (окно, сведения)."""
    got = []
    while len(got) < n:
        k = min(batch, n - len(got))
        ob = OscBatch()
        s.write(build_frame(CMD_TEXT, f"-osc {k}".encode("ascii")))
        r = wait_text(s, p, is_done_text, timeout, ob)
        if r is None or r.startswith("-err"):
            print("-osc:", r or "нет ответа", file=sys.stderr)
            break
        pr = ob.pairs()
        if pr is None:
            print(f"-osc: окон {len(ob.recs)}, строк {len(ob.infos)} — пачка отброшена", file=sys.stderr)
            continue
        got += pr
        print(f"  окон {len(got)}/{n}", flush=True)
        if not pr:
            print("-osc: импульсов нет (таймаут прошивки)", file=sys.stderr)
            break
    return got[:n]


def osc_arrays(got, pre=8):
    """[(окно, сведения)] → dict массивов для .npz (окна одной длины — самой частой)."""
    import numpy as np
    from collections import Counter
    W = Counter(len(w) for w, _ in got).most_common(1)[0][0]
    got = [(w, d) for w, d in got if len(w) == W]
    col = lambda k, t: np.array([d.get(k, 0) for _, d in got], dtype=t)
    return {"win": np.array([w for w, _ in got], dtype=np.uint16), "base": col("base", float),
            "amp": col("amp", float), "ph": col("ph", int), "pk": col("pk", int),
            "ch": col("ch", int), "pos": col("pos", np.int64), "pre": np.int64(pre)}


def report_residual(win, base, pre, q, name):
    import numpy as np
    y, im, ok = prep_windows(win, base, pre)
    if not ok.any():
        print(f"невязка на {name}: годных окон нет")
        return None
    A, _, rel = tpl_fit(y[ok], im[ok], q)
    med = float(np.median(rel))
    print(f"невязка на {name}: медиана {100 * med:.2f} %, 90 % — {100 * np.percentile(rel, 90):.2f} % "
          f"(A средн. {A.mean():.1f}, годных {int(ok.sum())} из {len(ok)})")
    return med


def do_osc(a):
    import numpy as np
    out = a.out if a.out.endswith(".npz") else os.path.splitext(a.out)[0] + ".npz"
    p = Parser()
    with open_port(a.port, a.baud) as s:
        s.reset_input_buffer()
        if a.dac:
            r = query(s, p, f"-dac {a.dac} 65535 {a.dac_period}", prefix="-ok dac")
            print("<", r)
            if not r or not r.startswith("-ok"):
                return 1
        try:
            got = osc_collect(s, p, a.n)
        finally:
            if a.dac:
                print("<", query(s, p, "-dac off", prefix="-ok dac"))
    if not got:
        return 1
    z = osc_arrays(got)
    np.savez(out, **z)
    print(f"записано {len(z['win'])} окон по {z['win'].shape[1]} отсчётов: {out}")
    if a.dac:   # в режиме ЦАП детектор подгоняет эталоном tpl_dac.h (main.c, dactest.h)
        report_residual(z["win"], z["base"], int(z["pre"]), load_tpl_h(TPL_DAC_H), "эталоне ЦАП (tpl_dac.h)")
    else:
        report_residual(z["win"], z["base"], int(z["pre"]), load_tpl_h(), "эталоне прошивки")
    return 0


def do_tpl(a):
    import numpy as np
    z = np.load(a.npz)
    win, base, pre = z["win"], z["base"], int(z["pre"])
    rows, info = build_template(win, base, pre)
    q = tpl_quant(rows)
    old = np.array(load_tpl_h())
    print(f"окон {info['n']} (отброшено {info['rejected']}), A средн. {info['amp_mean']:.1f}")
    dr = 100 * np.abs(q - old).max(1) / TPL_QMAX     # по строкам; у строк, где максимум окна
    # переходит на соседний отсчёт, отличие может быть большим и при близкой форме — смотреть медиану
    print(f"отличие от эталона прошивки по строкам: медиана {np.median(dr):.2f} %, наибольшее {dr.max():.2f} % пика")
    report_residual(win, base, pre, old, "эталоне прошивки")
    report_residual(win, base, pre, q, "новом эталоне")
    if a.out_h:
        with open(a.out_h, "w", encoding="utf-8", newline="\n") as f:
            f.write(tpl_header(q))
        print("записано:", a.out_h)
    if a.dry:
        return 0
    p = Parser()
    with open_port(a.port, a.baud) as s:
        s.reset_input_buffer()
        for c in tpl_commands(q):
            r = query(s, p, c, prefix="-ok")
            if r != "-ok":
                print(c, "→", r or "нет ответа", file=sys.stderr)
                return 1
        r = query(s, p, "-tplok", prefix="-ok tpl")
        print("<", r)
        return 0 if r == "-ok tpl" else 1


def lin_point(s, p, code, n, period):
    """Одна ступенька: спектр от n импульсов ЦАП → (среднее, σ, отсчётов) или None."""
    spec = SpecAsm()
    last = {"spec": None}

    def on(c, pl):
        if c == CMD_HIST:
            spec.feed_hist(pl)
        elif c == CMD_STAT:
            full = spec.feed_stat()
            if full is not None:
                last["spec"] = full

    for c in ("-sto", "-rst", "-sta"):
        query(s, p, c, prefix="-ok")
    r = query(s, p, f"-dac {code} {n} {period}", prefix="-ok dac")
    if not r or not r.startswith("-ok"):
        print(f"-dac {code}:", r or "нет ответа", file=sys.stderr)
        query(s, p, "-sto", prefix="-ok")
        return None
    if wait_text(s, p, lambda t: t.startswith("-ok dac done"), n * period * 1e-6 + 5.0, on) is None:
        print(f"-dac {code}: нет '-ok dac done'", file=sys.stderr)
    last["spec"] = None                       # нужен спектр, выданный ПОСЛЕ конца серии:
    spec.got = set()                          # куски, пришедшие до "done", не в счёт (иначе в
                                              # «полном» спектре — часть кусков с середины серии)
    t0 = time.time()
    while last["spec"] is None and time.time() - t0 < 3.0:
        wait_text(s, p, lambda t: False, 0.2, on)
    query(s, p, "-sto", prefix="-ok")
    return peak_stats(last["spec"]) if last["spec"] else None


def do_lin(a):
    xs, ys = [], []
    p = Parser()
    with open_port(a.port, a.baud) as s:
        s.reset_input_buffer()
        for code in a.codes:
            r = lin_point(s, p, code, a.n, a.period)
            if r is None:
                print(f"ступенька {code}: пика нет")
                continue
            m, sd, cnt = r
            print(f"ступенька {code:5d}: канал {m:9.2f}  σ {sd:5.2f}  в пике {cnt} из {a.n}", flush=True)
            xs.append(code)
            ys.append(m)
    if len(xs) >= 2:
        k, b, r2, dev = lin_fit(xs, ys)
        print(f"прямая: канал = {k:.4f}·код + {b:.2f}; R² = {r2:.6f}; наибольшее отклонение {dev:.3f} %")
    return 0 if xs else 1


def do_listen(a):
    listen(a.port, a.baud, a.secs)
    return 0


def do_flash(a):
    binpath = os.path.abspath(a.bin)
    if not os.path.isfile(binpath):
        print("нет файла", binpath, file=sys.stderr)
        return 2
    if not a.no_boot:
        acked = False
        p = Parser()
        with open_port(a.port, a.baud) as s:
            s.reset_input_buffer()
            for attempt in range(3):
                s.write(b"!boot")
                t0 = time.time()
                while time.time() - t0 < 0.4 and not acked:
                    for c, pl in p.feed(s.read(256)):
                        if c == CMD_TEXT:
                            print("<", text_of(pl))
                            acked = acked or "-ok boot" in text_of(pl)
                if acked:
                    break
        print("прошивка ушла в загрузчик" if acked else
              "ответа '-ok boot' нет — пробую загрузчик как есть (уже в нём? другая скорость?)")
    time.sleep(0.5)
    cmdline = flash_cmdline(a.port, binpath)
    print(" ".join(cmdline))
    try:
        r = subprocess.run(cmdline, capture_output=True, timeout=CLI_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        print(f"STM32_Programmer_CLI: таймаут {CLI_TIMEOUT_S} с", file=sys.stderr)
        return 3
    out = r.stdout.decode("cp866", "replace") + r.stderr.decode("cp866", "replace")
    print(cli_summary(out))
    print("rc", r.returncode)
    if r.returncode:
        print(out[-2000:])
    time.sleep(0.3)
    listen(a.port, a.baud, 5.0)
    return 0 if r.returncode == 0 else 4


def main(argv=None):
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ap = argparse.ArgumentParser(description="хост gamma_fw: shproto и перепрошивка")
    sub = ap.add_subparsers(dest="op", required=True)
    for name in ("cmd", "listen", "flash", "gst", "osc", "tpl", "lin"):
        sp = sub.add_parser(name)
        if name == "cmd":
            sp.add_argument("text")
            sp.add_argument("--wait", type=float, default=1.5)
        elif name == "listen":
            sp.add_argument("secs", type=float)
        elif name == "gst":
            sp.add_argument("--watch", type=float, default=0.0)
            sp.add_argument("--period", type=float, default=1.0)
            sp.add_argument("--settle", type=float, default=None)
            sp.add_argument("--tol", type=float, default=5.0)
        elif name == "osc":
            sp.add_argument("n", type=int)
            sp.add_argument("--out", default="pulses.npz")
            sp.add_argument("--dac", type=int, default=0, help="код ступеньки ЦАП на время записи")
            sp.add_argument("--dac-period", type=int, default=400)
        elif name == "tpl":
            sp.add_argument("npz")
            sp.add_argument("--dry", action="store_true")
            sp.add_argument("--out-h", default="")
        elif name == "lin":
            sp.add_argument("codes", type=int, nargs="+")
            sp.add_argument("--n", type=int, default=10000)
            sp.add_argument("--period", type=int, default=100)
        else:
            sp.add_argument("bin")
            sp.add_argument("--no-boot", action="store_true")
        sp.add_argument("--port", default=DEF_PORT)
        sp.add_argument("--baud", type=int, default=DEF_BAUD)
    a = ap.parse_args(argv)
    return {"cmd": do_cmd, "listen": do_listen, "flash": do_flash, "gst": do_gst,
            "osc": do_osc, "tpl": do_tpl, "lin": do_lin}[a.op](a)


if __name__ == "__main__":
    sys.exit(main())
