// stm32boot.cpp — см. stm32boot.h. Протокол AN3155: команда = байт + дополнение, ACK 0x79 / NACK 0x1F;
// адрес — 4 байта старшим вперёд + XOR; данные — (N−1), байты, XOR.
#include "stm32boot.h"
#include "shproto.h"
#include <chrono>
#include <thread>
#include <cstdio>

namespace stm32boot {

static const uint8_t ACK = 0x79, NACK = 0x1F;

static double now_ms()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

bool Boot::kick_firmware(const std::string& port, Log log)
{
    serialport::Serial s;
    if (!s.open(port, 600000)) { if (log) log("не открыть " + port); return false; }
    shproto::Parser p;
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        const char* k = "!boot";
        s.write((const uint8_t*)k, 5);
        double t0 = now_ms();
        uint8_t buf[512];
        while (now_ms() - t0 < 500 && !ok) {
            int n = s.read(buf, sizeof buf);
            if (n > 0)
                p.feed(buf, (size_t)n, [&](const shproto::Frame& f) {
                    if (f.cmd == shproto::CMD_TEXT) {
                        std::string t((const char*)f.payload.data(), f.payload.size());
                        if (t.find("-ok boot") != std::string::npos) ok = true;
                    }
                });
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    s.close();
    if (log) log(ok ? "прошивка ушла в загрузчик (ВВ выключено)" : "ответа «-ok boot» нет");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    return ok;
}

bool Boot::rx(uint8_t* buf, int n, int ms)
{
    int got = 0; double t0 = now_ms();
    while (got < n) {
        int r = s_.read(buf + got, n - got);
        if (r < 0) { error = "ошибка порта"; return false; }
        if (r > 0) { got += r; t0 = now_ms(); continue; }
        if (now_ms() - t0 > ms) { error = "таймаут ответа загрузчика"; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool Boot::wait_ack(int ms)
{
    uint8_t b = 0;
    if (!rx(&b, 1, ms)) return false;
    if (b == ACK) return true;
    char e[48]; snprintf(e, sizeof e, b == NACK ? "загрузчик ответил NACK" : "неожиданный ответ 0x%02X", b);
    error = e;
    return false;
}

bool Boot::cmd(uint8_t c)
{
    uint8_t f[2] = { c, (uint8_t)~c };
    s_.write(f, 2);
    return wait_ack(1000);
}

bool Boot::send_addr(uint32_t a)
{
    uint8_t f[5] = { (uint8_t)(a >> 24), (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a, 0 };
    f[4] = f[0] ^ f[1] ^ f[2] ^ f[3];
    s_.write(f, 5);
    return wait_ack(1000);
}

bool Boot::open(const std::string& port, int baud)
{
    close();
    if (!s_.open(port, baud, true)) { error = "не открыть " + port + " (занят?)"; return false; }
    for (int i = 0; i < 4; i++) {
        uint8_t sync = 0x7F;
        s_.write(&sync, 1);
        uint8_t b = 0;
        if (rx(&b, 1, 400) && (b == ACK || b == NACK)) {   // NACK — уже синхронизирован ранее
            error.clear();
            return true;
        }
    }
    error = "загрузчик не отвечает на 0x7F (BOOT0 на +3.3 В при включении? порт тот?)";
    s_.close();
    return false;
}

void Boot::close() { s_.close(); }

bool Boot::get_id(uint16_t& pid)
{
    if (!cmd(0x02)) return false;
    uint8_t b[3];
    if (!rx(b, 3, 500)) return false;              // N = 1, PID[2]
    pid = (uint16_t)((b[1] << 8) | b[2]);
    return wait_ack(500);
}

bool Boot::get_version(uint8_t& ver)
{
    if (!cmd(0x01)) return false;
    uint8_t b[3];
    if (!rx(b, 3, 500)) return false;
    ver = b[0];
    return wait_ack(500);
}

bool Boot::read(uint32_t addr, uint8_t* buf, int n)
{
    if (n < 1 || n > 256) return false;
    if (!cmd(0x11) || !send_addr(addr)) return false;
    uint8_t f[2] = { (uint8_t)(n - 1), (uint8_t)~(n - 1) };
    s_.write(f, 2);
    if (!wait_ack(1000)) return false;
    return rx(buf, n, 1000);
}

bool Boot::write(uint32_t addr, const uint8_t* buf, int n)
{
    if (n < 4 || n > 256 || n % 4) return false;
    if (!cmd(0x31) || !send_addr(addr)) return false;
    std::vector<uint8_t> f(n + 2);
    f[0] = (uint8_t)(n - 1);
    uint8_t x = f[0];
    for (int i = 0; i < n; i++) { f[1 + i] = buf[i]; x ^= buf[i]; }
    f[n + 1] = x;
    s_.write(f.data(), (int)f.size());
    return wait_ack(2000);
}

bool Boot::mass_erase()
{
    if (!cmd(0x44)) return false;
    uint8_t f[3] = { 0xFF, 0xFF, 0x00 };           // 0xFFFF — глобальное стирание, XOR = 0x00
    s_.write(f, 3);
    return wait_ack(30000);
}

bool Boot::erase_pages(uint16_t first, uint16_t count)
{
    if (count == 0) return true;
    if (!cmd(0x44)) return false;
    std::vector<uint8_t> f;
    uint16_t nm1 = (uint16_t)(count - 1);
    f.push_back((uint8_t)(nm1 >> 8)); f.push_back((uint8_t)nm1);
    for (uint16_t i = 0; i < count; i++) { uint16_t p = (uint16_t)(first + i); f.push_back((uint8_t)(p >> 8)); f.push_back((uint8_t)p); }
    uint8_t x = 0; for (uint8_t b : f) x ^= b;
    f.push_back(x);
    s_.write(f.data(), (int)f.size());
    return wait_ack(30000);
}

bool Boot::go(uint32_t addr)
{
    return cmd(0x21) && send_addr(addr);
}

bool Boot::read_ob(std::vector<uint8_t>& ob)
{
    ob.assign(OB_LEN, 0);
    return read(OB_BANK1, ob.data(), (int)OB_LEN);
}

bool Boot::write_ob_dbank0(const std::vector<uint8_t>& ob0)
{
    if (ob0.size() != OB_LEN) { error = "блок OB не прочитан"; return false; }
    std::vector<uint8_t> ob = ob0;
    uint32_t w0 = ob[0] | (ob[1] << 8) | (ob[2] << 16) | ((uint32_t)ob[3] << 24);
    uint32_t w1 = ob[4] | (ob[5] << 8) | (ob[6] << 16) | ((uint32_t)ob[7] << 24);
    if ((w0 & 0xFF) != 0xAA) { error = "RDP не уровень 0 — запись опций запрещена этим инструментом"; return false; }
    bool compl_word = (w1 == ~w0);
    uint32_t n0 = w0 & ~OPTR_DBANK;
    for (int i = 0; i < 4; i++) ob[i] = (uint8_t)(n0 >> (8 * i));
    if (compl_word) { uint32_t n1 = ~n0; for (int i = 0; i < 4; i++) ob[4 + i] = (uint8_t)(n1 >> (8 * i)); }
    char m[96]; snprintf(m, sizeof m, "FLASH_OPTR 0x%08X → 0x%08X (DBANK=0)%s", w0, n0, compl_word ? ", дополнение обновлено" : "");
    say(m);
    if (!cmd(0x31) || !send_addr(OB_BANK1)) return false;
    std::vector<uint8_t> f(OB_LEN + 2);
    f[0] = (uint8_t)(OB_LEN - 1);
    uint8_t x = f[0];
    for (uint32_t i = 0; i < OB_LEN; i++) { f[1 + i] = ob[i]; x ^= ob[i]; }
    f[OB_LEN + 1] = x;
    s_.write(f.data(), (int)f.size());
    wait_ack(3000);                                // после записи МК сбрасывается — ACK может не прийти
    s_.close();
    return true;
}

bool Boot::flash_image(const std::vector<uint8_t>& img0, Progress prog, bool mass)
{
    std::vector<uint8_t> img = img0;
    while (img.size() % 8) img.push_back(0xFF);    // запись флеш G4 — двойными словами
    if (FLASH_BASE + img.size() > SETTINGS_ADDR) { error = "образ залезает на страницу настроек 0x0801F000"; return false; }
    if (mass) {
        say("стирание всей флеш…");
        if (!mass_erase()) { error = "стирание: " + error; return false; }
    } else {
        uint16_t np = (uint16_t)((img.size() + PAGE_SB - 1) / PAGE_SB);
        char m[200]; snprintf(m, sizeof m, "стирание страниц 0…%u (настройки в 0x0801F000 сохраняются)…", np - 1);
        say(m);
        if (!erase_pages(0, np)) { error = "стирание: " + error; return false; }
    }
    size_t n = img.size();
    for (size_t off = 0; off < n; off += 256) {
        int len = (int)((n - off) < 256 ? (n - off) : 256);
        if (!write(FLASH_BASE + (uint32_t)off, img.data() + off, len)) {
            char e[64]; snprintf(e, sizeof e, "запись по 0x%08X: ", (unsigned)(FLASH_BASE + off));
            error = e + error; return false;
        }
        if (prog) prog(0.5 * (off + len) / n);
    }
    say("сверка…");
    std::vector<uint8_t> rb(256);
    for (size_t off = 0; off < n; off += 256) {
        int len = (int)((n - off) < 256 ? (n - off) : 256);
        if (!read(FLASH_BASE + (uint32_t)off, rb.data(), len)) { error = "чтение при сверке: " + error; return false; }
        for (int i = 0; i < len; i++)
            if (rb[i] != img[off + i]) {
                char e[80]; snprintf(e, sizeof e, "сверка: расхождение по 0x%08X", (unsigned)(FLASH_BASE + off + i));
                error = e; return false;
            }
        if (prog) prog(0.5 + 0.5 * (off + len) / n);
    }
    return true;
}

} // namespace stm32boot
