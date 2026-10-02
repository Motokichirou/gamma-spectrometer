# -*- coding: utf-8 -*-
"""cost.py — себестоимость одного прибора по фактическим ценам закупки.

Считает два разных числа, которые часто путают:
  «потрачено»     — вся сумма заказов, включая запас и излишки от минимальных партий;
  «себестоимость» — цена за штуку × количество, которое реально уходит в один прибор.

Количества на прибор берутся из `pcb/gamma_spectrometer.net`, цены — из таблицы BUYS
(заказы 2026-09-25 … 2026-10-02). Позиции, закрытые из домашних запасов, в денежную
смету не входят и перечислены отдельно.

Запуск: python stock/cost.py
"""
import collections
import io
import os
import re
import sys

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NET = os.path.join(ROOT, 'pcb', 'gamma_spectrometer.net')

# заказ -> [(PN в BOM, что реально куплено, цена за шт, куплено шт)]
BUYS = {
    'Электронщик, 25.09 (получен)': [
        ('GRM21BR61C475KA88L', 'GRM21BR61C475KA88L', 4.88, 5),
        ('CC0603FRNPO9BN511', 'CC0603FRNPO9BN511', 0.66, 5),
        ('RC1206FR-071M6L', 'RC1206FR-071M6L', 0.72, 5),
        ('CC1206JKNPOCBN221', 'CC1206JKNPOCBN221', 1.83, 5),
        ('RC1206FR-072ML', 'RC1206FR-072ML', 1.46, 5),
        ('CC1210KKX7RCBB223', 'CC1210KKX7RCBB223', 9.64, 5),
        ('GRM43DR73A473KW01L', 'GRM43DR73A473KW01L', 9.52, 5),
        ('BLM18EG121SN1D', 'BLM18EG121SN1D', 8.05, 5),
        ('MF-MSMF050-2', 'MF-MSMF050-2', 4.15, 5),
        ('1N4148W', '1N4148W', 2.20, 10),
        ('PRTR5V0U2X', 'PRTR5V0U2X', 4.88, 5),
        ('AMS1117-3.3', 'AMS1117-3.3', 5.49, 5),
        ('TL431', 'TL431', 3.42, 5),
        ('MMBTA42', 'MMBTA42', 6.59, 10),
        ('IRLML0060', 'IRLML0060', 4.27, 10),
        ('BAV23S', 'BAV23S', 2.07, 20),
        # ошибка: 50 В вместо 100 В, в прибор не идёт
        (None, 'CC0603KRX7R9BB103 (50 В, брак выбора)', 1.34, 5),
    ],
    'Заказ 2 (TPS/REF/флеш/FTDI/ADA/дроссели)': [
        ('TPS60400DBV', 'TPS60400DBVR', 140.00, 3),
        ('REF35250QDBVR', 'REF35250QDBVR', 610.00, 2),
        ('W25Q64JVSSIQ', 'W25Q64JVSSIQ', 260.00, 2),
        ('FT231XQ', 'FT231XQ-R', 960.00, 1),
        ('ADA4817-1ARDZ', 'ADA4817-1ARDZ-R7', 1000.00, 2),
        ('SRU5028-101Y', 'SRU5028-101Y', 56.00, 10),
        ('SRN3015-470M', 'SRN3015-470M', 110.00, 2),
    ],
    'Датчик температуры': [
        ('PT1000', 'RTS Pt1000 A M222', 350.00, 1),
    ],
    'Микроконтроллеры': [
        ('STM32G474MET6', 'STM32G474MCT6TR', 1930.00, 2),
    ],
    'Заказ 5 (ВВ-конденсаторы, снаббер, нагрузочные)': [
        ('C1812X104K102T', 'C1812X104K102T', 31.11, 30),
        ('CC0603KRX7R9BB103', 'CC0603KRX7R0BB103 (100 В)', 1.46, 20),
        ('CC0402FRNPO9BN180', 'CC0402FRNPO9BN100 (10 пФ)', 0.59, 20),
    ],
    'Корзина к оплате (минимальные партии)': [
        ('ATB322524-0110-T000', 'ATB322524-0110-T000', 140.00, 10),
        ('FT231XQ', 'FT231XQ-R (вторая партия)', 960.00, 2),
        ('SRN3015-470M', 'SRN3015-470M (вторая партия)', 25.00, 22),
        ('ABM8G-16.000MHZ-4Y-T3', 'KSE-7U16000M9Z143ZA3', 12.00, 25),
    ],
    'AliExpress': [
        ('DX07S024WJ3R400', 'DX07S024WJ3R400', 134.50, 2),
    ],
}

PCB_TOTAL, PCB_SETS = 4666.96, 2      # счёт-договор № 19696, 2 заготовки

