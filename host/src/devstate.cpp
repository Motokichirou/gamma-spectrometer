// devstate.cpp — см. devstate.h
#include "devstate.h"
#include <cstdlib>
#include <cerrno>
#include <vector>
#include <cstring>
#include <cstdio>
#include <ctime>

namespace ds {

// целое без знака: строка целиком, без пустоты и переполнения
static bool to_u32(const std::string& s, uint32_t& v)
{
    if (s.empty() || s[0] < '0' || s[0] > '9') return false;
    errno = 0;
    char* e = nullptr;
    unsigned long long x = strtoull(s.c_str(), &e, 10);
    if (errno || *e || x > 0xFFFFFFFFull) return false;
    v = (uint32_t)x;
    return true;
}

static bool to_dbl(const std::string& s, double& v)
{
    if (s.empty()) return false;
    char* e = nullptr;
    v = strtod(s.c_str(), &e);
    return e != s.c_str() && !*e;
}

static bool to_bool(const std::string& s, bool& v)
{
    if (s == "0") { v = false; return true; }
    if (s == "1") { v = true;  return true; }
    return false;
}

static std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t i = 0;
    for (;;) {
        size_t j = s.find(sep, i);
        out.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return out;
}

bool parse_setup(const std::string& reply, SetupRec& out)
{
    static const char pre[] = "-ok setup ";
    if (reply.compare(0, sizeof(pre) - 1, pre) != 0) return false;

    SetupRec r;
    enum { K_TPL = 1, K_CAL = 2, K_CHK = 4, K_ZERO = 8, K_HV = 16, K_TPLU = 32, K_RES = 64, K_DONE = 128, K_ALL = 255 };
    unsigned seen = 0;

    size_t i = sizeof(pre) - 1;
    while (i < reply.size()) {
        while (i < reply.size() && (reply[i] == ' ' || reply[i] == '\r' || reply[i] == '\n')) i++;
        size_t j = i;
        while (j < reply.size() && reply[j] != ' ' && reply[j] != '\r' && reply[j] != '\n') j++;
        if (j == i) break;
        std::string tok = reply.substr(i, j - i);
        i = j;
        size_t eq = tok.find('=');
        if (eq == std::string::npos) return false;
        std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);   // ключ целиком: tpl ≠ tplu
        bool ok = true;
        if (k == "tpl")       { ok = to_bool(v, r.tpl); seen |= K_TPL; }
        else if (k == "cal")  { ok = to_bool(v, r.cal); seen |= K_CAL; }
        else if (k == "zero") { ok = to_u32(v, r.zero_unix); seen |= K_ZERO; }
        else if (k == "tplu") { ok = to_u32(v, r.tpl_unix); seen |= K_TPLU; }
        else if (k == "done") { ok = to_u32(v, r.done_unix); seen |= K_DONE; }
        else if (k == "chk") {
            auto p = split(v, '/');
            ok = p.size() == 2 && to_u32(p[0], r.chk_unix) && to_bool(p[1], r.chk_ok);
            seen |= K_CHK;
        } else if (k == "hv") {
            auto p = split(v, '/');
            ok = p.size() == 3 && to_u32(p[0], r.hv_unix) && to_dbl(p[1], r.hv_v) && to_dbl(p[2], r.hv_peak_ch);
            seen |= K_HV;
        } else if (k == "res") {
            auto p = split(v, '/');
            ok = p.size() == 3 && to_u32(p[0], r.res_unix) && to_dbl(p[1], r.res_fwhm) && to_dbl(p[2], r.res_peak_ch);
            seen |= K_RES;
        }
        // незнакомые ключи (будущие прошивки) пропускаем
        if (!ok) return false;
    }
    if (seen != K_ALL) return false;
    out = r;
    return true;
}

Home home_view(const Snapshot& s)
{
    if (!s.connected) return Home::NotConnected;
    if (s.known == SetupKnown::Yes && s.s.tpl && s.s.cal && s.s.done_unix) return Home::Ready;
    return Home::NotConfigured;
}

