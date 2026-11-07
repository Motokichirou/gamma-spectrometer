// cli.cpp — сервисный CLI для автоматизации (без GUI), поверх класса Device. Прошивка gamma_fw.
//   gammapult list
//   gammapult info    --port COMx
//   gammapult gst     --port COMx
//   gammapult acquire --port COMx --seconds N [--out file.csv]
//   gammapult lin     --port COMx [--codes "50 100 … 1350"] [--n 10000] [--period 100]
//   gammapult cmd     --port COMx --send "-diag" [--read S]
//   gammapult tpl collect --port COMx [--n 2000] --out raw.bin   (сырые порции uint16 × 8192 подряд)
//   gammapult tpl build raw.bin [--out tpl.h] [--load --port COMx [--save]]
#include "device.h"
#include "tpl_learn.h"
#include "host_app.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

static std::string opt(int argc, char** argv, const char* key, const char* def = "")
{
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], key) == 0) return argv[i + 1];
    return def;
}

static void pump(Device& d, double sec)
{
    using namespace std::chrono;
    auto t0 = steady_clock::now();
    while (duration<double>(steady_clock::now() - t0).count() < sec) {
        d.poll();
        std::this_thread::sleep_for(milliseconds(2));
    }
}

static void print_rx(Device& d)
{
    for (auto& l : d.log)
        if (l.dir == 2) printf("%s\n", l.text.c_str());
}

static void usage()
{
    printf("gammapult — сервисный CLI гамма-спектрометра (прошивка gamma_fw)\n"
           "  gammapult list\n"
           "  gammapult info    --port COMx\n"
           "  gammapult gst     --port COMx\n"
           "  gammapult acquire --port COMx --seconds N [--out file.csv]\n"
           "  gammapult lin     --port COMx [--codes \"50 100 200 400 600 800 1000 1200 1350\"] [--n 10000] [--period 100]\n"
           "  gammapult cmd     --port COMx --send \"-diag\" [--read S]\n"
           "  gammapult tpl collect --port COMx [--n 2000] --out raw.bin\n"
           "  gammapult tpl build raw.bin [--out tpl.h] [--load --port COMx [--save]]\n");
}

