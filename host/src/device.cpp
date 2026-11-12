// device.cpp — см. device.h
#include "device.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <cmath>

static const int tpl_blk = 8192;   // порция АЦП (-osc raw)

static double now_sec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static bool starts(const std::string& s, const char* p)
{
    return s.rfind(p, 0) == 0;
}

void Device::log_add(int dir, const std::string& s)
{
    log.push_back({ dir, s });
    while (log.size() > 800) log.pop_front();
}

void Device::refresh_ports()
{
    ports = serialport::enumerate();
}

bool Device::connect(const std::string& port)
{
    disconnect();
    state = State::Connecting;
    status_text = "Подключение…";
    if (!serial_.open(port, 600000)) {
        last_open_error = serial_.last_error();
        state = State::Error;
        status_text = "Не удалось открыть " + port + " (занят BecqMoni?)";
        log_add(3, status_text);
        return false;
    }
    port_name = port;
    last_open_error = 0;
    state = State::Connected;
    status_text = "Связь установлена · простой";
    last_ka_ = last_gst_tx_ = now_sec();
    log_add(0, "Открыт " + port + " @ 600000 8N1");
    send_cmd("-stt");   // идёт ли набор (-ok collecting / -ok) — набор не трогаем
    send_cmd("-inf");   // версия прошивки
    send_cmd("-cal");   // 40 регистров BecqMoni + серийник последней строкой
    send_cmd("-thr");   // порог, коды АЦП
    send_cmd("-gst", true);
    setup_refresh();    // последним: "-err unknown" после него однозначно от -setup (старая прошивка)
    return true;
}

void Device::setup_refresh()
{
    if (!serial_.is_open()) return;
    send_cmd("-setup", true);
    setup_wait_ = true;
    setup_t_ = now_sec();
}

ds::Snapshot Device::snapshot() const
{
    ds::Snapshot s;
    s.connected = connected();
    s.fw_ok = !fw_info.empty();
    s.known = setup_known;
    s.s = setup;
    std::string sv = gstr("saved", "-");
    s.saved = sv == "1" ? 1 : sv == "0" ? 0 : -1;
    s.hv_on = hv_on();
    s.hv_v = std::fabs(gnum("hv_out", 0.0));
    return s;
}

void Device::disconnect()
{
    lin.running = false;
    hvp.running = false;
    raw.running = false; raw_wait_ = false;
    tc.running = false;
    q_.clear(); q_wait_ = false;
    serial_.close();
    state = State::Disconnected;
    status_text = "Нет связи · порт закрыт";
    acquiring = false;
    fw_info.clear(); serial.clear();
    setup = ds::SetupRec(); setup_known = ds::SetupKnown::Unknown; setup_wait_ = false;
    gst.clear(); gst_rx_ = -1.0; thr_codes = -1;
    port_name.clear();
}

void Device::send_cmd(const std::string& cmd, bool quiet)
{
    if (!serial_.is_open()) return;
    auto fr = shproto::build_text(cmd);
    serial_.write(fr.data(), (int)fr.size());
    if (!quiet) log_add(1, cmd);

    if (cmd == "-sta") {
        acquiring = true;
        if (state == State::Connected) state = State::Acquiring;
        status_text = "Связь · набор";
    } else if (cmd == "-sto") {
        acquiring = false;
        if (state == State::Acquiring) state = State::Connected;
        status_text = "Связь установлена · простой";
    }
}

void Device::start() { send_cmd("-sta"); }
void Device::stop()  { send_cmd("-sto"); }

void Device::reset_spectrum()
{
    send_cmd("-rst");
    for (auto& v : spectrum) v = 0;
    elapsed_s = cps = invalid = 0;
    chunk_mask_ = 0;
}

double Device::gst_age() const
{
    return gst_rx_ < 0 ? 1e9 : now_sec() - gst_rx_;
}

double Device::gnum(const char* key, double def) const
{
    auto it = gst.find(key);
    if (it == gst.end()) return def;
    char* e = nullptr;
    double v = strtod(it->second.c_str(), &e);
    return (e && e != it->second.c_str()) ? v : def;
}

std::string Device::gstr(const char* key, const char* def) const
{
    auto it = gst.find(key);
    return it == gst.end() ? std::string(def) : it->second;
}

