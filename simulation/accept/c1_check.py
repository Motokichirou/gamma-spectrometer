# -*- coding: utf-8 -*-
# c1_check.py — ЗАМОРОЖЕННЫЙ пост-процессор стенда C1. НЕ РЕДАКТИРОВАТЬ (часть эталона).
# Читает c1_bench.log, считает усиление ФЭУ = Π(зазор)^0.7 по множительным каскадам
# K->Dy1 ... Dy7->Dy8 (Dy8->анод исключён), сравнивает k=0 (шаг1) vs k=1 (шаг2).
# T_C1: |Δусиления| <= 1 %.
import sys, re

LOG = sys.argv[1] if len(sys.argv) > 1 else "c1_bench.log"
raw = open(LOG, "rb").read().replace(b"\x00", b"").decode("ascii", "replace")
raw = raw.replace("\r", "")

# {meas_name: {step:int -> value:float}}
meas, cur = {}, None
for line in raw.splitlines():
    m = re.match(r"Measurement:\s*(\S+)", line)
    if m:
        cur = m.group(1).lower(); meas[cur] = {}; continue
    if cur:
        mm = re.match(r"\s+(\d+)\s+([-\d.eE+]+)", line)
        if mm:
            meas[cur][int(mm.group(1))] = float(mm.group(2))

# электроды по порядку каскада (cath, dy1..dy8)
order = ["v_cath", "v_dy1", "v_dy2", "v_dy3", "v_dy4", "v_dy5", "v_dy6", "v_dy7", "v_dy8"]

def gain_at(step):
    V = [meas[n][step] for n in order]
    gaps = [V[i+1] - V[i] for i in range(len(V)-1)]   # 8 множительных зазоров
    g = 1.0
    for gap in gaps:
        g *= abs(gap) ** 0.7
    return g, gaps

g0, gaps0 = gain_at(1)   # k=0
g1, gaps1 = gain_at(2)   # k=1
shift = 100.0 * (g1 / g0 - 1.0)

print("=== C1: сдвиг усиления делителя (k=0 -> k=1, worst-case 30кимп/с@2.6МэВ) ===")
labels = ["K->Dy1","Dy1->Dy2","Dy2->Dy3","Dy3->Dy4","Dy4->Dy5","Dy5->Dy6","Dy6->Dy7","Dy7->Dy8"]
print("зазор      k=0(В)   k=1(В)   Δ%")
for L, a, b in zip(labels, gaps0, gaps1):
    print(f"{L:9} {abs(a):7.2f}  {abs(b):7.2f}  {100*(abs(b)/abs(a)-1):+6.2f}")
print(f"\nусиление ФЭУ:  k=0={g0:.4g}  k=1={g1:.4g}")
print(f"СДВИГ УСИЛЕНИЯ = {shift:+.3f} %   (порог T_C1: |Δ| <= 1.0 %)")
print("T_C1:", "PASS" if abs(shift) <= 1.0 else "FAIL")
