"""Тесты tools/gfw.py без COM-порта: CRC, кадры, разбор, пакеты 0x01/0x04, команда прошивки.
Запуск: python -I tests/test_gfw.py (из run_tests.cmd)."""
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gfw", os.path.join(HERE, "..", "tools", "gfw.py"))
gfw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gfw)

passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("  FAIL", what)


# CRC16 MODBUS — тот же вектор, что в test_shproto.c
check(gfw.crc16(b"123456789") == 0x4B37, "crc16 123456789")

# кадр: FF FE cmd payload crc A5; стаффинг FE/FD/A5 (в т. ч. в cmd и CRC)
f = gfw.build_frame(3, b"-inf")
c = gfw.crc16(b"\x03-inf")
check(f[:3] == b"\xff\xfe\x03" and f[3:7] == b"-inf" and f[-1] == 0xA5, "структура кадра")
check(f[7:-1] == bytes(gfw._stuff(c & 0xFF) + gfw._stuff(c >> 8)), "CRC младшим вперёд")
fs = gfw.build_frame(3, bytes([0xFE, 0xFD, 0xA5, 0x00, 0xFF]))
check(fs[3:11] == bytes([0xFD, 0x01, 0xFD, 0x02, 0xFD, 0x5A, 0x00, 0xFF]), "стаффинг FE/FD/A5")
check(0xFE not in fs[2:] and 0xA5 not in fs[2:-1], "внутри кадра нет START/FINISH")

# разбор: круг, несколько кадров в одном куске, побайтно
p = gfw.Parser()
pl = bytes(range(256)) * 9                       # больше 2 КБ, все значения байтов
fr = p.feed(gfw.build_frame(1, pl) + gfw.build_frame(3, b"ok"))
check(fr == [(1, pl), (3, b"ok")], "два кадра подряд, длинный payload")
p = gfw.Parser()
out = []
for b in gfw.build_frame(4, b"\xa5" * 14):
    out += p.feed(bytes([b]))
check(out == [(4, b"\xa5" * 14)], "побайтная подача")

# битый CRC — отброшен; обрывок без FINISH и мусор — ресинхронизация по START
p = gfw.Parser()
bad = bytearray(gfw.build_frame(3, b"abc"))
bad[4] ^= 1
fr = p.feed(bytes(bad) + b"\xff\xfe\x03xyz" + b"garbage" + gfw.build_frame(3, b"good"))
check(fr == [(3, b"good")] and p.bad == 1, "битый CRC и обрывок")
p = gfw.Parser(max_payload=8)
fr = p.feed(gfw.build_frame(3, b"x" * 20) + gfw.build_frame(3, b"y"))
check(fr == [(3, b"y")] and p.bad >= 1, "переполнение — кадр отброшен")
p = gfw.Parser()
check(p.feed(b"\xfe\x03\xfd\xa5") == [] and p.feed(gfw.build_frame(3, b"z")) == [(3, b"z")], "ESC перед FINISH")

# пакеты
st = gfw.decode_stat(struct.pack("<IHII", 61, 123, 4500, 7))
check(st == {"elapsed_s": 61, "cpu_pct": 12.3, "cps": 4500, "invalid": 7}, "0x04")
check(gfw.decode_stat(b"\x00" * 13) is None, "0x04 короткий")
h = gfw.decode_hist(struct.pack("<H3I", 7680, 1, 0xFFFFFFFF, 0x8000005))
check(h == (7680, [1, 0x7FFFFFF, 5]), "0x01: offset и маска 27 бит")
check(gfw.decode_hist(b"\x00\x00\x01") is None, "0x01 неполный канал")
check(gfw.text_of(b"-ok\r\n") == "-ok", "текст без CRLF")

# командная строка прошивки — как в SPEC §4 и bringup/tools/reflash.py
cl = gfw.flash_cmdline("COM4", "fw.bin")
check(cl[1:] == ["-c", "port=COM4", "br=115200", "P=EVEN", "db=8", "sb=1",
                 "-w", "fw.bin", "0x08000000", "-v", "-g", "0x08000000"], "STM32_Programmer_CLI")
