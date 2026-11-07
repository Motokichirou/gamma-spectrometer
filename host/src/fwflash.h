// fwflash.h — прошивка прибора целиком (голый МК или обновление) поверх stm32boot. Блокирующие
// функции: GUI зовёт их из фонового потока, CLI — напрямую.
#pragma once
#include "stm32boot.h"
#include <string>
#include <vector>
#include <cstdint>

namespace fwflash {

using Log = stm32boot::Log;
using Progress = stm32boot::Progress;

bool embedded(std::vector<uint8_t>& img, std::string& tag);          // встроенный образ gamma_fw
bool load_file(const std::string& path, std::vector<uint8_t>& img, std::string& err);

struct Info {
    bool fw_running = false;      // прибор отвечал прошивкой и ушёл в загрузчик по "!boot"
    uint16_t pid = 0; uint8_t blver = 0;
    uint32_t optr = 0; bool dbank = false;
    bool blank = true;            // флеш пуста (первое слово 0xFFFFFFFF)
};

// только чтение: войти в загрузчик, ID, версия, опции; затем вернуть МК в прошивку (если она есть)
bool info(const std::string& port, Info& out, Log log, std::string& err);

// стереть, при необходимости DBANK=0, записать, сверить, запустить, проверить ответ прошивки
bool flash(const std::string& port, const std::vector<uint8_t>& img, bool fix_dbank,
           Log log, Progress prog, std::string& err);

} // namespace fwflash
