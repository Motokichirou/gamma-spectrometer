// fwflash.cpp — см. fwflash.h
#include "fwflash.h"
#include "shproto.h"
#include "fw_embed.h"
#include <cstdio>
#include <chrono>
#include <thread>

namespace fwflash {

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool embedded(std::vector<uint8_t>& img, std::string& tag)
{
    if (g_fw_len == 0) return false;
    img.assign(g_fw_bin, g_fw_bin + g_fw_len);
    tag = g_fw_tag;
    return true;
}

bool load_file(const std::string& path, std::vector<uint8_t>& img, std::string& err)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "не открыть " + path; return false; }
    img.clear();
    uint8_t b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) img.insert(img.end(), b, b + n);
    fclose(f);
    if (img.size() < 1024 || img.size() > 128 * 1024) { err = "размер образа не похож на прошивку: " + std::to_string(img.size()); return false; }
    uint32_t sp = img[0] | (img[1] << 8) | (img[2] << 16) | ((uint32_t)img[3] << 24);
    if ((sp & 0xFFF00000u) != 0x20000000u) { err = "первое слово не указатель стека в SRAM — это не образ STM32"; return false; }
    return true;
}

static bool enter(stm32boot::Boot& b, const std::string& port, bool& fw_running, Log log, std::string& err)
{
    fw_running = stm32boot::Boot::kick_firmware(port, log);
    if (!b.open(port)) {
        err = b.error + (fw_running ? "" : ". Для голого прибора: BOOT0 (вывод 45) на +3.3 В через 10 кОм, затем переподключить USB.");
        return false;
    }
    return true;
}

static bool read_info(stm32boot::Boot& b, Info& in, Log log, std::string& err)
{
    if (!b.get_id(in.pid)) { err = "Get ID: " + b.error; return false; }
    b.get_version(in.blver);
    std::vector<uint8_t> ob;
    if (!b.read_ob(ob)) { err = "чтение опций: " + b.error; return false; }
    in.optr = ob[0] | (ob[1] << 8) | (ob[2] << 16) | ((uint32_t)ob[3] << 24);
    in.dbank = (in.optr & stm32boot::OPTR_DBANK) != 0;
    {
        std::string hex = "блок опций 0x1FFF7800:";
        char h[12];
        for (size_t i = 0; i < ob.size(); i += 4) { snprintf(h, sizeof h, " %08X", (unsigned)(ob[i] | (ob[i+1] << 8) | (ob[i+2] << 16) | ((uint32_t)ob[i+3] << 24))); hex += h; }
        if (log) log(hex);
    }
    uint8_t w[4];
    if (b.read(stm32boot::FLASH_BASE, w, 4)) in.blank = (w[0] & w[1] & w[2] & w[3]) == 0xFF;
    char m[160];
    snprintf(m, sizeof m, "МК: PID 0x%03X%s, загрузчик v%d.%d, FLASH_OPTR 0x%08X (DBANK=%d, RDP 0x%02X), флеш %s",
             in.pid, in.pid == stm32boot::PID_G47X ? " (STM32G47x)" : " — НЕ G47x!", in.blver >> 4, in.blver & 15,
             in.optr, in.dbank ? 1 : 0, in.optr & 0xFF, in.blank ? "пуста" : "с прошивкой");
    if (log) log(m);
    return true;
}

bool info(const std::string& port, Info& out, Log log, std::string& err)
{
    stm32boot::Boot b(log);
    if (!enter(b, port, out.fw_running, log, err)) return false;
    bool ok = read_info(b, out, log, err);
    if (ok && !out.blank) { b.go(stm32boot::FLASH_BASE); if (log) log("МК возвращён в прошивку (Go 0x08000000)"); }
    b.close();
    return ok;
}

static bool firmware_answers(const std::string& port, std::string& inf)
{
    serialport::Serial s;
    for (int i = 0; i < 20 && !s.open(port, 600000); i++) sleep_ms(200);
    if (!s.is_open()) return false;
    shproto::Parser p;
    bool ok = false;
    for (int attempt = 0; attempt < 6 && !ok; attempt++) {
        auto fr = shproto::build_text("-inf");
        s.write(fr.data(), (int)fr.size());
        for (int t = 0; t < 50 && !ok; t++) {
            uint8_t buf[512]; int n = s.read(buf, sizeof buf);
            if (n > 0) p.feed(buf, (size_t)n, [&](const shproto::Frame& f) {
                if (f.cmd == shproto::CMD_TEXT) {
                    std::string x((const char*)f.payload.data(), f.payload.size());
                    if (x.rfind("GammaSpec", 0) == 0) { inf = x; ok = true; }
                }
            });
            else sleep_ms(10);
        }
    }
    return ok;
}

bool flash(const std::string& port, const std::vector<uint8_t>& img, bool fix_dbank, Log log, Progress prog, std::string& err)
{
    if (img.empty()) { err = "нет образа прошивки"; return false; }
    stm32boot::Boot b(log);
    bool fw = false;
    if (!enter(b, port, fw, log, err)) return false;
    Info in;
    if (!read_info(b, in, log, err)) return false;
    if (in.pid != stm32boot::PID_G47X) { err = "это не STM32G47x — прошивка отменена"; return false; }
    bool dbank_changed = false;
    if (in.dbank) {
        dbank_changed = true;
        if (!fix_dbank) { err = "DBANK=1: настройки и калибровка gamma_fw не сохранятся; включи исправление DBANK"; return false; }
        if (log) log("DBANK=1 → ставлю DBANK=0 (запись опций, МК перезагрузится в загрузчик)");
        std::vector<uint8_t> ob;
        if (!b.read_ob(ob) || !b.write_ob_dbank0(ob)) { err = "запись опций: " + b.error; return false; }
        sleep_ms(1500);
        if (!b.open(port)) { err = "после записи опций загрузчик не отвечает: " + b.error + " (BOOT0 должен оставаться на +3.3 В)"; return false; }
        Info in2;
        if (!read_info(b, in2, log, err)) return false;
        if (in2.dbank) { err = "DBANK не сбросился"; return false; }
    }
    char m[96]; snprintf(m, sizeof m, "запись образа %zu байт…", img.size());
    if (log) log(m);
    bool mass = in.blank || dbank_changed;
    if (!b.flash_image(img, prog, mass)) { err = b.error; return false; }
    if (log) log("записано и сверено; запуск прошивки");
    b.go(stm32boot::FLASH_BASE);
    b.close();
    sleep_ms(1500);
    std::string inf;
    if (!firmware_answers(port, inf)) { err = "прошивка записана, но не ответила на -inf (переподключи USB, BOOT0 — на землю)"; return false; }
    if (log) log("прошивка отвечает: " + inf);
    return true;
}

} // namespace fwflash