void Device::poll()
{
    if (!serial_.is_open()) return;

    double now = now_sec();
    if (now - last_ka_ > 1.0) {
        uint8_t ka = shproto::KEEPALIVE;
        serial_.write(&ka, 1);
        last_ka_ = now;
    }
    if (now - last_gst_tx_ > 1.0) {
        send_cmd("-gst", true);
        last_gst_tx_ = now;
    }

    static uint8_t rx[16384];
    int n = serial_.read(rx, sizeof(rx));
    if (n < 0) {
        state = State::Error;
        status_text = "Ошибка связи · порт";
        log_add(3, status_text);
        lin.running = hvp.running = false;
        return;
    }
    if (n > 0)
        parser_.feed(rx, (size_t)n, [this](const shproto::Frame& f) { on_frame(f); });

    if (lin.running) lin_tick(now);
    if (hvp.running) hvp_tick(now);
    if (raw.running) raw_tick(now);
    if (tc.running)  tc_tick(now);
    if (logd.running) logd_tick(now);
    if (setup_wait_ && now - setup_t_ > 2.0) setup_wait_ = false;   // нет ответа — состояние остаётся прежним
    if (q_wait_ && now - q_t_ > 2.0) { q_wait_ = false; queue_errors++; queue_last_err = "нет ответа"; }
    if (!q_wait_ && !q_.empty()) {
        send_cmd(q_.front(), true);
        q_.pop_front(); q_wait_ = true; q_t_ = now;
    }
}

static void parse_kv(const std::string& t, std::map<std::string, std::string>& out)
{
    out.clear();
    size_t i = 0;
    while (i < t.size()) {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\r' || t[i] == '\n')) i++;
        size_t j = i;
        while (j < t.size() && t[j] != ' ' && t[j] != '\r' && t[j] != '\n') j++;
        std::string tok = t.substr(i, j - i);
        size_t eq = tok.find('=');
        if (eq != std::string::npos) out[tok.substr(0, eq)] = tok.substr(eq + 1);
        i = j;
    }
}

