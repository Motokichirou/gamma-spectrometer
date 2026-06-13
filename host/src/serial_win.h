// serial_win.h — COM-порт через Win32 API (без сторонних зависимостей)
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace serialport {

// Активные COM-порты из реестра (HARDWARE\DEVICEMAP\SERIALCOMM), напр. "COM6".
std::vector<std::string> enumerate();

class Serial {
public:
    ~Serial() { close(); }

    bool open(const std::string& port, int baud);
    void close();
    bool is_open() const { return handle_ != nullptr; }

    int  read(uint8_t* buf, int maxlen);          // байт прочитано (0 если нет), -1 ошибка
    bool write(const uint8_t* buf, int len);

private:
    void* handle_ = nullptr;   // HANDLE
};

} // namespace serialport
