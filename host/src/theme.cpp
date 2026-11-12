// theme.cpp — см. theme.h (дизайн-спека §9)
#include "theme.h"
#include "implot.h"
#include "theme_tokens.h"

namespace theme {

static inline ImVec4 hx(unsigned rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f,
                  ((rgb >> 8) & 0xFF) / 255.0f,
                  (rgb & 0xFF) / 255.0f, a);
}

static inline ImVec4 tk(unsigned rgb, float a = 1.0f) { return hx(rgb, a); }

// Палитра — из семантических токенов (theme_tokens.h); прежние имена c_* оставлены для кода панелей
ImVec4 c_accent        = tk(tok.accent);
ImVec4 c_accent_hover  = hx(0x8ab9ff);
ImVec4 c_accent_active = hx(0x5a94ec);
ImVec4 c_accent_sel    = tk(tok.sel_bg);
ImVec4 c_accent_line   = tk(tok.accent, 0.55f);
ImVec4 c_on_accent     = tk(tok.on_accent);
ImVec4 c_ok            = tk(tok.ok);
ImVec4 c_warn          = tk(tok.warn);
ImVec4 c_warn_bg       = tk(tok.warn_bg);
ImVec4 c_danger        = tk(tok.danger);
ImVec4 c_danger_bg     = tk(tok.danger, 0.16f);
ImVec4 c_hv            = tk(tok.hv);
ImVec4 c_hv_bg         = tk(tok.hv_bg);
ImVec4 c_trace         = tk(tok.accent);
ImVec4 c_trace2        = tk(tok.ok);
ImVec4 c_cursor        = tk(tok.warn);
ImVec4 c_text_strong   = hx(0xf4f5f7);
ImVec4 c_text          = tk(tok.text);
ImVec4 c_text_dim      = tk(tok.text2);
ImVec4 c_text_faint    = hx(0x80878f);   // неактивные элементы и мелкие засечки осей (не основной текст)
ImVec4 c_white         = hx(0xffffff);
ImVec4 c_plot_bg       = hx(0x111316);
ImVec4 c_grid          = hx(0x262a30);
ImVec4 c_border        = tk(tok.border);
ImVec4 c_head          = tk(tok.panel2);
ImVec4 c_child         = tk(tok.panel);

ImFont *font_ui = nullptr, *font_small = nullptr, *font_mono = nullptr,
       *font_mono_time = nullptr, *font_mono_big = nullptr;

void LoadFonts(ImGuiIO& io)
{
    const char* SEGOE   = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* CONSOLA = "C:\\Windows\\Fonts\\consola.ttf";
    static const ImWchar ranges[] = {
        0x0020, 0x00FF,   // Latin + Latin-1 (² · ° ±)
        0x0370, 0x03FF,   // греческий (Σ σ μ)
        0x0400, 0x04FF,   // кириллица
        0x2010, 0x2027,   // тире/кавычки (—)
        0x2190, 0x21FF,   // стрелки (→)
        0x2200, 0x22FF,   // математические знаки (≈ ≤ ≥ − ∥)
        0x2713, 0x2713,   // галочка (✓)
        0
    };
    const ImWchar* cyr = ranges;

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
    c[ImGuiCol_WindowBg]            = tk(tok.bg);
    c[ImGuiCol_ChildBg]             = tk(tok.panel);
    c[ImGuiCol_PopupBg]             = tk(tok.panel);
    c[ImGuiCol_Border]              = c_border;
    c[ImGuiCol_BorderShadow]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]             = tk(tok.panel2);
    c[ImGuiCol_FrameBgHovered]      = hx(0x31363e);
    c[ImGuiCol_FrameBgActive]       = tk(tok.border);
    c[ImGuiCol_TitleBg]             = tk(tok.bg);
    c[ImGuiCol_TitleBgActive]       = tk(tok.panel);
    c[ImGuiCol_TitleBgCollapsed]    = tk(tok.bg);
    c[ImGuiCol_MenuBarBg]           = tk(tok.panel);
    c[ImGuiCol_ScrollbarBg]         = tk(tok.bg);
    c[ImGuiCol_ScrollbarGrab]       = tk(tok.border);
    c[ImGuiCol_ScrollbarGrabHovered]= hx(0x4a505a);
    c[ImGuiCol_ScrollbarGrabActive] = c_accent;
    c[ImGuiCol_CheckMark]           = c_accent;
    c[ImGuiCol_SliderGrab]          = c_accent;
    c[ImGuiCol_SliderGrabActive]    = c_accent_active;
    c[ImGuiCol_Button]              = tk(tok.panel2);
    c[ImGuiCol_ButtonHovered]       = hx(0x31363e);
    c[ImGuiCol_ButtonActive]        = tk(tok.border);
    c[ImGuiCol_Header]              = c_accent_sel;
    c[ImGuiCol_HeaderHovered]       = hx(0x324566);
    c[ImGuiCol_HeaderActive]        = hx(0x3a5077);
    c[ImGuiCol_Separator]           = tk(tok.border);
    c[ImGuiCol_SeparatorHovered]    = hx(0x4a505a);
    c[ImGuiCol_SeparatorActive]     = c_accent;
    c[ImGuiCol_Tab]                 = tk(tok.panel);
    c[ImGuiCol_TabHovered]          = c_accent_sel;
    c[ImGuiCol_TabActive]           = tk(tok.sel_bg);
    c[ImGuiCol_TabUnfocused]        = tk(tok.bg);
    c[ImGuiCol_TabUnfocusedActive]  = tk(tok.panel);
    c[ImGuiCol_Text]                = c_text;
    c[ImGuiCol_TextDisabled]        = c_text_faint;
    c[ImGuiCol_TextSelectedBg]      = c_accent_sel;
    c[ImGuiCol_TableHeaderBg]       = tk(tok.panel2);
    c[ImGuiCol_TableBorderStrong]   = tk(tok.border);
    c[ImGuiCol_TableBorderLight]    = hx(0x2e3239);
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
