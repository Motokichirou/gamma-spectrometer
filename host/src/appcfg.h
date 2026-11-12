// appcfg.h — настройки самого пульта (не прибора): %APPDATA%\gammapult\gammapult.ini. Без ImGui.
#pragma once
#include <string>
#include "i18n.h"

struct AppCfg { Lang lang = Lang::Ru; };

// Загрузка: нет файла — false, cfg по умолчанию; битые строки пропускаются, файл прочитан — true.
bool appcfg_load(AppCfg& cfg);
bool appcfg_save(const AppCfg& cfg);           // каталог создаётся при записи
bool appcfg_load(AppCfg& cfg, const std::string& path);
bool appcfg_save(const AppCfg& cfg, const std::string& path);
std::string appcfg_default_path();