# Розничные цены (RUB с НДС, от 1 шт) на позиции, закрытые из домашних запасов.
# Сняты с electronshik.ru 2026-10-02 по партномерам из BOM. Нужны, чтобы знать
# полную себестоимость прибора, а не только потраченные сейчас деньги.
STOCK_PRICES = {
    'max1847eee+':         (1400.00, 'оценка: RS Online ~GBP 12.5'),
    'CC0805JKNPOBBN102':   (8.78, ''),
    'RC1206JR-07100ML':    (0.38, ''),
    'RC1206FR-076M8L':     (1.27, ''),
    'RC1206FR-071M3L':     (0.74, 'цена без логина не видна, взята как у 1206'),
    'RC1206FR-07680KL':    (0.74, ''),
    'RC1206FR-07750KL':    (0.13, ''),
    'RC1206FR-07499RL':    (0.61, 'ставим 510 Ом 0603 из книги'),
    'BAS516':              (0.84, ''),
    'BZT52C18':            (0.83, 'Diotec не поставляется, цена аналога'),
    'TR3B106K010C0750':    (20.00, 'оценка: нет в продаже, ставим CA45 корпус A'),
    'LP5907MFX-3.3':       (15.37, 'ставим LP2985AIM5X-3.3 из запасов'),
    'GRM32ER71A476KE15L':  (40.87, ''),
    'CC0603JRX7R9BB104':   (2.07, ''),
    'CC0603JRNPO9BN101':   (0.41, ''),
    'CC0603JRX7R7BB105':   (2.32, ''),
    'CC0805JKX7R9BB105':   (3.90, ''),
    'CC0805JRX7R9BB333':   (1.71, ''),
    'CC0805JKX7R9BB474':   (2.32, ''),
    'GRM1555C1H220JA01D':  (1.15, ''),
    'GRM1555C1H4R7WA01D':  (1.15, 'как 22 пФ 0402 Murata'),
    'CC0402FRNPO9BN180':   (0.59, 'ставим 10 пФ, цена из нашего заказа'),
    'RL0603FR-070R1L':     (2.20, ''),
    'RC0402FR-0722RL':     (0.50, 'оценка для резистора 0402'),
    'FYLS-0805UBC':        (3.42, ''),
    '150080GS75000':       (3.42, 'Wurth снят, ставим LTST-C195KGJRKT'),
}
for _pn in ['RC0603FR-0710KL', 'RC0603FR-07100KL', 'RC0603FR-071KL', 'RC0603FR-072M2L',
            'RC0603FR-07110KL', 'RC0603FR-07180KL', 'RT0603BRD071ML', 'RC0603FR-0722KL',
            'RT0603BRD07330RL', 'RC0603FR-075K1L', 'RC0603FR-078K2L', 'RC0603FR-07909RL',
            'RC0603FR-0714K7L', 'RC0603FR-071K2L', 'AC0603FR-0710RL']:
    STOCK_PRICES[_pn] = (0.61, '')      # все резисторы 0603 по одной цене


# закрыто из домашних запасов — в денежную смету не входит
FROM_STOCK = [
    ('max1847eee+', 'MAX1847EEE+', 1, 'самая дорогая позиция, 3 шт в наличии'),
    ('CC0805JKNPOBBN102', '1 нФ 500 В 0805', 8, 'умножитель'),
    ('RC1206JR-07100ML', '100 МОм 0805', 10, 'делитель обратной связи ВВ'),
    ('RC1206FR-076M8L', '6.8 МОм 0805', 10, 'делитель монитора'),
    ('BAS516', 'BAS516', 1, 'диод снаббера'),
    ('BZT52C18', 'BZT52C18', 2, 'стабилитроны снаббера'),
    ('TR3B106K010C0750', 'тантал 10 мкФ 16 В', 5, 'развязка питания'),
    ('LP5907MFX-3.3', 'LP2985AIM5X-3.3', 1, 'аналоговый LDO'),
    ('GRM32ER71A476KE15L', '22 мкФ 1206', 2, 'bulk +5V'),
    (None, 'резисторы 0402/0603/0805 из книг E24', 52, 'делитель ФЭУ, обвязка'),
    (None, 'конденсаторы 0402 из каталога', 35, 'развязка, шейпер'),
    (None, 'светодиоды LTST-C195KGJRKT', 2, 'индикация'),
]

NON_BOM = [
    ('цанги / эмалированный провод', 44, 'межплатная шина'),
    ('стойки М2.5', 9, 'есть в наличии'),
    ('коаксиал AWG40', 1, 'есть в наличии'),
    ('провод ПВВ-1 ≥5 кВ', 1, 'есть в наличии'),
    ('витая пара на Pt1000', 1, 'рекомендовано 0.1–0.2 мм'),
    ('МГТФ под SWD', 5, 'есть в наличии'),
    ('силикон Виксинт', 1, 'есть в наличии'),
]


def per_device():
    """Сколько каждого PN уходит в один прибор — по нетлисту."""
    s = io.open(NET, encoding='utf-8').read()
    body = s[s.index('(components'):s.index('(libparts')]
    cnt = collections.Counter()
    for m in re.finditer(r'\(comp \(ref "([^"]+)"\)(.*?)\n      \(tstamps ', body, re.S):
        blk = m.group(2)
        pn = re.search(r'\(field \(name "PN"\) "([^"]*)"\)', blk)
        val = re.search(r'\(value "([^"]*)"\)', blk)
        # у четырёх позиций поле PN пустое — для них ключом служит номинал
        key = (pn.group(1).strip() if pn and pn.group(1).strip() else
               (val.group(1).strip() if val else None))
        if key:
            cnt[key] += 1
    cnt['PT1000'] = 1          # вне нетлиста: датчик внешний, на корпусе кристалла
    return cnt


