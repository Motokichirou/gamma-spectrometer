# cyc.py — такты Cortex-M4 для детектора gamma_fw (src/core/detector.c) на эмуляторе.
#
# Собирает tools/cyc/harness.c + detector.c тем же тулчейном и с теми же флагами, что build.sh,
# гоняет в unicorn (Thumb-2 + VFP) и считает такты по трассе выполненных инструкций.
# Запуск — tools/cyc/run.sh (ставит unicorn/capstone/numpy в tools/cyc/.venv при первом запуске).
#
#   run.sh                 такты ДО (tests/detector_ref.c) и ПОСЛЕ (src/core/detector.c)
#   run.sh --only new|ref  только один вариант
#   run.sh --equiv         сверка нового детектора со старым на ARM бит в бит (без подсчёта тактов)
#   параметры: --rate 30000 --blocks 20 --seed 1 --kind cs|am|co|dac
#
# Модель тактов (ARM Cortex-M4 TRM r0p1, табл. 3-1; FPU — табл. 7-1), упрощённо:
#   АЛУ, MUL/SMLA*/SMLAD/SMLAL/UMULL — 1; MLA/MLS — 2; SDIV/UDIV — 7 (худшее ≈ 12, типично 2…7);
#   LDR*/VLDR — 2, подряд идущая загрузка — 1 (конвейер); STR* — 1; VSTR — 2; LDRD/STRD — 3;
#   LDM/STM/PUSH/POP/VLDM/VSTM/VPUSH/VPOP — 1+N (+3, если грузится PC);
#   переход: взятый +2 (заполнение конвейера), невзятый 1; TBB/TBH — 2 + переход; IT — 1;
#   VDIV/VSQRT — 14; VFMA/VFMS/VMLA/VMLS/VNMLA/VNMLS — 3; прочие VFP — 1.
#   Код горячего пути — в CCM SRAM (0 WS), как в прошивке. Прежние эмуляторы с этой моделью
#   совпали с DWT на плате в пределах 2–4 % — но там данные лежали в ОЗУ, а код во флеш.
# Модель памяти (по умолчанию включена, --no-mem — выключить):
#   - конфликт шин CCM: код исполняется из CCM SRAM, и данные там же (det_t в CCM_BSS, как в main.c) —
#     выборка команды по I-шине и обращение к данным по D-шине идут в один банк, +1 такт на каждое
#     обращение к данным в CCM (AN4296: код в CCM, данные в SRAM1 — чтобы этого не было);
#   - чтение констант из флеш (эталон tpl_* — static const): 4 WS при 156 МГц, кэш данных ART
#     8 строк × 32 байта (LRU), промах +4 такта.
#   - LDR/STR слова без выравнивания на 4: +1 такт (два обращения к шине).
#   Без модели памяти эмулятор давал 77 тактов/отсчёт медленного пути и 1175 на подгонку против
#   111 и 1620 по DWT на плате (2026-11-06); с ней — 93 и 1328, т. е. ДО всё ещё на 15–20 % ниже
#   платы (штраф CCM 2 такта даёт 108/отсчёт, но подгонку не догоняет: CYC_CCM_PEN=2 run.sh).
#   В новой прошивке det_t в ОЗУ (main.c), к CCM ПОСЛЕ не обращается — эта неопределённость его
#   не касается; быстрый проход (код в CCM, данные в ОЗУ) с платой совпал: 3.57 против 3.4.
import argparse, os, re, struct, subprocess, sys

import numpy as np
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC, UC_ARM_REG_LR,
                               UC_ARM_REG_R0, UC_ARM_REG_R2, UC_ARM_REG_SP)

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.normpath(os.path.join(HERE, '..', '..'))
OUT = os.path.join(HERE, 'out')
BLK = 8192
FS, CPU = 3466667.0, 156e6

# ---------------------------------------------------------------- сборка (флаги — как build.sh)
def toolchain():
    m = re.search(r'^TC=(\S+)', open(os.path.join(FW, 'build.sh'), encoding='utf-8').read(), re.M)
    tc = m.group(1)
    if os.name == 'nt' and tc.startswith('/c/'):
        tc = 'C:/' + tc[3:]
    return tc

