// cli.cpp — сервисный CLI для автоматизации (без GUI), поверх класса Device. Прошивка gamma_fw.
//   gammapult list
//   gammapult info    --port COMx
//   gammapult gst     --port COMx
//   gammapult acquire --port COMx --seconds N [--out file.csv]
//   gammapult lin     --port COMx [--codes "50 100 … 1350"] [--n 10000] [--period 100]
//   gammapult cmd     --port COMx --send "-diag" [--read S]
//   gammapult hvpeak  --port COMx --start V [--target 1190] [--lo 300] [--hi 6000] [--count 3000] [--timeout 120] [--off]
//                     (проход: до --count импульсов в фотопике ±1.5·FWHM, не дольше --timeout с)
//   gammapult tc      --port COMx [--secs 120] [--points N] [--lo 300] [--hi 6000]   (запись термокомпенсации)
//   gammapult tpl collect --port COMx [--n 2000] --out raw.bin [--baud 3000000]
//                         (сырые порции uint16 × 8192 подряд, дописываются по мере прихода; --baud — временно
//                          поднять скорость UART на время сбора, после — обратно 600000)
//   gammapult tpl build raw.bin [--out tpl.h] [--tail-out tail.bin] [--load --port COMx [--save]]
//                         (эталон и таблица хвоста для разбора наложений; --load пишет оба: -tplw/-tplok, -tlw/-tlm/-tlok)
//   gammapult log status|on P [D]|off|start|stop|time|erase --yes --port COMx   (регистратор, SPEC_logger.md)
//   gammapult log dump --port COMx --out <папка> [--session N] [--lo 300 --hi 6000] [--thermo] [--sum]
#include "device.h"
#include "tpl_learn.h"
#include "fwflash.h"
#include "peakfit.h"
#include "host_app.h"
#include "logdump.h"
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cmath>
#include <filesystem>
#include <algorithm>

static std::string opt(int argc, char** argv, const char* key, const char* def = "")
{
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], key) == 0) return argv[i + 1];
    return def;
}