check(cl[0].endswith("STM32_Programmer_CLI.exe"), "путь CLI")
check(gfw.DEF_BAUD == 600000 and gfw.CLI_TIMEOUT_S == 90, "скорость и таймаут")

# -gst: ключ=значение, hv_off — последним со свободным текстом
d = gfw.parse_kv("-ok gst hv=on hv_set=500 hv_out=499.2 hv_trip=- t=24.81 flash=none hv_off=TRIP VREF+ out of range")
check(d is not None and d["hv"] == "on" and d["hv_set"] == "500" and d["hv_out"] == "499.2", "parse_kv значения")
check(d["hv_off"] == "TRIP VREF+ out of range" and d["flash"] == "none", "parse_kv hv_off с пробелами")
check(gfw.parse_kv("-err notready") is None, "parse_kv -err")
check(gfw.gst_row({"hv": "off", "hv_out": "0.0"}, ("hv", "hv_out", "t")) == "hv=off hv_out=0.0 t=?", "строка watch")
check(not gfw.hv_settled(["495", "497"], 500, 5), "settle: мало замеров")
check(gfw.hv_settled(["300", "496", "501.2", "504.9"], 500, 5), "settle: 3 подряд в допуске")
check(not gfw.hv_settled(["496", "506", "501"], 500, 5), "settle: выброс")
check(not gfw.hv_settled(["nan", "x", "500"], 500, 5), "settle: нечисло")
# 0x02: запись режется по 512 отсчётов, index — номер первого отсчёта, total — длина записи
rec = [(645 + i) & 0xFFFF for i in range(1100)]
chunks = [struct.pack("<HH%dH" % len(rec[i:i + 512]), i, len(rec), *rec[i:i + 512]) for i in range(0, 1100, 512)]
check(gfw.decode_pulse(chunks[2])[:2] == (1024, 1100) and len(gfw.decode_pulse(chunks[2])[2]) == 76, "0x02 разбор")
check(gfw.decode_pulse(b"\x00\x00\x01") is None, "0x02 нечётная длина")
asm = gfw.PulseAsm()
out = [asm.feed(c) for c in chunks]
check(out[:2] == [None, None] and out[2] == rec, "0x02 сборка записи")
asm = gfw.PulseAsm()
check(asm.feed(chunks[0]) is None and asm.feed(chunks[2]) is None, "0x02 пропуск куска")
check(asm.feed(chunks[0]) is None and asm.feed(chunks[1]) is None and asm.feed(chunks[2]) == rec,
      "0x02 после пропуска — следующая запись цела")
w = [645, 700, 660] + [645] * 29
check(gfw.PulseAsm().feed(struct.pack("<HH32H", 0, 32, *w)) == w, "0x02 окно -osc одним кадром")
check(gfw.pulse_summary([1, 3]) == "[0x02] 2 отсчётов: среднее 2.00 σ 1.00 мин 1 макс 3", "сводка 0x02")
check(gfw.osc_waits_done("-osc 8") and gfw.osc_waits_done("-osc"), "-osc n ждёт done")
check(not gfw.osc_waits_done("-osc raw") and not gfw.osc_waits_done("-oscx 3")
      and not gfw.osc_waits_done("-noise"), "прочие не ждут")
check(gfw.dac_waits_done("-dac 200 10000") and gfw.dac_waits_done("-dac 200 10 400"), "-dac серия ждёт done")
check(not gfw.dac_waits_done("-dac") and not gfw.dac_waits_done("-dac off"), "-dac запрос/off не ждут")
check(gfw.is_done_text("-ok dac done 10000") and gfw.is_done_text("-ok osc done 3")
      and gfw.is_done_text("-err dac busy") and not gfw.is_done_text("-ok dac step=1 n=2 period=100"),
      "признак окончания")

# ---- Task 11: спектр, пик, линейность ----
sa = gfw.SpecAsm()
for off in range(0, 8192, 512):
    vals = [0] * 512
    if off == 1024:
        vals[100] = 5
    sa.feed_hist(struct.pack("<H512I", off, *vals))
