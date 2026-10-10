# -*- coding: utf-8 -*-
"""Общее ядро уточнения моментов для ОБЕИХ ветвей (csp.py и tia.py).

Совместный Гаусс-Ньютон по (амплитуды, моменты) с демпфированием Левенберга-Марквардта
и откатом; после каждого шага амплитуды пересчитываются линейно при новых моментах
(variable projection). Шаг принимается только если норма остатка упала.

⚠ Найдено при разборе mc_csp.py: в его solve_cluster якобиан J = dm/dt, а шаг
делается как `tq - dt` — знак наоборот. Поэтому ЛМ там отклонялся ВСЕГДА, и прогон
с уточнением совпадал с прогоном без него бит в бит (D11). Здесь знак правильный: t += dt.

Шаблон задаётся функциями f(t) и df(t) = df/dt; дополнительные неподвижные колонки
X (постоянная, ступеньки сбросов) передаются как есть.
"""
import numpy as np


def lin_solve(yy, H):
    G = H.T @ H
    n = len(G)
    th = np.linalg.solve(G + 1e-12 * np.trace(G) / n * np.eye(n), H.T @ yy)
    r = yy - H @ th
    return th, float(r @ r)


def build(tl, tq, f, X):
    H = np.empty((len(tl), len(tq) + X.shape[1]))
    if len(tq):
        H[:, :len(tq)] = f(tl[:, None] - tq[None, :])
    H[:, len(tq):] = X
    return H


def vp_refine(yy, tl, tq, f, df, X, T, n_iter=8, max_step=1.0):
    """Возвращает (амплитуды событий, уточнённые моменты, норма остатка)."""
    tq = np.asarray(tq, float).copy()
    K = len(tq)
    H = build(tl, tq, f, X)
    th, rn = lin_solve(yy, H)
    if K == 0:
        return th[:0], tq, rn
    lam = 1e-2
    for _ in range(n_iter):
        r = yy - H @ th
        Jt = -th[:K][None, :] * df(tl[:, None] - tq[None, :])
        Jf = np.hstack([H, Jt])
        A = Jf.T @ Jf
        d = np.diag(A).copy()
        d[d <= 0] = 1.0
        try:
            delta = np.linalg.solve(A + lam * np.diag(d), Jf.T @ r)
        except np.linalg.LinAlgError:
            break
        dt = np.clip(delta[-K:], -max_step * T, max_step * T)
        tq2 = tq + dt
        o = np.sort(tq2)
        if K > 1 and np.min(np.diff(o)) < 0.3 * T:
            lam *= 10
            if lam > 1e4:
                break
            continue
        H2 = build(tl, tq2, f, X)
        th2, rn2 = lin_solve(yy, H2)
        if rn2 < rn:
            tq, H, th, rn = tq2, H2, th2, rn2
            lam = max(lam / 3, 1e-6)
            if np.max(np.abs(dt)) < 1e-3 * T:
                break
        else:
            lam *= 10
            if lam > 1e4:
                break
    return th[:K], tq, rn


# ---------------------------------------------------------------- вариант с td каждого события
def vp_refine_td(yy, tl, tq, g, X, T, td0, n_iter=10, max_step=1.0, td_lo=0.8, td_hi=1.25):
    """То же, что vp_refine, но у КАЖДОГО события свободна и постоянная высвечивания td.

    g(t, td) — эталон, НОРМИРОВАННЫЙ НА ЗАРЯД (а не на пик), принимает массивы
    с бродкастом. Производные по t и td — численные (центральная разность).
    Применяется к обеим ветвям одинаково: проверка, чья амплитуда меньше зависит
    от формы, когда форму не навязывают.
    """
    tq = np.asarray(tq, float).copy()
    K = len(tq)
    tdq = np.full(K, td0)

    def bld(tq_, td_):
        H = np.empty((len(tl), K + X.shape[1]))
        if K:
            H[:, :K] = g(tl[:, None] - tq_[None, :], td_[None, :])
        H[:, K:] = X
        return H

    H = bld(tq, tdq)
    th, rn = lin_solve(yy, H)
    if K == 0:
        return th[:0], tq, tdq, rn
    lam = 1e-2
    ht = 0.05 * T
    for _ in range(n_iter):
        r = yy - H @ th
        dT = tl[:, None] - tq[None, :]
        Jt = -th[:K][None, :] * (g(dT + ht, tdq[None, :]) - g(dT - ht, tdq[None, :])) / (2 * ht)
        hd = 0.005 * tdq[None, :]
        Jd = th[:K][None, :] * (g(dT, tdq[None, :] + hd) - g(dT, tdq[None, :] - hd)) / (2 * hd)
        Jf = np.hstack([H, Jt, Jd])
        A = Jf.T @ Jf
        d = np.diag(A).copy()
        d[d <= 0] = 1.0
        try:
            delta = np.linalg.solve(A + lam * np.diag(d), Jf.T @ r)
        except np.linalg.LinAlgError:
            break
        n0 = K + X.shape[1]
        dt = np.clip(delta[n0:n0 + K], -max_step * T, max_step * T)
        dd = delta[n0 + K:]
        tq2 = tq + dt
        td2 = np.clip(tdq + dd, td_lo * td0, td_hi * td0)
        if K > 1 and np.min(np.diff(np.sort(tq2))) < 0.3 * T:
            lam *= 10
            if lam > 1e4:
                break
            continue
        H2 = bld(tq2, td2)
        th2, rn2 = lin_solve(yy, H2)
        if rn2 < rn:
            tq, tdq, H, th, rn = tq2, td2, H2, th2, rn2
            lam = max(lam / 3, 1e-6)
        else:
            lam *= 10
            if lam > 1e4:
                break
    return th[:K], tq, tdq, rn


class Refiner:
    """Единый интерфейс для обеих ветвей: fit_td=False -> vp_refine (форма задана),
    fit_td=True -> vp_refine_td (td свободна у каждого события).
    Хранит последние моменты и td, чтобы остаток для повторного поиска строился
    той же моделью, что и фит."""

    def __init__(self, f, df, g, td0, fit_td):
        self.f, self.df, self.g, self.td0, self.fit_td = f, df, g, td0, fit_td
        self.tq = np.zeros(0)
        self.tdq = np.zeros(0)

    def __call__(self, yy, tl, tq, X, T, n_iter=8, max_step=1.0):
        if self.fit_td:
            th, tq, tdq, rn = vp_refine_td(yy, tl, tq, self.g, X, T, self.td0,
                                           n_iter=max(n_iter, 10), max_step=max_step)
        else:
            th, tq, rn = vp_refine(yy, tl, tq, self.f, self.df, X, T, n_iter=n_iter,
                                   max_step=max_step)
            tdq = np.full(len(tq), self.td0)
        self.tq, self.tdq = tq, tdq
        return th, tq, rn

    def build(self, tl, X):
        H = np.empty((len(tl), len(self.tq) + X.shape[1]))
        if len(self.tq):
            if self.fit_td:
                H[:, :len(self.tq)] = self.g(tl[:, None] - self.tq[None, :], self.tdq[None, :])
            else:
                H[:, :len(self.tq)] = self.f(tl[:, None] - self.tq[None, :])
        H[:, len(self.tq):] = X
        return H
