// logdump.cpp — см. logdump.h
#include "logdump.h"
#include "peakfit.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace logdump {

bool parse(const std::vector<uint8_t>& raw, uint32_t sector, Record& r)
{
    r = Record();
    r.sector = sector;
    r.raw = raw;
    if (raw.size() < sizeof(logrec_hdr_t)) { r.err = "короткая запись"; return false; }
    memcpy(&r.h, raw.data(), sizeof r.h);
    if (!logrec_hdr_valid(&r.h)) { r.err = "заголовок испорчен"; return false; }
    if (raw.size() < LOGREC_HDR_PAGE + (size_t)r.h.payload_len) { r.err = "нагрузка не дошла"; return false; }
    const uint8_t* pl = raw.data() + LOGREC_HDR_PAGE;
    if (logrec_crc32(0, pl, r.h.payload_len) != r.h.payload_crc) { r.err = "CRC нагрузки"; return false; }
    r.spec.assign(LOGREC_CHANNELS, 0);
    if (!logrec_decode(pl, r.h.payload_len, r.spec.data(), LOGREC_CHANNELS)) { r.err = "нагрузка не разбирается"; return false; }
    r.ok = true;
    return true;
}

std::vector<uint32_t> sessions(const std::vector<Record>& all)
{
    std::vector<uint32_t> s;
    for (auto& r : all) if (r.ok && std::find(s.begin(), s.end(), r.h.session) == s.end()) s.push_back(r.h.session);
    std::sort(s.begin(), s.end());
    return s;
}

std::vector<const Record*> session(const std::vector<Record>& all, uint32_t sess)
{
    std::vector<const Record*> v;
    for (auto& r : all) if (r.ok && r.h.session == sess) v.push_back(&r);
    std::sort(v.begin(), v.end(), [](const Record* a, const Record* b) { return a->h.seq < b->h.seq; });
    return v;
}

std::vector<uint32_t> interval(const std::vector<const Record*>& s, size_t i)
{
    std::vector<uint32_t> d(LOGREC_CHANNELS, 0);
    if (i >= s.size()) return d;
    const auto& cur = s[i]->spec;
    if (i == 0 || (s[i]->h.flags & LOGF_HOST_RST)) return cur;
    const auto& prev = s[i - 1]->spec;
    for (uint32_t k = 0; k < LOGREC_CHANNELS; k++) d[k] = cur[k] >= prev[k] ? cur[k] - prev[k] : 0u;
    return d;
}

double temp_c(int16_t c100) { return c100 == (int16_t)LOGREC_T_NONE ? NAN : c100 / 100.0; }

std::vector<PeakPt> peaks(const std::vector<const Record*>& s, int lo, int hi)
{
    std::vector<PeakPt> out;
    for (size_t i = 1; i < s.size(); i++) {
        const auto& h = s[i]->h;
        PeakPt p;
        p.seq = h.seq; p.rec = h.rec_in_session; p.flags = h.flags;
        p.t_mid_s = (h.t_start_ms + h.t_end_ms) / 2000.0;
        p.temp = temp_c(h.t_avg_c100);
        double tmn = temp_c(h.t_min_c100), tmx = temp_c(h.t_max_c100);
        p.temp_span = (std::isfinite(tmn) && std::isfinite(tmx)) ? tmx - tmn : NAN;
        uint64_t lp = (h.flags & LOGF_HOST_RST) ? 0 : s[i - 1]->h.live_ms;
        p.live_s = h.live_ms >= lp ? (h.live_ms - lp) / 1000.0 : 0.0;
        auto d = interval(s, i);
        auto f = peakfit::fit(d.data(), (int)d.size(), lo, hi);
        if (f.ok) {
            p.ok = true; p.peak = f.peak; p.sigma = f.sigma; p.fwhm_pct = f.fwhm_pct;
            int a = std::max(0, (int)(f.peak - 3 * f.sigma)), b = std::min((int)d.size(), (int)(f.peak + 3 * f.sigma) + 1);
            for (int k = a; k < b; k++) p.n += d[k];
        }
        out.push_back(p);
    }
    return out;
}

bool aligned_sum(const std::vector<const Record*>& s, int lo, int hi, std::vector<double>& out, std::string& msg)
{
    out.assign(LOGREC_CHANNELS, 0.0);
    auto pk = peaks(s, lo, hi);
    double wsum = 0, ref = 0;
    for (auto& p : pk) if (p.ok && p.n > 0) { ref += p.peak * p.n; wsum += (double)p.n; }
    if (wsum <= 0) { msg = "в окне нет пика ни в одном интервале"; return false; }
    ref /= wsum;
    int used = 0, raw_only = 0;
    for (size_t i = 1; i < s.size(); i++) {
        auto d = interval(s, i);
        const PeakPt& p = pk[i - 1];
        double k = (p.ok && p.peak > 0) ? ref / p.peak : 1.0;
        if (!(p.ok && p.peak > 0)) raw_only++;
        for (uint32_t j = 0; j < LOGREC_CHANNELS; j++) {
            if (!d[j]) continue;
            double x = (j + 0.5) * k - 0.5;          // центр канала в новой шкале
            int x0 = (int)std::floor(x);
            double fr = x - x0;
            if (x0 >= 0 && x0 < (int)LOGREC_CHANNELS) out[x0] += d[j] * (1.0 - fr);
            if (x0 + 1 >= 0 && x0 + 1 < (int)LOGREC_CHANNELS) out[x0 + 1] += d[j] * fr;
        }
        used++;
    }
    char b[160];
    snprintf(b, sizeof b, "сложено интервалов %d (без поправки %d), опорный пик %.2f", used, raw_only, ref);
    msg = b;
    return true;
}

