"""Отправить команду прошивке запуска и слушать COM-порт.
python hv.py "<команда>|-" <секунд> [COMx]   ('-' — только слушать)
Команда отправляется сразу после строки статуса: у прошивки занята передача, пока она печатает."""
import sys, time, serial
sys.stdout.reconfigure(encoding="utf-8")
port = sys.argv[3] if len(sys.argv) > 3 else "COM4"
s = serial.Serial(port, 115200, timeout=0.05)
s.reset_input_buffer()
if sys.argv[1] != "-":
    b = b""; t0 = time.time()
    while not b.endswith(b" V\r\n") and time.time() - t0 < 3: b += s.read(1)
    s.write((sys.argv[1] + "\r").encode())
t0 = time.time(); buf = b""
while time.time() - t0 < float(sys.argv[2]): buf += s.read(1024)
print(buf.decode("ascii", "replace"))
