// appcfg.h — настройки самого пульта (не прибора): %APPDATA%\gammapult\gammapult.ini. Без ImGui.
#pragma once
#include <string>
#include "i18n.h"

// масштаб интерфейса (множитель поверх DPI монитора): по умолчанию 130 %, пределы 90…180 %
constexpr float UI_SCALE_DEF = 1.30f;
constexpr float UI_SCALE_MIN = 0.90f;
constexpr float UI_SCALE_MAX = 1.80f;

struct AppCfg {
    Lang  lang  = Lang::Ru;
    float scale = UI_SCALE_DEF;   // строка scale=1.30; битое — по умолчанию, вне пределов — обрезка
};

// Загрузка: нет файла — false, cfg по умолчанию; битые строки пропускаются, файл прочитан — true.
bool appcfg_load(AppCfg& cfg);
bool appcfg_save(const AppCfg& cfg);           // каталог создаётся при записи
bool appcfg_load(AppCfg& cfg, const std::string& path);
bool appcfg_save(const AppCfg& cfg, const std::string& path);
std::string appcfg_default_path();

float appcfg_clamp_scale(float s);                 // в [UI_SCALE_MIN, UI_SCALE_MAX]; NaN — UI_SCALE_DEF
float ui_total_scale(float ui, unsigned dpi);      // итог = clamp(ui) × dpi/96; dpi вне 96…480 — обрезка (0 — 96)
float ui_scale_step(float ui, int dir);            // шаг кнопок A−/A+: ±0.1 от ближайшей сетки 0.1, в пределах