FLAGS = ['-mcpu=cortex-m4', '-mthumb', '-mfpu=fpv4-sp-d16', '-mfloat-abi=hard',
         '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-fno-math-errno',
         '-ffunction-sections', '-fdata-sections', '-DSTM32G474xx']

def build(variant, det_ram=False):
    tc = toolchain()
    os.makedirs(OUT, exist_ok=True)
    tag = variant + ('_ram' if det_ram else '')
    elf = os.path.join(OUT, tag + '.elf')
    srcs, defs = [os.path.join(HERE, 'harness.c')], (['-DDET_IN_RAM'] if det_ram else [])
    if variant in ('new', 'both'):
        srcs.append(os.path.join(FW, 'src', 'core', 'detector.c')); defs.append('-DWITH_NEW')
    if variant in ('ref', 'both'):
        srcs.append(os.path.join(FW, 'tests', 'detector_ref.c')); defs.append('-DWITH_REF')
    cmd = [tc + '/arm-none-eabi-gcc'] + FLAGS + defs + [
        '-I' + os.path.join(FW, 'src', 'core'), '-I' + os.path.join(FW, 'tests'), '-I' + HERE,
        *srcs, '-nostartfiles', '-T', os.path.join(HERE, 'link.ld'),
        '--specs=nano.specs', '--specs=nosys.specs', '-Wl,--no-warn-rwx-segments', '-o', elf, '-lm']
    subprocess.run(cmd, check=True)
    nm = subprocess.run([tc + '/arm-none-eabi-nm', '-S', '--defined-only', elf],
                        capture_output=True, text=True, check=True).stdout
    syms, funcs = {}, []
    for ln in nm.splitlines():
        p = ln.split()
        if len(p) == 4:
            a, sz, t, n = int(p[0], 16), int(p[1], 16), p[2], p[3]
        elif len(p) == 3:
            a, sz, t, n = int(p[0], 16), 0, p[1], p[2]
        else:
            continue
        syms[n] = (a, sz)
        if t in 'tT' and sz:
            funcs.append((a & ~1, (a & ~1) + sz, n))
    if variant != 'both':
        dis = subprocess.run([tc + '/arm-none-eabi-objdump', '-d', '--no-show-raw-insn', elf],
                             capture_output=True, text=True).stdout
        open(os.path.join(OUT, tag + '.dis'), 'w', encoding='utf-8').write(dis)
    return elf, syms, sorted(funcs)

def load_segments(elf):
    b = open(elf, 'rb').read()
    phoff, = struct.unpack_from('<I', b, 28)
    phentsize, phnum = struct.unpack_from('<HH', b, 42)
    segs = []
    for k in range(phnum):
        typ, off, vaddr, paddr, filesz, memsz, flg, al = struct.unpack_from('<8I', b, phoff + k * phentsize)
        if typ == 1 and filesz:
            segs.append((vaddr, b[off:off + filesz]))
    return segs

