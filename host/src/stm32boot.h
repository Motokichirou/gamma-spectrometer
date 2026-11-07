// stm32boot.h — системный загрузчик STM32 по USART (протокол ST AN3155): прошивка голого МК без
// программатора. Прибор: USART1 PA9/PA10 через FT231X, 8E1. Вход в загрузчик — либо BOOT0 = 1 при
// включении (голый МК), либо команда "!boot" работающей прошивки gamma_fw (ВВ она выключает сама).
#pragma once
#include "serial_win.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace stm32boot {

constexpr uint32_t FLASH_BASE = 0x08000000;
constexpr uint32_t OB_BANK1   = 0x1FFF7800;   // блок опциональных байтов Bank 1 (вид загрузчика), 0x24 байта
constexpr uint32_t OB_LEN     = 0x24;
constexpr uint32_t OPTR_DBANK = 1u << 22;
constexpr uint16_t PID_G47X   = 0x469;
constexpr uint32_t PAGE_SB    = 0x1000;       // страница при DBANK=0
constexpr uint32_t SETTINGS_ADDR = 0x0801F000; // страница настроек gamma_fw (flashstore)

using Log = std::function<void(const std::string&)>;
using Progress = std::function<void(double)>;     // 0…1

class Boot {
public:
    explicit Boot(Log log = nullptr) : log_(log) {}
    ~Boot() { close(); }

    // работающая прошивка → загрузчик: "!boot" на 600000 8N1 (ответ "-ok boot"); false — нет ответа
    static bool kick_firmware(const std::string& port, Log log = nullptr);

    bool open(const std::string& port, int baud = 115200);   // 8E1 + синхронизация 0x7F
    void close();
    bool get_id(uint16_t& pid);
    bool get_version(uint8_t& ver);
    bool read(uint32_t addr, uint8_t* buf, int n);            // n ≤ 256
    bool write(uint32_t addr, const uint8_t* buf, int n);     // n ≤ 256, кратно 4
    bool mass_erase();                                        // расширенное стирание 0xFFFF (вся флеш)
    bool erase_pages(uint16_t first, uint16_t count);        // страницы (при DBANK=0 — по 4 КБ)
    bool go(uint32_t addr);

    bool read_ob(std::vector<uint8_t>& ob);                   // OB_LEN байт с OB_BANK1
    // DBANK = 0 (один банк, нужно для калибровки/настроек gamma_fw). Запись блока OB вызывает сброс МК
    // и перезагрузку опций; после неё загрузчик надо открыть заново (BOOT0 всё ещё = 1).
    bool write_ob_dbank0(const std::vector<uint8_t>& ob);

    // стереть (mass — всю флеш, иначе только страницы 4 КБ под образ: настройки в последней странице
    // 0x0801F000 сохраняются), записать образ с 0x08000000, сверить чтением
    bool flash_image(const std::vector<uint8_t>& img, Progress prog = nullptr, bool mass = false);

    std::string error;

private:
    serialport::Serial s_;
    Log log_;
    void say(const std::string& m) { if (log_) log_(m); }
    bool wait_ack(int ms);
    bool cmd(uint8_t c);
    bool send_addr(uint32_t a);
    bool rx(uint8_t* buf, int n, int ms);
};

} // namespace stm32boot
