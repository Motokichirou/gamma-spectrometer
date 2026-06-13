// probe.cpp — headless-проверка стека serial_win + shproto против реального прибора.
// Консольная утилита (не часть GUI): перечисляет порты, шлёт -inf/-cal,
// запускает набор, принимает кадры спектра/статуса, печатает сводку.
//   gamma_probe.exe [COMx]
#include "serial_win.h"
#include "shproto.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

using namespace std::chrono;

static uint32_t spectrum[8192];

int main(int argc, char** argv)
{
    auto ports = serialport::enumerate();
    printf("COM-порты: ");
    for (auto& p : ports) printf("%s ", p.c_str());
    printf("\n");

    std::string port = (argc > 1) ? argv[1]
                     : (ports.empty() ? std::string() : ports.front());
    if (port.empty()) { printf("Нет COM-портов\n"); return 1; }
    printf("Открываю %s @ 600000 8N1...\n", port.c_str());

    serialport::Serial s;
    if (!s.open(port, 600000)) { printf("Не удалось открыть %s\n", port.c_str()); return 1; }

    shproto::Parser parser;
    static uint8_t rx[16384];
    int n_txt = 0, n_spec = 0, n_stat = 0;

    auto handler = [&](const shproto::Frame& f) {
        if (f.cmd == shproto::CMD_TEXT) {
            std::string t((const char*)f.payload.data(), f.payload.size());
            printf("<- TEXT: %s\n", t.c_str());
            n_txt++;
        } else if (f.cmd == shproto::CMD_SPECTRUM) {
            n_spec++;
            const auto& p = f.payload;
            if (p.size() >= 2) {
                uint32_t off = (uint32_t)(p[0] | (p[1] << 8));
                size_t nch = (p.size() - 2) / 4;
                for (size_t i = 0; i < nch; i++) {
                    uint32_t v = (uint32_t)(p[2+4*i] | (p[3+4*i]<<8) | (p[4+4*i]<<16) | (p[5+4*i]<<24));
                    v &= 0x7FFFFFF;
                    if (off + i < 8192) spectrum[off + i] = v;
                }
            }
        } else if (f.cmd == shproto::CMD_STATUS) {
            n_stat++;
            const auto& p = f.payload;
            if (p.size() >= 14) {
                uint32_t el  = (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)) & 0x7FFFFFF;
                uint32_t cps = (uint32_t)(p[6] | (p[7]<<8) | (p[8]<<16) | (p[9]<<24));
                uint32_t inv = (uint32_t)(p[10]|(p[11]<<8)|(p[12]<<16)|(p[13]<<24));
                printf("<- STATUS: t=%us cps=%u invalid=%u\n", el, cps, inv);
            }
        }
    };

    auto pump = [&](double seconds) {
        auto t0 = steady_clock::now();
        auto lastKa = t0;
        while (duration<double>(steady_clock::now() - t0).count() < seconds) {
            if (duration<double>(steady_clock::now() - lastKa).count() > 1.0) {
                uint8_t ka = 0xFF; s.write(&ka, 1); lastKa = steady_clock::now();
            }
            int n = s.read(rx, sizeof(rx));
            if (n > 0) parser.feed(rx, (size_t)n, handler);
            else std::this_thread::sleep_for(milliseconds(2));
        }
    };

    auto send = [&](const char* cmd) {
        printf("-> %s\n", cmd);
        auto fr = shproto::build_text(cmd);
        s.write(fr.data(), (int)fr.size());
    };

    send("-sto"); pump(0.4);
    send("-rst"); pump(0.4);
    send("-inf"); pump(0.6);
    send("-cal"); pump(0.6);

    send("-tst spec"); pump(0.4);   // включить тестовый генератор (проверка пути спектра)

    send("-sta");
    printf("...набор 4 c...\n");
    pump(4.0);
    send("-sto"); pump(0.4);
    send("-tst off"); pump(0.3);    // вернуть генератор в выкл (boot-состояние)

    uint64_t total = 0; int peak = 0; uint32_t peakv = 0;
    for (int i = 0; i < 8192; i++) {
        total += spectrum[i];
        if (spectrum[i] > peakv) { peakv = spectrum[i]; peak = i; }
    }
    printf("\nИТОГ: текст=%d спектр-кадров=%d статус-кадров=%d\n", n_txt, n_spec, n_stat);
    printf("Гистограмма: сумма=%llu, пик в канале %d (=%u)\n",
           (unsigned long long)total, peak, peakv);

    s.close();
    return (n_txt > 0 && n_stat > 0) ? 0 : 2;
}
