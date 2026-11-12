// devstate.cpp — см. devstate.h
#include "devstate.h"
#include <cstdlib>
#include <cerrno>
#include <vector>

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

} // namespace ds