# ---------------------------------------------------------------- эмулятор и модель тактов
COND = {'eq', 'ne', 'cs', 'hs', 'cc', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al'}
REGS_RE = re.compile(r'\b(r\d+|sb|sl|fp|ip|lr|pc|sp|s\d+|d\d+)\b')

def classify(ins):
    """(такты без штрафа перехода, загрузка?, переход?)"""
    m = ins.mnemonic.split('.')[0]
    ops = ins.op_str
    isbr = (m in ('b', 'bl', 'blx', 'bx', 'cbz', 'cbnz', 'tbb', 'tbh')
            or (len(m) == 3 and m[0] == 'b' and m[1:] in COND))
    if m.startswith('it') and len(m) <= 4 and set(m[2:]) <= {'t', 'e'}:
        return 1, False, False
    if m in ('tbb', 'tbh'):
        return 2, False, True
    if m in ('ldrd', 'strd'):
        return 3, False, False
    if m in ('ldm', 'ldmia', 'ldmdb', 'ldmfd', 'pop', 'stm', 'stmia', 'stmdb', 'stmfd', 'push',
             'vldm', 'vldmia', 'vstm', 'vstmia', 'vstmdb', 'vpush', 'vpop'):
        lst = ops[ops.find('{'):] if '{' in ops else ops
        n = len(REGS_RE.findall(lst))
        r = re.search(r'([sd])(\d+)-[sd](\d+)', lst)
        if r:
            n += int(r.group(3)) - int(r.group(2))
        return 1 + n + (3 if 'pc' in lst else 0), False, False
    if m.startswith('ldr') or m == 'vldr':
        if 'pc' in ops.split(',')[0]:
            return 4, False, False
        return 2, True, isbr
    if m.startswith('str'):
        return 1, False, False
    if m == 'vstr':
        return 2, False, False
    if m in ('vdiv', 'vsqrt'):
        return 14, False, False
    if m in ('vfma', 'vfms', 'vfnma', 'vfnms', 'vmla', 'vmls', 'vnmla', 'vnmls'):
        return 3, False, False
    if m in ('mla', 'mls'):
        return 2, False, False
    if m in ('sdiv', 'udiv'):
        return 7, False, False
    if m in ('mov', 'add') and ops.split(',')[0].strip() == 'pc':
        return 1, False, True
    return 1, False, isbr

STOP = 0x080FFF00
CCM_PEN = int(os.environ.get('CYC_CCM_PEN', '1'))   # такт на обращение к данным в CCM из кода в CCM

class Emu:
    def __init__(self, elf, syms, funcs, count=True, memmodel=True):
        self.syms, self.funcs = syms, funcs
        mu = self.mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        mu.mem_map(0x08000000, 0x00100000)
        mu.mem_map(0x10000000, 0x00010000)
        mu.mem_map(0x20000000, 0x00200000)
        for va, data in load_segments(elf):
            mu.mem_write(va, data)
        mu.mem_write(STOP, b'\x00\xbf\x00\xbf')
        try:                                   # CPACR: CP10/CP11 (у части версий unicorn уже включены)
            mu.reg_write(UC_ARM_REG_C1_C0_2, mu.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
        except Exception:
            pass
        mu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        self.cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        self.cache = {}
        self.fname_cache = {}
        self.reset_counts()
        self.fs_lo, self.fs_hi = self.func_range('fast_scan')
        self.memmodel = memmodel
        if count:
            mu.hook_add(UC_HOOK_CODE, self._hook)
            if memmodel:
                mu.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self._mhook)

    def func_range(self, name):
        for lo, hi, n in self.funcs:
            if n == name:
                return lo, hi
        return -1, -1

    def fname(self, pc):
        n = self.fname_cache.get(pc)
        if n is None:
            n = '?'
            for lo, hi, nm in self.funcs:
                if lo <= pc < hi:
                    n = nm.split('.')[0]
                    break
            self.fname_cache[pc] = n
        return n

    def reset_counts(self):
        self.cyc = 0
        self.fcyc = {}
        self.prev = None
        self.prevload = False
        self.fast_i = None
        self.fast_samples = 0
        self.dcache = []
        self.mem_pen = 0
        self.pcprof = {} if getattr(self, 'want_prof', False) else None

    def _mhook(self, uc, access, addr, size, value, ud):
        p = self.prev
        if p is None:
            return
        pc, fn = p[0], p[3]
        c = 1 if (size == 4 and addr & 3) else 0         # LDR/STR без выравнивания — два обращения
        if 0x10000000 <= addr < 0x10010000:
            if 0x10000000 <= pc < 0x10010000:
                c += CCM_PEN                            # I- и D-шина в один банк CCM
        elif 0x08000000 <= addr < 0x08100000:
            line = addr >> 5
            dc = self.dcache
            if line in dc:
                dc.remove(line)
            else:
                c += 4                                  # промах кэша данных ART: 4 WS
                if len(dc) >= 8:
                    dc.pop(0)
            dc.append(line)
        if c:
            self.cyc += c
            self.mem_pen += c
            self.fcyc[fn] = self.fcyc.get(fn, 0) + c

    def _hook(self, uc, addr, size, ud):
        c = 0
        p = self.prev
        if p is not None:
            pa, ps, pbr, pfn = p
            if pbr and addr != pa + ps:
                c += 2
            if pa >= self.fs_lo and pa < self.fs_hi and not (self.fs_lo <= addr < self.fs_hi):
                self.fast_samples += uc.reg_read(UC_ARM_REG_R0) - self.fast_i      # возврат fast_scan
        if addr == self.fs_lo:
            self.fast_i = uc.reg_read(UC_ARM_REG_R2)
        info = self.cache.get(addr)
        if info is None:
            ins = next(self.cs.disasm(bytes(uc.mem_read(addr, size)), addr))
            info = classify(ins) + (self.fname(addr),)
            self.cache[addr] = info
        cost, isload, isbr, fn = info
        if isload and self.prevload:
            cost -= 1
        self.prevload = isload
        c += cost
        self.cyc += c
        self.fcyc[fn] = self.fcyc.get(fn, 0) + c
        if self.pcprof is not None:
            self.pcprof[addr] = self.pcprof.get(addr, 0) + c
        self.prev = (addr, size, isbr, fn)

    def call(self, name, *args):
        mu = self.mu
        for k, a in enumerate(args):
            mu.reg_write(UC_ARM_REG_R0 + k, int(a) & 0xFFFFFFFF)
        mu.reg_write(UC_ARM_REG_SP, 0x201F0000)
        mu.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.prev = None
        self.prevload = False
        mu.emu_start(self.syms[name][0] | 1, STOP)
        return mu.reg_read(UC_ARM_REG_R0)

    def read(self, name, n=None, off=0):
        a, sz = self.syms[name]
        return bytes(self.mu.mem_read(a + off, sz if n is None else n))

    def u32(self, name):
        return struct.unpack('<I', self.read(name, 4))[0]

    def write(self, name, data, off=0):
        self.mu.mem_write(self.syms[name][0] + off, data)

# ---------------------------------------------------------------- синтез потока (как tests/pulsegen_host.c)
def load_tpl(name):
    txt = open(os.path.join(FW, 'src', 'core', name + '.h'), encoding='utf-8').read()
    body = txt[txt.index('= {'):]
    rows = re.findall(r'\{([^{}]+)\}', body)
    return np.array([[int(v) for v in r.split(',') if v.strip()] for r in rows], dtype=np.int64)

class Shape:
    """Непрерывная форма из эталона [nph][7], шаг T/nph, линейная интерполяция (pg_init/pg_shape)."""
    def __init__(self, tpl):
        nph = len(tpl)
        ps = nph
        for p in range(nph - 1):
            if tpl[p][0] - tpl[p + 1][0] > 3000:
                ps = p + 1
                break
        self.nph, self.len, self.i0 = nph, 7 * nph, 2 * nph - ps
        h = np.zeros(self.len)
        for p in range(nph):
            sh = 1 if p < ps else 2
            for j in range(7):
                h[(j - sh) * nph + p + self.i0] = tpl[p][j] / 16384.0
        self.h = h

    def __call__(self, tau):
        u = tau * self.nph + self.i0
        out = np.zeros_like(u)
        mid = (u >= 0) & (u < self.len - 1)
        i = np.floor(u[mid]).astype(int)
        f = u[mid] - i
        out[mid] = self.h[i] * (1 - f) + self.h[i + 1] * f
        tail = u >= self.len - 1
        k = 1.0 - (u[tail] - (self.len - 1)) / (8.0 * self.nph)
        out[tail] = np.where(k > 0, self.h[-1] * k, 0.0)
        return out

def amplitudes(rng, kind, n):
    u = rng.random(n)
    if kind in ('cs', 'dac'):       # Cs-137: фотопик 580 (24 %) + комптон 15…500
        return np.where(u < 0.24, 580.0, rng.uniform(15, 500, n))
    if kind == 'am':                # Am-241: 59.5 кэВ ≈ 52 кода + немного фона
        return np.where(u < 0.8, rng.normal(52, 3, n), rng.uniform(12, 300, n))
    if kind == 'co':                # Co-60: 1025 и 1160 + комптон 15…1000
        return np.where(u < 0.15, 1025.0, np.where(u < 0.3, 1160.0, rng.uniform(15, 1000, n)))
    raise ValueError(kind)

def make_stream(shape, nblk, rate, seed, kind='cs', sigma=1.8, base=645.0, quiet=2):
    """nblk порций; первые quiet — только шум (разгон базы); пуассоновский поток rate имп/с."""
    rng = np.random.default_rng(seed)
    n = nblk * BLK
    x = np.zeros(n)
    t, tp = quiet * BLK + 0.0, []
    while rate > 0:
        t += rng.exponential(FS / rate)
        if t >= n - 32:
            break
        tp.append(t)
    amps = amplitudes(rng, kind, len(tp))
    for t0, a in zip(tp, amps):
        k0, k1 = max(0, int(np.floor(t0)) - 4), min(n, int(np.ceil(t0)) + 16)
        k = np.arange(k0, k1)
        x[k0:k1] += a * shape(k - t0)
    x += rng.normal(0, sigma, n)
    return np.clip(np.rint(base + x), 0, 4095).astype(np.uint16), len(tp)

# ---------------------------------------------------------------- замер тактов
def stats_of(emu, p):
    emu.call(p + 'dump')
    raw = emu.read(p + 'out')
    acc, pil, shp, wid, bud, jmp = struct.unpack_from('<6I', raw, 0)
    return dict(accepted=acc, rej_pileup=pil, rej_shape=shp, rej_width=wid, rej_budget=bud)

def measure(variant, args):
    det_ram = args.det_place == 'ram' or (args.det_place == 'fw' and variant == 'new')
    elf, syms, funcs = build(variant, det_ram)
    p = variant + '_'
    Emu.want_prof = bool(args.prof)
    emu = Emu(elf, syms, funcs, count=True, memmodel=not args.no_mem)
    tplname = 'tpl_dac' if args.kind == 'dac' else 'tpl_3467k'
    shape = Shape(load_tpl(tplname))
    s, npulse = make_stream(shape, args.blocks + 4, args.rate, args.seed, kind=args.kind)
    dac = args.kind == 'dac'                 # режим самотеста ЦАП, как в main.c: tpl_dac, порог 24
    emu.call(p + 'init', 24 if dac else 9, 0, 1 if dac else 0, 0, 64 if dac else 0)  # (DACT_THR), ширина 64
    # разгон базы на двух порциях шума, затем две порции шума — замер холостого хода
    emu.write('h_buf', s[:2 * BLK].tobytes())
    for b in range(2):
        emu.call(p + 'block', b * BLK, BLK)
    if dac:
        emu.call(p + 'hold', 1)              # база заморожена (det_set_base_hold)
    noise = make_stream(shape, 2, 0, args.seed + 1000)[0]
    emu.write('h_buf', noise.tobytes())
    emu.reset_counts()
    for b in range(2):
        emu.call(p + 'block', b * BLK, BLK)
    idle_cyc, idle_fast = emu.cyc, emu.fcyc.get('fast_scan', 0)
    idle_per = idle_cyc / (2 * BLK)
    idle_over = (idle_cyc - idle_fast) / 2               # на порцию вне fast_scan (update_base и т.п.)
    # поток
    st0 = stats_of(emu, p)
    emu.reset_counts()
    data = s[2 * BLK:]
    tot = fast = fit = fsamp = 0
    for b0 in range(0, args.blocks + 2, 16):
        nb = min(16, args.blocks + 2 - b0)
        emu.write('h_buf', data[b0 * BLK:(b0 + nb) * BLK].tobytes())
        for b in range(nb):
            emu.call(p + 'block', b * BLK, BLK)
    tot, fast, fsamp = emu.cyc, emu.fcyc.get('fast_scan', 0), emu.fast_samples
    fit = emu.fcyc.get('fit', 0) + emu.fcyc.get('finish', 0)          # ДО: fit + finish, ПОСЛЕ: одна fit
    st1 = stats_of(emu, p)
    if args.prof:                                   # такты по адресам функции --prof (на порцию)
        lo, hi = emu.func_range(args.prof)
        dis = {}
        for ln in open(os.path.join(OUT, variant + ('_ram' if det_ram else '') + '.dis'), encoding='utf-8'):
            m = re.match(r'\s*([0-9a-f]+):\s+(.*)', ln)
            if m:
                dis[int(m.group(1), 16)] = m.group(2)
        for a in sorted(k for k in emu.pcprof if lo <= k < hi):
            print(f'{a:08x} {emu.pcprof[a] / (args.blocks + 2):9.1f}  {dis.get(a, "")}')
    d = {k: st1[k] - st0[k] for k in st1}
    nblk = args.blocks + 2
    fits = d['accepted'] + d['rej_shape']
    trig = fits + d['rej_width'] + d['rej_pileup']
    slow = tot - fast - idle_over * nblk
    slow_samples = nblk * BLK - fsamp
    blk_ms = BLK / FS
    r = dict(variant=variant, idle_per=idle_per, fast_per=fast / max(1, fsamp),
             slow_per_pulse=slow / max(1, trig), fit_per=fit / max(1, fits),
             slow_nofit_per_sample=(slow - fit) / max(1, slow_samples),
             slow_samples_per_pulse=slow_samples / max(1, trig),
             cpu=100.0 * tot / nblk / (blk_ms * CPU), trig=trig, fits=fits, acc=d['accepted'],
             rate=trig / (nblk * blk_ms), top=sorted(emu.fcyc.items(), key=lambda kv: -kv[1])[:8],
             tot=tot, nblk=nblk, mem=emu.mem_pen / max(1, trig))
    return r

def report(r):
    name = {'ref': 'ДО (tests/detector_ref.c)', 'new': 'ПОСЛЕ (src/core/detector.c)'}[r['variant']]
    print(f"== {name}")
    print(f"  холостой ход (шум, вся det_process_block): {r['idle_per']:.2f} такт/отсч")
    print(f"  быстрый проход (fast_scan) в потоке:       {r['fast_per']:.2f} такт/отсч")
    print(f"  медленный путь:                            {r['slow_per_pulse']:.0f} тактов/импульс "
          f"({r['slow_samples_per_pulse']:.1f} отсч/импульс)")
    print(f"    из них подгонка и выдача (fit+finish):   {r['fit_per']:.0f} тактов/подгонку")
    print(f"    без подгонки:                            {r['slow_nofit_per_sample']:.1f} такт/отсч медл. пути")
    print(f"  в том числе модель памяти (CCM, флеш):     {r['mem']:.0f} тактов/импульс")
    print(f"  триггеров {r['trig']} (подгонок {r['fits']}, принято {r['acc']}), "
          f"{r['rate'] / 1e3:.1f} ктриг/с; ЦП при 156 МГц: {r['cpu']:.1f} %")
    print('  по функциям, тактов/порцию: ' + ', '.join(f"{k} {v / r['nblk']:.0f}" for k, v in r['top']))

# ---------------------------------------------------------------- сверка бит в бит на ARM
def equiv(args):
    elf, syms, funcs = build('both')
    emu = Emu(elf, syms, funcs, count=False)
    shape = Shape(load_tpl('tpl_3467k'))
    shape_dac = Shape(load_tpl('tpl_dac'))
    total = {'pulses': 0, 'emitted': 0, 'blocks': 0}
    fails = []

    def cmp(tag):
        for p in ('new_', 'ref_'):
            emu.call(p + 'dump')
        a, b = emu.read('new_out'), emu.read('ref_out')
        na, nb = emu.u32('new_nch'), emu.u32('ref_nch')
        if a != b or na != nb or emu.read('new_ch', 2 * min(na, 4096)) != emu.read('ref_ch', 2 * min(nb, 4096)) \
           or emu.read('new_capbuf') != emu.read('ref_capbuf') or emu.read('new_capinfo') != emu.read('ref_capinfo'):
            fails.append(tag)
        total['emitted'] += na
        for p in ('new_', 'ref_'):
            emu.call(p + 'clear')

    def both(fn, *a):
        emu.call('new_' + fn, *a)
        emu.call('ref_' + fn, *a)

    def run(tag, s, blks, setup=None, between=None):
        both('init', *setup) if setup else both('init', 9, 0, 0, 0, 0)
        i, k = 0, 0
        n = len(s)
        CH = 16 * BLK
        for c0 in range(0, n, CH):
            emu.write('h_buf', s[c0:c0 + CH].tobytes())
            i = 0
            m = min(CH, n - c0)
            while i < m:
                L = min(blks[k % len(blks)], m - i)
                if between:
                    between(k)
                both('block', i, L)
                i += L
                k += 1
                total['blocks'] += 1
                if total['emitted'] > 3000 or k % 4 == 0:
                    cmp(f'{tag} порция {k}')
        cmp(tag + ' конец')

    rng = np.random.default_rng(args.seed)
    scen = []
    for kind in ('cs', 'am', 'co'):
        for sig in (1.2, 1.5, 1.8):
            for rate in (30000, 120000):
                scen.append((kind, sig, rate))
    for idx, (kind, sig, rate) in enumerate(scen):
        s, npl = make_stream(shape, 40, rate, args.seed + idx, kind=kind, sigma=sig)
        total['pulses'] += npl
        blks = [BLK] if idx % 3 == 0 else ([BLK, 7, 1, 333, 4096, 5000] if idx % 3 == 1 else [1000, 3, 2048])
        cap = idx % 2 == 0
        run(f'{kind} σ{sig} {rate}', s, blks,
            between=(lambda k: both('capture', 1) if k == 2 else None) if cap else None)
    # перегрузка шумом/бюджет: 400 кимп/с, бюджет 20 подгонок на порцию
    s, npl = make_stream(shape, 10, 400000, args.seed + 77, kind='cs', sigma=1.8)
    total['pulses'] += npl
    run('бюджет 400к', s, [BLK, 3000], setup=(9, 20, 0, 0, 0))
    # выбросы помехи и ступенька базы / перегрузка
    s, npl = make_stream(shape, 12, 30000, args.seed + 78, kind='cs', sigma=1.8)
    s = s.astype(np.int32)
    for t in range(2 * BLK, 12 * BLK - 4, 27):
        s[t:t + 1 + (t // 27) % 2] += 12 + (t % 5) * 7
    s[5 * BLK:8 * BLK] += 50
    total['pulses'] += npl
    run('выбросы+перегрузка', np.clip(s, 0, 4095).astype(np.uint16), [BLK, 1234])
    # авто-порог при σ 4
    s, npl = make_stream(shape, 8, 30000, args.seed + 79, kind='cs', sigma=4.0)
    total['pulses'] += npl
    run('авто-порог σ4', s, [BLK], setup=(0, 0, 0, 0, 0))
    # режим ЦАП: эталон tpl_dac, заморозка базы, порог 30
    s, npl = make_stream(shape_dac, 8, 20000, args.seed + 80, kind='dac', sigma=1.5)
    total['pulses'] += npl
    run('ЦАП', s, [BLK], setup=(30, 0, 1, 0, 0),
        between=lambda k: both('hold', 1) if k == 2 else (both('capture', 1) if k == 3 else None))
    print(f"сверка на ARM: сценариев {len(scen) + 4}, порций {total['blocks']}, импульсов в потоках "
          f"{total['pulses']}, выдано каналов {total['emitted']} (по каждой реализации)")
    if fails:
        print('РАСХОЖДЕНИЯ: ' + '; '.join(fails[:20]))
        return 1
    print('новый детектор совпал со старым бит в бит (каналы, статистика, база, σ, порог, окна, '
          'суммы базы, счёт ступеньки, состояние)')
    return 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', choices=('new', 'ref'))
    ap.add_argument('--equiv', action='store_true')
    ap.add_argument('--no-mem', action='store_true', help='без модели памяти (конфликт CCM, флеш WS)')
    ap.add_argument('--prof', metavar='ФУНКЦИЯ', help='такты по инструкциям функции (на порцию)')
    ap.add_argument('--det-place', choices=('fw', 'ccm', 'ram'), default='fw',
                    help='где det_t: fw — как в прошивке (ДО — CCM, как в main.c до c247193+; ПОСЛЕ — ОЗУ, '
                         'как сейчас), ccm/ram — у обоих')
    ap.add_argument('--rate', type=float, default=30000)
    ap.add_argument('--blocks', type=int, default=20)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--kind', default='cs', choices=('cs', 'am', 'co', 'dac'))
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding='utf-8')
    if args.equiv:
        return equiv(args)
    res = [measure(v, args) for v in ((args.only,) if args.only else ('ref', 'new'))]
    for r in res:
        report(r)
    if len(res) == 2:
        a, b = res
        print(f"== итог: медленный путь {a['slow_per_pulse']:.0f} → {b['slow_per_pulse']:.0f} тактов/импульс "
              f"(×{a['slow_per_pulse'] / b['slow_per_pulse']:.2f}), подгонка {a['fit_per']:.0f} → {b['fit_per']:.0f}, "
              f"ЦП {a['cpu']:.1f} → {b['cpu']:.1f} %")
    return 0

if __name__ == '__main__':
    sys.exit(main())
