// theme.h — палитра, шрифты и стиль «ГаммаПульт» (дизайн-спека §9)
#pragma once
#include "imgui.h"
struct ImFont;

namespace theme {

// Палитра (sRGB)
extern ImVec4 c_accent, c_accent_hover, c_accent_active, c_accent_sel, c_accent_line;
extern ImVec4 c_ok, c_warn, c_danger, c_danger_bg;
extern ImVec4 c_trace, c_trace2, c_cursor;
extern ImVec4 c_text_strong, c_text, c_text_dim, c_text_faint, c_white;
extern ImVec4 c_plot_bg, c_grid, c_border, c_head, c_child;

// Шрифты (грузятся в LoadFonts)
extern ImFont *font_ui;        // Segoe UI базовый
extern ImFont *font_small;     // мелкие подписи/заголовки панелей
extern ImFont *font_mono;      // моно: лог, таблицы, телеметрия
extern ImFont *font_mono_time; // моно крупнее: время набора
extern ImFont *font_mono_big;  // моно крупно: cps

void LoadFonts(ImGuiIO& io);
void ApplyStyle();       // ImGuiStyle + цвета
void ApplyPlotStyle();   // ImPlotStyle + цвета

inline ImU32 U32(const ImVec4& v) { return ImGui::ColorConvertFloat4ToU32(v); }

} // namespace theme
