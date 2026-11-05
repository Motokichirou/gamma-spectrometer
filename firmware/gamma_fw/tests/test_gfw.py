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
print("%d passed, %d failed" % (passed, failed))
sys.exit(1 if failed or not passed else 0)