void Device::on_text(const std::string& t0)
{
    std::string t = t0;
    while (!t.empty() && (t.back() == '\r' || t.back() == '\n')) t.pop_back();

    if (starts(t, "-ok gst")) {                       // телеметрия — без лога
        parse_kv(t.substr(7), gst);
        gst_rx_ = now_sec();
        return;
    }
    if (starts(t, "-ok setup ")) {                    // запись о настройке
        ds::SetupRec r;
        if (ds::parse_setup(t, r)) {
            setup = r; setup_known = ds::SetupKnown::Yes;
            if (setup_wait_) { setup_wait_ = false; return; }   // наш опрос — без лога
            // иначе -setup послал пользователь (CLI cmd, консоль, очередь) — дальше обычным путём
        }
    }
    if (setup_wait_ && !q_wait_ && starts(t, "-err unknown")) {   // старая прошивка не знает -setup — не ошибка, без лога
        setup_wait_ = false;
        setup_known = ds::SetupKnown::OldFirmware;
        return;
    }
    if (t == "-ok saved") setup_refresh();            // после -save сверить запись в приборе
    if (starts(t, "-log rec ")) {                     // строка списка журнала — без лога
        if (logd.running && logd.phase == 0) {
            unsigned long s = 0, q = 0, ss = 0;
            if (sscanf(t.c_str(), "-log rec %lu %lu %lu", &s, &q, &ss) == 3 &&
                (!logd.only_session || logd.only_session == ss))
                logd.items.push_back({ (uint32_t)s, (uint32_t)q, (uint32_t)ss });
        }
        return;
    }
    if (starts(t, "-ok log list") && t.find("begin") == std::string::npos) { logd_list_done_ = true; return; }
    if (starts(t, "-ok log list begin") || starts(t, "-ok log read begin")) return;
    if (starts(t, "-ok log read")) { logd_rec_done_ = true; return; }
    if (starts(t, "-err log read") || (logd.running && starts(t, "-err log"))) { logd_rec_err_ = true; logd_list_done_ = true; log_add(3, t); return; }
    if (starts(t, "-ok log") || starts(t, "-err log")) log_status = t;
    if (raw_wait_ && (starts(t, "-ok osc raw") || starts(t, "-err"))) {   // порция для эталона — без лога
        raw_wait_ = false;
        if (starts(t, "-ok osc raw") && raw_total_ == tpl_blk && raw_got_ == tpl_blk) raw.blocks.push_back(raw_cur_);
        else { raw.bad++; if (t.find("acq") != std::string::npos) send_cmd("-sto", true); }
        return;
    }
    if (q_wait_ && (starts(t, "-ok") || starts(t, "-err"))) {
        q_wait_ = false;
        if (starts(t, "-err")) { queue_errors++; queue_last_err = t; }
        if (starts(t, "-ok tc")) {
            tc_reply = t;
            if (tc_capture_old_) { old_tc_from(t); tc_capture_old_ = false; }
        }
        if (!starts(t, "-ok") || t.size() > 3) log_add(2, t);   // голые "-ok" от -tplw не засоряют лог
        last_reply = t;
        return;
    }

    // ответ -cal: 40 регистров по строке, последняя непустая — серийник (конвенция BecqMoni)
    int lines = 0;
    for (char ch : t) if (ch == '\n') lines++;
    if (lines >= 30) {
        std::string acc, sn;
        for (char ch : t) {
            if (ch == '\n' || ch == '\r') { if (!acc.empty()) sn = acc; acc.clear(); }
            else acc.push_back(ch);
        }
        if (!acc.empty()) sn = acc;
        serial = sn;
        log_add(2, "калибровка BecqMoni: 40 регистров, серийник " + sn);
        return;
    }

    log_add(2, t);
    last_reply = t;
    if (starts(t, "GammaSpec")) {
        fw_info = t;
    } else if (starts(t, "-ok thr")) {
        thr_codes = atoi(t.c_str() + 7);
    } else if (t == "-ok collecting") {
        acquiring = true;
        if (state == State::Connected) state = State::Acquiring;
        status_text = "Связь · набор";
    }

    if (lin.running) {
        if (starts(t, "-ok dac done")) {
            if (lin.phase == 1) { lin.phase = 2; lin_seq_ = full_seq_; chunk_mask_ = 0; lin.t0 = now_sec(); }
        } else if (starts(t, "-err dac") || starts(t, "-err acq")) {
            lin.msg = "прибор отказал: " + t;
            lin_abort();
        }
    }
    if (hvp.running && (starts(t, "-err hv") || starts(t, "-err vref"))) {
        hvp.msg = "прибор отказал: " + t;
        hvp_abort();
    }
}

void Device::on_frame(const shproto::Frame& f)
{
    if (f.cmd == shproto::CMD_TEXT) {
        on_text(std::string((const char*)f.payload.data(), f.payload.size()));
    } else if (f.cmd == shproto::CMD_PULSE) {
        const auto& p = f.payload;
        if (raw_wait_ && p.size() >= 4 && p.size() % 2 == 0) {
            int idx = p[0] | (p[1] << 8), tot = p[2] | (p[3] << 8), n = (int)(p.size() - 4) / 2;
            raw_total_ = tot;
            for (int i = 0; i < n && idx + i < (int)raw_cur_.size(); i++)
                raw_cur_[idx + i] = (uint16_t)(p[4 + 2 * i] | (p[5 + 2 * i] << 8));
            raw_got_ += n;
        }
    } else if (f.cmd == 0x05) {                       // кусок записи журнала регистратора
        const auto& p = f.payload;
        if (logd.running && logd.phase == 1 && p.size() >= 10) {
            uint32_t off = p[2] | (p[3] << 8) | (p[4] << 16) | ((uint32_t)p[5] << 24);
            uint32_t tot = p[6] | (p[7] << 8) | (p[8] << 16) | ((uint32_t)p[9] << 24);
            if (tot <= LOGREC_HDR_PAGE + LOGREC_PAYLOAD_MAX) {
                if (logd_buf_.size() != tot) logd_buf_.assign(tot, 0xFF);
                logd_total_ = tot;
                size_t n = p.size() - 10;
                if (off + n <= tot) { memcpy(logd_buf_.data() + off, p.data() + 10, n); logd_got_ += (uint32_t)n; }
            }
        }
    } else if (f.cmd == shproto::CMD_SPECTRUM) {
        const auto& p = f.payload;
        if (p.size() >= 2) {
            uint32_t off = (uint32_t)(p[0] | (p[1] << 8));
            size_t nch = (p.size() - 2) / 4;
            for (size_t i = 0; i < nch; i++) {
                uint32_t v = (uint32_t)(p[2 + 4 * i] | (p[3 + 4 * i] << 8) |
                                        (p[4 + 4 * i] << 16) | (p[5 + 4 * i] << 24));
                v &= 0x7FFFFFF;
                if (off + i < (uint32_t)CHANNELS) spectrum[off + i] = v;
            }
            uint32_t ci = off / 512u;
            if (ci < (uint32_t)CHUNKS) chunk_mask_ |= 1u << ci;
        }
    } else if (f.cmd == shproto::CMD_STATUS) {
        const auto& p = f.payload;
        if (p.size() >= 14) {
            elapsed_s = (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) & 0x7FFFFFF;
            cps       = (uint32_t)(p[6] | (p[7] << 8) | (p[8] << 16) | (p[9] << 24));
            invalid   = (uint32_t)(p[10] | (p[11] << 8) | (p[12] << 16) | (p[13] << 24));
        }
        if (chunk_mask_ == (1u << CHUNKS) - 1u) full_seq_++;   // весь спектр снят после прошлого статуса
        chunk_mask_ = 0;
    }
}

