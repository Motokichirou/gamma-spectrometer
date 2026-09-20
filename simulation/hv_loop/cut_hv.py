# -*- coding: utf-8 -*-
"""Копия платы только с ВВ-платой — чтобы земля не была межплатной (читает оригинал, пишет копию)."""
import math, os
import pcbnew
SRC=r'C:\gamma-spectrometer\pcb\gamma_spectrometer\gamma_spectrometer.kicad_pcb'
DST=r'C:\Users\motok\AppData\Local\Temp\claude\C--gamma-spectrometer\82694962-96d6-4aad-96dd-c493e2d0e08f\scratchpad\hv_only.kicad_pcb'
CX,CY,R=170.0,100.0,23.0
b=pcbnew.LoadBoard(SRC)
mm=pcbnew.ToMM
def out(p): return math.hypot(mm(p.x)-CX, mm(p.y)-CY) > R
rm=[]
for t in b.GetTracks():
    if out(t.GetPosition() if t.GetClass()=='PCB_VIA' else t.GetStart()): rm.append(t)
for fp in b.GetFootprints():
    if out(fp.GetPosition()): rm.append(fp)
for z in b.Zones():
    bb=z.GetBoundingBox()
    c=bb.Centre()
    # зона принадлежит плате ВВ, если её габарит пересекает окружность платы
    if math.hypot(mm(c.x)-CX, mm(c.y)-CY) > R + mm(bb.GetWidth())/2 + mm(bb.GetHeight())/2:
        rm.append(z)
for d in b.GetDrawings():
    if out(d.GetPosition()): rm.append(d)
for it in rm: b.Remove(it)
b.BuildConnectivity()
b.Save(DST)
print('удалено объектов:', len(rm), '→', DST)
