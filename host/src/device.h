// device.h — сессия связи с прибором поверх serial_win + shproto.
// Держит спектр, статус, идентификацию, лог кадров и конечный автомат состояния.
#pragma once
#include "serial_win.h"
#include "shproto.h"
#include <string>
#include <vector>
#include <deque>
#include <cstdint>

class Device {
public:
    static const int CHANNELS = 8192;

    enum class State { Disconnected, Connecting, Connected, Acquiring, Error };

    // результат самотеста ENC (разбор текстового отчёта -tst enc)
    struct EncPoint { int amp = 0; float ch = 0.0f, sig = 0.0f, fwhm = 0.0f; };
    struct EncResult {
        bool  running = false;
        bool  valid   = false;
        int   n       = 0;
        EncPoint pts[8];
        float k = 0.0f, b = 0.0f, inl = 0.0f, r2 = 0.0f;
    };
    EncResult enc;

    // данные
    uint32_t spectrum[CHANNELS] = { 0 };
    uint32_t elapsed_s = 0, cps = 0, invalid = 0;
    uint32_t crc_err = 0;
    bool acquiring = false;
    int  threshold_ch = 90;   // порог DSP, каналов (команда -thr)

    // идентификация (из -inf / -cal)
    std::string fw_info;     // строка ответа -inf
    std::string serial;      // серийник из -cal
    std::string cal_info;    // первая строка -cal

    State state = State::Disconnected;
    std::string status_text = "Нет связи · порт закрыт";
    std::string port_name;

    // лог кадров для консоли
    struct LogLine { int dir; std::string text; };   // dir: 0=sys 1=tx 2=rx 3=err
    std::deque<LogLine> log;

    // порты
    std::vector<std::string> ports;
    void refresh_ports();

    // управление
    bool connect(const std::string& port);
    void disconnect();
    void poll();                          // звать каждый кадр UI
    void send_cmd(const std::string& cmd);
    void start();
    void stop();
    void reset_spectrum();

    // производные
    uint64_t total() const;
    int peak_channel() const;
    const char* state_str() const;

private:
    serialport::Serial serial_;
    shproto::Parser parser_;
    double last_ka_ = 0.0;
    void on_frame(const shproto::Frame& f);
    void log_add(int dir, const std::string& s);
    void parse_enc(const std::string& t);
};