uint64_t Device::total() const
{
    uint64_t s = 0;
    for (int i = 0; i < CHANNELS; i++) s += spectrum[i];
    return s;
}

int Device::peak_channel(int lo) const
{
    int pk = lo; uint32_t v = 0;
    for (int i = lo; i < CHANNELS; i++) if (spectrum[i] > v) { v = spectrum[i]; pk = i; }
    return pk;
}

bool Device::peak_stats(const uint32_t* sp, int lo, int hi, double& m, double& sd, uint64_t& n) const
{
    if (lo < 0) lo = 0;
    if (hi > CHANNELS) hi = CHANNELS;
    if (hi - lo < 3) return false;
    // максимум по сглаженному (9 каналов) — устойчив к одиночным выбросам
    int best = lo; double bv = -1;
    for (int i = lo; i < hi; i++) {
        double s = 0; int c = 0;
        for (int k = i - 4; k <= i + 4; k++) if (k >= lo && k < hi) { s += sp[k]; c++; }
        if (s / c > bv) { bv = s / c; best = i; }
    }
    if (bv <= 0) return false;
    m = best; sd = 0.03 * (best > 50 ? best : 50); n = 0;
    for (int it = 0; it < 6; it++) {
        int w = (int)(4 * sd + 0.5); if (w < 5) w = 5;
        int a = (int)m - w, b = (int)m + w + 1;
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        double s0 = 0, s1 = 0;
        for (int i = a; i < b; i++) { s0 += sp[i]; s1 += (double)i * sp[i]; }
        if (s0 <= 0) return false;
        m = s1 / s0;
        double s2 = 0;
        for (int i = a; i < b; i++) s2 += (i - m) * (i - m) * sp[i];
        sd = sqrt(s2 / s0); if (sd < 0.3) sd = 0.3;
        n = (uint64_t)s0;
    }
    return true;
}

const char* Device::state_str() const
{
    switch (state) {
    case State::Disconnected: return "Отключено";
    case State::Connecting:   return "Подключение";
    case State::Connected:    return "Подключено · простой";
    case State::Acquiring:    return "Идёт набор";
    case State::Error:        return "Ошибка связи";
    }
    return "?";
}

// ================= самотест линейности по ЦАП =================

void Device::lin_start(const std::vector<int>& codes, int n, int period)
{
    if (!connected() || codes.empty()) return;
    hvp_abort();
    lin = LinRun{};
    lin.codes = codes; lin.n = n; lin.period = period;
    lin.running = true; lin.phase = 0;
    lin.msg = "идёт прогон…";
    log_add(0, "самотест ЦАП: " + std::to_string(codes.size()) + " ступенек по " + std::to_string(n) + " импульсов");
}

