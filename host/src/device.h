// device.h — сессия связи с прибором (прошивка firmware/gamma_fw) поверх serial_win + shproto.
// Держит спектр, статус набора, телеметрию -gst, идентификацию, лог кадров, конечный автомат
// состояния и два сценария: самотест линейности по ЦАП и подстройку ВВ по пику.
#pragma once
#include "serial_win.h"
#include "shproto.h"
#include "logdump.h"
#include "devstate.h"
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
    double becq_c[5] = { 0, 0, 0, 0, 0 };    // шкала BecqMoni из -cal: E = c0 + c1·ch + …
    bool becq_ok = false;                    // шкала в приборе есть (не нули)

    // ---- запись о настройке (-setup) ----
    ds::SetupRec setup;
    ds::SetupKnown setup_known = ds::SetupKnown::Unknown;
    // запросить -setup: зовётся после подключения, после выхода из мастера, после "-ok saved"
    void setup_refresh();
    ds::Snapshot snapshot() const;           // срез для ds::home_view / step_done / top_bar
    bool setup_pending() const { return setup_wait_; }   // ждём ответа на -setup (главный экран: «читаю…»)

    State state = State::Disconnected;
    std::string status_text = "Нет связи · порт закрыт";
    std::string port_name;
    unsigned long last_open_error = 0;       // GetLastError() неудачного connect (→ ds::connect_hint_key)
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

    // ---- очередь команд: по одной, следующая — после "-ok…"/"-err…" (загрузка эталона и т. п.) ----
    void queue(const std::vector<std::string>& cmds);
    bool queue_busy() const { return !q_.empty() || q_wait_; }
    int  queue_errors = 0;
    std::string queue_last_err;

    // ---- сбор сырых порций АЦП ("-osc raw") для обучения эталона ----
    struct RawRun {
        bool running = false, was_acq = false;
        int target = 2000, bad = 0;
        std::vector<std::vector<uint16_t>> blocks;
        std::string msg;
    } raw;
    void raw_start(int nblocks);
    void raw_abort();

    // ---- термокомпенсация: пик источника против температуры (поправка в приборе выключена) ----
    struct TcPoint { double t = 0, peak = 0, sd = 0; uint64_t n = 0; };
    struct TcRun {
        bool running = false;
        double secs = 120, lo = 300, hi = 6000;
        int phase = 0; double t0 = 0, tsum = 0; int tn = 0;
        std::vector<TcPoint> pts;
        std::string old_tc;                  // "t0 k1 k2" до начала — вернуть, если новое не записали
        std::string msg;
    } tc;
    std::string tc_reply;                    // последний ответ "-ok tc …"
    void tc_start();
    void tc_stop(bool restore_old);
    // МНК: пик = a·(1 + k1·(T − t0) + k2·(T − t0)²); order 1 или 2. false — мало точек
    bool tc_fit(double t0, int order, double& k1, double& k2, double& rms_pct) const;

    // ---- регистратор: выгрузка журнала ("-log list" → "-log read <сектор>" по каждой записи) ----
    struct LogDump {
        bool running = false, done = false;
        int phase = 0;                       // 0 — список, 1 — чтение записей
        uint32_t only_session = 0;           // 0 — все сессии
        struct Item { uint32_t sector = 0, seq = 0, session = 0; };
        std::vector<Item> items;
        size_t idx = 0;
        std::vector<logdump::Record> recs;
        int bad = 0;
        std::string msg;
    } logd;
    std::string log_status;                  // последний ответ "-ok log …" / "-err log …" (не list/read)
    void logd_start(uint32_t only_session = 0);
    void logd_abort();

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
    void raw_tick(double now);
    void tc_tick(double now);
    void old_tc_from(const std::string& t);
    uint32_t lin_seq_ = 0, hvp_seq_ = 0, tc_seq_ = 0;
    std::deque<std::string> q_;
    bool q_wait_ = false; double q_t_ = 0;
    std::vector<uint16_t> raw_cur_;
    int raw_got_ = 0, raw_total_ = 0; bool raw_wait_ = false; double raw_t_ = 0;
    bool tc_capture_old_ = false; double tc_last_gst_ = -1;
    void logd_tick(double now);
    bool logd_wait_ = false, logd_list_done_ = false, logd_rec_done_ = false, logd_rec_err_ = false;
    double logd_t_ = 0;
    bool setup_wait_ = false; double setup_t_ = 0;
    std::vector<uint8_t> logd_buf_;
    uint32_t logd_total_ = 0, logd_got_ = 0;
};
