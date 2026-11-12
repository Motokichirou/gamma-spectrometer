# -*- coding: utf-8 -*-
"""pumodel.py — Python-модель детектора gamma_fw (src/core/detector.c) и прототип разбора хвостовых
наложений. Не бит в бит: база — по порции (среднее спокойных отсчётов), а не EMA; логика медленного
пути (гистерезис, ширина, повторный рост на спаде, POST/GUARD, подгонка 16+4 фазы, χ² с ε·A) — как в
прошивке.

Режимы detect():
  mode='fw'     — как сейчас: наложение на спаде отбраковывается целиком, импульс после спада ниже
                  порога подгоняется от базы (хвост прошлого — в амплитуду);
  mode='unfold' — импульс кончается, как только дописано окно эталона (peak+5) и идёт спад; его хвост
                  A·S(n − t) вычитается из следующих отсчётов (pred), новые триггеры ищутся по остатку
                  r = y − pred с порогом thr + η·|pred|. Фронтовое наложение (повторный рост до конца
                  окна) — отбраковка, как сейчас.
"""
import re
import numpy as np

NS, IPK, NPH, Q = 7, 1, 64, 14
BLK = 8192


def load_tpl(path):
    txt = open(path, encoding='utf-8', errors='replace').read()
    body = txt[txt.index('=') + 1:]
    nums = [int(v) for v in re.findall(r'-?\d+', body)]
    t = np.array(nums[:NPH * NS], dtype=np.float64).reshape(NPH, NS)
    return t


class Tpl:
    def __init__(self, path):
        self.h = load_tpl(path)                        # Q14
        self.hh = (self.h ** 2).sum(1)
        self.ihh = 1.0 / self.hh

    def fit(self, w):
        """w — 7 отсчётов (y − база). Возврат (A, фаза, невязка) или None. Как fit() прошивки."""
        h = self.h
        yh = h @ w
        sc = yh * np.abs(yh) * self.ihh
        best, bp = 0.0, -1
        for p in range(0, NPH, 4):
            if sc[p] > best:
                best, bp = sc[p], p
        if bp < 0:
            return None
        c = bp
        for p in ((c - 2) % NPH, (c - 1) % NPH, c + 1, c + 2):
            if sc[p] > best:
                best, bp = sc[p], p
        A = yh[bp] * self.ihh[bp] * (1 << Q)
        resid = float(w @ w) - best
        return A, bp, max(resid, 0.0)


class Shape:
    """Длинная форма S(τ), τ — отсчёты от времени импульса t = imax + off[фаза]."""
    def __init__(self, tau, S, off):
        self.tau = np.asarray(tau, float)
        self.S = np.asarray(S, float)
        self.off = np.asarray(off, float)
        self.t0, self.dt = self.tau[0], self.tau[1] - self.tau[0]
        self.tmax = self.tau[-1]

    def __call__(self, x):
        return np.interp(x, self.tau, self.S, left=0.0, right=0.0)

    @staticmethod
    def load(path):
        z = np.load(path)
        return Shape(z['tau'], z['S'], z['off'])


def portion_base(s, thr, guard=40):
    """База и σ² по спокойным отсчётам порции (вне импульсов и guard после них)."""
    s = s.astype(np.float64)
    b = np.median(s)
    for _ in range(2):
        hi = (s - b) > thr
        m = hi.copy()
        idx = np.flatnonzero(hi)
        if idx.size:
            # маска [i-2, i+guard] вокруг каждого отсчёта выше порога — свёрткой
            k = np.zeros(s.size + guard + 3)
            np.add.at(k, np.clip(idx - 2, 0, None), 1)
            np.add.at(k, idx + guard + 1, -1)
            m = np.cumsum(k)[:s.size] > 0
        q = s[~m]
        if q.size > 100:
            b = q.mean()
    return b, (q.var() if q.size > 100 else 16.0)


class Stats:
    def __init__(self):
        self.acc = 0; self.pile = 0; self.shape = 0; self.width = 0
        self.acc2 = 0          # unfold: принято с вычетом хвоста (pred ≠ 0 в окне)
        self.front = 0         # unfold: фронтовые наложения (отбраковано импульсов)

    def add(self, o):
        for k in vars(self):
            setattr(self, k, getattr(self, k) + getattr(o, k))

    def trig(self):
        return self.acc + self.pile + self.shape + self.width

    def __str__(self):
        t = self.trig()
        return (f"принято {self.acc} (из них по остатку {self.acc2}), наложения {self.pile} "
                f"(фронт {self.front}), форма {self.shape}, ширина {self.width}, всего {t}, "
                f"брак {100.0 * (t - self.acc) / max(t, 1):.2f} %")


