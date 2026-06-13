// device.cpp — см. device.h
#include "device.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

static double now_sec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void Device::log_add(int dir, const std::string& s)
{
    log.push_back({ dir, s });
    while (log.size() > 600) log.pop_front();
}

void Device::refresh_ports()
{
    ports = serialport::enumerate();
}

bool Device::connect(const std::string& port)
{
    disconnect();
    state = State::Connecting;
    status_text = "Подключение… запрос -inf";
    if (!serial_.open(port, 600000)) {
        state = State::Error;
        status_text = "Не удалось открыть " + port;
        log_add(3, status_text);
        return false;
    }
    port_name = port;
    state = State::Connected;
    status_text = "Связь установлена · простой";
    last_ka_ = now_sec();
    log_add(0, "Открыт " + port + " @ 600000 8N1");
    // идентификация
    send_cmd("-sto");
    send_cmd("-inf");
    send_cmd("-cal");
    send_cmd("-thr 0");   // запрос текущего порога
    return true;
}

void Device::disconnect()
{
    serial_.close();
    state = State::Disconnected;
    status_text = "Нет связи · порт закрыт";
    acquiring = false;
    fw_info.clear(); serial.clear(); cal_info.clear();
    port_name.clear();
}

void Device::send_cmd(const std::string& cmd)
{
    if (!serial_.is_open()) return;
    auto fr = shproto::build_text(cmd);
    serial_.write(fr.data(), (int)fr.size());
    log_add(1, cmd);

    // Состояние набора синхронизируем независимо от способа отправки
    // (кнопка «Старт/Стоп» или чип консоли -sta/-sto).
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

    static uint8_t rx[16384];
    int n = serial_.read(rx, sizeof(rx));
    if (n < 0) {
        state = State::Error;
        status_text = "Ошибка связи · порт";
        log_add(3, status_text);
        return;
    }
    if (n > 0)
        parser_.feed(rx, (size_t)n, [this](const shproto::Frame& f) { on_frame(f); });
}

void Device::on_frame(const shproto::Frame& f)
{
    if (f.cmd == shproto::CMD_TEXT) {
        std::string t((const char*)f.payload.data(), f.payload.size());
        log_add(2, t);
        if (t.rfind("GammaSpec", 0) == 0) {
            fw_info = t;
        } else if (t.rfind("CAL", 0) == 0) {
            cal_info = t;
            // серийник = последняя непустая строка ответа -cal (конвенция BecqMoni)
            std::string acc;
            std::string sn;
            for (char ch : t) {
                if (ch == '\n' || ch == '\r') {
                    if (!acc.empty() && acc.rfind("CAL", 0) != 0) sn = acc;
                    acc.clear();
                } else acc.push_back(ch);
            }
            if (!acc.empty() && acc.rfind("CAL", 0) != 0) sn = acc;
            if (!sn.empty()) serial = sn;
        } else if (t.rfind("ENC test", 0) == 0) {
            parse_enc(t);
        } else if (t.rfind("-ok thr", 0) == 0) {
            int v = atoi(t.c_str() + 7);
            if (v > 0) threshold_ch = v;
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
        }
    } else if (f.cmd == shproto::CMD_STATUS) {
        const auto& p = f.payload;
        if (p.size() >= 14) {
            elapsed_s = (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) & 0x7FFFFFF;
            cps       = (uint32_t)(p[6] | (p[7] << 8) | (p[8] << 16) | (p[9] << 24));
            invalid   = (uint32_t)(p[10] | (p[11] << 8) | (p[12] << 16) | (p[13] << 24));
        }
    }
}

void Device::parse_enc(const std::string& t)
{
    EncResult r;
    size_t pos = 0;
    while (pos < t.size() && r.n < 8) {
        size_t nl = t.find('\n', pos);
        std::string line = t.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? t.size() : nl + 1;
        int amp; float ch, sig, fwhm;
        if (sscanf(line.c_str(), "A=%d: ch=%f sig=%f FWHM=%f", &amp, &ch, &sig, &fwhm) == 4)
            r.pts[r.n++] = { amp, ch, sig, fwhm };
    }
    if (r.n >= 2) {
        // МНК-прямая ch = k*amp + b, плюс R² и INL (%FS)
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < r.n; i++) {
            double x = r.pts[i].amp, y = r.pts[i].ch;
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        double den = r.n * sxx - sx * sx;
        double k = (den != 0.0) ? (r.n * sxy - sx * sy) / den : 0.0;
        double b = (sy - k * sx) / r.n;
        double meany = sy / r.n, sstot = 0, ssres = 0, maxdev = 0;
        for (int i = 0; i < r.n; i++) {
            double y = r.pts[i].ch, fit = k * r.pts[i].amp + b;
            sstot += (y - meany) * (y - meany);
            ssres += (y - fit) * (y - fit);
            double d = y - fit; if (d < 0) d = -d;
            if (d > maxdev) maxdev = d;
        }
        r.k   = (float)k;
        r.b   = (float)b;
        r.r2  = (sstot > 0) ? (float)(1.0 - ssres / sstot) : 1.0f;
        r.inl = (float)(100.0 * maxdev / 8192.0);
        r.valid = true;
    }
    r.running = false;
    enc = r;
}

uint64_t Device::total() const
{
    uint64_t s = 0;
    for (int i = 0; i < CHANNELS; i++) s += spectrum[i];
    return s;
}

int Device::peak_channel() const
{
    int pk = 0; uint32_t v = 0;
    for (int i = 0; i < CHANNELS; i++) if (spectrum[i] > v) { v = spectrum[i]; pk = i; }
    return pk;
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
