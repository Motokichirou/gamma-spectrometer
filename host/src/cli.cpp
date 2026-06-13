// cli.cpp — сервисный CLI для автоматизации (без GUI), поверх класса Device.
//   gammacli list
//   gammacli info    --port COMx
//   gammacli acquire --port COMx --seconds N [--gen spec|mono|off] [--out file.csv]
//   gammacli enc     --port COMx
//   gammacli cmd     --port COMx --send "-inf" [--read S]
#include "device.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
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
    printf("gammacli — сервисный CLI гамма-спектрометра\n"
           "  gammacli list\n"
           "  gammacli info    --port COMx\n"
           "  gammacli acquire --port COMx --seconds N [--gen spec|mono|off] [--out file.csv]\n"
           "  gammacli enc     --port COMx\n"
           "  gammacli cmd     --port COMx --send \"-inf\" [--read S]\n");
}

int main(int argc, char** argv)
{
    if (argc < 2) { usage(); return 1; }
    std::string sub = argv[1];

    if (sub == "list") {
        for (auto& p : serialport::enumerate()) printf("%s\n", p.c_str());
        return 0;
    }

    std::string port = opt(argc, argv, "--port");
    if (port.empty()) { printf("нужен --port COMx\n"); return 1; }

    Device dev;
    if (!dev.connect(port)) { printf("не удалось открыть %s\n", port.c_str()); return 2; }
    pump(dev, 0.8);   // идентификация (-inf/-cal)

    int rc = 0;
    if (sub == "info") {
        printf("port: %s\n", port.c_str());
        printf("fw:   %s\n", dev.fw_info.c_str());
        printf("sn:   %s\n", dev.serial.c_str());
    } else if (sub == "acquire") {
        double secs = atof(opt(argc, argv, "--seconds", "5").c_str());
        std::string gen = opt(argc, argv, "--gen");
        if (!gen.empty()) { dev.send_cmd("-tst " + gen); pump(dev, 0.4); }
        dev.reset_spectrum(); pump(dev, 0.2);
        dev.start();
        printf("...набор %.1f c...\n", secs);
        pump(dev, secs);
        dev.stop(); pump(dev, 0.4);
        if (!gen.empty() && gen != "off") { dev.send_cmd("-tst off"); pump(dev, 0.3); }
        int pk = dev.peak_channel();
        printf("итог: cps=%u total=%llu peak_ch=%d (=%u) invalid=%u\n",
               dev.cps, (unsigned long long)dev.total(), pk, dev.spectrum[pk], dev.invalid);
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
    } else if (sub == "enc") {
        dev.log.clear();
        dev.send_cmd("-tst enc");
        printf("...ENC ~8 c...\n");
        pump(dev, 8.0);
        print_rx(dev);
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
