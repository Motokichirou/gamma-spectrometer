// device.cpp — см. device.h
#include "device.h"
#include <chrono>

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
}

void Device::start()
{
    send_cmd("-sta");
    acquiring = true;
    state = State::Acquiring;
    status_text = "Связь · набор";
}

void Device::stop()
{
    send_cmd("-sto");
    acquiring = false;
    if (state == State::Acquiring) state = State::Connected;
    status_text = "Связь установлена · простой";
}

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