def main():
    need = per_device()
    print('=' * 74)
    print('СЕБЕСТОИМОСТЬ ОДНОГО ПРИБОРА'.center(74))
    print('=' * 74)
    spent = 0.0
    unit = 0.0
    rows = []
    # позиция может встречаться в двух заказах (докупали) — в прибор она идёт один
    # раз, и считаем её по наименьшей из уплаченных цен: столько стоит повторить
    best = {}
    for items in BUYS.values():
        for bom_pn, _, price, _ in items:
            if bom_pn and (bom_pn not in best or price < best[bom_pn]):
                best[bom_pn] = price
    counted = set()
    for order, items in BUYS.items():
        osum = sum(p * q for _, _, p, q in items)
        spent += osum
        for bom_pn, bought, price, qty in items:
            n = need.get(bom_pn, 0) if bom_pn else 0
            if bom_pn in counted or price > best.get(bom_pn, price) + 1e-9:
                n = 0          # эта строка — повторная закупка, в смету не вторым разом
            elif n:
                counted.add(bom_pn)
            cost = price * n
            unit += cost
            rows.append((bought, price, qty, n, cost, order))
    rows.sort(key=lambda r: -r[4])
    print('\n%-34s %9s %6s %5s %10s' % ('позиция', 'цена/шт', 'взято', 'нужно', 'на прибор'))
    print('-' * 74)
    for bought, price, qty, n, cost, _ in rows:
        mark = '' if n else '   (в прибор не идёт)'
        print('%-34s %9.2f %6d %5d %10.2f%s' % (bought[:34], price, qty, n, cost, mark))
    print('-' * 74)
    pcb = PCB_TOTAL / PCB_SETS
    print('%-34s %9s %6s %5s %10.2f' % ('печатные платы (заготовка /2)', '', '', 1, pcb))
    print('-' * 74)
    print('%-57s %10.2f ₽' % ('ИТОГО комплектующие на один прибор', unit))
    print('%-57s %10.2f ₽' % ('платы', pcb))
    print('%-57s %10.2f ₽' % ('ВСЕГО СЕБЕСТОИМОСТЬ ОДНОГО ПРИБОРА', unit + pcb))
    print()
    print('%-57s %10.2f ₽' % ('фактически потрачено на комплектующие', spent))
    print('%-57s %10.2f ₽' % ('фактически потрачено на платы (2 заготовки)', PCB_TOTAL))
    print('%-57s %10.2f ₽' % ('ВСЕГО ПОТРАЧЕНО', spent + PCB_TOTAL))
    print('%-57s %10.2f ₽' % ('из них запас, излишки и минимальные партии', spent + PCB_TOTAL - (unit + pcb)))
    print()
    print('Второй прибор обойдётся дешевле: платы уже оплачены, комплектующих')
    print('на него хватает почти везде. Докупать пришлось бы только то, чего')
    print('взято по одной штуке.')
    print()
    print('=' * 74)
    print('ЗАКРЫТО ИЗ ЗАПАСОВ — по розничным ценам'.center(74))
    print('=' * 74)
    print('%-24s %8s %4s %9s  %s' % ('PN', 'цена/шт', 'шт', 'сумма', 'примечание'))
    print('-' * 74)
    stock_sum = 0.0
    rows2 = []
    for pn, (price, note) in STOCK_PRICES.items():
        n = need.get(pn, 0)
        if n:
            rows2.append((price * n, pn, price, n, note))
            stock_sum += price * n
    for cost, pn, price, n, note in sorted(rows2, reverse=True):
        print('%-24s %8.2f %4d %9.2f  %s' % (pn[:24], price, n, cost, note[:24]))
    print('-' * 74)
    print('%-39s %9.2f RUB' % ('ИТОГО из запасов по рознице', stock_sum))
    print('%-39s %9d' % ('позиций / компонентов', len(rows2)), end='')
    print(' / %d' % sum(r[3] for r in rows2))
    print()
    print('%-39s %9.2f RUB' % ('ПОЛНАЯ СЕБЕСТОИМОСТЬ ПРИБОРА', unit + pcb + stock_sum))
    print('   (купленное + платы + взятое из запасов по рознице)')
    print('\n' + '=' * 74)
    print('ВНЕ НЕТЛИСТА'.center(74))
    print('=' * 74)
    for name, n, note in NON_BOM:
        print('   %-32s %3d      %s' % (name[:32], n, note))
    print('\nДетектор (ФЭУ Hamamatsu R1307 + кристалл NaI(Tl) Ø63×63) в смету')
    print('не включён — был в наличии до начала проекта.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