int run_cli(int argc, char** argv)
{
    if (argc < 2) { usage(); return 1; }
    std::string sub = argv[1];

    if (sub == "help" || sub == "--help" || sub == "-h") { usage(); return 0; }

    if (sub == "list") {
        for (auto& p : serialport::enumerate()) printf("%s\n", p.c_str());
        return 0;
    }

    // эталон из файла — без прибора (с --load нужен порт)
    tpl::Result tr;
    bool tpl_build = (sub == "tpl" && argc >= 4 && std::string(argv[2]) == "build");
    if (tpl_build) {
        FILE* f = fopen(argv[3], "rb");
        if (!f) { printf("не открыть %s\n", argv[3]); return 2; }
        std::vector<std::vector<uint16_t>> blocks;
        std::vector<uint16_t> b(tpl::BLK);
        while (fread(b.data(), 2, tpl::BLK, f) == (size_t)tpl::BLK) blocks.push_back(b);
        fclose(f);
        tpl::Singles s = tpl::find_singles(blocks);
        printf("порций %d, σ шума %.2f кода; пересечений %d, одиночных %d, в 100…1200 кодов %d\n",
               s.blocks, s.sigma, s.cross, s.single, s.amp);
        tr = tpl::build(s);
        printf("%s\n", tr.msg.c_str());
        if (!tr.ok) return 4;
        std::string out = opt(argc, argv, "--out");
        if (!out.empty()) {
            FILE* h = fopen(out.c_str(), "wb");
            if (h) { std::string t = tpl::header(tr); fwrite(t.data(), 1, t.size(), h); fclose(h); printf("эталон -> %s\n", out.c_str()); }
        }
        bool load = false;
        for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--load") == 0) load = true;
        if (!load) return 0;
    }

    std::string port = opt(argc, argv, "--port");
    if (port.empty()) { printf("нужен --port COMx\n"); return 1; }

    Device dev;
    if (!dev.connect(port)) { printf("не удалось открыть %s (занят BecqMoni?)\n", port.c_str()); return 2; }
    pump(dev, 1.2);   // идентификация (-inf/-cal/-thr/-gst)

    int rc = 0;
    if (sub == "info") {
        printf("port: %s\n", port.c_str());
        printf("fw:   %s\n", dev.fw_info.c_str());
        printf("sn:   %s\n", dev.serial.c_str());
        printf("thr:  %d (коды АЦП, 0 — авто)\n", dev.thr_codes);
    } else if (sub == "gst") {
        if (dev.gst.empty()) pump(dev, 1.5);
        for (auto& kv : dev.gst) printf("%-10s %s\n", kv.first.c_str(), kv.second.c_str());
        if (dev.gst.empty()) { printf("нет ответа -gst\n"); rc = 3; }
    } else if (sub == "acquire") {
        double secs = atof(opt(argc, argv, "--seconds", "5").c_str());
        dev.stop(); dev.reset_spectrum(); pump(dev, 0.3);
        dev.start();
        printf("...набор %.1f c...\n", secs);
        pump(dev, secs + 1.2);   // + последний полный спектр
        dev.stop(); pump(dev, 0.4);
        double m = 0, sd = 0; uint64_t n = 0;
        bool pk = dev.peak_stats(dev.spectrum, 30, Device::CHANNELS, m, sd, n);
        printf("итог: cps=%u total=%llu invalid=%u", dev.cps, (unsigned long long)dev.total(), dev.invalid);
        if (pk && m > 0) printf(" пик=%.1f σ=%.2f (≈FWHM %.2f %%)", m, sd, 235.48 * sd / m);
        printf("\n");
        std::string out = opt(argc, argv, "--out");
        if (!out.empty()) {
            FILE* f = fopen(out.c_str(), "w");
            if (f) {
                fprintf(f, "channel,count\n");
                for (int i = 0; i < Device::CHANNELS; i++)
                    fprintf(f, "%d,%u\n", i, dev.spectrum[i]);
                fclose(f);
                printf("спектр -> %s\n", out.c_str());
            } else { printf("не открыть %s\n", out.c_str()); rc = 3; }
        }
    } else if (sub == "lin") {
        std::string cs = opt(argc, argv, "--codes", "50 100 200 400 600 800 1000 1200 1350");
        std::vector<int> codes;
        const char* p = cs.c_str();
        while (*p) { char* e; long v = strtol(p, &e, 10); if (e == p) { p++; continue; } codes.push_back((int)v); p = e; }
        int n = atoi(opt(argc, argv, "--n", "10000").c_str());
        int per = atoi(opt(argc, argv, "--period", "100").c_str());
        dev.lin_start(codes, n, per);
        size_t shown = 0;
        while (dev.lin.running) {
            pump(dev, 0.05);
            while (shown < dev.lin.pts.size()) {
                const auto& q = dev.lin.pts[shown++];
                if (q.ok) printf("ступенька %5d: канал %9.2f  σ %5.2f  в пике %llu из %d\n", q.code, q.ch, q.sd, (unsigned long long)q.n, n);
                else      printf("ступенька %5d: пика нет\n", q.code);
                fflush(stdout);
            }
        }
        printf("%s\n", dev.lin.msg.c_str());
        rc = dev.lin.fitted ? 0 : 4;
    } else if (tpl_build) {
        dev.queue(tpl::commands(tr));
        dev.queue({ "-tplok" });
        bool save = false;
        for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--save") == 0) save = true;
        if (save) dev.queue({ "-save" });
        while (dev.queue_busy()) pump(dev, 0.05);
        printf("загрузка эталона: ошибок %d%s%s\n", dev.queue_errors, dev.queue_errors ? " — " : "", dev.queue_last_err.c_str());
        printf("ответ: %s\n", dev.last_reply.c_str());
        rc = dev.queue_errors ? 5 : 0;
    } else if (sub == "tpl" && argc >= 3 && std::string(argv[2]) == "collect") {
        int n = atoi(opt(argc, argv, "--n", "2000").c_str());
        std::string out = opt(argc, argv, "--out");
        if (out.empty()) { printf("нужен --out raw.bin\n"); dev.disconnect(); return 1; }
        dev.raw_start(n);
        size_t shown = 0;
        while (dev.raw.running) {
            pump(dev, 0.1);
            if (dev.raw.blocks.size() >= shown + 100) { shown = dev.raw.blocks.size(); printf("  порций %zu/%d, сбоев %d\n", shown, n, dev.raw.bad); fflush(stdout); }
        }
        FILE* f = fopen(out.c_str(), "wb");
        if (!f) { printf("не открыть %s\n", out.c_str()); rc = 3; }
        else {
            for (auto& b : dev.raw.blocks) fwrite(b.data(), 2, b.size(), f);
            fclose(f);
            printf("%s -> %s\n", dev.raw.msg.c_str(), out.c_str());
        }
    } else if (sub == "cmd") {
        std::string c = opt(argc, argv, "--send");
        if (c.empty()) { printf("нужен --send \"...\"\n"); dev.disconnect(); return 1; }
        double rd = atof(opt(argc, argv, "--read", "1").c_str());
        dev.log.clear();
        dev.send_cmd(c);
        pump(dev, rd);
        print_rx(dev);
    } else {
        usage(); rc = 1;
    }

    dev.disconnect();
    return rc;
}
