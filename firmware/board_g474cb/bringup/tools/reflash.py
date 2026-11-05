"""Перепрошить через системный загрузчик: '!boot' -> STM32_Programmer_CLI -> слушать 12 с.
python reflash.py [bin] [COMx]"""
import sys, time, serial, subprocess, os
sys.stdout.reconfigure(encoding="utf-8")
here = os.path.dirname(os.path.abspath(__file__))
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "bringup.bin")
port = sys.argv[2] if len(sys.argv) > 2 else "COM4"
s = serial.Serial(port, 115200, timeout=0.3)
s.write(b"!boot"); time.sleep(0.5); print(s.read(200).decode("ascii", "replace")); s.close()
cli = r"C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe"
r = subprocess.run([cli, "-c", f"port={port}", "br=115200", "P=EVEN", "db=8", "sb=1", "-w", BIN, "0x08000000", "-v", "-g", "0x08000000"],
                   capture_output=True, timeout=90)
out = r.stdout.decode("cp866", "replace")
print("\n".join(l for l in out.splitlines() if any(k in l for k in ("rror", "verified", "Start operation", "Size"))), "rc", r.returncode)
s = serial.Serial(port, 115200, timeout=0.2); t0 = time.time(); buf = b""
while time.time() - t0 < 12: buf += s.read(1024)
print(buf.decode("ascii", "replace"))
