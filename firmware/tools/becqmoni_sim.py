#!/usr/bin/env python3
"""
becqmoni_sim.py — мини-BecqMoni для проверки прошивки этапа 1.

Подключается к COM-порту, повторяет startup-последовательность BecqMoni
(-sto → -rst → -sta), принимает спектр и статус, печатает сводку.

Использование:
    pip install pyserial
    python becqmoni_sim.py COM5
"""

import sys
import time
import serial

START, ESC, FINISH, KEEPALIVE = 0xFE, 0xFD, 0xA5, 0xFF
CMD_HISTOGRAM, CMD_PULSE, CMD_TEXT, CMD_STAT = 0x01, 0x02, 0x03, 0x04
CHANNELS = 8192


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def build_frame(cmd: int, payload: bytes) -> bytes:
    body = bytes([cmd]) + payload
    crc = crc16_modbus(body)
    raw = body + bytes([crc & 0xFF, crc >> 8])
    out = bytearray([KEEPALIVE, START])
    for b in raw:
        if b in (START, ESC, FINISH):
            out += bytes([ESC, (~b) & 0xFF])
        else:
            out.append(b)
    out.append(FINISH)
    return bytes(out)


class Parser:
    def __init__(self):
        self.state, self.esc, self.cmd, self.buf = 0, False, 0, bytearray()

    def feed(self, byte: int):
        """Возвращает (cmd, payload) когда принят валидный кадр, иначе None."""
        if byte == START:
            self.state, self.esc, self.buf = 1, False, bytearray()
            return None
        if self.state == 0:
            return None
        if byte == FINISH and not self.esc:
            result = None
            if self.state == 2 and len(self.buf) >= 2:
                got = self.buf[-2] | (self.buf[-1] << 8)
                calc = crc16_modbus(bytes([self.cmd]) + bytes(self.buf[:-2]))
                if got == calc:
                    result = (self.cmd, bytes(self.buf[:-2]))
                else:
                    print(f"  !! CRC mismatch: got {got:04X}, calc {calc:04X}")
            self.state = 0
            return result
        if byte == ESC and not self.esc:
            self.esc = True
            return None
        if self.esc:
            byte = (~byte) & 0xFF
            self.esc = False
        if self.state == 1:
            self.cmd, self.state = byte, 2
        else:
            self.buf.append(byte)
        return None


def send_text_and_wait_ok(port: serial.Serial, parser: Parser, text: str, timeout=2.0) -> bool:
    print(f"→ {text}")
    port.write(build_frame(CMD_TEXT, text.encode()))
    deadline = time.time() + timeout
    while time.time() < deadline:
        data = port.read(256)
        for b in data:
            frame = parser.feed(b)
            if frame and frame[0] == CMD_TEXT:
                resp = frame[1].decode(errors="replace")
                print(f"← {resp}")
                return True
    print("  (нет ответа)")
    return False


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    com = sys.argv[1]

    port = serial.Serial(com, 600000, timeout=0.1)
    parser = Parser()
    spectrum = [0] * CHANNELS

    print(f"Порт {com} открыт @ 600000")
    send_text_and_wait_ok(port, parser, "-sto")
    send_text_and_wait_ok(port, parser, "-rst")
    send_text_and_wait_ok(port, parser, "-inf")
    send_text_and_wait_ok(port, parser, "-sta")
    print("Набор запущен, Ctrl+C для остановки\n")

    last_keepalive = time.time()
    try:
        while True:
            if time.time() - last_keepalive > 1.0:
                port.write(bytes([KEEPALIVE]))
                last_keepalive = time.time()

            data = port.read(4096)
            for b in data:
                frame = parser.feed(b)
                if not frame:
                    continue
                cmd, pl = frame
                if cmd == CMD_HISTOGRAM:
                    off = pl[0] | (pl[1] << 8)
                    n = (len(pl) - 2) // 4
                    for i in range(n):
                        v = int.from_bytes(pl[2 + 4 * i: 6 + 4 * i], "little") & 0x7FFFFFF
                        if off + i < CHANNELS:
                            spectrum[off + i] = v
                elif cmd == CMD_STAT:
                    # elapsed — в СЕКУНДАХ (BecqMoni: TimeSpan.FromSeconds)
                    elapsed = int.from_bytes(pl[0:4], "little") & 0x7FFFFFF
                    cps = int.from_bytes(pl[6:10], "little")
                    total = sum(spectrum)
                    top = sorted(range(CHANNELS), key=lambda c: spectrum[c], reverse=True)[:3]
                    print(f"[{elapsed:5d}s] cps={cps:5d} total={total:8d} "
                          f"топ-каналы: {top[0]}({spectrum[top[0]]}) "
                          f"{top[1]}({spectrum[top[1]]}) {top[2]}({spectrum[top[2]]})")
                elif cmd == CMD_TEXT:
                    print(f"← TEXT: {pl.decode(errors='replace')}")
    except KeyboardInterrupt:
        print("\nОстанавливаю набор...")
        send_text_and_wait_ok(port, parser, "-sto")
        port.close()


if __name__ == "__main__":
    main()
