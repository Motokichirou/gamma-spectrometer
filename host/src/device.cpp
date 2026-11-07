// device.cpp — см. device.h
#include "device.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>

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
        state = State::Error;
        status_text = "Не удалось открыть " + port + " (занят BecqMoni?)";
        log_add(3, status_text);
        return false;
    }
    port_name = port;
    state = State::Connected;
    status_text = "Связь установлена · простой";
    last_ka_ = last_gst_tx_ = now_sec();
    log_add(0, "Открыт " + port + " @ 600000 8N1");
    send_cmd("-stt");   // идёт ли набор (-ok collecting / -ok) — набор не трогаем
    send_cmd("-inf");   // версия прошивки
    send_cmd("-cal");   // 40 регистров BecqMoni + серийник последней строкой
    send_cmd("-thr");   // порог, коды АЦП
    send_cmd("-gst", true);
    return true;
}

void Device::disconnect()
{
    lin.running = false;
    hvp.running = false;
    serial_.close();
    state = State::Disconnected;
    status_text = "Нет связи · порт закрыт";
    acquiring = false;
    fw_info.clear(); serial.clear();
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
    char c[64];
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