bool step_done(const Snapshot& s, Step st)
{
    const SetupRec& r = s.s;
    switch (st) {
    case Step::Flash: return s.fw_ok && s.known == SetupKnown::Yes;
    case Step::Check: return r.chk_unix && r.chk_ok;
    case Step::Zero:  return r.zero_unix != 0;
    case Step::Hv:    return r.hv_unix != 0;
    case Step::Tpl:   return r.tpl;
    case Step::Hv2:   return r.tpl && r.hv_unix > r.tpl_unix;
    case Step::Res:   return r.res_unix != 0;
    case Step::Scale: return r.cal;
    case Step::Save:  return r.done_unix && s.saved == 1;
    default:          return false;
    }
}

Step first_open_step(const Snapshot& s)
{
    for (int i = 0; i < (int)Step::N; i++)
        if (!step_done(s, (Step)i)) return (Step)i;
    return Step::Save;
}

int step_prereq(const Snapshot& s, Step st)
{
    switch (st) {
    case Step::Hv2:   return step_done(s, Step::Tpl) ? -1 : (int)Step::Tpl;
    case Step::Scale: return step_done(s, Step::Res) ? -1 : (int)Step::Res;
    case Step::Tpl:
    case Step::Res:   // настроенный прибор (в т.ч. перенесённый с v1, где дат подбора ВВ нет) — уставка ВВ уже есть
        return (step_done(s, Step::Hv) || s.s.done_unix) ? -1 : (int)Step::Hv;
    default:          return -1;
    }
}

TopBar top_bar(const Snapshot& s, Mode m)
{
    TopBar b;
    b.modes_enabled = s.connected;
    b.wiz_on = m == Mode::Wizard;
    b.adv_on = m == Mode::Advanced;
    b.hv_chip_on = s.connected && s.hv_on;
    b.kill_visible = s.connected && s.hv_on;      // в любом режиме, пока ВВ включено
    return b;
}

int steps_done_count(const Snapshot& s)
{
    int n = 0;
    for (int i = 0; i < (int)Step::N; i++) if (step_done(s, (Step)i)) n++;
    return n;
}

bool step_needs_cs(Step st)
{
    switch (st) {
    case Step::Hv: case Step::Tpl: case Step::Hv2: case Step::Res: case Step::Scale: return true;
    default: return false;
    }
}

static bool hex_u32(const std::string& s, uint32_t& v)
{
    if (s.empty() || s.size() > 8) return false;
    v = 0;
    for (char c : s) {
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = (v << 4) | (uint32_t)d;
    }
    return true;
}

bool parse_becq_cal(const std::string& reply, double c[5])
{
    std::vector<std::string> lines;
    std::string acc;
    for (char ch : reply) {
        if (ch == '\r' || ch == '\n') { if (!acc.empty()) lines.push_back(acc); acc.clear(); }
        else acc.push_back(ch);
    }
    if (!acc.empty()) lines.push_back(acc);
    if (lines.size() < 30) return false;
    uint32_t w[10];
    for (int i = 0; i < 10; i++) if (!hex_u32(lines[i], w[i])) return false;
    bool any = false;
    double r[5];
    for (int k = 0; k < 5; k++) {
        uint64_t u = ((uint64_t)w[2 * k] << 32) | w[2 * k + 1];
        double d;
        static_assert(sizeof d == sizeof u, "double 64 бит");
        std::memcpy(&d, &u, sizeof d);
        if (!(d == d) || d > 1e300 || d < -1e300) return false;   // NaN/бесконечность — шкалы нет
        if (d != 0) any = true;
        r[k] = d;
    }
    if (!any) return false;
    for (int k = 0; k < 5; k++) c[k] = r[k];
    return true;
}

bool parse_tc(const std::string& reply, double& t0, double& k1, double& k2)
{
    static const char pre[] = "-ok tc ";
    if (reply.compare(0, sizeof(pre) - 1, pre) != 0) return false;
    auto p = split(reply.substr(sizeof(pre) - 1), ' ');
    std::vector<std::string> v;
    for (auto& x : p) {
        std::string y = x;
        while (!y.empty() && (y.back() == '\r' || y.back() == '\n')) y.pop_back();
        if (!y.empty()) v.push_back(y);
    }
    double a, b, c;
    if (v.size() != 3 || !to_dbl(v[0], a) || !to_dbl(v[1], b) || !to_dbl(v[2], c)) return false;
    t0 = a; k1 = b; k2 = c;
    return true;
}