void Device::lin_abort()
{
    if (!lin.running) return;
    lin.running = false;
    send_cmd("-dac off");
    send_cmd("-sto");
    if (lin.msg.empty() || lin.msg == "идёт прогон…") lin.msg = "прервано";
}

void Device::lin_tick(double now)
{
    if (lin.idx >= lin.codes.size()) {
        lin.running = false;
        std::vector<double> x, y;
        for (auto& p : lin.pts) if (p.ok) { x.push_back(p.code); y.push_back(p.ch); }
        if (x.size() >= 2) {
            double n = (double)x.size(), mx = 0, my = 0;
            for (size_t i = 0; i < x.size(); i++) { mx += x[i]; my += y[i]; }
            mx /= n; my /= n;
            double sxx = 0, sxy = 0, syy = 0;
            for (size_t i = 0; i < x.size(); i++) {
                sxx += (x[i] - mx) * (x[i] - mx); sxy += (x[i] - mx) * (y[i] - my); syy += (y[i] - my) * (y[i] - my);
            }
            lin.k = sxy / sxx; lin.b = my - lin.k * mx;
            double ssr = 0, dev = 0;
            for (size_t i = 0; i < x.size(); i++) {
                double r = y[i] - (lin.k * x[i] + lin.b);
                ssr += r * r;
                double d = fabs(r) / y.back() * 100.0;
                if (d > dev) dev = d;
            }
            lin.r2 = syy > 0 ? 1.0 - ssr / syy : 1.0;
            lin.dev = dev;
            lin.fitted = true;
            char b[160];
            snprintf(b, sizeof b, "готово: канал = %.4f·код %+.2f; R² = %.6f; отклонение ≤ %.3f %%", lin.k, lin.b, lin.r2, lin.dev);
            lin.msg = b;
        } else {
            lin.msg = "мало точек для прямой";
        }
        log_add(0, "самотест ЦАП: " + lin.msg);
        return;
    }
    int code = lin.codes[lin.idx];
    if (lin.phase == 0) {
        send_cmd("-sto"); send_cmd("-rst"); send_cmd("-sta");
        for (auto& v : spectrum) v = 0;
        char c[64]; snprintf(c, sizeof c, "-dac %d %d %d", code, lin.n, lin.period);
        send_cmd(c);
        lin.phase = 1; lin.t0 = now;
    } else if (lin.phase == 1) {
        if (now - lin.t0 > lin.n * lin.period * 1e-6 + 5.0) {
            lin.msg = "нет ответа «-ok dac done»";
            lin_abort();
        }
    } else if (lin.phase == 2) {
        bool have = full_seq_ > lin_seq_;
        if (have || now - lin.t0 > 3.0) {
            LinPoint p; p.code = code;
            p.ok = have && peak_stats(spectrum, 4, CHANNELS, p.ch, p.sd, p.n);
            lin.pts.push_back(p);
            send_cmd("-sto");
            lin.idx++; lin.phase = 0;
        }
    }
}

// ================= подстройка ВВ по пику =================

void Device::hvp_start(double v_start)
{
    if (!connected()) return;
    lin_abort();
    double target = hvp.target, lo = hvp.lo, hi = hvp.hi, ge = hvp.gain_exp, secs = hvp.secs, tol = hvp.tol_pct;
    hvp = HvPeak{};
    hvp.target = target; hvp.lo = lo; hvp.hi = hi; hvp.gain_exp = ge; hvp.secs = secs; hvp.tol_pct = tol;
    hvp.v = v_start; hvp.running = true; hvp.phase = 0;
    hvp.msg = "выставляю ВВ…";
    log_add(0, "подстройка ВВ по пику: старт с " + std::to_string((int)v_start) + " В, цель канал " + std::to_string((int)target));
}

void Device::hvp_abort()
{
    if (!hvp.running) return;
    hvp.running = false;
    send_cmd("-sto");
    if (hvp.msg.empty() || starts(hvp.msg, "жду") || starts(hvp.msg, "набор") || starts(hvp.msg, "выставляю"))
        hvp.msg = "прервано (ВВ оставлено как есть)";
}