static FILE* uopen(const fs::path& p, const wchar_t* mode) { return _wfopen(p.wstring().c_str(), mode); }

static bool write_spec_csv(const fs::path& p, const uint32_t* s)
{
    FILE* f = uopen(p, L"w");
    if (!f) return false;
    fprintf(f, "channel,count\n");
    for (uint32_t i = 0; i < LOGREC_CHANNELS; i++) fprintf(f, "%u,%u\n", i, s[i]);
    fclose(f);
    return true;
}

static std::string tstr(int16_t c100)
{
    double t = temp_c(c100);
    if (!std::isfinite(t)) return "";
    char b[16]; snprintf(b, sizeof b, "%.2f", t); return b;
}

bool write_dir(const std::string& dir, const std::vector<Record>& recs, int lo, int hi, std::string& err)
{
    fs::path root = fs::u8path(dir);
    std::error_code ec;
    fs::create_directories(root / "raw", ec);
    fs::create_directories(root / "spec", ec);
    if (ec) { err = "не создать папку: " + ec.message(); return false; }

    for (auto& r : recs) {
        char n[48]; snprintf(n, sizeof n, "%08u_sec%04u.bin", r.ok ? r.h.seq : 0u, r.sector);
        FILE* f = uopen(root / "raw" / n, L"wb");
        if (f) { fwrite(r.raw.data(), 1, r.raw.size(), f); fclose(f); }
    }

    FILE* f = uopen(root / "records.csv", L"w");
    if (!f) { err = "не открыть records.csv"; return false; }
    fprintf(f, "session,rec,seq,sector,t_start_s,t_end_s,unix_start,t_start_c,t_end_c,t_avg_c,t_min_c,t_max_c,"
               "hv_set_v,hv_avg_v,cps_in,cps_acc,real_s,live_s,total,base,sigma,rej_shape,rej_width,rej_pile,rej_budget,"
               "overrun,lost,tc_t0,tc_k1,tc_k2,tc_gain,flags,fw,serial");
    if (lo > 0 && hi > lo) fprintf(f, ",peak_ch,peak_sigma,peak_fwhm_pct,peak_n");
    fprintf(f, "\n");
    for (uint32_t sid : sessions(recs)) {
        auto s = session(recs, sid);
        std::vector<PeakPt> pk;
        if (lo > 0 && hi > lo) pk = peaks(s, lo, hi);
        for (size_t i = 0; i < s.size(); i++) {
            const auto& h = s[i]->h;
            fprintf(f, "%u,%u,%u,%u,%.3f,%.3f,%llu,%s,%s,%s,%s,%s,%u,%.2f,%.1f,%.1f,%.3f,%.3f,%llu,%.2f,%.3f,%u,%u,%u,%u,%u,%u,"
                       "%.3f,%.6g,%.6g,%.6f,0x%04X,%.16s,%.16s",
                    h.session, h.rec_in_session, h.seq, s[i]->sector, h.t_start_ms / 1000.0, h.t_end_ms / 1000.0,
                    (unsigned long long)h.unix_base, tstr(h.t_start_c100).c_str(), tstr(h.t_end_c100).c_str(),
                    tstr(h.t_avg_c100).c_str(), tstr(h.t_min_c100).c_str(), tstr(h.t_max_c100).c_str(),
                    h.hv_set_v, h.hv_avg_v, h.cps_in, h.cps_acc, h.real_ms / 1000.0, h.live_ms / 1000.0,
                    (unsigned long long)h.total, h.base, h.sigma, h.rej_shape, h.rej_width, h.rej_pile, h.rej_budget,
                    h.overrun, h.lost, h.tc_t0, h.tc_k1, h.tc_k2, h.tc_gain, h.flags, h.fw_tag, h.serial);
            if (lo > 0 && hi > lo) {
                if (i > 0 && pk[i - 1].ok) fprintf(f, ",%.3f,%.3f,%.3f,%llu", pk[i - 1].peak, pk[i - 1].sigma, pk[i - 1].fwhm_pct, (unsigned long long)pk[i - 1].n);
                else fprintf(f, ",,,,");
            }
            fprintf(f, "\n");
            auto d = interval(s, i);
            char n[48]; snprintf(n, sizeof n, "s%u_%04u.csv", h.session, h.rec_in_session);
            write_spec_csv(root / "spec" / n, d.data());
        }
        if (!s.empty()) {                    // сумма интервалов (после -rst хоста накопленный начинается заново)
            std::vector<uint64_t> acc(LOGREC_CHANNELS, 0);
            for (size_t i = 0; i < s.size(); i++) { auto d = interval(s, i); for (uint32_t k = 0; k < LOGREC_CHANNELS; k++) acc[k] += d[k]; }
            std::vector<uint32_t> sum(LOGREC_CHANNELS);
            for (uint32_t k = 0; k < LOGREC_CHANNELS; k++) sum[k] = acc[k] > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)acc[k];
            char n[32]; snprintf(n, sizeof n, "sum_s%u.csv", sid);
            write_spec_csv(root / n, sum.data());
        }
    }
    fclose(f);
    int bad = 0;
    for (auto& r : recs) if (!r.ok) bad++;
    if (bad) { char b[64]; snprintf(b, sizeof b, "испорченных записей: %d (сырые — в raw/)", bad); err = b; }
    return true;
}

} // namespace logdump
