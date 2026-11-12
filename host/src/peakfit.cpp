// peakfit.cpp — см. peakfit.h
#include "peakfit.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>

namespace peakfit {

// модель A·exp(−½((x−m)/s)²) + c0 + c1·(x − x0); Левенберг–Марквардт, 5 параметров
static bool lm(const std::vector<double>& x, const std::vector<double>& y, const std::vector<double>& w,
               double x0, double p[5], double cov[5][5])
{
    auto model = [&](double xi, const double* q, double* d) {
        double u = (xi - q[1]) / q[2], e = exp(-0.5 * u * u);
        if (d) { d[0] = e; d[1] = q[0] * e * u / q[2]; d[2] = q[0] * e * u * u / q[2]; d[3] = 1; d[4] = xi - x0; }
        return q[0] * e + q[3] + q[4] * (xi - x0);
    };
    double lam = 1e-3;
    auto chi2 = [&](const double* q) { double s = 0; for (size_t i = 0; i < x.size(); i++) { double r = (y[i] - model(x[i], q, nullptr)) / w[i]; s += r * r; } return s; };
    double c = chi2(p);
    for (int it = 0; it < 200; it++) {
        double J[5], A[5][5] = {}, g[5] = {};
        for (size_t i = 0; i < x.size(); i++) {
            double f = model(x[i], p, J), r = (y[i] - f) / w[i];
            for (int a = 0; a < 5; a++) { g[a] += J[a] * r / w[i]; for (int b = 0; b < 5; b++) A[a][b] += J[a] * J[b] / (w[i] * w[i]); }
        }
        double M[5][6];
        for (int a = 0; a < 5; a++) { for (int b = 0; b < 5; b++) M[a][b] = A[a][b] * (a == b ? 1 + lam : 1); M[a][5] = g[a]; }
        bool sing = false;
        for (int col = 0; col < 5 && !sing; col++) {
            int piv = col;
            for (int r = col + 1; r < 5; r++) if (fabs(M[r][col]) > fabs(M[piv][col])) piv = r;
            for (int k = 0; k < 6; k++) { double t = M[col][k]; M[col][k] = M[piv][k]; M[piv][k] = t; }
            if (fabs(M[col][col]) < 1e-300) { sing = true; break; }
            for (int r = 0; r < 5; r++) if (r != col) { double f = M[r][col] / M[col][col]; for (int k = col; k < 6; k++) M[r][k] -= f * M[col][k]; }
        }
        if (sing) return false;
        double q[5]; for (int a = 0; a < 5; a++) q[a] = p[a] + M[a][5] / M[a][a];
        if (q[2] <= 0) { lam *= 10; continue; }
        double cn = chi2(q);
        if (cn < c) {
            bool small = fabs(c - cn) < 1e-9 * c;
            memcpy(p, q, sizeof q); c = cn; lam *= 0.3;
            if (small) break;
        } else lam *= 10;
        if (lam > 1e12) break;
    }
    // ковариация = (JᵀWJ)⁻¹
    double A[5][5] = {}, J[5];
    for (size_t i = 0; i < x.size(); i++) { model(x[i], p, J); for (int a = 0; a < 5; a++) for (int b = 0; b < 5; b++) A[a][b] += J[a] * J[b] / (w[i] * w[i]); }
    double M[5][10];
    for (int a = 0; a < 5; a++) for (int b = 0; b < 10; b++) M[a][b] = b < 5 ? A[a][b] : (b - 5 == a ? 1 : 0);
    for (int col = 0; col < 5; col++) {
        int piv = col;
        for (int r = col + 1; r < 5; r++) if (fabs(M[r][col]) > fabs(M[piv][col])) piv = r;
        for (int k = 0; k < 10; k++) { double t = M[col][k]; M[col][k] = M[piv][k]; M[piv][k] = t; }
        if (fabs(M[col][col]) < 1e-300) return false;
        double d = M[col][col]; for (int k = 0; k < 10; k++) M[col][k] /= d;
        for (int r = 0; r < 5; r++) if (r != col) { double f = M[r][col]; for (int k = 0; k < 10; k++) M[r][k] -= f * M[col][k]; }
    }
    for (int a = 0; a < 5; a++) for (int b = 0; b < 5; b++) cov[a][b] = M[a][5 + b];
    return true;
}

Result fit(const uint32_t* s, int n, int lo, int hi)
{
    Result R;
    if (lo < 4) lo = 4;
    if (hi > n) hi = n;
    int pk = lo; double bv = -1;
    for (int i = lo; i < hi; i++) {
        double a = 0; int c = 0;
        for (int k = i - 4; k <= i + 4; k++) if (k >= 0 && k < n) { a += s[k]; c++; }
        if (a / c > bv) { bv = a / c; pk = i; }
    }
    if (bv <= 0) return R;
    double m = pk, sg = 0.035 * pk;
    double p[5], cov[5][5];
    for (int it = 0; it < 3; it++) {
        int a = (int)(m - 2.2 * sg), b = (int)(m + 2.2 * sg);
        if (a < 0) a = 0;
        if (b > n) b = n;
        if (b - a < 8) return R;
        std::vector<double> x, y, w;
        double ymax = 0, ymin = 1e300;
        for (int i = a; i < b; i++) { x.push_back(i); y.push_back(s[i]); w.push_back(sqrt(s[i] > 1 ? (double)s[i] : 1.0)); if (s[i] > ymax) ymax = s[i]; if (s[i] < ymin) ymin = s[i]; }
        p[0] = ymax - ymin; p[1] = m; p[2] = sg; p[3] = ymin; p[4] = 0;
        if (!lm(x, y, w, (double)pk, p, cov)) return R;
        m = p[1]; sg = fabs(p[2]);
    }
    R.ok = m > 0 && sg > 0;
    R.peak = m; R.sigma = sg; R.height = p[0];
    R.bg0 = p[3]; R.bg1 = p[4]; R.x0 = pk;
    R.fwhm_pct = 235.48 * sg / m;
    R.fwhm_err = 235.48 * sqrt(fabs(cov[2][2])) / m;
    return R;
}

double Result::model(double x) const
{
    if (sigma <= 0) return bg0 + bg1 * (x - x0);
    double u = (x - peak) / sigma;
    return height * exp(-0.5 * u * u) + bg0 + bg1 * (x - x0);
}

PeakWin peak_window(const uint32_t* s, int n, int lo, int hi, double k)
{
    PeakWin W;
    Result r = fit(s, n, lo, hi);
    if (!r.ok || !std::isfinite(r.peak) || !std::isfinite(r.sigma) || !std::isfinite(r.fwhm_err)) return W;
    double fw = 2.35482 * r.sigma;
    double area = r.height * r.sigma * 2.5066283;
    bool sure = r.peak >= lo + 0.5 * fw && r.peak < hi &&
                r.fwhm_pct >= 1.0 && r.fwhm_pct <= 30.0 &&
                r.height > 0 && area >= 30.0 &&
                r.fwhm_err < 0.5 * r.fwhm_pct;
    if (!sure) return W;
    W.ok = true; W.center = r.peak; W.fwhm = fw; W.fit = r;
    W.lo = (int)floor(r.peak - k * fw + 0.5);
    W.hi = (int)floor(r.peak + k * fw + 0.5) + 1;
    if (W.lo < 0) W.lo = 0;
    if (W.hi > n) W.hi = n;
    return W;
}

uint64_t window_sum(const uint32_t* s, int n, int lo, int hi)
{
    if (lo < 0) lo = 0;
    if (hi > n) hi = n;
    uint64_t t = 0;
    for (int i = lo; i < hi; i++) t += s[i];
    return t;
}

Stop count_stop(uint64_t in_peak, uint64_t target, double elapsed_s, double timeout_s)
{
    if (in_peak >= target) return Stop::Count;
    if (elapsed_s >= timeout_s) return Stop::Timeout;
    return Stop::Run;
}

static uint32_t crc32_zip(const char* s, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { c ^= (uint8_t)s[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return c ^ 0xFFFFFFFFu;
}

void becq_cal_commands(const double c[5], char out[11][40])
{
    uint32_t w[11];
    for (int k = 0; k < 5; k++) {
        uint64_t u; memcpy(&u, &c[k], 8);
        w[2 * k] = (uint32_t)(u >> 32); w[2 * k + 1] = (uint32_t)u;
    }
    char s[81];
    for (int i = 0; i < 10; i++) snprintf(s + 8 * i, 9, "%08X", w[i]);
    w[10] = crc32_zip(s, 80);
    for (int i = 0; i < 11; i++) snprintf(out[i], 40, "-cal %d %08X", i, w[i]);
}

} // namespace peakfit
