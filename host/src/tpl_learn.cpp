// tpl_learn.cpp — см. tpl_learn.h. Алгоритм один в один с firmware/gamma_fw/tools/tpl_pmt.py:
// find_singles → build_rows (ранг признака f, затем refine × ранг времени τ по всей форме) →
// rows_from_shape → поправка масштаба строк по фотопику (pp_scale × pp_iters) → Q14.
#include "tpl_learn.h"
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
    // общая форма для экрана (каждый 4-й узел: шаг 1/16 отсчёта)
    for (size_t g = 0; g < f.S.size(); g += 4) { R.shape_x.push_back((float)f.x[g]); R.shape_y.push_back((float)f.S[g]); }

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

} // namespace tpl
