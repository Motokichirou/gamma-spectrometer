// theme.cpp — см. theme.h (дизайн-спека §9)
#include "theme.h"
#include "implot.h"

namespace theme {

static inline ImVec4 hx(unsigned rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f,
                  ((rgb >> 8) & 0xFF) / 255.0f,
                  (rgb & 0xFF) / 255.0f, a);
}

// Палитра
ImVec4 c_accent        = hx(0x4c8df0);
ImVec4 c_accent_hover  = hx(0x5e9bf5);
ImVec4 c_accent_active = hx(0x3b7ae0);
ImVec4 c_accent_sel    = ImVec4(0x4c/255.0f, 0x8d/255.0f, 0xf0/255.0f, 0.22f);
ImVec4 c_accent_line   = ImVec4(0x4c/255.0f, 0x8d/255.0f, 0xf0/255.0f, 0.55f);
ImVec4 c_ok            = hx(0x4fb07a);
ImVec4 c_warn          = hx(0xe0a23c);
ImVec4 c_danger        = hx(0xe5484d);
ImVec4 c_danger_bg     = ImVec4(0xe5/255.0f, 0x48/255.0f, 0x4d/255.0f, 0.14f);
ImVec4 c_trace         = hx(0x6fb0f0);
ImVec4 c_trace2        = hx(0x4fb07a);
ImVec4 c_cursor        = hx(0xe0a23c);
ImVec4 c_text_strong   = hx(0xe9edf3);
ImVec4 c_text          = hx(0xc6ccd6);
ImVec4 c_text_dim      = hx(0x79818e);
ImVec4 c_text_faint    = hx(0x555d6a);
ImVec4 c_white         = hx(0xffffff);
ImVec4 c_plot_bg       = hx(0x10131a);
ImVec4 c_grid          = hx(0x1f2630);
ImVec4 c_border        = hx(0x2e3540);
ImVec4 c_head          = hx(0x232a35);
ImVec4 c_child         = hx(0x1b1f27);

ImFont *font_ui = nullptr, *font_small = nullptr, *font_mono = nullptr,
       *font_mono_time = nullptr, *font_mono_big = nullptr;

void LoadFonts(ImGuiIO& io)
{
    const char* SEGOE   = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* CONSOLA = "C:\\Windows\\Fonts\\consola.ttf";
    const ImWchar* cyr = io.Fonts->GetGlyphRangesCyrillic();

    font_ui = io.Fonts->AddFontFromFileTTF(SEGOE, 15.0f, nullptr, cyr);
    if (!font_ui) {                       // нет системного шрифта — дефолт на всё (ASCII)
        ImFont* d = io.Fonts->AddFontDefault();
        font_ui = font_small = font_mono = font_mono_time = font_mono_big = d;
        return;
    }
    font_small     = io.Fonts->AddFontFromFileTTF(SEGOE,   13.0f, nullptr, cyr);
    if (!font_small) font_small = font_ui;
    font_mono      = io.Fonts->AddFontFromFileTTF(CONSOLA, 14.0f, nullptr, cyr);
    if (!font_mono) font_mono = font_ui;
    font_mono_time = io.Fonts->AddFontFromFileTTF(CONSOLA, 20.0f, nullptr, cyr);
    if (!font_mono_time) font_mono_time = font_mono;
    font_mono_big  = io.Fonts->AddFontFromFileTTF(CONSOLA, 34.0f, nullptr, cyr);
    if (!font_mono_big) font_mono_big = font_mono;
}

void ApplyStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();

    // Форма: прямые углы, рамки 1px (дизайн-спека §9)
    s.WindowRounding = s.FrameRounding = s.ChildRounding =
        s.TabRounding = s.PopupRounding = s.ScrollbarRounding = s.GrabRounding = 0.0f;
    s.WindowBorderSize = s.FrameBorderSize = s.ChildBorderSize = 1.0f;
    s.FramePadding  = ImVec2(8, 4);
    s.ItemSpacing   = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.WindowPadding = ImVec2(9, 9);
    s.CellPadding   = ImVec2(8, 4);
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize   = 10.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]            = hx(0x15181e);
    c[ImGuiCol_ChildBg]             = hx(0x1b1f27);
    c[ImGuiCol_PopupBg]             = hx(0x1b1f27);
    c[ImGuiCol_Border]              = c_border;
    c[ImGuiCol_BorderShadow]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]             = hx(0x20252e);
    c[ImGuiCol_FrameBgHovered]      = hx(0x2a313c);
    c[ImGuiCol_FrameBgActive]       = hx(0x323a47);
    c[ImGuiCol_TitleBg]             = hx(0x15181e);
    c[ImGuiCol_TitleBgActive]       = hx(0x1c212a);
    c[ImGuiCol_TitleBgCollapsed]    = hx(0x15181e);
    c[ImGuiCol_MenuBarBg]           = hx(0x15181e);
    c[ImGuiCol_ScrollbarBg]         = hx(0x10131a);
    c[ImGuiCol_ScrollbarGrab]       = hx(0x2e3540);
    c[ImGuiCol_ScrollbarGrabHovered]= hx(0x3a434f);
    c[ImGuiCol_ScrollbarGrabActive] = hx(0x4c8df0);
    c[ImGuiCol_CheckMark]           = c_accent;
    c[ImGuiCol_SliderGrab]          = c_accent;
    c[ImGuiCol_SliderGrabActive]    = c_accent_active;
    c[ImGuiCol_Button]              = hx(0x20252e);
    c[ImGuiCol_ButtonHovered]       = hx(0x2a313c);
    c[ImGuiCol_ButtonActive]        = hx(0x323a47);
    c[ImGuiCol_Header]              = c_accent_sel;
    c[ImGuiCol_HeaderHovered]       = ImVec4(0x4c/255.0f, 0x8d/255.0f, 0xf0/255.0f, 0.32f);
    c[ImGuiCol_HeaderActive]        = ImVec4(0x4c/255.0f, 0x8d/255.0f, 0xf0/255.0f, 0.42f);
    c[ImGuiCol_Separator]           = hx(0x242a33);
    c[ImGuiCol_SeparatorHovered]    = hx(0x3a434f);
    c[ImGuiCol_SeparatorActive]     = c_accent;
    c[ImGuiCol_Tab]                 = hx(0x1b1f27);
    c[ImGuiCol_TabHovered]          = c_accent_sel;
    c[ImGuiCol_TabActive]           = hx(0x232a35);
    c[ImGuiCol_TabUnfocused]        = hx(0x15181e);
    c[ImGuiCol_TabUnfocusedActive]  = hx(0x1b1f27);
    c[ImGuiCol_Text]                = c_text;
    c[ImGuiCol_TextDisabled]        = c_text_faint;
    c[ImGuiCol_TextSelectedBg]      = c_accent_sel;
    c[ImGuiCol_TableHeaderBg]       = hx(0x232a35);
    c[ImGuiCol_TableBorderStrong]   = hx(0x2e3540);
    c[ImGuiCol_TableBorderLight]    = hx(0x242a33);
    c[ImGuiCol_TableRowBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]       = ImVec4(1, 1, 1, 0.02f);
    c[ImGuiCol_NavHighlight]        = c_accent;
}

void ApplyPlotStyle()
{
    ImPlotStyle& ps = ImPlot::GetStyle();
    ps.PlotPadding   = ImVec2(8, 6);
    ps.LabelPadding  = ImVec2(4, 4);
    ps.LegendPadding = ImVec2(6, 6);
    ps.MinorAlpha    = 0.25f;

    ImVec4* c = ps.Colors;
    c[ImPlotCol_FrameBg]    = c_plot_bg;
    c[ImPlotCol_PlotBg]     = c_plot_bg;
    c[ImPlotCol_PlotBorder] = c_border;
    c[ImPlotCol_AxisGrid]   = c_grid;
    c[ImPlotCol_AxisText]   = c_text_dim;
    c[ImPlotCol_AxisTick]   = c_text_faint;
    c[ImPlotCol_TitleText]  = c_text;
    c[ImPlotCol_InlayText]  = c_text_dim;
    c[ImPlotCol_Line]       = c_trace;
}

} // namespace theme
