// appcfg.cpp — настройки пульта в %APPDATA%\gammapult\gammapult.ini (строки key=value).
// Неизвестные ключи и битые строки пропускаются: язык остаётся по умолчанию.
#include "appcfg.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

std::string appcfg_default_path()
{
    const char* a = std::getenv("APPDATA");
    std::string dir = a && *a ? std::string(a) : std::string(".");
    return dir + "\\gammapult\\gammapult.ini";
}

static void trim_(std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    s = b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

bool appcfg_load(AppCfg& cfg, const std::string& path)
{
    cfg = AppCfg{};
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        std::string s(line);
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        trim_(k); trim_(v);
        if (k == "lang") {
            if (v == "ru") cfg.lang = Lang::Ru;
            else if (v == "en") cfg.lang = Lang::En;
        }
    }
    std::fclose(f);
    return true;
}

bool appcfg_save(const AppCfg& cfg, const std::string& path)
{
    size_t sl = path.find_last_of("\\/");
    if (sl != std::string::npos) CreateDirectoryA(path.substr(0, sl).c_str(), nullptr);  // уже есть — не ошибка
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fprintf(f, "lang=%s\r\n", cfg.lang == Lang::En ? "en" : "ru") > 0;
    ok = std::fclose(f) == 0 && ok;
    return ok;
}

bool appcfg_load(AppCfg& cfg) { return appcfg_load(cfg, appcfg_default_path()); }
bool appcfg_save(const AppCfg& cfg) { return appcfg_save(cfg, appcfg_default_path()); }
