// device.h — сессия связи с прибором (прошивка firmware/gamma_fw) поверх serial_win + shproto.
// Держит спектр, статус набора, телеметрию -gst, идентификацию, лог кадров, конечный автомат
// состояния и два сценария: самотест линейности по ЦАП и подстройку ВВ по пику.
#pragma once
#include "serial_win.h"
#include "shproto.h"
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <cstdint>

class Device {
public:
    static const int CHANNELS = 8192;
    static const int CHUNKS   = 16;          // спектр приходит кусками по 512 каналов

    enum class State { Disconnected, Connecting, Connected, Acquiring, Error };

    // ---- данные набора ----
    uint32_t spectrum[CHANNELS] = { 0 };
    uint32_t elapsed_s = 0, cps = 0, invalid = 0;
    bool acquiring = false;

    // ---- телеметрия: последняя строка -gst (key=value), обновляется раз в секунду ----
    std::map<std::string, std::string> gst;
    double gst_age() const;                  // секунд с прихода последнего -gst (1e9 — не было)
    double gnum(const char* key, double def = 0.0) const;
    std::string gstr(const char* key, const char* def = "—") const;
    bool hv_on() const { return gstr("hv", "off") == "on"; }

    int thr_codes = -1;                      // порог детектора, коды АЦП (0 — авто 5σ; −1 — неизвестно)

    // ---- идентификация ----
    std::string fw_info;                     // ответ -inf
    std::string serial;                      // серийник (последняя строка ответа -cal)

    State state = State::Disconnected;
    std::string status_text = "Нет связи · порт закрыт";
    std::string port_name;
    std::string last_reply;                  // последний текстовый ответ (не -gst)

    // лог кадров для консоли (опрос -gst в лог не пишется)
    struct LogLine { int dir; std::string text; };   // dir: 0=sys 1=tx 2=rx 3=err
    std::deque<LogLine> log;

    std::vector<std::string> ports;
    void refresh_ports();

    bool connect(const std::string& port);
    void disconnect();
    void poll();                             // звать часто (каждый кадр UI / цикл CLI)
    void send_cmd(const std::string& cmd, bool quiet = false);
    void start();
    void stop();
    void reset_spectrum();
    bool connected() const { return state == State::Connected || state == State::Acquiring; }

    uint64_t total() const;
    int peak_channel(int lo = 4) const;
    // пик в окне [lo, hi): центроид, σ и отсчёты по окну ±max(5, 4σ) (как gfw.py peak_stats)
    bool peak_stats(const uint32_t* sp, int lo, int hi, double& m, double& sd, uint64_t& n) const;
    const char* state_str() const;

    // ---- самотест линейности по ЦАП (как gfw.py lin) ----
    struct LinPoint { int code = 0; bool ok = false; double ch = 0, sd = 0; uint64_t n = 0; };
    struct LinRun {
        bool running = false, fitted = false;
        std::vector<int> codes;
        int n = 10000, period = 100;
        size_t idx = 0; int phase = 0; double t0 = 0;
        std::vector<LinPoint> pts;
        double k = 0, b = 0, r2 = 0, dev = 0;   // канал = k·код + b; dev — наибольшее отклонение, %
        std::string msg;
    } lin;
    void lin_start(const std::vector<int>& codes, int n = 10000, int period = 100);
    void lin_abort();

    // ---- подстройка ВВ по пику ----
    struct HvPeak {
        bool running = false, done = false;
        double target = 1180, lo = 300, hi = 6000, gain_exp = 5.3, secs = 20, tol_pct = 1.0;
        int iter = 0, max_iter = 6, phase = 0; double t0 = 0;
        double v = 0, peak = 0;
        std::vector<std::pair<double, double>> hist;   // (В, канал пика)
        std::string msg;
    } hvp;
    void hvp_start(double v_start);
    void hvp_abort();

private:
    serialport::Serial serial_;
    shproto::Parser parser_;
    double last_ka_ = 0.0, last_gst_tx_ = 0.0, gst_rx_ = -1.0;
    uint32_t chunk_mask_ = 0;                // какие куски пришли с момента сброса маски
    uint32_t full_seq_ = 0;                  // счётчик полных спектров (куски + статус)
    void on_frame(const shproto::Frame& f);
    void on_text(const std::string& t);
    void log_add(int dir, const std::string& s);
    void lin_tick(double now);
    void hvp_tick(double now);
    uint32_t lin_seq_ = 0, hvp_seq_ = 0;
};