full = sa.feed_stat()
check(full is not None and full[1124] == 5 and sum(full) == 5, "спектр из 16 кусков")
sa.feed_hist(struct.pack("<H512I", 0, *([0] * 512)))
check(sa.feed_stat() is None, "неполный спектр — None")
import math
spec = [0] * 8192
for i in range(1100, 1221):                     # гаусс: центр 1160.3, σ 2.5
    spec[i] = int(round(1000 * math.exp(-0.5 * ((i - 1160.3) / 2.5) ** 2)))
spec[2] = 50                                    # ниже lo — не смотреть
pk = gfw.peak_stats(spec)
check(pk is not None and abs(pk[0] - 1160.3) < 0.05 and abs(pk[1] - 2.5) < 0.1, "пик: среднее и σ")
check(gfw.peak_stats([0] * 8192) is None, "пик: пусто")
k, b, r2, dev = gfw.lin_fit([100, 400, 700, 1000], [310.0, 1210.0, 2110.0, 3010.0])
check(abs(k - 3.0) < 1e-9 and abs(b - 10.0) < 1e-9 and abs(r2 - 1.0) < 1e-12 and dev < 1e-9, "прямая точная")
k, b, r2, dev = gfw.lin_fit([100, 400, 700, 1000], [310.0, 1210.0, 2116.0, 3010.0])
check(r2 < 0.999999 and 0.1 < dev < 0.2, "прямая: отклонение в % от последней точки")

# ---- Task 11: эталон с тракта ----
import numpy as np
q0 = gfw.load_tpl_h()
check(len(q0) == 64 and all(len(r) == 7 and max(r) <= 16384 and r.index(max(r)) == 1 for r in q0),
      "tpl_3467k.h: 64 × 7, максимум на позиции 1")
cmds = gfw.tpl_commands(np.array(q0))
check(len(cmds) == 64 and cmds[0] == "-tplw 0 " + " ".join(str(v) for v in q0[0]), "-tplw строки")
check(gfw.parse_osc_line("osc i=3 pos=123456 ch=1160 amp=580.25 ph=17 pk=2 base=645.50")
      == {"i": 3, "pos": 123456, "ch": 1160, "amp": 580.25, "ph": 17, "pk": 2, "base": 645.5},
      "строка осциллографа")
check(gfw.parse_osc_line("-ok osc done 3") is None, "не строка осциллографа")
check(gfw.tpl_quant([[1.2, -1.5, 0.5]]).tolist() == [[16384, -16384, 8192]], "Q14 с ограничением")


def cr_rc2(nbin=256, fs=3466666.67, tau_s=0.23e-6, tau_cr=1e-6, tau_rc=0.51e-6):
    """Непрерывная форма CR-RC² от экспоненты сцинтиллятора: fn(x), x — отсчёты от пика."""
    dt = 1.0 / fs / nbin
    t = np.arange(0, 30 / fs, dt)
    x = np.exp(-t / tau_s)
    out = []
    for tau, hp in ((tau_cr, True), (tau_rc, False), (tau_rc, False)):
        a = dt / (tau + dt)
        y = np.zeros_like(x)
        acc = 0.0
        for i, v in enumerate(x):
            acc += a * (v - acc)
            y[i] = v - acc if hp else acc
        x = y
    x = x / x.max()
    tt = (t - t[np.argmax(x)]) * fs
    return lambda q: np.interp(q, tt, x, left=0.0, right=float(x[-1]))


def synth(fn, n, rng, sigma=1.8, base=645.0, thr=9, pre=8, W=32):
    win, bases = [], []
    for _ in range(n):
        A = rng.uniform(300, 900)
        t0 = 40 + rng.uniform(0, 1)
        k = np.arange(120)
        s = np.rint(base + A * fn(k - t0) + rng.normal(0, sigma, len(k)))
        tr = int(np.argmax(s > base + thr))
        win.append(s[tr - pre: tr - pre + W])
        bases.append(base + rng.normal(0, 0.05))
    return np.array(win), np.array(bases)


