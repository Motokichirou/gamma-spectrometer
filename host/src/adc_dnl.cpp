// adc_dnl.cpp — таблица поправки DNL сигнального АЦП по плотности кодов (см. adc_dnl.h).
#include "adc_dnl.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace adc_dnl {

void add_samples(std::vector<uint64_t>& hist, const uint16_t* s, size_t n, uint64_t* over)
{
    if (hist.size() != (size_t)CODES) hist.assign(CODES, 0);
    for (size_t i = 0; i < n; i++) {
        if (s[i] < CODES) hist[s[i]]++;
        else if (over) (*over)++;
    }
}

// медиана H по кодам окна [c − w, c + w] без c, где use[k]; окно расширяется до 2w, если годных < w
static double median_near(const std::vector<uint64_t>& H, const std::vector<char>& use, int c, int w)
{
    std::vector<double> v;
    for (int ww = w; ww <= 2 * w; ww += w) {
        v.clear();
        for (int k = std::max(0, c - ww); k <= std::min(CODES - 1, c + ww); k++)
            if (k != c && use[k]) v.push_back((double)H[k]);
        if ((int)v.size() >= w) break;
    }
    if ((int)v.size() < w) return NAN;
    size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + m, v.end());
    double hi = v[m];
    if (v.size() % 2) return hi;
    double lo = *std::max_element(v.begin(), v.begin() + m);
    return 0.5 * (lo + hi);
}

Result compute(const std::vector<uint64_t>& H, const Params& p)
{
    Result r;
    if (H.size() != (size_t)CODES) { r.msg = "гистограмма не на 4096 кодов"; return r; }
    for (uint64_t x : H) r.samples += x;
    const int W = p.half_win;
    // 0 и 4095 — насыщение: в нормах и таблице не участвуют
    std::vector<char> valid(CODES, 1);
    valid[0] = valid[CODES - 1] = 0;
    std::vector<double> m1(CODES, NAN), N(CODES, NAN);
    for (int c = 0; c < CODES; c++) m1[c] = median_near(H, valid, c, W);
    std::vector<char> flag(CODES, 0), use2(CODES, 0);
    for (int c = 0; c < CODES; c++) {
        if (valid[c] && std::isfinite(m1[c]) && m1[c] >= p.min_norm) {
            double d = std::fabs((double)H[c] - m1[c]);
            flag[c] = d > std::max(p.sig_k * std::sqrt(m1[c]), p.rel_k * m1[c]);
        }
        use2[c] = valid[c] && !flag[c];
    }
    for (int c = 0; c < CODES; c++) N[c] = median_near(H, use2, c, W);
    std::vector<char> ok(CODES, 0);
    for (int c = W; c < CODES - W; c++) {
        if (!(std::isfinite(N[c]) && std::isfinite(N[c - W]) && std::isfinite(N[c + W]) && N[c] >= p.min_norm))
            continue;
        if (std::fabs(N[c - W] + N[c + W] - 2 * N[c]) > p.curv * N[c]) continue;
        if (std::fabs(N[c + W] - N[c - W]) > p.slope * N[c]) continue;
        int pos = 0;
        for (int k = c - W; k <= c + W; k++) pos += flag[k] && (double)H[k] > m1[k];
        ok[c] = pos <= p.max_pos;
    }
    std::vector<double> w(CODES, 1.0), off(CODES, 0.0);
    std::vector<char> good(CODES, 0);
    for (int c = 0; c < CODES; c++) {
        good[c] = flag[c] && ok[c];
        if (good[c]) { w[c] = (double)H[c] / N[c]; r.outliers.push_back(c); }
    }
    // группы выбросов (через ≤ 1 код): центр — по накопленной сумме (w − 1) от начала группы
    for (int c = 0; c < CODES;) {
        if (!good[c]) { c++; continue; }
        int b = c;
        while (b + 1 < CODES && (good[b + 1] || (b + 2 < CODES && good[b + 2]))) b++;
        // дефект АЦП — несколько широких кодов и пропущенные (w < min_width) рядом; много «узких, но
        // встречающихся» или широких подряд — это не DNL, а провал/горб плотности, которого норма не видит
        // (впадина между горбом базы и спектром): группа в таблицу не идёт
        int narrow = 0, wide = 0;
        for (int k = c; k <= b; k++) {
            if (!good[k]) continue;
            if (w[k] > 1.0) wide++;
            else if (w[k] >= p.min_width) narrow++;
        }
        if (narrow > p.max_narrow || wide > p.max_wide) {
            for (int k = c; k <= b; k++) good[k] = 0;
            c = b + 1;
            continue;
        }
        double s = 0;
        for (int k = c; k <= b; k++) {
            if (good[k]) off[k] = s + 0.5 * (w[k] - 1.0);
            s += good[k] ? w[k] - 1.0 : 0.0;
        }
        c = b + 1;
    }
    for (int c = 0; c < CODES; c++) {
        if (!good[c] || H[c] == 0 || w[c] < p.min_width || std::fabs(off[c]) < p.min_off) continue;
        Entry e;
        e.code = c; e.off = off[c]; e.width = w[c]; e.norm = N[c]; e.count = H[c];
        long o = std::lround(off[c] * 16.0);
        e.off16 = (int)std::max(-(long)OFF_MAX, std::min((long)OFF_MAX, o));
        if (e.off16 == 0) continue;
        r.candidates.push_back(e);
    }
    std::stable_sort(r.candidates.begin(), r.candidates.end(),
                     [](const Entry& a, const Entry& b) { return std::fabs(a.off) > std::fabs(b.off); });
    r.table.assign(r.candidates.begin(), r.candidates.begin() + std::min<size_t>(r.candidates.size(), MAX_PAIRS));
    std::sort(r.table.begin(), r.table.end(), [](const Entry& a, const Entry& b) { return a.code < b.code; });
    char m[160];
    snprintf(m, sizeof m, "отсчётов %llu, кодов-выбросов %zu, в таблицу %zu из %zu", (unsigned long long)r.samples,
             r.outliers.size(), r.table.size(), r.candidates.size());
    r.msg = m;
    r.ok = true;
    return r;
}

