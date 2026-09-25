# -*- coding: utf-8 -*-
"""Сборка закупочной таблицы stock/bom_zakupka.xlsx из pcb/gamma_spectrometer.net.

Ссылки на магазин берутся из пользовательского поля схемы («Ссылка на закупку»).
Колонка «Наличие» заполняется из таблицы STATUS ниже — она отражает сверку с
`stock/required.md` (фото номенклатуры от 2026-09-23). При изменении запасов
править STATUS, а не .xlsx.

Запуск: python stock/make_bom_xlsx.py
"""
import io
import os
import re
import sys
from collections import OrderedDict

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NET = os.path.join(ROOT, 'pcb', 'gamma_spectrometer.net')
OUT = os.path.join(ROOT, 'stock', 'bom_zakupka.xlsx')

EST, ORD, PART, NA = 'есть', 'ЗАКАЗАТЬ', 'частично', '—'

# ключ: PN (или номинал, если PN не задан) -> (статус, сколько заказать, заметка)
STATUS = {
    # ---- конденсаторы ----
    'TR3B106K010C0750': (EST, 0, 'тантал CA45 корпус A 10 мкФ 16 В x20; корпус A меньше пада 3528 — встаёт'),
    'CC0402FRNPO9BN180': (EST, 0, '18 пФ 0402 C0G +-5 % из каталога'),
    'GRM1555C1H220JA01D': (EST, 0, '22 пФ 0402 C0G из каталога'),
    'GRM1555C1H4R7WA01D': (EST, 0, '4.7 пФ 0402 C0G; допуск +-0.25 пФ вместо +-0.05 -> полоса TIA +-5 %'),
    'CC0603JRX7R9BB104': (EST, 0, 'GRM155R71H104KE14D — 0.1 мкФ 0402 50 В X7R x54'),
    'CC0603JRNPO9BN101': (EST, 0, 'CC0402FRNPO9BN101 — 100 пФ 0402 C0G +-1 %, допуск как в BOM'),
    'CC0603KRX7R9BB103': (ORD, 5, 'ВНИМАНИЕ: нужно 100 В, а не 50 В из BOM — снаббер 35.7 В с пиками 43-45 В'),
    'CC0603JRX7R7BB105': (EST, 0, 'GRM155R61C105KA12D — 1 мкФ 0402 16 В X5R'),
    'CC0603FRNPO9BN511': (ORD, 5, 'C0G +-1 %, интегратор шейпера. В каталоге 0402 C0G только до 220 пФ'),
    'CC0805JKX7R9BB474': (EST, 0, 'ставим своё 1 мкФ 0402 16 В -> под 4.25 В ~0.7 мкФ, ближе к даташиту MAX1847'),
    'CC0805JKNPOBBN102': (EST, 0, 'ровно этот PN в наличии, 30 шт'),
    'CC0805JKX7R9BB105': (EST, 0, '1 мкФ 0402 16 В X5R'),
    'CC0805JRX7R9BB333': (EST, 0, 'GRM155R71H333KE14D — 33 нФ 0402 50 В'),
    'GRM21BR61C475KA88L': (PART, 5, 'C106/C310 (3.3 В) — из 0402 6.3 В. C411/C413 (+-5 В, питание ADA4817) — заказать >=16 В, 2 шт'),
    'CC1206JKNPOCBN221': (ORD, 5, '1 кВ C0G, ВЧ-фильтр выхода ВВ, два в серии'),
    'CC1210KKX7RCBB223': (ORD, 3, '>=250 В: накопитель между динодами ФЭУ'),
    'GRM32ER71A476KE15L': (EST, 0, 'замена: своё 22 мкФ 1206 35 В (~20 мкФ под 5 В против 25). Заказывать не обязательно'),
    'C1812X104K102T': (EST, 0, 'ровно этот PN, 12 шт при потребности 11 — запасной всего один'),
    'GRM43DR73A473KW01L': (ORD, 3, '>=250 В: накопитель между динодами ФЭУ'),
    # ---- резисторы ----
    'RC0402FR-0722RL': (EST, 0, 'книга 0402 E24'),
    'RL0603FR-070R1L': (EST, 0, '0.1 Ом 0603 +-1 % отдельной лентой'),
    'RC0603FR-071K2L': (EST, 0, 'книга 0603'),
    'RC0603FR-07100KL': (EST, 0, 'книга 0603'),
    'AC0603FR-0710RL': (EST, 0, 'книга 0603'),
    'RC0603FR-0710KL': (EST, 0, 'книга 0603'),
    'RC0603FR-07110KL': (EST, 0, 'книга 0603, 1 %'),
    'RC0603FR-0714K7L': (EST, 0, 'ставим 15 кОм (E24): смещение АЦП на 2 %, базовую линию DSP измеряет сам'),
    'RC0603FR-07180KL': (EST, 0, 'книга 0603, 1 % — задаёт 252 кГц'),
    'RT0603BRD071ML': (EST, 0, 'книга 0603'),
    'RC0603FR-071KL': (EST, 0, 'книга 0603'),
    'RC0603FR-072M2L': (EST, 0, 'книга 0603, 1 % — нижнее плечо FB'),
    'RC0603FR-0722KL': (EST, 0, 'книга 0603'),
    'RT0603BRD07330RL': (EST, 0, 'книга 0603'),
    'RC1206FR-07499RL': (EST, 0, 'ВНИМАНИЕ: PN и ссылка от 1206, а посадка 0603. Ставим 510 Ом из книги: усиление TIA +2.2 %, калибровка поглотит'),
    'RC0603FR-075K1L': (EST, 0, 'книга 0603'),
    'RC0603FR-078K2L': (EST, 0, 'книга 0603'),
    'RC0603FR-07909RL': (EST, 0, 'ставим 910 Ом (E24), отличие 0.1 %'),
    'RC1206FR-071M3L': (EST, 0, '0805 из книги: 126 В при уставке 1200 В против предела 150 В'),
    'RC1206FR-071M6L': (ORD, 5, '155 В при 1200 В — выше предела 0805. Нужен 1206 (200 В)'),
    'RC1206JR-07100ML': (EST, 0, 'RC0805JR-07100ML x50: 120 В на резистор при 1200 В, предел 0805 = 150 В'),
    'RC1206FR-072ML': (ORD, 5, '193 В при 1200 В — только 1206'),
    'RC1206FR-076M8L': (EST, 0, '0805 из книги: 120 В при 1200 В'),
    'RC1206FR-07680KL': (EST, 0, '0805 из книги: 66 В при 1200 В'),
    'RC1206FR-07750KL': (EST, 0, '0805 из книги: 73 В при 1200 В'),
    # ---- микросхемы, дискретные, механика ----
    'max1847eee+': (EST, 0, 'в наличии 3 шт — самая дорогая позиция проекта'),
    'ATB322524-0110-T000': (EST, 0, 'тот самый экземпляр, под который считался узел ВВ. Запасного нет — паять аккуратно'),
    'ABM8G-16.000MHZ-4Y-T3': (ORD, 2, 'заменяется легко, но CL должен остаться ~8-9 пФ (у нас C301/C302 = 18 пФ)'),
    '1N4148W': (ORD, 10, 'ссылки в схеме нет — дозаполнить поле'),
    'BZT52C18': (EST, 0, '50 шт. ВНИМАНИЕ: проверить корпус SOD-123 (не -123W/F); отобрать пару с близким напряжением'),
    'BAS516': (EST, 0, '33 шт, маркировка YJ'),
    'MF-MSMF050-2': (ORD, 2, ''),
    'PMT': (NA, 0, 'паяется проводами к выводам ФЭУ, покупать нечего'),
    'FYLS-0805UBC': (EST, 0, 'LTST-C195KGJRKT (0603) из запасов встаёт на пад 0805'),
    '150080GS75000': (EST, 0, 'LTST-C195KGJRKT (0603) из запасов встаёт на пад 0805'),
    'STM32G474MET6': (ORD, 2, 'ВНИМАНИЕ: именно M = LQFP-80. RET6 (64) и CET6 (48) на плату не встанут. Риск перемаркировки'),
    'BLM18EG121SN1D': (ORD, 3, 'заменяется любым 120 Ом @100 МГц в 0603'),
    'SRU5028-101Y': (ORD, 2, 'через L103 идёт вся ВВ-плата — смотреть ток насыщения, не только индуктивность'),
    'SRN3015-470M': (ORD, 3, 'ток небольшой, заменяется по габариту'),
    'FT231XQ': (ORD, 2, 'ВНИМАНИЕ: FTDI массово подделывают — брать у дистрибьютора'),
    'ADA4817-1ARDZ': (ORD, 2, 'ССЫЛКИ НЕТ, гуглить отдельно. Именно -1ARDZ (SOIC-8 EP); ACPZ — это LFCSP, не встанет. Замены в схеме нет'),
    'W25Q64JVSSIQ': (ORD, 2, ''),
    'PRTR5V0U2X': (ORD, 3, ''),
    'AMS1117-3.3': (ORD, 3, 'на выходе обязателен тантал (есть), с керамикой неустойчив'),
    'BAV23S': (ORD, 12, 'умножитель, 500 В на корпус; вывод 3 не подключён'),
    'IRLML0060': (ORD, 3, 'ВНИМАНИЕ: именно 0060 (60 В). IRLML0040 в запасах есть — ставить НЕЛЬЗЯ: пик на стоке 43-45 В'),
    'MMBTA42': (ORD, 6, '300 В, буферы динодов'),
    'TL431': (ORD, 3, 'опора монитора ВВ; точность влияет только на шкалу монитора, калибруется'),
    'LP5907MFX-3.3': (EST, 0, 'ставим LP2985AIM5X-3.3 из запасов: вывод 4 у нас не подключён (у LP2985 это BYPASS)'),
    'TPS60400DBV': (ORD, 2, 'C110/C111 обязательно X7R (есть)'),
    'REF35250QDBVR': (ORD, 2, 'ССЫЛКИ НЕТ, гуглить отдельно. Сверить по даташиту: последовательная опора 2.5 В в SOT-23-6. TL431 не замена — это точность шкалы'),
    'DX07S024WJ3R400': (ORD, 2, 'футпринт под конкретный корпус JAE, замена = переразводка'),
}

