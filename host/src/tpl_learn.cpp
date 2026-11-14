// tpl_learn.cpp — см. tpl_learn.h. Алгоритм один в один с firmware/gamma_fw/tools/tpl_pmt.py:
// find_singles → build_rows (ранг признака f, затем refine × ранг времени τ по всей форме) →
// rows_from_shape → поправка масштаба строк по фотопику (pp_scale × pp_iters) → Q14.
#include "tpl_learn.h"
#include "i18n.h"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdio>
#include <limits>

namespace tpl {

static const double NaN = std::numeric_limits<double>::quiet_NaN();

static double median(std::vector<double> v)
{
    if (v.empty()) return NaN;
    size_t n = v.size(), h = n / 2;
    std::nth_element(v.begin(), v.begin() + h, v.end());
    double a = v[h];
    if (n % 2) return a;
    double b = *std::max_element(v.begin(), v.begin() + h);
    return 0.5 * (a + b);
}

// база для таблицы хвоста — как у детектора прибора: среднее спокойных отсчётов (вне [i − 2, i + 40] вокруг
// отсчётов выше th5 = 5σ), а не медиана всей порции: медиана выше на ≈ 0.05 % A (импульсы), а хвост на +40…+64
// (−0.5 % A) вычитается из сигнала с базой детектора (EMA спокойных отсчётов). Как portion_base в pumodel.py
static double quiet_base(const std::vector<double>& x, double b, double th5)
{
    std::vector<char> m(x.size());
    for (int it = 0; it < 2; it++) {
        std::fill(m.begin(), m.end(), 0);
        for (int i = 0; i < (int)x.size(); i++)
            if (x[i] - b > th5) for (int q = std::max(0, i - 2); q <= std::min((int)x.size() - 1, i + 40); q++) m[q] = 1;
        double sum = 0; int n = 0;
        for (size_t i = 0; i < x.size(); i++) if (!m[i]) { sum += x[i]; n++; }
        if (n <= 100) break;
        b = sum / n;
    }
    return b;
}

// одна порция: счётчики в s и (keep) окна одиночных импульсов; возвращает σ шума порции.
// view (для экрана): метки принятых/отбракованных и нормированные окна вида — на отбор не влияет
static double singles_block(const std::vector<uint16_t>& blk, double thr, double amin, double amax, int clip,
                            Singles& s, std::vector<double>& x, std::vector<double>& d, bool keep,
                            BlockView* view = nullptr)
{
    s.blocks++;
    x.assign(blk.begin(), blk.end());
    double b = median(x);
    d.resize(x.size());
    for (size_t i = 0; i < x.size(); i++) d[i] = fabs(x[i] - b);
    double sd = 1.4826 * median(d);
    double th = thr * sd;
    if (view) { view->base = b; view->sigma = sd; }
    std::vector<int> up;
    for (int i = 1; i < BLK; i++) if (x[i] - b > th && x[i - 1] - b <= th) up.push_back(i);
    s.cross += (int)up.size();
    double bq = NaN;                          // база для длинных окон (quiet_base), по первому требованию
    for (size_t k = 0; k < up.size(); k++) {
        int c = up[k];
        if (c - ISO < 0 || c + ISO >= BLK) continue;
        if ((k > 0 && up[k - 1] >= c - ISO) || (k + 1 < up.size() && up[k + 1] <= c + ISO)) {
            if (view) view->bad.push_back(c);
            continue;
        }
        bool quiet = true;
        for (int i = c - ISO; i < c - 2; i++) if (fabs(x[i] - b) > th) { quiet = false; break; }
        if (!quiet) { if (view) view->bad.push_back(c); continue; }
        s.single++;
        int im = c;
        for (int i = c; i < c + 10; i++) if (x[i] > x[im]) im = i;   // argmax(y[c:c+10]) — первый максимум
        double A = x[im] - b;
        if (im - PRE < 0 || im + POST >= BLK) continue;
        // наложение на фронте: порог пересёк первый (меньший) импульс, а максимум — у севшего следом.
        // У одиночного фронт от порога до максимума ≤ ~6 отсчётов (приёмка 2026-11-12)
        if (im - c > RISE_MAX) { if (view) view->bad.push_back(im); continue; }
        // наложение на хвост: второй импульс, севший, пока первый выше порога, не даёт пересечения
        // снизу — ловим по подъёму над текущим минимумом спада больше порога (шум σ ≈ 3 кода ≪ 10σ)
        {
            bool rise = false;
            double mn = x[im];
            for (int i = im + 1; i <= c + ISO && i < BLK; i++) {
                if (x[i] < mn) mn = x[i];
                else if (x[i] - mn > th) { rise = true; break; }
            }
            if (rise) { if (view) view->bad.push_back(im); continue; }
        }
        double mx = 0;
        for (int i = im - PRE; i <= im + POST; i++) mx = std::max(mx, x[i]);
        if (!(A >= amin && A <= amax) || mx >= clip) { if (view) view->bad.push_back(im); continue; }
        s.amp++;
        if (view) {
            view->ok.push_back(im);
            if (im - VPRE >= 0 && im + VPOST < BLK && A > 0) {
                VWin v;
                for (int i = 0; i < VW; i++) v[i] = (float)((x[im - VPRE + i] - b) / A);
                view->pulses.push_back(v);
            }
        }
        if (!keep) continue;
        Win w;
        for (int i = 0; i < W; i++) w[i] = x[im - PRE + i] - b;
        s.wins.push_back(w);
        // длинное окно (таблица хвоста): до +LPOST нет ни пересечения, ни подъёма над спадом больше порога
        if (im - LPRE < 0 || im + LPOST >= BLK) continue;
        if (k + 1 < up.size() && up[k + 1] <= im + LPOST) continue;
        {
            bool quiet_tail = true;
            double mn = x[im];
            for (int i = im + 1; i <= im + LPOST; i++) {
                if (x[i] < mn) mn = x[i];
                else if (x[i] - mn > th) { quiet_tail = false; break; }
                if (i > im + 30 && fabs(x[i] - b) > th) { quiet_tail = false; break; }
            }
            if (!quiet_tail) continue;
        }
        if (!std::isfinite(bq)) bq = quiet_base(x, b, 5.0 * sd);
        LWin lw;
        for (int i = 0; i < LW; i++) lw[i] = (float)(x[im - LPRE + i] - bq);
        s.lwins.push_back(lw);
    }
    return sd;
}

Singles find_singles(const std::vector<std::vector<uint16_t>>& blocks, double thr, double amin, double amax, int clip)
{
    Singles s;
    double sigsum = 0;
    std::vector<double> x, d;
    for (const auto& blk : blocks) {
        if (blk.size() != (size_t)BLK) continue;
        sigsum += singles_block(blk, thr, amin, amax, clip, s, x, d, true);
    }
    s.sigma = s.blocks ? sigsum / s.blocks : 0;
    return s;
}

int count_singles(const std::vector<std::vector<uint16_t>>& blocks, size_t from, size_t to,
                  double thr, double amin, double amax, int clip)
{
    Singles s;
    std::vector<double> x, d;
    if (to > blocks.size()) to = blocks.size();
    for (size_t k = from; k < to; k++)
        if (blocks[k].size() == (size_t)BLK) singles_block(blocks[k], thr, amin, amax, clip, s, x, d, false);
    return s.amp;
}

BlockView view_block(const std::vector<uint16_t>& blk, double thr, double amin, double amax, int clip)
{
    BlockView v;
    if (blk.size() != (size_t)BLK) return v;
    Singles s;
    std::vector<double> x, d;
    singles_block(blk, thr, amin, amax, clip, s, x, d, false, &v);
    return v;
}

// ---- непрерывная форма из строк (interleave) ----
struct Shape { std::vector<double> x, S; };

static double interp(double t, const Shape& f)
{
    const auto& x = f.x; const auto& S = f.S;
    if (t <= x.front()) return t < x.front() ? 0.0 : S.front();
    if (t >= x.back()) return S.back();
    size_t i = std::upper_bound(x.begin(), x.end(), t) - x.begin();
    double a = x[i - 1], b = x[i];
    return S[i - 1] + (S[i] - S[i - 1]) * (t - a) / (b - a);
}

static bool solve(int m, double A[5][6], double* out)
{
    for (int col = 0; col < m; col++) {
        int piv = col;
        for (int r = col + 1; r < m; r++) if (fabs(A[r][col]) > fabs(A[piv][col])) piv = r;
        for (int c = 0; c <= m; c++) std::swap(A[col][c], A[piv][c]);
        if (fabs(A[col][col]) < 1e-300) return false;
        for (int r = 0; r < m; r++) if (r != col) {
            double f = A[r][col] / A[col][col];
            for (int c = col; c <= m; c++) A[r][c] -= f * A[col][c];
        }
    }
    for (int i = 0; i < m; i++) out[i] = A[i][m] / A[i][i];
    return true;
}

static Shape interleave(const std::vector<Win>& rows)
{
    int nph = (int)rows.size();
    std::vector<double> S(nph * W);
    for (int j = 0; j < W; j++) for (int p = 0; p < nph; p++) S[j * nph + p] = rows[p][j];
    {   // медиана по 5 узлам (как tpl_pmt.py): убирает одиночный выброс узла на отсчёте максимума
        std::vector<double> m(S.size());
        int N = (int)S.size();
        auto pad = [&](int q) {             // np.r_[S[:2], S, S[-2:]]
            if (q < 2) return S[q];
            if (q < N + 2) return S[q - 2];
            return S[q == N + 2 ? N - 2 : N - 1];
        };
        for (int g = 0; g < N; g++) {
            double v[5];
            for (int k = 0; k < 5; k++) v[k] = pad(g + k);
            std::sort(v, v + 5); m[g] = v[2];
        }
        S.swap(m);
    }
    int ip = (int)(std::max_element(S.begin(), S.end()) - S.begin());
    // полином 4-й степени по |g − ip| ≤ nph/2 (переменная нормирована на nph/2)
    double A[5][6] = {};
    const double sc = nph / 2.0;
    for (int g = 0; g < (int)S.size(); g++) {
        if (std::abs(g - ip) > nph / 2) continue;
        double u = (g - ip) / sc, pw[5] = { 1, u, u * u, u * u * u, u * u * u * u };
        for (int r = 0; r < 5; r++) { for (int c = 0; c < 5; c++) A[r][c] += pw[r] * pw[c]; A[r][5] += pw[r] * S[g]; }
    }
    double cf[5] = { S[ip], 0, 0, 0, 0 };
    solve(5, A, cf);
    double best = -1e300, bx = 0;
    for (int i = 0; i <= 4000; i++) {
        double xf = -nph / 2.0 + nph * i / 4000.0, u = xf / sc;
        double v = cf[0] + u * (cf[1] + u * (cf[2] + u * (cf[3] + u * cf[4])));
        if (v > best) { best = v; bx = xf; }
    }
    double g0 = ip + bx;
    Shape f;
    f.x.resize(S.size()); f.S.resize(S.size());
    for (size_t g = 0; g < S.size(); g++) { f.x[g] = (g - g0) / nph; f.S[g] = S[g] / best; }
    return f;
}

static std::vector<double> fit_tau(const std::vector<Win>& wins, const Shape& f)
{
    const double step = 1.0 / 128;
    int T = (int)std::lround(2.0 / step) + 1;
    std::vector<Win> H(T);
    std::vector<double> hh(T, 0);
    for (int t = 0; t < T; t++) {
        double tau = -1.0 + t * step;
        for (int j = 0; j < W; j++) { H[t][j] = interp(j - PRE + tau, f); hh[t] += H[t][j] * H[t][j]; }
    }
    std::vector<double> out(wins.size());
    std::vector<double> r(T);
    for (size_t k = 0; k < wins.size(); k++) {
        const Win& w = wins[k];
        double ww = 0; for (int j = 0; j < W; j++) ww += w[j] * w[j];
        for (int t = 0; t < T; t++) {
            double wh = 0; for (int j = 0; j < W; j++) wh += w[j] * H[t][j];
            double a = wh / hh[t];
            r[t] = ww - a * wh;
        }
        int b = (int)(std::min_element(r.begin(), r.end()) - r.begin());
        if (b < 1) b = 1;
        if (b > T - 2) b = T - 2;
        double rm = r[b - 1], r0 = r[b], rp = r[b + 1], den = rm - 2 * r0 + rp;
        double fr = den > 0 ? 0.5 * (rm - rp) / den : 0.0;
        fr = std::max(-1.0, std::min(1.0, fr));
        out[k] = -1.0 + b * step + fr * step;
    }
    return out;
}

// строки 64 × 7 из непрерывной формы (как gfw.tpl_rows_from_shape): отсчёты в k + p/64, окно от максимума − IPK
static bool rows_from_shape(const Shape& f, double rows[NPH][NS])
{
    for (int p = 0; p < NPH; p++) {
        double y[28];
        for (int i = 0; i < 28; i++) y[i] = interp((i - 4) + (double)p / NPH, f);
        int im = (int)(std::max_element(y, y + 28) - y);
        if (im - IPK < 0 || im - IPK + NS > 28) return false;
        for (int i = 0; i < NS; i++) rows[p][i] = y[im - IPK + i];
        int am = (int)(std::max_element(rows[p], rows[p] + NS) - rows[p]);
        if (am != IPK) return false;
    }
    return true;
}

static void quant(const double rows[NPH][NS], int q[NPH][NS])
{
    for (int p = 0; p < NPH; p++) for (int i = 0; i < NS; i++) {
        double v = std::nearbyint(rows[p][i] * QMAX);
        q[p][i] = (int)std::max(-(double)QMAX, std::min((double)QMAX, v));
    }
}

// подгонка как в детекторе (окно от максимума − 1, одна амплитуда, перебор 64 фаз)
static void fit(const std::vector<Win>& wins, const int q[NPH][NS], std::vector<double>& A, std::vector<int>& ph, std::vector<double>* res)
{
    double h[NPH][NS], hh[NPH];
    for (int p = 0; p < NPH; p++) { hh[p] = 0; for (int i = 0; i < NS; i++) { h[p][i] = q[p][i] / (double)QMAX; hh[p] += h[p][i] * h[p][i]; } }
    A.resize(wins.size()); ph.resize(wins.size());
    if (res) res->resize(wins.size());
    for (size_t k = 0; k < wins.size(); k++) {
        const double* w = &wins[k][PRE - IPK];
        double br = 1e300, ba = 0; int bp = 0;
        for (int p = 0; p < NPH; p++) {
            double wh = 0; for (int i = 0; i < NS; i++) wh += w[i] * h[p][i];
            double a = wh / hh[p], r = 0;
            for (int i = 0; i < NS; i++) { double e = w[i] - a * h[p][i]; r += e * e; }
            if (r < br) { br = r; ba = a; bp = p; }
        }
        A[k] = ba; ph[k] = bp;
        if (res) (*res)[k] = sqrt(br / NS) / std::max(ba, 1e-9);
    }
}

static double centroid_near(const std::vector<double>& A, double c, double w = 0.08, int it = 6)
{
    for (int k = 0; k < it; k++) {
        double s = 0; int n = 0;
        for (double a : A) if (fabs(a - c) <= w * c) { s += a; n++; }
        if (n < 10) return NaN;
        c = s / n;
    }
    return c;
}

static void photopeak(const std::vector<double>& A, double& c, double& sd, int& n, double lo = 0.6)
{
    c = sd = NaN; n = 0;
    if (A.size() < 50) return;
    std::vector<double> s(A); std::sort(s.begin(), s.end());
    double q = s[(size_t)(lo * (s.size() - 1))], mx = s.back();
    int nb = (int)(mx + 2 - q);
    if (nb < 5) return;
    std::vector<double> h(nb, 0), hs(nb, 0);
    for (double a : A) { int i = (int)floor(a - q); if (i >= 0 && i < nb) h[i] += 1; }
    for (int i = 0; i < nb; i++)
        for (int k = -6; k <= 6; k++) if (i + k >= 0 && i + k < nb) hs[i] += h[i + k] * exp(-0.5 * (k / 2.5) * (k / 2.5));
    int im = (int)(std::max_element(hs.begin(), hs.end()) - hs.begin());
    c = q + im + 0.5;
    for (int k = 0; k < 5; k++) {
        double sm = 0; int m = 0;
        for (double a : A) if (fabs(a - c) <= 0.08 * c) { sm += a; m++; }
        if (!m) return;
        c = sm / m;
    }
    double s2 = 0; int m = 0;
    for (double a : A) if (fabs(a - c) <= 0.08 * c) { s2 += (a - c) * (a - c); m++; }
    sd = m ? sqrt(s2 / m) : NaN; n = m;
}

static double phase_spread(const std::vector<double>& A, const std::vector<int>& ph, double c0)
{
    double lo = 1e300, hi = -1e300;
    for (int b = 0; b < 8; b++) {
        std::vector<double> sub;
        for (size_t k = 0; k < A.size(); k++) if (ph[k] * 8 / NPH == b) sub.push_back(A[k]);
        if (sub.size() <= 100) continue;
        double c, sd; int n; photopeak(sub, c, sd, n);
        if (!std::isfinite(c)) continue;
        lo = std::min(lo, c); hi = std::max(hi, c);
    }
    return hi > lo ? 100.0 * (hi - lo) / c0 : 0.0;
}

static void pp_scale(double rows[NPH][NS], const std::vector<Win>& wins)
{
    int q[NPH][NS]; quant(rows, q);
    std::vector<double> A; std::vector<int> ph;
    fit(wins, q, A, ph, nullptr);
    double P0, sd; int n; photopeak(A, P0, sd, n);
    if (!std::isfinite(P0)) return;
    double c[NPH];
    for (int p = 0; p < NPH; p++) {
        std::vector<double> sub;
        for (size_t k = 0; k < A.size(); k++) {
            int d = std::abs(((ph[k] - p + NPH / 2) % NPH + NPH) % NPH - NPH / 2);
            if (d <= 4) sub.push_back(A[k]);
        }
        double v = centroid_near(sub, P0);
        c[p] = std::isfinite(v) ? v / P0 : NaN;
    }
    double mean = 0; int cnt = 0;
    for (double v : c) if (std::isfinite(v)) { mean += v; cnt++; }
    mean = cnt ? mean / cnt : 1.0;
    for (double& v : c) if (!std::isfinite(v)) v = mean;
    double cs[NPH], lg = 0;
    for (int p = 0; p < NPH; p++) {               // круговое сглаживание по 5 строкам
        double s = 0; for (int k = -2; k <= 2; k++) s += c[(p + k + NPH) % NPH];
        cs[p] = s / 5.0; lg += log(cs[p]);
    }
    double gm = exp(lg / NPH), mx = 0;
    for (int p = 0; p < NPH; p++) for (int i = 0; i < NS; i++) { rows[p][i] *= cs[p] / gm; mx = std::max(mx, fabs(rows[p][i])); }
    if (mx > 1.0) for (int p = 0; p < NPH; p++) for (int i = 0; i < NS; i++) rows[p][i] /= mx;
}

Result build(const Singles& s, int refine, bool pp, int pp_iters)
{
    Result R;
    R.sigma = s.sigma;
    const auto& wins = s.wins;
    if ((int)wins.size() < NPH * 10) { R.msg = "мало импульсов: " + std::to_string(wins.size()) + " (нужно ≥ 640)"; return R; }
    size_t n = wins.size();
    std::vector<double> key(n), E(n);
    for (size_t k = 0; k < n; k++) {
        const Win& w = wins[k];
        key[k] = -(w[PRE + 1] - w[PRE - 1]) / w[PRE];
        double ss = 0; for (double v : w) ss += v * v;
        E[k] = sqrt(std::max(ss - W * s.sigma * s.sigma, 1e-9));
    }
    Shape f;
    std::vector<Win> rows(NPH);
    for (int it = 0; it <= refine; it++) {
        std::vector<size_t> o(n); std::iota(o.begin(), o.end(), 0);
        std::stable_sort(o.begin(), o.end(), [&](size_t a, size_t b) { return key[a] < key[b]; });
        size_t base = n / NPH, extra = n % NPH, pos = 0;          // np.array_split: первые extra групп на 1 больше
        for (int g = 0; g < NPH; g++) {
            size_t cnt = base + ((size_t)g < extra ? 1 : 0);
            Win acc{}; for (auto& v : acc) v = 0;
            for (size_t i = 0; i < cnt; i++) { const Win& w = wins[o[pos + i]]; double e = E[o[pos + i]]; for (int j = 0; j < W; j++) acc[j] += w[j] / e; }
            for (int j = 0; j < W; j++) acc[j] /= (double)cnt;
            rows[g] = acc; pos += cnt;
        }
        f = interleave(rows);
        if (it < refine) key = fit_tau(wins, f);
    }
    double r7[NPH][NS];
    if (!rows_from_shape(f, r7)) { R.msg = "форма не годится: максимум строки не на позиции 1"; return R; }
    // ширина на половине высоты
    { double a = 1e300, b = -1e300; for (size_t g = 0; g < f.S.size(); g++) if (f.S[g] >= 0.5) { a = std::min(a, f.x[g]); b = std::max(b, f.x[g]); } R.fwhm50 = b - a; }
    // общая форма для экрана (каждый 4-й узел: шаг 1/16 отсчёта). Вплетённые 64 строки дают «пилу» с
    // периодом ровно 1 отсчёт (строки нормированы на свой отсчёт максимума, а не на вершину) — скользящее
    // среднее шириной 1 отсчёт (NPH узлов) убирает её целиком; затем максимум снова 1. Только для показа.
    {
        const int N = (int)f.S.size(), H = NPH / 2;
        std::vector<double> sm(N, 0.0);
        double mx = 0;
        for (int g = 0; g < N; g++) {
            double s = 0; int c = 0;
            for (int k = g - H; k < g + H; k++) if (k >= 0 && k < N) { s += f.S[k]; c++; }
            sm[g] = c ? s / c : f.S[g];
            mx = std::max(mx, sm[g]);
        }
        int im = (int)(std::max_element(sm.begin(), sm.end()) - sm.begin());
        double x0 = f.x[im];
        for (int g = H; g + H <= N; g += 4) {     // края без полного окна не показываем
            R.shape_x.push_back((float)(f.x[g] - x0));
            R.shape_y.push_back((float)(mx > 0 ? sm[g] / mx : sm[g]));
        }
    }

    int q[NPH][NS]; std::vector<double> A; std::vector<int> ph;
    quant(r7, q); fit(wins, q, A, ph, nullptr);
    double c0, sd0; int n0; photopeak(A, c0, sd0, n0);
    R.spread_before = std::isfinite(c0) ? phase_spread(A, ph, c0) : 0;
    if (pp) for (int i = 0; i < pp_iters; i++) pp_scale(r7, wins);
    quant(r7, R.q);
    std::vector<double> res;
    fit(wins, R.q, A, ph, &res);
    photopeak(A, R.peak, R.peak_sd, n0);
    R.spread_after = std::isfinite(R.peak) ? phase_spread(A, ph, R.peak) : 0;
    R.resid_med = 100.0 * median(res);
    R.n = (int)n;
    R.ok = true;
    char b[200];
    snprintf(b, sizeof b, "импульсов %d, невязка (медиана) %.2f %%, ширина формы %.1f отсч., размах фотопика по фазам %.2f %% → %.2f %%",
             R.n, R.resid_med, R.fwhm50, R.spread_before, R.spread_after);
    R.msg = b;
    return R;
}

std::string header(const Result& r, const char* name, const char* note)
{
    std::string g = name; for (auto& ch : g) ch = (char)toupper((unsigned char)ch);
    std::string s = "/* " + std::string(name) + ".h — " + note + " (СГЕНЕРИРОВАНО). Формат как tpl_3467k.h:\n"
                    " * 64 фаз × 7 отсчётов, Q14, максимум строки на позиции 1. */\n#ifndef " + g + "_H\n#define " + g + "_H\n\n"
                    "#include <stdint.h>\n\nstatic const int16_t " + name + "[64][7] = {\n";
    char b[96];
    for (int p = 0; p < NPH; p++) {
        snprintf(b, sizeof b, "    { %6d, %6d, %6d, %6d, %6d, %6d, %6d },\n", r.q[p][0], r.q[p][1], r.q[p][2], r.q[p][3], r.q[p][4], r.q[p][5], r.q[p][6]);
        s += b;
    }
    s += "};\n\n#endif /* " + g + "_H */\n";
    return s;
}

std::vector<std::string> commands(const Result& r)
{
    std::vector<std::string> v;
    char b[128];
    for (int p = 0; p < NPH; p++) {
        snprintf(b, sizeof b, "-tplw %d %d %d %d %d %d %d %d", p, r.q[p][0], r.q[p][1], r.q[p][2], r.q[p][3], r.q[p][4], r.q[p][5], r.q[p][6]);
        v.push_back(b);
    }
    return v;
}


// ---- таблица хвоста (разбор наложений): порт tools/pileup/shape.py с уточнениями ----
// одиночные с тишиной до +LPOST (база — среднее спокойных отсчётов) → A и фаза подгонкой эталона (как детектор)
// → время импульса по 50 % фронта (CFD), off[фаза] = медиана (t_cfd − imax) → общая форма S(τ) на сетке 1/16
// отсчёта (МНК по A; хвост τ > 15 — квадратичный Савицкий-Голей по соседним целым отсчётам) → чистка одиночных
// по остатку > 6σ → off[фаза] уточняется подгонкой к S (2 итерации) → строки: фазы делятся 16 равными шагами по
// диапазону off; время строки t_r — среднее off её импульсов с весом A²; масштаб κ_r — МНК по её импульсам;
// s[r][j − 1] = κ_r·S(j − t_r); у вершины (j ≤ 4) — поправка на средний остаток импульсов строки.
static const int TL_MIN = 300;   // одиночных с тишиной до +LPOST, меньше — таблица не строится
static const int TL_GRID = 16;   // шагов сетки S на отсчёт
static const int TL_NEAR = 4;    // у вершины (j ≤ 4) строка берёт поправку по своим импульсам

namespace {
struct TailShape {
    double tau0 = 0;
    std::vector<double> S;
    double at(double tau) const
    {
        double g = (tau - tau0) * TL_GRID;
        if (g <= 0) return S.front();
        int i = (int)g;
        if (i >= (int)S.size() - 1) return S.back();
        double f = g - i;
        return S[i] + (S[i + 1] - S[i]) * f;
    }
};
}

static TailShape tail_shape(const std::vector<LWin>& lw, const std::vector<double>& A, const std::vector<double>& tsp,
                            const std::vector<size_t>& use)
{
    TailShape f;
    f.tau0 = -LPRE + 2;
    const int NG = (LPOST + 2 - (-LPRE + 2)) * TL_GRID;     // τ до +72: с запасом на сглаживание у +64 − off
    std::vector<double> sum(NG, 0.0), wsum(NG, 0.0);
    for (size_t k : use) {
        const double a = A[k];
        for (int i = 0; i < LW; i++) {
            double tau = (i - LPRE) - tsp[k];
            long g = std::lround((tau - f.tau0) * TL_GRID);
            if (g < 0 || g >= NG) continue;
            sum[g] += lw[k][i] * a; wsum[g] += a * a;
        }
    }
    f.S.assign(NG, NaN);
    for (int g = 0; g < NG; g++) if (wsum[g] > 0) f.S[g] = sum[g] / wsum[g];
    // пустые узлы — линейно между соседями, по краям — ближайшим
    int last = -1;
    for (int g = 0; g < NG; g++) {
        if (!std::isfinite(f.S[g])) continue;
        if (last < 0) for (int q = 0; q < g; q++) f.S[q] = f.S[g];
        else for (int q = last + 1; q < g; q++) f.S[q] = f.S[last] + (f.S[g] - f.S[last]) * (q - last) / (double)(g - last);
        last = g;
    }
    if (last < 0) { f.S.assign(NG, 0.0); return f; }
    for (int q = last + 1; q < NG; q++) f.S[q] = f.S[last];
    // хвост τ > 15: сглаживание по соседним целым отсчётам (узлы g ± 16, g ± 32), квадратичное (Савицкий-
    // Голей, 5 точек). В shape.py — скользящее среднее ±1 отсчёт по всем узлам; здесь иначе по двум причинам:
    // среднее на изогнутом спаде смещает форму (до −0.15 % A на +13…+20), а усреднение соседних узлов внутри
    // отсчёта смешивает фазы с разным смещением A эталона (до ±0.15 % A по строкам)
    static const double wsg[5] = { -3.0 / 35, 12.0 / 35, 17.0 / 35, 12.0 / 35, -3.0 / 35 };
    std::vector<double> sm(f.S);
    for (int g = 2 * TL_GRID; g + 2 * TL_GRID < NG; g++) {
        if (f.tau0 + (double)g / TL_GRID <= 15) continue;
        double v = 0;
        for (int k = -2; k <= 2; k++) v += wsg[k + 2] * f.S[g + k * TL_GRID];
        sm[g] = v;
    }
    f.S.swap(sm);
    return f;
}

Tail build_tail(const Singles& s, const Result& tpl)
{
    Tail T;
    const size_t n0 = s.lwins.size();
    T.n_all = (int)n0;
    auto fail = [&](Tail::Err e) { T.err = e; T.ok = false; T.msg = tail_text(T); return T; };
    if (!tpl.ok) return fail(Tail::NoTpl);
    if ((int)n0 < TL_MIN) return fail(Tail::Few);
    // A и фаза — подгонкой эталона по окну −1…+5 от максимума, как в детекторе
    std::vector<Win> wins(n0);
    for (size_t k = 0; k < n0; k++) for (int i = 0; i < W; i++) wins[k][i] = s.lwins[k][LPRE - PRE + i];
    std::vector<double> A; std::vector<int> ph;
    fit(wins, tpl.q, A, ph, nullptr);
    // время импульса: 50 % фронта, линейно между отсчётами, от максимума (отсчёт LPRE окна)
    std::vector<double> cfd(n0, NaN);
    for (size_t k = 0; k < n0; k++) {
        const double a = A[k];
        if (!(a > 0)) continue;
        const LWin& w = s.lwins[k];
        int j = LPRE - 1;
        while (j > 0 && w[j] >= 0.5 * a) j--;
        if (w[j] >= 0.5 * a || w[j + 1] < 0.5 * a) continue;
        cfd[k] = j + (0.5 * a - w[j]) / (w[j + 1] - w[j]) - LPRE;
    }
    // off[фаза] — медиана по фазе (> 5 импульсов), пропуски — линейно по фазе, края — ближайшим
    double off[NPH];
    auto fill_off = [&](const std::vector<int>& known) {
        for (int p = 0; p < NPH; p++) {
            if (std::find(known.begin(), known.end(), p) != known.end()) continue;
            if (p < known.front()) { off[p] = off[known.front()]; continue; }
            if (p > known.back()) { off[p] = off[known.back()]; continue; }
            int a = known.front(), b = known.back();
            for (int q : known) { if (q < p) a = q; if (q > p) { b = q; break; } }
            off[p] = off[a] + (off[b] - off[a]) * (p - a) / (double)(b - a);
        }
    };
    std::vector<int> known;
    for (int p = 0; p < NPH; p++) {
        std::vector<double> v;
        for (size_t k = 0; k < n0; k++) if (ph[k] == p && std::isfinite(cfd[k])) v.push_back(cfd[k]);
        off[p] = v.size() > 5 ? median(v) : NaN;
        if (std::isfinite(off[p])) known.push_back(p);
    }
    if (known.empty()) return fail(Tail::NoTime);
    fill_off(known);
    std::vector<double> tsp(n0, 0.0);
    std::vector<size_t> use;
    for (size_t k = 0; k < n0; k++) if (std::isfinite(cfd[k])) { tsp[k] = off[ph[k]]; use.push_back(k); }
    // чистка: остаток после A·S на отсчётах ≥ +6 не больше 6σ, иначе в окне неотловленный импульс
    const double cut = 6.0 * (s.sigma > 0 ? s.sigma : 4.0);
    std::vector<size_t> keep = use;
    TailShape f;
    auto clean = [&]() {
        keep = use;
        for (int it = 0; it < 3; it++) {
            f = tail_shape(s.lwins, A, tsp, keep);
            keep.clear();
            for (size_t k : use) {
                double mx = 0;
                for (int i = LPRE + 6; i < LW; i++) mx = std::max(mx, fabs(s.lwins[k][i] - A[k] * f.at((i - LPRE) - tsp[k])));
                if (mx <= cut) keep.push_back(k);
            }
        }
    };
    clean();
    // уточнение off[фаза] по общей форме: 50 % фронта по двум отсчётам смещено на 0.02…0.07 отсчёта и по-разному
    // у разных фаз (фронт изогнут), отчего S размывается. Сдвиг δ фазы — минимум Σ (m_p(n) − S(n − δ))² на
    // n = −3…+30, где m_p — МНК-среднее w/A одиночных этой фазы; затем S заново. Начало отсчёта времени S
    // (в среднем 50 % фронта) не меняется.
    for (int it = 0; it < 2; it++) {
        std::vector<int> kn;
        for (int p = 0; p < NPH; p++) {
            double m[34] = {}, aa = 0; int cnt = 0;
            for (size_t k : keep) {
                if (ph[k] != p) continue;
                for (int n = -3; n <= 30; n++) m[n + 3] += s.lwins[k][LPRE + n] * A[k];
                aa += A[k] * A[k]; cnt++;
            }
            if (cnt < 6) continue;
            for (double& v : m) v /= aa;
            double best = 1e300, bd = off[p];
            for (int q = -150; q <= 150; q++) {
                double d = off[p] + q * 0.002, r = 0;
                for (int n = -3; n <= 30; n++) { double e = m[n + 3] - f.at(n - d); r += e * e; }
                if (r < best) { best = r; bd = d; }
            }
            off[p] = bd; kn.push_back(p);
        }
        if (kn.empty()) break;
        fill_off(kn);
        for (size_t k : use) tsp[k] = off[ph[k]];
        clean();
    }
    if ((int)keep.size() < TL_MIN) {
        T.n = (int)keep.size();
        return fail(Tail::Cleaned);
    }
    f = tail_shape(s.lwins, A, tsp, keep);
    T.smax = *std::max_element(f.S.begin(), f.S.end());
    if (!(T.smax > 0.8 && T.smax < 1.2)) return fail(Tail::Shape);
    // строки таблицы: фазы сгруппированы 16 равными шагами по диапазону off; время строки — среднее off её
    // импульсов (вес A², как у МНК), а не середина шага: фазы ложатся в шаг неравномерно (у эталона с
    // реального тракта бывают и пропуски), и середина шага дала бы лишнее смещение
    double lo = off[0], hi = off[0];
    for (int p = 1; p < NPH; p++) { lo = std::min(lo, off[p]); hi = std::max(hi, off[p]); }
    const double step = (hi - lo) / TL_ROWS;
    for (int p = 0; p < NPH; p++) {
        int r = step > 0 ? (int)floor((off[p] - lo) / step) : 0;
        T.row[p] = (uint8_t)std::max(0, std::min(TL_ROWS - 1, r));
    }
    double tw[TL_ROWS] = {}, ww[TL_ROWS] = {};
    for (size_t k : keep) { int r = T.row[ph[k]]; tw[r] += A[k] * A[k] * off[ph[k]]; ww[r] += A[k] * A[k]; }
    for (int r = 0; r < TL_ROWS; r++) T.t_row[r] = ww[r] > 0 ? tw[r] / ww[r] : lo + (r + 0.5) * step;
    // масштаб строки: у фаз эталона A смещена по-разному (до ±0.3 %), и хвост в единицах A подгонки у строки
    // свой — κ_r = МНК по её одиночным (j = 1…64), в пределах ±3 %
    double ky[TL_ROWS] = {}, kk[TL_ROWS] = {};
    for (size_t k : keep) {
        const int r = T.row[ph[k]];
        for (int j = 1; j <= TL_LEN; j++) {
            double h = A[k] * f.at(j - T.t_row[r]);
            ky[r] += s.lwins[k][LPRE + j] * h; kk[r] += h * h;
        }
    }
    for (int r = 0; r < TL_ROWS; r++) {
        const double o = T.t_row[r];
        const double kap = kk[r] > 0 ? std::max(0.97, std::min(1.03, ky[r] / kk[r])) : 1.0;
        T.k_row[r] = kap;
        for (int j = 1; j <= TL_LEN; j++) {
            double v = std::nearbyint(32768.0 * kap * f.at(j - o));
            T.s[r][j - 1] = (int16_t)std::max(-32768.0, std::min(32767.0, v));
        }
    }
    // остаток одиночных после вычета хвоста по таблице: систематика — МНК-среднее остатка/A по (строке, j),
    // rms по всем (строка, j) с весом числа импульсов строки; шум — rms остатка в кодах
    std::vector<double> m(TL_ROWS * TL_LEN), aa(TL_ROWS);
    std::vector<int> cnt(TL_ROWS);
    double e2 = 0; size_t ne = 0;
    auto resid = [&]() {
        std::fill(m.begin(), m.end(), 0.0); std::fill(aa.begin(), aa.end(), 0.0); std::fill(cnt.begin(), cnt.end(), 0);
        e2 = 0; ne = 0;
        for (size_t k : keep) {
            const int r = T.row[ph[k]];
            for (int j = 1; j <= TL_LEN; j++) {
                double e = s.lwins[k][LPRE + j] - A[k] * T.s[r][j - 1] / 32768.0;
                m[r * TL_LEN + j - 1] += e * A[k]; e2 += e * e; ne++;
            }
            aa[r] += A[k] * A[k]; cnt[r]++;
        }
    };
    resid();
    // у вершины (j = 1…4) форма по фазам эталона различается сильнее, чем даёт общая S со сдвигом и масштабом
    // (на cs_only у крайних строк до −0.6 % A на j = 1): там строка берёт поправку по своим импульсам
    for (int r = 0; r < TL_ROWS; r++) {
        if (cnt[r] < 100) continue;
        for (int j = 1; j <= TL_NEAR; j++) {
            double v = T.s[r][j - 1] + std::nearbyint(32768.0 * m[r * TL_LEN + j - 1] / aa[r]);
            T.s[r][j - 1] = (int16_t)std::max(-32768.0, std::min(32767.0, v));
        }
    }
    resid();
    double ms = 0, ws = 0;
    for (int r = 0; r < TL_ROWS; r++) {
        if (!cnt[r]) continue;
        for (int j = 0; j < TL_LEN; j++) {
            double v = m[r * TL_LEN + j] / aa[r];
            ms += cnt[r] * v * v;
            if (cnt[r] >= 20) T.max_pct = std::max(T.max_pct, 100.0 * fabs(v));
        }
        ws += (double)cnt[r] * TL_LEN;
    }
    T.rms_pct = ws > 0 ? 100.0 * sqrt(ms / ws) : 0;
    T.noise_rms = ne ? sqrt(e2 / ne) : 0;
    T.off_lo = lo; T.off_hi = hi;
    T.n = (int)keep.size();
    T.ok = true;
    int smin = 0;
    for (int j = 0; j < TL_LEN; j++) { int v = T.s[TL_ROWS / 2][j]; if (v < smin) smin = v; if (!T.jneg && v < 0) T.jneg = j + 1; }
    T.smin_pct = 100.0 * smin / 32768.0;
    T.msg = tail_text(T);
    return T;
}

std::vector<std::string> tail_commands(const Tail& t)
{
    std::vector<std::string> v;
    char b[256];
    for (int r = 0; r < TL_ROWS; r++) for (int c = 0; c < TL_LEN / 16; c++) {
        int n = snprintf(b, sizeof b, "-tlw %d %d", r, c);
        for (int i = 0; i < 16; i++) n += snprintf(b + n, sizeof b - n, " %d", (int)t.s[r][16 * c + i]);
        v.push_back(b);
    }
    for (int c = 0; c < NPH / 16; c++) {
        int n = snprintf(b, sizeof b, "-tlm %d", c);
        for (int i = 0; i < 16; i++) n += snprintf(b + n, sizeof b - n, " %d", (int)t.row[16 * c + i]);
        v.push_back(b);
    }
    v.push_back("-tlok");
    return v;
}

bool save_tail_bin(const Tail& t, const std::string& path)
{
    unsigned char buf[TL_ROWS * TL_LEN * 2 + NPH];
    size_t n = 0;
    for (int r = 0; r < TL_ROWS; r++) for (int j = 0; j < TL_LEN; j++) {
        uint16_t u = (uint16_t)t.s[r][j];
        buf[n++] = (unsigned char)(u & 0xFF); buf[n++] = (unsigned char)(u >> 8);
    }
    for (int p = 0; p < NPH; p++) buf[n++] = t.row[p];
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(buf, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    return ok;
}

std::string tail_text(const Tail& t)
{
    char b[512];
    switch (t.err) {
    case Tail::None:
        snprintf(b, sizeof b, T("tail.ok"), t.n, t.n_all, t.off_lo, t.off_hi, t.rms_pct, t.max_pct, t.noise_rms, t.jneg, t.smin_pct);
        break;
    case Tail::NoTpl:   snprintf(b, sizeof b, "%s", T("tail.no_tpl")); break;
    case Tail::Few:     snprintf(b, sizeof b, T("tail.few"), LPOST, t.n_all, TL_MIN); break;
    case Tail::NoTime:  snprintf(b, sizeof b, "%s", T("tail.no_time")); break;
    case Tail::Cleaned: snprintf(b, sizeof b, T("tail.cleaned"), t.n, TL_MIN); break;
    case Tail::Shape:   snprintf(b, sizeof b, T("tail.shape"), t.smax); break;
    default:            b[0] = 0; break;
    }
    return b;
}

std::vector<std::string> TplLoad::start(const Result& r, const Tail& t, bool save)
{
    *this = TplLoad();
    save_ = save;
    if (t.ok) tail_cmds_ = tail_commands(t);
    std::vector<std::string> v = commands(r);
    v.push_back("-tplok");
    stage = Tpl;
    return v;
}

std::vector<std::string> TplLoad::after_tail()
{
    if (save_) { save_sent = true; stage = Save; return { "-save" }; }
    stage = Done;
    return {};
}

void TplLoad::abort(const std::string& err)
{
    if (!busy()) return;
    tpl_ok = tail_ok = save_ok = false;
    tpl_err = err;
    stage = Done;
}

std::vector<std::string> TplLoad::next(int errors, const std::string& last_err)
{
    switch (stage) {
    case Tpl:
        if (errors) { tpl_err = last_err; stage = Done; return {}; }     // эталон не принят: ни хвоста, ни -save
        tpl_ok = true;
        if (!tail_cmds_.empty()) { tail_sent = true; stage = TailSt; return tail_cmds_; }
        return after_tail();
    case TailSt:
        tail_ok = errors == 0;
        if (errors) tail_err = last_err;
        return after_tail();
    case Save:
        save_ok = errors == 0;
        if (errors) save_err = last_err;
        stage = Done;
        return {};
    default:
        return {};
    }
}

} // namespace tpl

namespace tpl {

size_t append_blocks(FILE* f, std::vector<std::vector<uint16_t>>& blocks, bool& ok)
{
    ok = f != nullptr;
    size_t n = 0;
    for (auto& b : blocks) {
        if (!ok) break;
        if (b.size() != (size_t)BLK) continue;
        if (fwrite(b.data(), 2, b.size(), f) != b.size()) { ok = false; break; }
        n++;
    }
    if (ok && fflush(f) != 0) ok = false;
    blocks.clear();
    return n;
}

} // namespace tpl