std::string format_table(const Result& r, const std::string& comment)
{
    std::string s = "# таблица поправки DNL АЦП (gammapult adc dnl): код сдвиг_центра_1/16_LSB\n";
    if (!comment.empty()) {
        std::istringstream is(comment);
        std::string ln;
        while (std::getline(is, ln)) s += "# " + ln + "\n";
    }
    char b[160];
    snprintf(b, sizeof b, "# %s\n", r.msg.c_str());
    s += b;
    for (auto& e : r.table) {
        snprintf(b, sizeof b, "%d %d    # ширина %.2f LSB, сдвиг %+.2f LSB, отсчётов %llu, норма %.1f\n", e.code, e.off16,
                 e.width, e.off, (unsigned long long)e.count, e.norm);
        s += b;
    }
    return s;
}

bool parse_table(const std::string& text, std::vector<std::pair<int, int>>& out, std::string& err)
{
    out.clear();
    std::istringstream is(text);
    std::string ln;
    int no = 0;
    while (std::getline(is, ln)) {
        no++;
        size_t h = ln.find('#');
        if (h != std::string::npos) ln.resize(h);
        std::istringstream ls(ln);
        long c, o;
        if (!(ls >> c)) {
            std::string rest;
            ls.clear();
            if (ls >> rest) { err = "строка " + std::to_string(no) + ": не число"; return false; }
            continue;                                   // пустая
        }
        std::string rest;
        if (!(ls >> o) || (ls >> rest)) { err = "строка " + std::to_string(no) + ": нужно «код сдвиг»"; return false; }
        if (c < 0 || c >= CODES || o == 0 || o < -OFF_MAX || o > OFF_MAX) {
            err = "строка " + std::to_string(no) + ": код 0…4095, сдвиг ±1…1024";
            return false;
        }
        for (auto& q : out)
            if (q.first == c) { err = "строка " + std::to_string(no) + ": код повторяется"; return false; }
        if ((int)out.size() >= MAX_PAIRS) { err = "больше 16 пар"; return false; }
        out.push_back({ (int)c, (int)o });
    }
    return true;
}

std::vector<std::string> load_commands(const std::vector<std::pair<int, int>>& t, bool save)
{
    std::vector<std::string> v{ "-dnl clr" };
    if (!t.empty()) {
        std::string c = "-dnl";
        for (auto& q : t) c += " " + std::to_string(q.first) + " " + std::to_string(q.second);
        v.push_back(c);
    }
    if (save) v.push_back("-save");
    return v;
}

}  // namespace adc_dnl
