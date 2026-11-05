"""Выгрузить 8192 быстрых отсчёта узла (команда прошивки 'd <узел>'), спектр и выбросы.
python dump.py <out.npy> [PB1|PA1|PA3|PA2|PA7|PA5|PA6|PB0|PA0] [COMx]"""
import sys, time, serial, numpy as np
sys.stdout.reconfigure(encoding="utf-8")
out = sys.argv[1]; pt = sys.argv[2] if len(sys.argv) > 2 else "PB1"
port = sys.argv[3] if len(sys.argv) > 3 else "COM4"
s = serial.Serial(port, 115200, timeout=0.05); s.reset_input_buffer()
b = b""; t0 = time.time()
while not b.endswith(b" V\r\n") and time.time() - t0 < 3: b += s.read(1)
s.write(("d " + pt + "\r").encode())
buf = b""; t0 = time.time()
while b"END" not in buf and time.time() - t0 < 15: buf += s.read(4096)
txt = buf.decode("ascii", "replace")
fs = 914282.0
for l in txt.splitlines():
    if l.startswith("NOISE fs="): fs = float(l.split("=")[1].split()[0])
i = txt.index("DUMP"); body = txt[txt.index("\n", i):txt.index("END")]
hexs = "".join(body.split()); v = np.array([int(hexs[k:k+3], 16) for k in range(0, len(hexs), 3)], float)
np.save(out, v)
x = (v - v.mean()) * np.hanning(len(v)); F = np.abs(np.fft.rfft(x)) / (len(v) / 4); f = np.fft.rfftfreq(len(v), 1 / fs)
d = v - np.median(v)
print(f"{pt}: n={len(v)} fs={fs/1e3:.0f} kSa/s sd={v.std():.2f} LSB  max-med={d.max():.0f}  spikes>+8={int((d>8).sum())}")
top = np.argsort(F[3:])[-5:][::-1] + 3
print("  top:", ", ".join(f"{f[k]/1e3:.0f}k {F[k]:.2f}" for k in top))