void Device::hvp_tick(double now)
{
    if (gstr("hv_trip", "-") != "-" && gst_age() < 3) {
        hvp.msg = "авария ВВ: " + gstr("hv_trip");
        hvp_abort();
        return;
    }
    char c[256];
    switch (hvp.phase) {
    case 0:   // уставка
        snprintf(c, sizeof c, "-hv %d", (int)lround(hvp.v));
        send_cmd(c);
        hvp.phase = 1; hvp.t0 = now;
        hvp.msg = "жду установления ВВ…";
        break;
    case 1: { // установление по монитору
        double out = fabs(gnum("hv_out", 0));
        bool fresh = gst_age() < 1.5;
        if (now - hvp.t0 > 4.0 && fresh && fabs(out - hvp.v) < 3.0) {
            send_cmd("-sto"); send_cmd("-rst"); send_cmd("-sta");
            for (auto& v : spectrum) v = 0;
            hvp.phase = 2; hvp.t0 = now;
            snprintf(c, sizeof c, "набор %.0f с при %.0f В…", hvp.secs, hvp.v);
            hvp.msg = c;
        } else if (now - hvp.t0 > 180.0) {
            hvp.msg = "ВВ не установилось за 3 мин";
            hvp_abort();
        }
        break;
    }
    case 2:   // набор
        if (now - hvp.t0 >= hvp.secs) { hvp_seq_ = full_seq_; chunk_mask_ = 0; hvp.phase = 3; hvp.t0 = now; }
        break;
    case 3: { // свежий полный спектр → пик
        if (full_seq_ <= hvp_seq_ && now - hvp.t0 < 3.0) break;
        send_cmd("-sto");
        double m = 0, sd = 0; uint64_t n = 0;
        if (!peak_stats(spectrum, (int)hvp.lo, (int)hvp.hi, m, sd, n) || n < 200) {
            hvp.msg = "пик в окне не найден (мало счёта или окно не то)";
            hvp_abort();
            break;
        }
        hvp.peak = m;
        hvp.hist.push_back({ hvp.v, m });
        hvp.iter++;
        double err = (m / hvp.target - 1.0) * 100.0;
        if (fabs(err) <= hvp.tol_pct) {
            hvp.running = false; hvp.done = true;
            snprintf(c, sizeof c, "готово: %.0f В, пик %.1f (%+.2f %%). Сохрани -save", hvp.v, m, err);
            hvp.msg = c;
            log_add(0, "подстройка ВВ: " + hvp.msg);
            break;
        }
        if (hvp.iter >= hvp.max_iter) {
            hvp.running = false;
            snprintf(c, sizeof c, "не сошлось за %d шагов: %.0f В, пик %.1f", hvp.max_iter, hvp.v, m);
            hvp.msg = c;
            break;
        }
        double nv = hvp.v * pow(hvp.target / m, 1.0 / hvp.gain_exp);
        if (nv > hvp.v * 1.15) nv = hvp.v * 1.15;
        if (nv < hvp.v * 0.85) nv = hvp.v * 0.85;
        if (nv > 1200) nv = 1200;
        if (nv < 300) nv = 300;
        hvp.v = nv;
        hvp.phase = 0;
        break;
    }
    }
}

// ================= очередь команд =================

void Device::queue(const std::vector<std::string>& cmds)
{
    for (auto& c : cmds) q_.push_back(c);
}

// ================= сбор сырых порций =================

void Device::raw_start(int nblocks)
{
    if (!connected()) return;
    lin_abort(); hvp_abort(); tc_stop(true);
    raw = RawRun{};
    raw.target = nblocks; raw.running = true;
    raw.was_acq = acquiring;
    if (acquiring) send_cmd("-sto");
    raw_cur_.assign(tpl_blk, 0); raw_wait_ = false;
    raw.msg = "сбор порций…";
    log_add(0, "эталон: сбор " + std::to_string(nblocks) + " сырых порций (-osc raw)");
}

void Device::raw_abort()
{
    if (!raw.running) return;
    raw.running = false; raw_wait_ = false;
    if (raw.was_acq) send_cmd("-sta");
    raw.msg = "сбор прерван: порций " + std::to_string(raw.blocks.size());
}