def detect(y, thr, var, tpl, mode='fw', shape=None, eta=0.02, min_w=4, max_w=56, guard=40,
           chi2_max=4.0 * NS, eps=0.01, ev=None, tail_len=64, start_armed=False, front_k=4, pred_out=None):
    """y — отсчёты минус база (float). Возврат Stats; ev — список, куда пишутся события
    (n_peak, A, phase, kind, pred_in_window), kind: 'acc' | 'rej_pile' | 'rej_shape' | 'rej_width'."""
    st = Stats()
    n = y.size
    half = thr // 2
    thr2 = 2 * thr
    var = max(var, 0.25)
    unfold = mode == 'unfold'
    L = int(tail_len or (shape.tmax if shape is not None else 0))
    pred = np.zeros(n + L + 16) if unfold else None
    if pred_out is not None and unfold:
        pred_out.append(pred)

    def emit(kind, ip, A=0.0, ph=-1, pw=0.0):
        if ev is not None:
            ev.append((ip, A, ph, kind, pw))

    def do_fit(r, ip, thr_):
        """подгонка окна r[ip-1 .. ip+5]; (A, ph) или None + причина"""
        w = r[ip - IPK: ip - IPK + NS]
        if ip - IPK < 0 or w.size < NS:
            return None
        # отсев выбросов: подряд выше thr/2 вокруг пика
        run = 1
        k = IPK + 1
        while k < NS and run < 3 and 2 * w[k] > thr_:
            run += 1; k += 1
        k = IPK - 1
        while k >= 0 and run < 3 and 2 * w[k] > thr_:
            run += 1; k -= 1
        if run < 3:
            return None
        f = tpl.fit(w)
        if f is None:
            return None
        A, ph, res = f
        ea = eps * A
        if A <= 0 or res > chi2_max * (var + ea * ea):
            return None
        return A, ph

    # состояние: как replay — начало порции в GUARD без взвода
    i = 0
    armed = start_armed
    guard_left = 0 if start_armed else guard
    state = 'guard'
    # ---------------- основной цикл ----------------
    while i < n:
        if state == 'guard':
            # GUARD / ожидание взвода; в unfold — по остатку
            while i < n:
                v = y[i] - (pred[i] if unfold else 0.0)
                tol = eta * abs(pred[i]) if unfold else 0.0
                if armed and v > thr + tol:
                    break
                if v < half + tol:
                    armed = True
                if guard_left:
                    guard_left -= 1
                if guard_left == 0 and armed:
                    # IDLE: быстрый поиск
                    if unfold:
                        rr = y[i + 1:] - pred[i + 1:n]
                        tt = thr + eta * np.abs(pred[i + 1:n])
                        j = np.flatnonzero(rr > tt)
                    else:
                        j = np.flatnonzero(y[i + 1:] > thr)
                    if j.size == 0:
                        i = n
                        break
                    i = i + 1 + j[0]
                    break
                i += 1
            if i >= n:
                break
            state = 'pulse'
        # ---------------- импульс от триггера i ----------------
        it = i
        if unfold:
            r = y - pred[:n]        # остаток (pred меняется только после подгонки)
        else:
            r = y
        if it == 0:
            # нет до-триггерного отсчёта (начало порции) — пропуск
            i += 1; state = 'guard'; armed = False; guard_left = guard
            continue
        peak = r[it - 1]; ip = it - 1
        lpeak = 0.0; falling = False; fall_min = 0.0; pileup = False; npile = 0
        wide = False
        k = it
        nabove = 0
        arm_in = False        # unfold: спад ниже thr/2 внутри окна, затем снова выше thr — наложение
        ended = None          # индекс последнего отсчёта импульса
        while k < n:
            dev = r[k]
            if dev > peak:
                peak = dev; ip = k
            if not falling and dev > lpeak:
                lpeak = dev
            if lpeak >= thr2:
                if not falling:
                    if 4 * dev + 4 <= 3 * lpeak:
                        falling = True; fall_min = dev
                elif dev < fall_min:
                    fall_min = dev
                else:
                    rise = max(int(lpeak) // 4, thr)
                    if dev > fall_min + rise and 3 * lpeak < 5 * dev:
                        pileup = True; npile += 1; falling = False; lpeak = dev
            if dev > thr:
                nabove += 1
            if unfold:
                if arm_in and dev > thr:
                    pileup = True; npile += 1; ended = k; break
                if dev < half:
                    arm_in = True
                # конец импульса: окно дописано и (спад или ниже порога)
                if k >= ip + (NS - IPK - 1) and (falling or dev <= thr):
                    ended = k; break
                if k - it + 1 > max_w:
                    wide = True; ended = k; break
            else:
                if dev <= thr:
                    if (k - it + 1) - 1 > max_w:
                        wide = True
                    ended = k; break
                if (k - it + 1) > max_w:
                    wide = True; ended = k; break
            k += 1
        if ended is None:
            break                         # порция кончилась посреди импульса — как replay, пропуск
        i = ended + 1
        if wide:
            st.width += 1; emit('rej_width', ip)
            state = 'guard'; armed = False; guard_left = guard
            continue
        if pileup:
            st.pile += 1 + npile
            if unfold:
                st.front += 1 + npile
            emit('rej_pile', ip)
            state = 'guard'; armed = False; guard_left = guard
            continue
        width = nabove
        if width < min_w:
            st.width += 1; emit('rej_width', ip)
            state = 'guard'; armed = False; guard_left = guard
            continue
        # POST: дописать окно (только fw: в unfold окно уже дописано)
        armed2 = r[ended] < half
        went_post = ip + (NS - IPK) > i
        post_trig = False
        while ip + (NS - IPK) > i and i < n:
            dev = r[i]
            if armed2 and dev > thr:
                post_trig = True; break
            if dev < half:
                armed2 = True
            i += 1
        if post_trig:
            st.pile += 1; emit('rej_pile', ip)
            state = 'pulse'
            # новый импульс начинается с i (триггер)
            state = 'guard'; armed = True; guard_left = 0
            # сразу триггер: в цикле guard первая же проверка сработает
            continue
        if ip + (NS - IPK) > i:
            break                          # конец порции
        f = do_fit(r, ip, thr) if ip >= it else None     # максимум на до-триггерном — отказ
        pw = float(pred[ip - 1: ip + 6].max()) if unfold else 0.0
        if f is None:
            st.shape += 1; emit('rej_shape', ip)
            state = 'guard'; armed = armed2 if went_post else False; guard_left = guard
            continue
        A, ph = f
        if unfold:
            t = ip + shape.off[ph]
            a = i
            b = min(int(t + L), n)
            add = A * shape(np.arange(a, b) - t) if b > a else None
            if add is not None:
                pred[a:b] += add
            # фронтовое наложение: триггер по остатку в первых front_k отсчётах после окна —
            # фронт следующего импульса уже лежал в окне этого: отбраковка обоих, как сейчас
            j = min(i + front_k, n)
            rr = y[i:j] - pred[i:j]
            if (rr > thr + eta * np.abs(pred[i:j])).any():
                if add is not None:
                    pred[a:b] -= add
                st.pile += 2; st.front += 2
                emit('rej_pile', ip)
                # пропустить до спада ниже порога (по остатку старых хвостов), затем GUARD без взвода
                k = i
                while k < n and y[k] - pred[k] > thr:
                    k += 1
                i = k
                state = 'guard'; armed = False; guard_left = guard
                continue
            st.acc += 1
            if abs(pw) > 3.0:
                st.acc2 += 1
            emit('acc', ip, A, ph, pw)
            # сразу по остатку: взвод по остатку, без GUARD (база не нужна)
            state = 'guard'; armed = False; guard_left = 0
            continue
        st.acc += 1
        emit('acc', ip, A, ph, pw)
        if False:
            pass
        else:
            # прошивка: после post_end — GUARD с прежним взводом; после term с готовым окном — to_guard (armed=0)
            state = 'guard'; armed = armed2 if went_post else False; guard_left = guard
    return st


def load_portions(path):
    return np.fromfile(path, dtype=np.uint16).reshape(-1, BLK)


def collect_singles(x, ev, thr, alo, ahi, pre=30, post=150, quiet_from=60, maxn=None):
    """Окна одиночных принятых импульсов (y − база) из порций x по событиям ev (run.py).
    Возврат: w (n × (pre+post+1)), A, phase, ip, portion."""
    W, A, P, I, R = [], [], [], [], []
    for por in np.unique(ev[:, 1]).astype(int):
        e = ev[ev[:, 1] == por]
        sel = np.flatnonzero((e[:, 5] == 0) & (e[:, 3] >= alo) & (e[:, 3] <= ahi))
        if sel.size == 0:
            continue
        b, v = portion_base(x[por], thr)
        y = x[por].astype(np.float64) - b
        for j in sel:
            ip = int(e[j, 2])
            if ip - pre < 0 or ip + post >= x.shape[1]:
                continue
            others = (e[:, 2] > ip - pre) & (e[:, 2] < ip + post)
            others[j] = False
            if others.any():
                continue
            w = y[ip - pre: ip + post + 1]
            if (w[:pre - 8] > thr).any() or (np.abs(w[pre + quiet_from:]) > thr).any():
                continue
            W.append(w); A.append(e[j, 3]); P.append(int(e[j, 4])); I.append(ip); R.append(por)
            if maxn and len(A) >= maxn:
                return np.array(W), np.array(A), np.array(P), np.array(I), np.array(R)
    return np.array(W), np.array(A), np.array(P), np.array(I), np.array(R)
