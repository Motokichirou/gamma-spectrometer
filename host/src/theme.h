// theme.h — палитра, шрифты и стиль «ГаммаПульт» (дизайн-спека §9)
#pragma once
#include "imgui.h"
#include "theme_tokens.h"
struct ImFont;

namespace theme {

// Палитра (sRGB), выводится из семантических токенов theme_tokens.h (tok)
extern ImVec4 c_accent, c_accent_hover, c_accent_active, c_accent_sel, c_accent_line;
extern ImVec4 c_on_accent;
extern ImVec4 c_ok, c_warn, c_warn_bg, c_danger, c_danger_bg, c_hv, c_hv_bg;
extern ImVec4 c_trace, c_trace2, c_cursor;
extern ImVec4 c_text_strong, c_text, c_text_dim, c_text_faint, c_white;
extern ImVec4 c_plot_bg, c_grid, c_border, c_head, c_child;

// Шрифты (грузятся в LoadFonts)
extern ImFont *font_ui;        // Segoe UI базовый
extern ImFont *font_small;     // мелкие подписи/заголовки панелей
extern ImFont *font_mono;      // моно: лог, таблицы, телеметрия
extern ImFont *font_mono_time; // моно крупнее: время набора
extern ImFont *font_mono_big;  // моно крупно: cps
extern ImFont *font_title;     // Segoe UI Semibold 22 px: заголовки главного экрана

// Масштаб интерфейса: итог = ui_scale × dpi/96 (appcfg.h, ui_total_scale). Шрифты набираются
// сразу нужного размера (без FontGlobalScale), стиль и раскладка — через px().
float scale();                                // текущий итоговый масштаб (1 — 96 dpi, 100 %)
inline float px(float v) { return v * scale(); }
inline ImVec2 px(float x, float y) { return ImVec2(x * scale(), y * scale()); }

void LoadFonts(ImGuiIO& io, float scale);     // атлас: размеры 15/13/14/20/34/22 × scale
void ApplyStyle();       // ImGuiStyle + цвета (размеры — для 100 %)
void ApplyPlotStyle();   // ImPlotStyle + цвета (размеры — для 100 %)
// Сброс стилей к исходным, ApplyStyle/ApplyPlotStyle и ×scale (не копится при повторных вызовах).
// Атлас не трогает: шрифты пересобирает вызывающий (LoadFonts) между кадрами.
void ApplyScale(float scale);

inline ImU32 U32(const ImVec4& v) { return ImGui::ColorConvertFloat4ToU32(v); }

} // namespace theme