rng = np.random.default_rng(7)
fn = cr_rc2()
exact = gfw.tpl_rows_from_shape(fn)
win, bases = synth(fn, 2000, rng)
rows, info = gfw.build_template(win, bases, 8)
# Положение пика по самой форме определено до ≈0.015 отсчёта (вершина плоская) — это ≈1 шаг фазы:
# строка у точки, где максимум окна переходит на соседний отсчёт, может взять другое окно.
# Поэтому строки — с допуском, а главное — амплитуда и невязка подгонки (ниже).
err = np.abs(rows - exact).max(1)
check(info["n"] >= 1990 and (err < 0.02).sum() >= 62, "эталон с тракта совпал с точным по строкам")
y, im, ok = gfw.prep_windows(win, bases, 8)
_, _, rel_new = gfw.tpl_fit(y[ok], im[ok], gfw.tpl_quant(rows))
_, _, rel_ex = gfw.tpl_fit(y[ok], im[ok], gfw.tpl_quant(exact))
fn_w = cr_rc2(tau_rc=0.60e-6)                   # чужая форма — невязка заметно больше
_, _, rel_bad = gfw.tpl_fit(y[ok], im[ok], gfw.tpl_quant(gfw.tpl_rows_from_shape(fn_w)))
check(np.median(rel_new) < 1.05 * np.median(rel_ex), "невязка нового эталона — как у точного")
check(np.median(rel_bad) > 1.5 * np.median(rel_new), "невязка чужого эталона больше")
check(np.median(rel_ex) < 0.006, "невязка точного эталона на уровне шума (σ/A)")
A, ph, _ = gfw.tpl_fit(y[ok], im[ok], gfw.tpl_quant(rows))
Ax, _, _ = gfw.tpl_fit(y[ok], im[ok], gfw.tpl_quant(exact))
check(abs(np.mean(A / Ax) - 1.0) < 0.002 and np.std(A / Ax) < 0.002, "амплитуда: новый эталон = точный (0.2 %)")
hdr = gfw.tpl_header(gfw.tpl_quant(rows))
import tempfile
with tempfile.TemporaryDirectory() as td:
    pth = os.path.join(td, "t.h")
    open(pth, "w", encoding="utf-8").write(hdr)
    check(gfw.load_tpl_h(pth) == gfw.tpl_quant(rows).tolist(), ".h эталона читается обратно")
z = gfw.osc_arrays([([645] * 32, {"i": 0, "pos": 5, "ch": 10, "amp": 5.5, "ph": 3, "pk": 2, "base": 645.0}),
                    ([645] * 31, {"i": 1, "pos": 6, "ch": 11, "amp": 6.5, "ph": 4, "pk": 2, "base": 645.0}),
                    ([646] * 32, {"i": 2, "pos": 7, "ch": 12, "amp": 7.5, "ph": 5, "pk": 3, "base": 645.5})])
check(z["win"].shape == (2, 32) and z["ch"].tolist() == [10, 12] and int(z["pre"]) == 8, "массивы .npz")
ob = gfw.OscBatch()
ob(gfw.CMD_PULSE, struct.pack("<HH32H", 0, 32, *([645] * 32)))
ob(gfw.CMD_TEXT, b"osc i=0 pos=1 ch=2 amp=3.00 ph=4 pk=2 base=645.00\r\n")
ob(gfw.CMD_PULSE, struct.pack("<HH32H", 0, 32, *([646] * 32)))
check(ob.pairs() is None, "пачка: запись без строки — отбросить")
ob(gfw.CMD_TEXT, b"osc i=1 pos=9 ch=2 amp=3.00 ph=4 pk=2 base=645.00\r\n")
check(len(ob.pairs()) == 2 and ob.pairs()[1][1]["pos"] == 9, "пачка: пары окно-строка")

print("%d passed, %d failed" % (passed, failed))
sys.exit(1 if failed or not passed else 0)