SHEET_RU = {'USB_Power_Board': 'USB/питание', 'HV_Board': 'ВВ',
            'MCU_Board': 'МК', 'Divider_Board': 'делитель', '': '—'}


def parse_net():
    s = io.open(NET, encoding='utf-8').read()
    body = s[s.index('(components'):s.index('(libparts')]
    comps = []
    pat = r'\(comp \(ref "([^"]+)"\)(.*?)\n      \(tstamps '
    for m in re.finditer(pat, body, re.S):
        ref, blk = m.group(1), m.group(2)

        def one(p):
            r = re.search(p, blk)
            return r.group(1) if r else ''

        pn, link = '', ''
        for fm in re.finditer(r'\(field \(name "([^"]*)"\) "([^"]*)"\)', blk):
            nm, v = fm.group(1).strip(), fm.group(2).strip()
            if nm.upper() in ('PN', 'MPN'):
                pn = v
            elif v.startswith('http') and nm.lower() != 'datasheet':
                link = v
        comps.append(dict(ref=ref, val=one(r'\(value "([^"]*)"\)'),
                          fp=one(r'\(footprint "([^"]*)"\)').split(':')[-1],
                          pn=pn, link=link,
                          sheet=one(r'\(property \(name "Sheetname"\) \(value "([^"]*)"\)\)')))
    return comps


