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

    bool open(const std::string& port, int baud, bool even_parity = false);   // 8N1 или 8E1 (загрузчик ST)
    void close();
    bool is_open() const { return handle_ != nullptr; }
    unsigned long last_error() const { return last_error_; }   // GetLastError() неудачного open (0 — успех)

    bool set_baud(int baud);                      // сменить скорость на открытом порту (SetCommState), без переоткрытия
    void purge_rx();                              // выбросить принятое (мусор после смены скорости)
    int  read(uint8_t* buf, int maxlen);          // байт прочитано (0 если нет), -1 ошибка
    bool write(const uint8_t* buf, int len);

private:
    void* handle_ = nullptr;   // HANDLE
    unsigned long last_error_ = 0;
};

} // namespace serialport