bool parse_log_status(const std::string& reply, LogState& out)
{
    static const char pre[] = "-ok log ";
    if (reply.compare(0, sizeof(pre) - 1, pre) != 0) return false;
    auto p = split(reply.substr(sizeof(pre) - 1), ' ');
    if (p.empty() || (p[0] != "on" && p[0] != "off")) return false;
    LogState r;
    r.enabled = p[0] == "on";
    bool have_rec = false, have_act = false;
    for (size_t i = 1; i + 1 < p.size(); i++) {
        uint32_t v;
        if (p[i] == "records" && to_u32(p[i + 1], v)) { r.records = (long)v; have_rec = true; }
        if (p[i] == "active" && to_u32(p[i + 1], v)) { r.active = v != 0; have_act = true; }
    }
    if (!have_rec || !have_act) return false;
    out = r;
    return true;
}

const char* connect_hint_key(unsigned long win_error)
{
    switch (win_error) {
    case 5:  return "conn.port_busy";    // ERROR_ACCESS_DENIED — порт держит другая программа
    case 2:  return "conn.port_gone";    // ERROR_FILE_NOT_FOUND — порта нет (кабель вынут)
    default: return "conn.failed";
    }
}

std::string display_serial(const std::string& cal_last)
{
    if (cal_last.size() != 8 || cal_last.compare(0, 4, "0474") != 0) return cal_last;
    for (char c : cal_last)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return cal_last;
    return "GS474-" + cal_last.substr(4);
}

std::string setup_cmd(Step s, uint32_t unix, double a, double b)
{
    char c[96];
    unsigned long u = (unsigned long)unix;
    switch (s) {
    case Step::Check: snprintf(c, sizeof c, "-setup chk %lu %d", u, a != 0 ? 1 : 0); break;
    case Step::Zero:  snprintf(c, sizeof c, "-setup zero %lu", u); break;
    case Step::Hv:
    case Step::Hv2:   snprintf(c, sizeof c, "-setup hv %lu %.1f %.1f", u, a, b); break;
    case Step::Tpl:   snprintf(c, sizeof c, "-setup tpl %lu", u); break;
    case Step::Res:   snprintf(c, sizeof c, "-setup res %lu %.2f %.1f", u, a, b); break;
    case Step::Save:  snprintf(c, sizeof c, "-setup done %lu", u); break;
    default:          return std::string();
    }
    return c;
}

std::string report_not_run(uint32_t last_unix)
{
    std::string d;
    if (last_unix == 0) d = "нет данных";
    else if (last_unix < 1000000000u) d = "дата неизвестна";
    else {
        time_t t = (time_t)last_unix;
        struct tm lt;
        char b[32];
        if (localtime_s(&lt, &t) == 0 && strftime(b, sizeof b, "%d.%m.%Y", &lt)) d = b;
        else d = "дата неизвестна";
    }
    return "не проводилось в этот раз (последний раз: " + d + ")";
}

bool baud_allowed(uint32_t baud)
{
    return baud == 600000u || baud == 1000000u || baud == 1500000u || baud == 2000000u || baud == 3000000u;
}

long parse_baud_reply(const std::string& r0)
{
    std::string r = r0;
    while (!r.empty() && (r.back() == '\r' || r.back() == '\n' || r.back() == ' ')) r.pop_back();
    if (r == "-err baud" || r == "-err unknown") return -1;
    if (r.rfind("-ok baud ", 0) != 0) return 0;
    const char* p = r.c_str() + 9;
    char* e;
    unsigned long v = strtoul(p, &e, 10);
    if (e == p || *e || !baud_allowed((uint32_t)v)) return 0;
    return (long)v;
}

int queue_reply_kind(const std::string& t)
{
    auto starts = [&](const char* p) { return t.compare(0, strlen(p), p) == 0; };
    if (starts("-ok gst")) return 0;
    if (starts("-err")) return 2;
    if (starts("-ok")) return 1;
    return 0;
}

} // namespace ds