void Device::raw_tick(double now)
{
    if (raw_wait_) {
        if (now - raw_t_ > 5.0) { raw_wait_ = false; raw.bad++; }
        return;
    }
    if ((int)raw.blocks.size() >= raw.target) {
        raw.running = false;
        if (raw.was_acq) send_cmd("-sta");
        raw.msg = "собрано порций: " + std::to_string(raw.blocks.size()) + ", сбоев " + std::to_string(raw.bad);
        log_add(0, "эталон: " + raw.msg);
        return;
    }
    if (raw.bad > 50 && raw.bad > (int)raw.blocks.size()) {
        raw.running = false;
        raw.msg = "слишком много сбоев при чтении порций";
        return;
    }
    std::fill(raw_cur_.begin(), raw_cur_.end(), (uint16_t)0);
    raw_got_ = raw_total_ = 0;
    send_cmd("-osc raw", true);
    raw_wait_ = true; raw_t_ = now;
}

// ================= термокомпенсация =================

void Device::old_tc_from(const std::string& t)
{
    tc.old_tc = t.size() > 7 ? t.substr(7) : "";      // "-ok tc t0 k1 k2" → "t0 k1 k2"
}

void Device::tc_start()
{
    if (!connected()) return;
    lin_abort(); hvp_abort(); raw_abort();
    double secs = tc.secs, lo = tc.lo, hi = tc.hi;
    tc = TcRun{};
    tc.secs = secs; tc.lo = lo; tc.hi = hi;
    tc_capture_old_ = true;
    queue({ "-tc", "-tc 25 0 0" });          // запомнить действующие коэффициенты и выключить поправку
    tc.running = true; tc.phase = -1; tc.t0 = now_sec();
    tc.msg = "выключаю термокомпенсацию…";
    log_add(0, "термокомпенсация: запись (поправка в приборе выключена на время записи)");
}

void Device::tc_stop(bool restore_old)
{
    if (!tc.running) return;
    tc.running = false;
    send_cmd("-sto");
    if (restore_old && !tc.old_tc.empty()) { queue({ "-tc " + tc.old_tc }); tc.msg = "остановлено, прежние коэффициенты возвращены"; }
    else tc.msg = "остановлено";
}

void Device::tc_tick(double now)
{
    switch (tc.phase) {
    case -1:
        if (!queue_busy()) tc.phase = 0;
        break;
    case 0:
        send_cmd("-sto"); send_cmd("-rst"); send_cmd("-sta");
        for (auto& v : spectrum) v = 0;
        tc.tsum = 0; tc.tn = 0; tc.t0 = now; tc.phase = 1;
        if (tc.pts.empty()) tc.msg = "набор первой точки…";
        break;
    case 1:
        if (gst_rx_ > tc_last_gst_ && gstr("pt", "-") == "ok") { tc.tsum += gnum("t"); tc.tn++; }
        tc_last_gst_ = gst_rx_;
        if (now - tc.t0 >= tc.secs) { tc_seq_ = full_seq_; chunk_mask_ = 0; tc.phase = 2; tc.t0 = now; }
        break;
    case 2: {
        if (full_seq_ <= tc_seq_ && now - tc.t0 < 3.0) break;
        double m = 0, sd = 0; uint64_t n = 0;
        if (tc.tn > 0 && peak_stats(spectrum, (int)tc.lo, (int)tc.hi, m, sd, n) && n >= 200) {
            TcPoint p; p.t = tc.tsum / tc.tn; p.peak = m; p.sd = sd; p.n = n;
            tc.pts.push_back(p);
            char b[96]; snprintf(b, sizeof b, "точка %d: %.2f °C, пик %.2f", (int)tc.pts.size(), p.t, m);
            tc.msg = b;
        } else {
            tc.msg = tc.tn == 0 ? "нет температуры (Pt1000 не ok)" : "пик в окне не найден";
        }
        tc.phase = 0;
        break;
    }
    }
}