static bool starts_with_ok(const std::string& s) { return s.rfind("-ok", 0) == 0; }

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
           "  gammapult bootinfo --port COMx            (загрузчик: ID МК, опции; только чтение)\n"
           "  gammapult flash   --port COMx [--bin file.bin] [--no-dbank-fix]   (прошить; без --bin — встроенная)\n"
           "  gammapult dump    --port COMx --out DIR            (вся флеш и опции через загрузчик, дважды со сверкой)\n"
           "  gammapult setpage erase|restore --port COMx [--from DIR]  (страница настроек 0x0801F000)\n"
           "  gammapult hvpeak  --port COMx --start V [--target 1190] [--lo 300] [--hi 6000] [--count 3000] [--timeout 120] [--off]\n"
           "  gammapult tc      --port COMx [--secs 120] [--points N] [--lo 300] [--hi 6000]\n"
           "  gammapult tpl collect --port COMx [--n 2000] --out raw.bin [--baud 3000000]   (пишет по ходу)\n"
           "  gammapult tpl build raw.bin [--out tpl.h] [--tail-out tail.bin] [--load --port COMx [--save]]\n"
           "  gammapult log status|off|start|stop|time --port COMx      (регистратор: состояние/управление)\n"
           "  gammapult log on <период_с> [выдержка_с] --port COMx     (автозапуск регистратора)\n"
           "  gammapult log erase --yes --port COMx                   (стереть журнал)\n"
           "  gammapult log dump --port COMx --out <папка> [--session N] [--lo 300 --hi 6000] [--thermo] [--sum]\n");
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

    if (sub == "fit" && argc >= 3) {          // подгонка пика по CSV channel,count — без прибора
        FILE* f = fopen(argv[2], "r");
        if (!f) { printf("не открыть %s\n", argv[2]); return 2; }
        static uint32_t sp[Device::CHANNELS] = { 0 };
        char line[128]; int ch; unsigned v;
        while (fgets(line, sizeof line, f)) if (sscanf(line, "%d,%u", &ch, &v) == 2 && ch >= 0 && ch < Device::CHANNELS) sp[ch] = v;
        fclose(f);
        int lo = atoi(opt(argc, argv, "--lo", "300").c_str()), hi = atoi(opt(argc, argv, "--hi", "7000").c_str());
        peakfit::Result r = peakfit::fit(sp, Device::CHANNELS, lo, hi);
        if (!r.ok) { printf("пик не найден\n"); return 4; }
        printf("пик %.2f  σ %.2f  FWHM %.3f ± %.3f %%\n", r.peak, r.sigma, r.fwhm_pct, r.fwhm_err);
        return 0;
    }
    if (sub == "calenc" && argc >= 7) {       // E = c0 + c1·x + … → команды -cal (формат BecqMoni)
        double c[5]; for (int i = 0; i < 5; i++) c[i] = atof(argv[2 + i]);
        char cmds[11][40]; peakfit::becq_cal_commands(c, cmds);
        for (auto& s : cmds) printf("%s\n", s);
        return 0;
    }

    // эталон из файла — без прибора (с --load нужен порт)
    tpl::Result tr;
    tpl::Tail tt;
    int tail_rc = 0;
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
        // таблица хвоста (разбор наложений) — по тем же одиночным и новому эталону
        tt = tpl::build_tail(s, tr);
        printf("таблица хвоста: %s\n", tt.msg.c_str());
        if (tt.ok) {
            printf("строки (время от максимума, отсч. / масштаб):");
            for (int r = 0; r < tpl::TL_ROWS; r++) printf(" %.3f/%.4f", tt.t_row[r], tt.k_row[r]);
            printf("\n");
        }
        std::string tout = opt(argc, argv, "--tail-out");
        if (!tout.empty()) {
            if (!tt.ok) { printf("таблица хвоста не построена, %s не записан\n", tout.c_str()); tail_rc = 4; }
            else if (tpl::save_tail_bin(tt, tout)) printf("таблица хвоста -> %s (2112 байт)\n", tout.c_str());
            else { printf("не записать %s\n", tout.c_str()); tail_rc = 2; }
        }
        bool load = false;
        for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--load") == 0) load = true;
        if (!load) return tail_rc;
    }

    std::string port = opt(argc, argv, "--port");
    if (port.empty()) { printf("нужен --port COMx\n"); return 1; }

    // загрузчик МК — без сессии shproto (порт открывает сам)
    auto clog = [](const std::string& m) { printf("  %s\n", m.c_str()); fflush(stdout); };
    if (sub == "bootinfo") {
        fwflash::Info in; std::string err;
        bool ok = fwflash::info(port, in, clog, err);
        if (!ok) printf("ошибка: %s\n", err.c_str());
        return ok ? 0 : 6;
    }
    if (sub == "obtest") {                      // проверка пути записи опций: DBANK=0 поверх DBANK=0
        stm32boot::Boot b(clog);
        if (!stm32boot::Boot::kick_firmware(port, clog) || !b.open(port)) { printf("ошибка: нет загрузчика: %s\n", b.error.c_str()); return 6; }
        std::vector<uint8_t> ob0, ob1;
        if (!b.read_ob(ob0)) { printf("ошибка чтения опций: %s\n", b.error.c_str()); return 6; }
        auto dump = [](const std::vector<uint8_t>& o) { std::string s; char h[12]; for (size_t i = 0; i < o.size(); i += 4) { snprintf(h, sizeof h, " %08X", (unsigned)(o[i] | (o[i+1] << 8) | (o[i+2] << 16) | ((uint32_t)o[i+3] << 24))); s += h; } return s; };
        printf("до:   %s\n", dump(ob0).c_str());
        uint32_t w0 = ob0[0] | (ob0[1] << 8) | (ob0[2] << 16) | ((uint32_t)ob0[3] << 24);
        if (w0 & stm32boot::OPTR_DBANK) { printf("DBANK=1 — это уже не тест, отмена\n"); b.go(stm32boot::FLASH_BASE); return 6; }
        if (!b.write_ob_dbank0(ob0)) { printf("ошибка записи опций: %s\n", b.error.c_str()); return 6; }
        printf("  опции записаны, жду перезагрузки МК (BOOT0 на земле — стартует прошивка)…\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        stm32boot::Boot b2(clog);
        bool fw = stm32boot::Boot::kick_firmware(port, clog);
        if (!b2.open(port)) { printf("ошибка: после записи загрузчик недоступен (прошивка %s): %s\n", fw ? "отвечала" : "НЕ отвечала", b2.error.c_str()); return 6; }
        if (!b2.read_ob(ob1)) { printf("ошибка чтения опций после записи: %s\n", b2.error.c_str()); return 6; }
        printf("после:%s\n", dump(ob1).c_str());
        bool same = ob0 == ob1;
        printf("%s\n", same ? "блок опций не изменился — путь записи работает" : "БЛОК ОПЦИЙ ИЗМЕНИЛСЯ");
        b2.go(stm32boot::FLASH_BASE);
        return same ? 0 : 6;
    }
    // dump --out DIR: вся флеш (128 КБ) и опции через загрузчик, дважды со сверкой; затем запуск прошивки.
    // setpage erase | restore --from DIR: только страница настроек 0x0801F000 (4 КБ, DBANK=0).
    // Вход в загрузчик — "!boot": прошивка сама выключает ВВ; после go() автозапуск ВВ (если записан) поднимет его снова.
    if (sub == "dump" || sub == "setpage") {
        const uint32_t FLASH_LEN = 0x20000;
        auto u32 = [](const std::vector<uint8_t>& o, size_t i) { return (uint32_t)(o[i] | (o[i+1] << 8) | (o[i+2] << 16) | ((uint32_t)o[i+3] << 24)); };
        std::string mode = sub == "setpage" && argc >= 3 ? argv[2] : "";
        if (sub == "setpage" && mode != "erase" && mode != "restore") { printf("нужно: setpage erase | setpage restore --from DIR\n"); return 1; }
        std::string dir = opt(argc, argv, sub == "dump" ? "--out" : "--from", "");
        if (dir.empty() && !(sub == "setpage" && mode == "erase")) { printf("нужен %s DIR\n", sub == "dump" ? "--out" : "--from"); return 1; }
        std::vector<uint8_t> page;
        if (mode == "restore") {                 // страница из дампа — до входа в загрузчик
            FILE* f = fopen((dir + "/flash.bin").c_str(), "rb");
            if (!f) { printf("нет %s/flash.bin\n", dir.c_str()); return 1; }
            std::vector<uint8_t> all(FLASH_LEN);
            size_t n = fread(all.data(), 1, all.size(), f); fclose(f);
            if (n != FLASH_LEN) { printf("flash.bin не 128 КБ (%zu)\n", n); return 1; }
            uint32_t off = stm32boot::SETTINGS_ADDR - stm32boot::FLASH_BASE;
            page.assign(all.begin() + off, all.begin() + off + stm32boot::PAGE_SB);
        }
        stm32boot::Boot b(clog);
        if (!stm32boot::Boot::kick_firmware(port, clog) || !b.open(port)) { printf("ошибка: нет загрузчика: %s\n", b.error.c_str()); return 6; }
        std::vector<uint8_t> ob;
        if (!b.read_ob(ob)) { printf("ошибка чтения опций: %s\n", b.error.c_str()); return 6; }
        if (u32(ob, 0) & stm32boot::OPTR_DBANK) { printf("DBANK=1 — страница настроек в другом месте, отмена\n"); b.go(stm32boot::FLASH_BASE); return 6; }
        auto read_all = [&](std::vector<uint8_t>& out) {
            out.assign(FLASH_LEN, 0);
            for (uint32_t off = 0; off < FLASH_LEN; off += 256)
                if (!b.read(stm32boot::FLASH_BASE + off, out.data() + off, 256)) return false;
            return true;
        };
        int rc = 0;
        if (sub == "dump") {
            std::vector<uint8_t> a, c;
            printf("  чтение флеш, проход 1…\n"); fflush(stdout);
            if (!read_all(a)) { printf("ошибка чтения: %s\n", b.error.c_str()); rc = 6; }
            printf("  чтение флеш, проход 2…\n"); fflush(stdout);
            if (!rc && !read_all(c)) { printf("ошибка чтения: %s\n", b.error.c_str()); rc = 6; }
            if (!rc && a != c) { printf("ОШИБКА: два чтения различаются\n"); rc = 6; }
            if (!rc) {
                std::filesystem::create_directories(dir);
                FILE* f = fopen((dir + "/flash.bin").c_str(), "wb"); fwrite(a.data(), 1, a.size(), f); fclose(f);
                f = fopen((dir + "/ob.bin").c_str(), "wb"); fwrite(ob.data(), 1, ob.size(), f); fclose(f);
                printf("  записано: %s/flash.bin (%zu байт, два чтения совпали), ob.bin (OPTR %08X)\n", dir.c_str(), a.size(), (unsigned)u32(ob, 0));
            }
        } else {
            const uint16_t pg = (uint16_t)((stm32boot::SETTINGS_ADDR - stm32boot::FLASH_BASE) / stm32boot::PAGE_SB);
            printf("  стирание страницы %u (0x%08X)…\n", (unsigned)pg, (unsigned)stm32boot::SETTINGS_ADDR); fflush(stdout);
            if (!b.erase_pages(pg, 1)) { printf("ошибка стирания: %s\n", b.error.c_str()); rc = 6; }
            if (!rc && mode == "restore") {
                for (uint32_t off = 0; off < stm32boot::PAGE_SB && !rc; off += 256)
                    if (!b.write(stm32boot::SETTINGS_ADDR + off, page.data() + off, 256)) { printf("ошибка записи: %s\n", b.error.c_str()); rc = 6; }
            }
            if (!rc) {                           // сверка чтением
                std::vector<uint8_t> rb(stm32boot::PAGE_SB);
                for (uint32_t off = 0; off < stm32boot::PAGE_SB && !rc; off += 256)
                    if (!b.read(stm32boot::SETTINGS_ADDR + off, rb.data() + off, 256)) { printf("ошибка чтения: %s\n", b.error.c_str()); rc = 6; }
                if (!rc) {
                    bool ok = mode == "restore" ? rb == page : std::all_of(rb.begin(), rb.end(), [](uint8_t x) { return x == 0xFF; });
                    printf("  %s\n", ok ? (mode == "restore" ? "страница восстановлена и сверена" : "страница стёрта (вся 0xFF)") : "ОШИБКА: сверка не сошлась");
                    if (!ok) rc = 6;
                }
            }
        }
        b.go(stm32boot::FLASH_BASE);
        printf("  запуск прошивки\n");
        return rc;
    }
    if (sub == "flash") {
        std::vector<uint8_t> img; std::string tag, err;
        std::string bin = opt(argc, argv, "--bin");
        if (!bin.empty()) { if (!fwflash::load_file(bin, img, err)) { printf("ошибка: %s\n", err.c_str()); return 2; } tag = bin; }
        else if (!fwflash::embedded(img, tag)) { printf("встроенного образа нет — укажи --bin\n"); return 2; }
        printf("образ: %s, %zu байт\n", tag.c_str(), img.size());
        bool fix = true;
        for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--no-dbank-fix") == 0) fix = false;
        int last = -1;
        bool ok = fwflash::flash(port, img, fix, clog, [&](double f) { int p = (int)(f * 20); if (p != last) { last = p; printf("  %3d %%\r", (int)(f * 100)); fflush(stdout); } }, err);
        printf("\n%s\n", ok ? "готово: прибор прошит и отвечает" : ("ошибка: " + err).c_str());
        return ok ? 0 : 7;
    }

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
    } else if (sub == "hvpeak") {
        double v0 = atof(opt(argc, argv, "--start", "0").c_str());
        if (v0 < 300 || v0 > 1200) { printf("нужен --start 300…1200 В\n"); dev.disconnect(); return 1; }
        dev.hvp.target = atof(opt(argc, argv, "--target", "1190").c_str());
        dev.hvp.lo = atof(opt(argc, argv, "--lo", "300").c_str());
        dev.hvp.hi = atof(opt(argc, argv, "--hi", "6000").c_str());
        dev.hvp.count = atof(opt(argc, argv, "--count", "3000").c_str());
        dev.hvp.timeout = atof(opt(argc, argv, "--timeout", "120").c_str());
        dev.hvp_start(v0);
        std::string last; size_t shown = 0;
        while (dev.hvp.running) {
            pump(dev, 0.2);
            if (dev.hvp.msg != last) { last = dev.hvp.msg; printf("  %s\n", last.c_str()); fflush(stdout); }
            while (shown < dev.hvp.hist.size()) { auto& h = dev.hvp.hist[shown++]; printf("  шаг %zu: %.0f В → пик %.2f\n", shown, h.first, h.second); fflush(stdout); }
        }
        printf("%s\n", dev.hvp.msg.c_str());
        bool off = false;
        for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--off") == 0) off = true;
        if (off) { dev.send_cmd("-hv off"); pump(dev, 0.5); printf("ВВ выключено\n"); }
        else printf("ВВ оставлено: %.0f В\n", fabs(dev.gnum("hv_set")));
        rc = dev.hvp.done ? 0 : 4;
    } else if (sub == "tc") {
        dev.tc.secs = atof(opt(argc, argv, "--secs", "120").c_str());
        dev.tc.lo = atof(opt(argc, argv, "--lo", "300").c_str());
        dev.tc.hi = atof(opt(argc, argv, "--hi", "6000").c_str());
        int npts = atoi(opt(argc, argv, "--points", "5").c_str());
        dev.tc_start();
        size_t shown = 0;
        while (dev.tc.running && (int)dev.tc.pts.size() < npts) {
            pump(dev, 0.2);
            while (shown < dev.tc.pts.size()) { auto& p = dev.tc.pts[shown++]; printf("  точка %zu: %.2f °C  пик %.2f  σ %.2f  n %llu\n", shown, p.t, p.peak, p.sd, (unsigned long long)p.n); fflush(stdout); }
        }
        if (dev.tc.running) dev.tc_stop(true);
        while (dev.queue_busy()) pump(dev, 0.05);
        double k1, k2, rms, t0 = 0;
        for (auto& p : dev.tc.pts) t0 += p.t;
        if (!dev.tc.pts.empty()) t0 /= dev.tc.pts.size();
        double tmn = 1e9, tmx = -1e9;
        for (auto& p : dev.tc.pts) { if (p.t < tmn) tmn = p.t; if (p.t > tmx) tmx = p.t; }
        if (tmx - tmn < 5) printf("внимание: размах температуры %.2f °C < 5 °C — k1 ненадёжен, нужен прогрев/остывание\n", tmx - tmn);
        if (dev.tc_fit(t0, 1, k1, k2, rms)) printf("линейно: t0 %.2f °C, k1 = %.4e /°C (%.3f %%/°C), остаток %.3f %%\n", t0, k1, 100 * k1, rms);
        else printf("для оценки k1 мало точек или нет разброса температуры\n");
        printf("в приборе возвращено: %s\n", dev.tc_reply.c_str());
    } else if (tpl_build) {
        // по шагам (tpl::TplLoad): таблица и -save — только после -tplok без ошибок
        bool save = false;
        for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--save") == 0) save = true;
        if (!tt.ok) printf("таблицы хвоста нет: разбор наложений в приборе будет выключен (pile=-)\n");
        tpl::TplLoad L;
        int e0 = dev.queue_errors;
        dev.queue(L.start(tr, tt, save));
        while (!L.done()) {
            while (dev.queue_busy()) pump(dev, 0.05);
            auto v = L.next(dev.queue_errors - e0, dev.queue_last_err);
            e0 = dev.queue_errors;
            dev.queue(v);
        }
        if (!L.tpl_ok) printf("эталон не принят: %s; таблица хвоста и -save не отправлялись\n", L.tpl_err.c_str());
        else printf("эталон принят\n");
        if (L.tail_sent) printf("таблица хвоста: %s\n", L.tail_ok ? "принята, разбор наложений включён" : ("не принята, разбор наложений не включён: " + L.tail_err).c_str());
        if (L.save_sent) printf("-save: %s\n", L.save_ok ? "ok" : L.save_err.c_str());
        printf("ответ: %s\n", dev.last_reply.c_str());
        rc = !L.tpl_ok || (L.save_sent && !L.save_ok) ? 5 : (L.tail_sent && !L.tail_ok) ? 6 : tail_rc;
    } else if (sub == "tpl" && argc >= 3 && std::string(argv[2]) == "collect") {
        int n = atoi(opt(argc, argv, "--n", "2000").c_str());
        std::string out = opt(argc, argv, "--out");
        if (out.empty()) { printf("нужен --out raw.bin\n"); dev.disconnect(); return 1; }
        uint32_t baud = (uint32_t)strtoul(opt(argc, argv, "--baud", "600000").c_str(), nullptr, 10);
        if (!ds::baud_allowed(baud)) { printf("--baud: 600000, 1000000, 1500000, 2000000 или 3000000\n"); dev.disconnect(); return 1; }
        FILE* f = fopen(out.c_str(), "wb");
        if (!f) { printf("не открыть %s\n", out.c_str()); dev.disconnect(); return 3; }
        if (baud != ds::BAUD_DEFAULT) {
            std::string err;
            if (!dev.set_baud(baud, err)) { printf("скорость %u: %s\n", baud, err.c_str()); fclose(f); dev.disconnect(); return 6; }
            printf("скорость UART %u\n", baud);
        }
        dev.raw_start(n);
        // порции — на диск по мере прихода (и из памяти вон): при сбое собранное остаётся в файле
        size_t written = 0, shown = 0, w_last = 0;
        bool wok = true;
        auto t_start = std::chrono::steady_clock::now(), t_last = t_start;
        auto drain = [&]() {
            bool ok = true;
            written += tpl::append_blocks(f, dev.raw.blocks, ok);
            if (!ok && wok) { wok = false; printf("ошибка записи в %s — сбор прерван\n", out.c_str()); dev.raw_abort(); }
        };
        while (dev.raw.running) {
            pump(dev, 0.05);
            if (!dev.raw.blocks.empty()) drain();
            if (written >= shown + 100) {
                auto now = std::chrono::steady_clock::now();
                double dt = std::chrono::duration<double>(now - t_last).count();
                double rate = dt > 0 ? (written - w_last) / dt : 0.0;
                shown = written; w_last = written; t_last = now;
                printf("  порций %zu/%d, сбоев %d, %.1f порций/с\n", written, n, dev.raw.bad, rate);
                fflush(stdout);
            }
        }
        drain();
        fclose(f);
        double tot = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
        printf("%s -> %s (%zu порций, %.1f порций/с в среднем)\n", dev.raw.msg.c_str(), out.c_str(), written,
               tot > 0 ? written / tot : 0.0);
        if (!wok) rc = 3;
        if (dev.baud() != ds::BAUD_DEFAULT) {   // вернуть 600000 явно (disconnect сделал бы то же)
            std::string err;
            if (!dev.set_baud(ds::BAUD_DEFAULT, err)) printf("возврат на 600000: %s\n", err.c_str());
        }
    } else if (sub == "log") {
        std::string a2 = argc >= 3 ? argv[2] : "status";
        auto has = [&](const char* k) { for (int i = 1; i < argc; i++) if (strcmp(argv[i], k) == 0) return true; return false; };
        auto ask = [&](const std::string& c, double secs) {
            dev.log_status.clear();
            dev.send_cmd(c, true);
            for (double t = 0; t < secs && dev.log_status.empty(); t += 0.05) pump(dev, 0.05);
            printf("%s\n", dev.log_status.empty() ? "нет ответа" : dev.log_status.c_str());
            return starts_with_ok(dev.log_status);
        };
        if (a2 == "status")      rc = ask("-log", 2) ? 0 : 3;
        else if (a2 == "off")    rc = ask("-log off", 2) ? 0 : 3;
        else if (a2 == "start")  rc = ask("-log start", 2) ? 0 : 3;
        else if (a2 == "stop")   rc = ask("-log stop", 2) ? 0 : 3;
        else if (a2 == "on") {
            std::string c = "-log on";
            if (argc >= 4 && argv[3][0] != '-') c += std::string(" ") + argv[3];
            if (argc >= 5 && argv[4][0] != '-') c += std::string(" ") + argv[4];
            rc = ask(c, 2) ? 0 : 3;
        } else if (a2 == "time") {
            char c[48]; snprintf(c, sizeof c, "-log time %lld", (long long)time(nullptr));
            rc = ask(c, 2) ? 0 : 3;
        } else if (a2 == "erase") {
            if (!has("--yes")) { printf("стирание журнала необратимо — добавь --yes\n"); rc = 1; }
            else {
                rc = ask("-log erase", 2) ? 0 : 3;
                if (!rc) {                       // ждать конца стирания (≈ 20 с)
                    for (int i = 0; i < 120; i++) { pump(dev, 0.5); dev.log_status.clear(); dev.send_cmd("-log", true); pump(dev, 0.3);
                        if (!dev.log_status.empty() && dev.log_status.find("erasing") == std::string::npos) break; }
                    printf("%s\n", dev.log_status.c_str());
                }
            }
        } else if (a2 == "dump") {
            std::string out = opt(argc, argv, "--out");
            if (out.empty()) { printf("нужен --out <папка>\n"); dev.disconnect(); return 1; }
            uint32_t only = (uint32_t)atoi(opt(argc, argv, "--session", "0").c_str());
            int lo = atoi(opt(argc, argv, "--lo", "300").c_str()), hi = atoi(opt(argc, argv, "--hi", "6000").c_str());
            dev.logd_start(only);
            std::string last;
            while (dev.logd.running) {
                pump(dev, 0.1);
                if (dev.logd.msg != last) { last = dev.logd.msg; printf("  %s\r", last.c_str()); fflush(stdout); }
            }
            printf("\n%s\n", dev.logd.msg.c_str());
            std::string err;
            if (!dev.logd.done) rc = 4;
            else if (!logdump::write_dir(out, dev.logd.recs, lo, hi, err)) { printf("ошибка: %s\n", err.c_str()); rc = 3; }
            else {
                printf("выгружено в %s%s%s\n", out.c_str(), err.empty() ? "" : " — ", err.c_str());
                for (uint32_t sid : logdump::sessions(dev.logd.recs)) {
                    auto s = logdump::session(dev.logd.recs, sid);
                    double t0 = logdump::temp_c(s.front()->h.t_start_c100), t1 = logdump::temp_c(s.back()->h.t_end_c100);
                    printf("  сессия %u: записей %zu, %.1f ч, T %.2f → %.2f °C\n", sid, s.size(), s.back()->h.t_end_ms / 3.6e6, t0, t1);
                    if (has("--thermo")) {   // пик против температуры → k1/k2 тем же МНК, что вкладка «Термо»
                        auto pk = logdump::peaks(s, lo, hi);
                        dev.tc.pts.clear();
                        for (auto& p : pk)
                            if (p.ok && std::isfinite(p.temp) && !(p.flags & (LOGF_HOST_RST | LOGF_HOST_STO | LOGF_HV_TRIP | LOGF_TC_ON))
                                && (!std::isfinite(p.temp_span) || p.temp_span <= 1.0))
                                dev.tc.pts.push_back({ p.temp, p.peak, p.sigma, p.n });
                        double tmn = 1e9, tmx = -1e9, tm = 0;
                        for (auto& q : dev.tc.pts) { tm += q.t; if (q.t < tmn) tmn = q.t; if (q.t > tmx) tmx = q.t; }
                        if (!dev.tc.pts.empty()) tm /= dev.tc.pts.size();
                        double k1, k2, rms;
                        printf("    термо: точек %zu, T %.2f…%.2f °C\n", dev.tc.pts.size(), tmn, tmx);
                        if (dev.tc_fit(25.0, 1, k1, k2, rms)) printf("    линейно (t0 25): k1 %.4e /°C (%.3f %%/°C), остаток %.3f %%\n", k1, 100 * k1, rms);
                        if (dev.tc_fit(25.0, 2, k1, k2, rms)) printf("    квадратично (t0 25): k1 %.4e, k2 %.4e, остаток %.3f %%  → -tc 25 %.6g %.6g\n", k1, k2, rms, k1, k2);
                        (void)tm;
                    }
                    if (has("--sum")) {
                        std::vector<double> sum; std::string msg;
                        if (logdump::aligned_sum(s, lo, hi, sum, msg)) {
                            char n[64]; snprintf(n, sizeof n, "/sum_aligned_s%u.csv", sid);
                            FILE* f = _wfopen(std::filesystem::u8path(out + n).wstring().c_str(), L"w");
                            if (f) { fprintf(f, "channel,count\n"); for (int i = 0; i < (int)sum.size(); i++) fprintf(f, "%d,%.0f\n", i, sum[i]); fclose(f); }
                            printf("    %s → sum_aligned_s%u.csv\n", msg.c_str(), sid);
                        } else printf("    сложение: %s\n", msg.c_str());
                    }
                }
            }
        } else { usage(); rc = 1; }
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
