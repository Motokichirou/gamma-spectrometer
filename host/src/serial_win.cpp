// serial_win.cpp — см. serial_win.h
#include "serial_win.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace serialport {

std::vector<std::string> enumerate()
{
    std::vector<std::string> out;
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return out;   // ключа нет = нет ни одного COM-порта

    char  valName[256];
    BYTE  data[256];
    for (DWORD idx = 0;; idx++) {
        DWORD valLen = sizeof(valName);
        DWORD dataLen = sizeof(data);
        DWORD type = 0;
        LONG r = RegEnumValueA(hKey, idx, valName, &valLen, nullptr,
                               &type, data, &dataLen);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r == ERROR_SUCCESS && type == REG_SZ)
            out.emplace_back(reinterpret_cast<char*>(data));
    }
    RegCloseKey(hKey);
    return out;
}

bool Serial::open(const std::string& port, int baud)
{
    close();
    std::string path = "\\\\.\\" + port;   // \\.\COMxx — работает и для COM10+
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return false; }
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fParity  = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return false; }

    // Неблокирующее чтение: вернуть сразу, сколько есть в буфере.
    COMMTIMEOUTS to = {};
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutConstant    = 0;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.WriteTotalTimeoutConstant   = 200;
    to.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(h, &to);

    SetupComm(h, 1 << 16, 1 << 16);              // большие буферы приёма/передачи
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    handle_ = h;
    return true;
}

void Serial::close()
{
    if (handle_) { CloseHandle((HANDLE)handle_); handle_ = nullptr; }
}

int Serial::read(uint8_t* buf, int maxlen)
{
    if (!handle_) return -1;
    DWORD n = 0;
    if (!ReadFile((HANDLE)handle_, buf, (DWORD)maxlen, &n, nullptr)) return -1;
    return (int)n;
}

bool Serial::write(const uint8_t* buf, int len)
{
    if (!handle_) return false;
    DWORD n = 0;
    return WriteFile((HANDLE)handle_, buf, (DWORD)len, &n, nullptr) && (int)n == len;
}

} // namespace serialport
