// shproto.cpp — см. shproto.h
#include "shproto.h"

namespace shproto {

uint16_t crc16_modbus(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

std::vector<uint8_t> build_frame(uint8_t cmd, const uint8_t* payload, size_t len)
{
    std::vector<uint8_t> body;
    body.reserve(len + 3);
    body.push_back(cmd);
    for (size_t i = 0; i < len; i++) body.push_back(payload[i]);
    uint16_t crc = crc16_modbus(body.data(), body.size());
    body.push_back((uint8_t)(crc & 0xFF));
    body.push_back((uint8_t)((crc >> 8) & 0xFF));

    std::vector<uint8_t> out;
    out.reserve(body.size() + 4);
    out.push_back(KEEPALIVE);
    out.push_back(START);
    for (uint8_t b : body) {
        if (b == START || b == ESC || b == FINISH) {
            out.push_back(ESC);
            out.push_back((uint8_t)(~b));
        } else {
            out.push_back(b);
        }
    }
    out.push_back(FINISH);
    return out;
}

std::vector<uint8_t> build_text(const std::string& text)
{
    return build_frame(CMD_TEXT, (const uint8_t*)text.data(), text.size());
}

void Parser::feed(const uint8_t* data, size_t len,
                  const std::function<void(const Frame&)>& on_frame)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        if (byte == START) {             // начало кадра — сброс
            state_ = 1; esc_ = false; buf_.clear();
            continue;
        }
        if (state_ == 0) continue;       // вне кадра (вкл. keep-alive 0xFF) — игнор

        if (byte == FINISH && !esc_) {   // конец кадра
            if (state_ == 2 && buf_.size() >= 2) {
                uint16_t got = (uint16_t)(buf_[buf_.size() - 2] |
                                         (buf_[buf_.size() - 1] << 8));
                std::vector<uint8_t> body;
                body.reserve(buf_.size() - 1);
                body.push_back(cmd_);
                for (size_t k = 0; k + 2 < buf_.size(); k++) body.push_back(buf_[k]);
                if (got == crc16_modbus(body.data(), body.size())) {
                    Frame f;
                    f.cmd = cmd_;
                    f.payload.assign(buf_.begin(), buf_.end() - 2);
                    on_frame(f);
                }
            }
            state_ = 0; esc_ = false;
            continue;
        }
        if (byte == ESC && !esc_) { esc_ = true; continue; }
        if (esc_) { byte = (uint8_t)(~byte); esc_ = false; }

        if (state_ == 1) { cmd_ = byte; state_ = 2; }
        else             { buf_.push_back(byte); }
    }
}

} // namespace shproto
