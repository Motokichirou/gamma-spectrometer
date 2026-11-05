"""gfw.py — хост для прошивки gamma_fw: shproto-клиент и перепрошивка через системный загрузчик.

  python -I gfw.py cmd "<текст>" [--port COM4] [--baud 600000] [--wait 1.5]
        текстовая команда кадром 0x03, печать текстовых ответов (до первого ответа + 0.3 с тишины)
  python -I gfw.py listen <секунд> [--port COM4] [--baud 600000]
        печать текстовых кадров 0x03 и статусов 0x04, счёт кадров спектра 0x01
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


def text_of(pl):
    return pl.decode("ascii", "replace").rstrip("\r\n")


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
        else:
            st["other"] += 1


def do_cmd(a):
    st = dict(text=0, stat=0, hist=0, other=0)
    p = Parser()
    with open_port(a.port, a.baud) as s:
        s.reset_input_buffer()
        s.write(build_frame(CMD_TEXT, a.text.encode("ascii")))
        t0 = time.time()
        last_text = None
        while time.time() - t0 < a.wait:
            fr = p.feed(s.read(4096))
            if any(c == CMD_TEXT for c, _ in fr):
                last_text = time.time()
            _print_frames(fr, st)
            if last_text and time.time() - last_text > 0.3:
                break
    if st["hist"] or st["stat"]:
        print(f"(попутно: 0x01 ×{st['hist']}, 0x04 ×{st['stat']})")
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
          f"прочих ×{st['other']}, битых ×{p.bad}")
    return st


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
    for name in ("cmd", "listen", "flash"):
        sp = sub.add_parser(name)
        if name == "cmd":
            sp.add_argument("text")
            sp.add_argument("--wait", type=float, default=1.5)
        elif name == "listen":
            sp.add_argument("secs", type=float)
        else:
            sp.add_argument("bin")
            sp.add_argument("--no-boot", action="store_true")
        sp.add_argument("--port", default=DEF_PORT)
        sp.add_argument("--baud", type=int, default=DEF_BAUD)
    a = ap.parse_args(argv)
    return {"cmd": do_cmd, "listen": do_listen, "flash": do_flash}[a.op](a)


if __name__ == "__main__":
    sys.exit(main())
