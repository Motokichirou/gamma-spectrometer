// shproto.h — протокол BecqMoni / AtomSpectra (порт из firmware/core/shproto.*)
//
// Кадр: 0xFF 0xFE [CMD][PAYLOAD...][CRC_LSB][CRC_MSB] 0xA5
// Стаффинг: 0xFE/0xFD/0xA5 внутри тела -> 0xFD + (~байт)
// CRC16-MODBUS (init 0xFFFF, poly 0xA001) по нестаффленным CMD+PAYLOAD.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <functional>

namespace shproto {

constexpr uint8_t KEEPALIVE = 0xFF;
constexpr uint8_t START     = 0xFE;
constexpr uint8_t ESC       = 0xFD;
constexpr uint8_t FINISH    = 0xA5;

constexpr uint8_t CMD_SPECTRUM = 0x01;
constexpr uint8_t CMD_PULSE    = 0x02;
constexpr uint8_t CMD_TEXT     = 0x03;
constexpr uint8_t CMD_STATUS   = 0x04;

uint16_t crc16_modbus(const uint8_t* data, size_t len);

// Готовый к отправке кадр (вкл. 0xFF, START, стаффинг, FINISH).
std::vector<uint8_t> build_frame(uint8_t cmd, const uint8_t* payload, size_t len);
std::vector<uint8_t> build_text(const std::string& text);   // cmd = 0x03

struct Frame {
    uint8_t cmd = 0;
    std::vector<uint8_t> payload;
};

// Потоковый парсер: скармливаешь байты, для каждого валидного кадра зовётся on_frame.
class Parser {
public:
    void feed(const uint8_t* data, size_t len,
              const std::function<void(const Frame&)>& on_frame);
private:
    int     state_ = 0;     // 0 = idle, 1 = ждём CMD, 2 = сбор тела
    bool    esc_   = false;
    uint8_t cmd_   = 0;
    std::vector<uint8_t> buf_;   // payload + 2 байта CRC
};

} // namespace shproto