bool Device::tc_fit(double t0, int order, double& k1, double& k2, double& rms_pct) const
{
    int m = order == 2 ? 3 : 2;
    if ((int)tc.pts.size() < m + 1) return false;
    double A[3][4] = {};
    for (auto& p : tc.pts) {
        double d = p.t - t0, pw[3] = { 1, d, d * d };
        for (int r = 0; r < m; r++) { for (int c = 0; c < m; c++) A[r][c] += pw[r] * pw[c]; A[r][m] += pw[r] * p.peak; }
    }
    for (int col = 0; col < m; col++) {
        int piv = col;
        for (int r = col + 1; r < m; r++) if (fabs(A[r][col]) > fabs(A[piv][col])) piv = r;
        for (int k = 0; k <= m; k++) { double t = A[col][k]; A[col][k] = A[piv][k]; A[piv][k] = t; }
        if (fabs(A[col][col]) < 1e-12) return false;
        for (int r = 0; r < m; r++) if (r != col) { double f = A[r][col] / A[col][col]; for (int k = col; k <= m; k++) A[r][k] -= f * A[col][k]; }
    }
    double c[3] = { 0, 0, 0 };
    for (int i = 0; i < m; i++) c[i] = A[i][m] / A[i][i];
    if (c[0] <= 0) return false;
    k1 = c[1] / c[0]; k2 = m == 3 ? c[2] / c[0] : 0.0;
    double s = 0;
    for (auto& p : tc.pts) { double d = p.t - t0, f = c[0] + c[1] * d + (m == 3 ? c[2] * d * d : 0); s += (p.peak - f) * (p.peak - f) / (f * f); }
    rms_pct = 100.0 * sqrt(s / tc.pts.size());
    return true;
}

// ================= регистратор: выгрузка журнала =================

void Device::logd_start(uint32_t only_session)
{
    logd = LogDump();
    logd.running = true;
    logd.only_session = only_session;
    logd_wait_ = false;
    logd_list_done_ = logd_rec_done_ = logd_rec_err_ = false;
    logd.msg = "читаю список записей…";
}

void Device::logd_abort()
{
    if (!logd.running) return;
    logd.running = false;
    logd.msg = "выгрузка прервана";
}

void Device::logd_tick(double now)
{
    if (logd.phase == 0) {
        if (!logd_wait_) { send_cmd("-log list", true); logd_wait_ = true; logd_t_ = now; return; }
        if (logd_list_done_) {
            logd_wait_ = false;
            if (logd_rec_err_) { logd.running = false; logd.msg = "прибор не отдал список (" + log_status + ")"; return; }
            std::sort(logd.items.begin(), logd.items.end(), [](const LogDump::Item& a, const LogDump::Item& b) { return a.seq < b.seq; });
            logd.phase = 1;
            logd.idx = 0;
            return;
        }
        if (now - logd_t_ > 60.0) { logd.running = false; logd.msg = "нет ответа на -log list"; }
        return;
    }
    // чтение записей по одной
    if (logd_wait_) {
        bool fin = logd_rec_done_ || logd_rec_err_;
        if (!fin && now - logd_t_ < 15.0) return;
        logd_wait_ = false;
        logdump::Record r;
        const auto& it = logd.items[logd.idx];
        if (fin && !logd_rec_err_ && logd_total_ && logd_got_ + (LOGREC_HDR_PAGE - sizeof(logrec_hdr_t)) >= logd_total_) {
            if (!logdump::parse(logd_buf_, it.sector, r)) logd.bad++;
        } else {
            r.sector = it.sector; r.raw = logd_buf_; r.err = fin ? "ошибка чтения" : "нет ответа"; logd.bad++;
        }
        logd.recs.push_back(std::move(r));
        logd.idx++;
    }
    if (logd.idx >= logd.items.size()) {
        logd.running = false;
        logd.done = true;
        char b[96];
        snprintf(b, sizeof b, "выгружено записей: %zu, испорчено %d", logd.recs.size(), logd.bad);
        logd.msg = b;
        log_add(0, "регистратор: " + logd.msg);
        return;
    }
    char c[40];
    snprintf(c, sizeof c, "-log read %u", logd.items[logd.idx].sector);
    logd_buf_.clear();
    logd_total_ = logd_got_ = 0;
    logd_rec_done_ = logd_rec_err_ = false;
    send_cmd(c, true);
    logd_wait_ = true;
    logd_t_ = now;
    char b[64];
    snprintf(b, sizeof b, "читаю запись %zu из %zu…", logd.idx + 1, logd.items.size());
    logd.msg = b;
}