def refkey(r):
    m = re.match(r'([A-Za-z_]+)(\d*)', r)
    return (m.group(1), int(m.group(2) or 0))


def main():
    comps = parse_net()
    groups = OrderedDict()
    for c in comps:
        key = (c['pn'], c['val'], c['fp'], c['link'])
        g = groups.setdefault(key, dict(refs=[], sheets=set()))
        g['refs'].append(c['ref'])
        g['sheets'].add(c['sheet'])

    rows, unknown = [], []
    for (pn, val, fp, link), g in groups.items():
        k = pn or val
        if k in STATUS:
            st, oq, note = STATUS[k]
        else:
            st, oq, note = '?', 0, 'нет в таблице STATUS — проверить вручную'
            unknown.append(k)
        rows.append(dict(pn=pn, val=val, fp=fp, link=link, st=st, oq=oq, note=note,
                         qty=len(g['refs']),
                         refs=', '.join(sorted(g['refs'], key=refkey)),
                         sheets=', '.join(sorted(SHEET_RU.get(x, x) for x in g['sheets']))))

    order = {ORD: 0, PART: 1, '?': 2, EST: 3, NA: 4}
    rows.sort(key=lambda r: (order[r['st']], r['fp'], r['val']))

    import openpyxl
    from openpyxl.styles import Alignment, Font, PatternFill
    from openpyxl.utils import get_column_letter

    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = 'Закупка'
    head = ['Наличие', 'Заказать, шт', 'Название (PN)', 'Номинал', 'Корпус',
            'Нужно, шт', 'Ссылка', 'Заметка', 'Позиции', 'Плата']
    ws.append(head)
    for c in ws[1]:
        c.font = Font(bold=True, color='FFFFFF')
        c.fill = PatternFill('solid', fgColor='404040')
        c.alignment = Alignment(vertical='center', wrap_text=True)
    ws.freeze_panes = 'C2'

    fills = {ORD: PatternFill('solid', fgColor='FCE4E4'),
             PART: PatternFill('solid', fgColor='FFF2CC'),
             '?': PatternFill('solid', fgColor='FFD9B3'),
             EST: PatternFill('solid', fgColor='E8F3E3'),
             NA: PatternFill('solid', fgColor='EFEFEF')}

    for r in rows:
        ws.append([r['st'], r['oq'] or None, r['pn'], r['val'], r['fp'], r['qty'],
                   r['link'], r['note'], r['refs'], r['sheets']])
        i = ws.max_row
        for col in range(1, len(head) + 1):
            ws.cell(row=i, column=col).fill = fills[r['st']]
        ws.cell(row=i, column=1).font = Font(bold=(r['st'] in (ORD, PART)))
        for col in (1, 2, 6):
            ws.cell(row=i, column=col).alignment = Alignment(horizontal='center')
        if r['link']:
            cl = ws.cell(row=i, column=7)
            cl.hyperlink = r['link']
            cl.font = Font(color='0563C1', underline='single')
        for col in (8, 9):
            ws.cell(row=i, column=col).alignment = Alignment(wrap_text=True, vertical='top')

    for i, w in enumerate([11, 13, 26, 13, 32, 10, 50, 62, 40, 14], 1):
        ws.column_dimensions[get_column_letter(i)].width = w
    ws.auto_filter.ref = 'A1:J%d' % ws.max_row

    n_ord = sum(1 for r in rows if r['st'] in (ORD, PART))
    ws.append([])
    ws.append(['ИТОГО', sum(r['oq'] for r in rows), 'позиций %d, из них к заказу %d'
               % (len(rows), n_ord), '', '', sum(r['qty'] for r in rows)])
    for col in (1, 2, 3, 6):
        ws.cell(row=ws.max_row, column=col).font = Font(bold=True)

    wb.save(OUT)
    print('сохранено: %s' % OUT)
    print('строк %d, компонентов %d, к заказу позиций %d (%d шт с запасом)'
          % (len(rows), sum(r['qty'] for r in rows), n_ord, sum(r['oq'] for r in rows)))
    if unknown:
        print('нет в STATUS: %s' % ', '.join(unknown))
    return 1 if unknown else 0


if __name__ == '__main__':
    raise SystemExit(main())
