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


def do_cmd(a):
    st = dict(text=0, stat=0, hist=0, other=0)
    p = Parser()
    with open_port(a.port, a.baud) as s:
        s.reset_input_buffer()
        s.write(build_frame(CMD_TEXT, a.text.encode("ascii")))
        t0 = time.time()
        last_text = None
        # -osc <n>: окна приходят позже ответа — ждать "-ok osc done" (или -err), не дольше --wait
        need_done = osc_waits_done(a.text)
        while time.time() - t0 < a.wait:
            fr = p.feed(s.read(4096))
            texts = [text_of(pl) for c, pl in fr if c == CMD_TEXT]
            if texts:
                last_text = time.time()
            if any(t.startswith("-ok osc done") or t.startswith("-err") for t in texts):
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
    for name in ("cmd", "listen", "flash", "gst"):
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
        else:
            sp.add_argument("bin")
            sp.add_argument("--no-boot", action="store_true")
        sp.add_argument("--port", default=DEF_PORT)
        sp.add_argument("--baud", type=int, default=DEF_BAUD)
    a = ap.parse_args(argv)
    return {"cmd": do_cmd, "listen": do_listen, "flash": do_flash, "gst": do_gst}[a.op](a)


if __name__ == "__main__":
    sys.exit(main())
