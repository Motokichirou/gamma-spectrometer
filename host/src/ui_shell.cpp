// ui_shell.cpp — см. ui_shell.h. Раскладка и цвета — по макету host/design/mockup.html (.top, .status).
#include "ui_shell.h"
#include "imgui.h"
#include "theme.h"
#include "i18n.h"
#include "appcfg.h"
#include "device.h"
#include <cstdio>
#include <cmath>

Ui g_ui;

static const char* APP_NAME    = "gammapult";
static const char* APP_VERSION = "v0.6.0";
// высоты полос при 100 %; в пикселях — × theme::scale()
static const float TOP_H_100 = 52.0f, STATUS_H_100 = 28.0f;

float TopBarHeight()    { return theme::px(TOP_H_100); }
float StatusBarHeight() { return theme::px(STATUS_H_100); }

void ui_save_cfg()
{
    AppCfg cfg;
    cfg.lang = i18n_get();
    cfg.scale = g_ui.ui_scale;
    appcfg_save(cfg);
}

void ui_go(ds::Mode m, ds::Step s)
{
    ds::Mode prev = g_ui.mode;
    if (m == ds::Mode::Wizard) {
        if (s != ds::Step::N) g_ui.wiz_step = s;
        else if (prev != ds::Mode::Wizard) g_ui.wiz_step = ds::first_open_step(g_dev.snapshot());
    }
    g_ui.mode = m;
    if (m == ds::Mode::Home && prev != ds::Mode::Home) g_dev.setup_refresh();   // сверить запись о настройке
}

// ---------- мелкие виджеты ----------

// фон окна (токен bg) — подложка чипов, как в макете
static ImVec4 BgApp()
{
    unsigned c = theme::tok.bg;
    return ImVec4(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, 1.0f);
}

enum class Dot { None, On, Off };

// чип состояния: рамка, подложка, точка (заполненная — есть связь, пустая — нет) и текст
static float ChipW(const char* text, Dot dot)
{
    using namespace theme;
    float dotw = dot == Dot::None ? 0.0f : 2 * px(4.5f) + px(7.0f);
    return 2 * px(10.0f) + dotw + ImGui::CalcTextSize(text).x;
}

static void Chip(const char* text, const ImVec4& fg, const ImVec4& bg, const ImVec4& border, Dot dot)
{
    using namespace theme;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 ts = ImGui::CalcTextSize(text);
    float h = ImGui::GetFrameHeight(), padx = px(10.0f), r = px(4.5f);
    float dotw = dot == Dot::None ? 0.0f : 2 * r + px(7.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 q(p.x + 2 * padx + dotw + ts.x, p.y + h);
    dl->AddRectFilled(p, q, U32(bg), 2.0f);
    dl->AddRect(p, q, U32(border), 2.0f);
    float cx = p.x + padx + r, cy = p.y + h * 0.5f;
    if (dot == Dot::On)  dl->AddCircleFilled(ImVec2(cx, cy), r, U32(c_ok));
    if (dot == Dot::Off) dl->AddCircle(ImVec2(cx, cy), r - px(1.0f), U32(c_text_dim), 0, px(2.0f));
    dl->AddText(ImVec2(p.x + padx + dotw, p.y + (h - ts.y) * 0.5f), U32(fg), text);
    ImGui::Dummy(ImVec2(q.x - p.x, h));
}

// кнопка режима/выбора: «on» — подложка выбранного, рамка и черта снизу цветом акцента
static bool SegButton(const char* label, bool on, const ImVec2& sz = ImVec2(0, 0))
{
    using namespace theme;
    int nc = 0;
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, c_accent_sel);
        ImGui::PushStyleColor(ImGuiCol_Border, c_accent);
        ImGui::PushStyleColor(ImGuiCol_Text,   c_text_strong);
        nc = 3;
    }
    bool r = ImGui::Button(label, sz);
    if (on) {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x, b.y - px(2.0f)), b, U32(c_accent));
    }
    ImGui::PopStyleColor(nc);
    return r;
}

static bool KillButton(const char* label)
{
    using namespace theme;
    ImGui::PushStyleColor(ImGuiCol_Button,        c_danger_bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c_danger);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  c_danger);
    ImGui::PushStyleColor(ImGuiCol_Text,          c_text_strong);
    ImGui::PushStyleColor(ImGuiCol_Border,        c_danger);
    bool r = ImGui::Button(label);
    ImGui::PopStyleColor(5);
    return r;
}

static float ButtonW(const char* label)
{
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

static bool BeginBar(const char* id, ImVec2 pos, ImVec2 size, ImVec2 pad)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::c_child);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
    bool r = ImGui::Begin(id, nullptr, f);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    return r;
}

// ВВ известно, если -gst приходил недавно
static bool HvKnown(const ds::Snapshot& s) { return s.connected && g_dev.gst_age() < 5.0; }

// ---------- верхняя полоса ----------

static void DrawTopBar(const ds::Snapshot& s, const ds::TopBar& tb)
{
    using namespace theme;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float TOP_H = TopBarHeight();
    float pady = (px(30.0f) - ImGui::GetFontSize()) * 0.5f;
    if (pady < px(2.0f)) pady = px(2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(12.0f), pady));
    float fh = ImGui::GetFontSize() + 2.0f * ImGui::GetStyle().FramePadding.y;   // кнопка 30 px при 100 %
    BeginBar("##topbar", vp->Pos, ImVec2(vp->Size.x, TOP_H), ImVec2(px(14.0f), (TOP_H - fh) * 0.5f));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        dl->AddLine(ImVec2(wp.x, wp.y + TOP_H - 1), ImVec2(wp.x + vp->Size.x, wp.y + TOP_H - 1), U32(c_border));

        // Раскладка: левая группа (имя, прибор, ВВ, «Выключить ВВ», «Сохранить») и правая (режимы)
        // не должны налезать друг на друга. Пока не помещаются — по шагам убираем второстепенное:
        // 1 версия; 2 короткие подписи режимов; 3 в чипе прибора только порт; 4 «Сохранить» коротко;
        // 5 чип ВВ, когда ВВ выключено или неизвестно; 6 имя программы; 7 чип ВВ и при включённом ВВ
        // (о нём кричит «Выключить ВВ»). «Выключить ВВ» и «Отключить» не убираются никогда.
        const float pad = px(14.0f), gap = px(8.0f), sep = px(10.0f), min_mid = px(16.0f);
        const float win_w = ImGui::GetWindowWidth();
        bool home_btn = g_ui.mode != ds::Mode::Home;     // явный путь назад из мастера и расширенного режима
        bool dirty = s.saved != 1;

        std::string dev_full, dev_short;
        if (s.connected) {
            std::string sn = ds::display_serial(g_dev.serial);
            dev_full  = sn.empty() ? g_dev.port_name : sn + " · " + g_dev.port_name;
            dev_short = g_dev.port_name.empty() ? sn : g_dev.port_name;
        }
        char hv_on_txt[96];
        snprintf(hv_on_txt, sizeof hv_on_txt, T("top.hv_on"), (int)std::lround(s.hv_v));
        const char* hv_txt = !HvKnown(s) ? T("top.hv_unknown") : tb.hv_chip_on ? hv_on_txt : T("top.hv_off");

        struct Lay { bool ver, name, hv, dev_short, save_short, modes_short; };
        auto left_w = [&](const Lay& L) {
            float w = 0.0f;
            if (L.name) {
                w += ImGui::CalcTextSize(APP_NAME).x;
                if (L.ver) w += px(6) + ImGui::CalcTextSize(APP_VERSION).x;
                w += px(18);
            }
            w += s.connected ? ChipW((L.dev_short ? dev_short : dev_full).c_str(), Dot::On)
                             : ChipW(T("top.no_link"), Dot::Off);
            if (s.connected && L.hv) w += sep + ChipW(hv_txt, Dot::None);
            if (tb.kill_visible) w += sep + ButtonW(T("top.kill_hv"));
            if (s.connected) w += sep + ButtonW(T(!dirty ? "top.saved" : L.save_short ? "top.save_short" : "top.save"));
            return w;
        };
        auto right_w = [&](const Lay& L) {
            float w = ButtonW(T(L.modes_short ? "top.wizard_short" : "top.wizard")) +
                      ButtonW(T(L.modes_short ? "top.advanced_short" : "top.advanced")) +
                      ButtonW(T("top.disconnect")) + 2 * gap;
            if (home_btn) w += ButtonW(T("top.home")) + gap;
            return w;
        };
        Lay L = { true, true, true, false, false, false };
        auto fits = [&] { return pad + left_w(L) + min_mid + right_w(L) + pad <= win_w; };
        for (int step = 1; step <= 7 && !fits(); step++) {
            switch (step) {
            case 1: L.ver = false; break;
            case 2: L.modes_short = true; break;
            case 3: L.dev_short = true; break;
            case 4: L.save_short = true; break;
            case 5: if (!(HvKnown(s) && tb.hv_chip_on)) L.hv = false; break;
            case 6: L.name = false; break;
            case 7: L.hv = false; break;
            }
        }

        // имя и версия
        if (L.name) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(c_text_strong, "%s", APP_NAME);
            if (L.ver) {
                ImGui::SameLine(0, px(6));
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(c_text_dim, "%s", APP_VERSION);
            }
            ImGui::SameLine(0, px(18));
        }

        // прибор и порт
        if (s.connected) Chip((L.dev_short ? dev_short : dev_full).c_str(), c_text, BgApp(), c_border, Dot::On);
        else             Chip(T("top.no_link"), c_text_dim, BgApp(), c_border, Dot::Off);
        if (ImGui::IsItemHovered() && s.connected && L.dev_short) ImGui::SetTooltip("%s", dev_full.c_str());

        // высокое напряжение
        if (s.connected && L.hv) {
            ImGui::SameLine(0, sep);
            if (!HvKnown(s))         Chip(hv_txt, c_text_dim, BgApp(), c_border, Dot::None);
            else if (tb.hv_chip_on)  Chip(hv_txt, c_hv, c_hv_bg, c_hv, Dot::None);
            else                     Chip(hv_txt, c_text_dim, BgApp(), c_border, Dot::None);
        }
        if (tb.kill_visible) {
            ImGui::SameLine(0, sep);
            if (KillButton(T("top.kill_hv"))) { g_dev.hvp_abort(); g_dev.send_cmd("-hv off"); }
        }

        // сохранить настройки во флеш прибора: всегда под рукой, жёлтая, пока есть несохранённое
        if (s.connected) {
            ImGui::SameLine(0, sep);
            if (dirty) {
                ImGui::PushStyleColor(ImGuiCol_Button,        c_warn_bg);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c_warn);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  c_warn);
                ImGui::PushStyleColor(ImGuiCol_Text,          c_text_strong);
                ImGui::PushStyleColor(ImGuiCol_Border,        c_warn);
            }
            if (ImGui::Button(T(!dirty ? "top.saved" : L.save_short ? "top.save_short" : "top.save"))) g_dev.send_cmd("-save");
            if (dirty) ImGui::PopStyleColor(5);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("top.save_tip"));
        }
        float left_end = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;

        // режимы справа; если места нет и после всех шагов — встают сразу за левой группой
        // (без наложения, хвост уходит за край окна)
        const char* lw = T(L.modes_short ? "top.wizard_short" : "top.wizard");
        const char* la = T(L.modes_short ? "top.advanced_short" : "top.advanced");
        const char* ld = T("top.disconnect");
        const char* lh = T("top.home");
        float rx = win_w - pad - right_w(L);
        if (rx < left_end + min_mid) rx = left_end + min_mid;
        ImGui::SameLine(rx);
        if (home_btn) {
            if (ImGui::Button(lh)) ui_go(ds::Mode::Home);
            ImGui::SameLine(0, gap);
        }
        ImGui::BeginDisabled(!tb.modes_enabled);
        if (SegButton(lw, tb.wiz_on)) ui_go(tb.wiz_on ? ds::Mode::Home : ds::Mode::Wizard);
        if (L.modes_short && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", T("top.wizard"));
        ImGui::SameLine(0, gap);
        if (SegButton(la, tb.adv_on)) ui_go(tb.adv_on ? ds::Mode::Home : ds::Mode::Advanced);
        if (L.modes_short && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", T("top.advanced"));
        ImGui::SameLine(0, gap);
        if (ImGui::Button(ld)) { g_dev.disconnect(); ui_go(ds::Mode::Home); }
        ImGui::EndDisabled();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// ---------- строка состояния ----------

static void Span(const char* text, const ImVec4& col, bool first = false)
{
    if (!first) ImGui::SameLine(0, theme::px(22.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col, "%s", text);
}

static void DrawStatusBar(const ds::Snapshot& s)
{
    using namespace theme;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::PushFont(font_small);
    const float STATUS_H = StatusBarHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(10.0f), px(3.0f)));
    float fh = ImGui::GetFontSize() + 2.0f * px(3.0f);
    BeginBar("##statusbar", ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - STATUS_H),
             ImVec2(vp->Size.x, STATUS_H), ImVec2(px(14.0f), (STATUS_H - fh) * 0.5f));
    {
        ImVec2 wp = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(wp, ImVec2(wp.x + vp->Size.x, wp.y), U32(c_border));

        char b[160];
        if (s.connected) {
            std::string sn = ds::display_serial(g_dev.serial);
            std::string t = sn.empty() ? g_dev.port_name : sn + " · " + g_dev.port_name;
            snprintf(b, sizeof b, T("status.link"), t.c_str());
            Span(b, c_text, true);
        } else {
            Span(T("status.no_link"), c_text_dim, true);
        }

        if (!HvKnown(s))   Span(T("status.hv_unknown"), c_text_dim);
        else if (s.hv_on) { snprintf(b, sizeof b, T("status.hv_on"), (int)std::lround(s.hv_v)); Span(b, c_hv); }
        else               Span(T("status.hv_off"), c_text_dim);

        if (s.connected) {
            if (s.saved == 1)      Span(T("status.saved"), c_text_dim);
            else if (s.saved == 0) Span(T("status.unsaved"), c_warn);
        } else {
            snprintf(b, sizeof b, T("status.ports"), (int)g_dev.ports.size());
            Span(b, c_text_dim);
        }

        // справа: масштаб, язык и справка
        const char* lsd = T("status.scale_down");
        const char* lsu = T("status.scale_up");
        const char* lru = T("top.lang_ru");
        const char* len = T("top.lang_en");
        const char* lh  = T("status.help");
        float w = ButtonW(lsd) + ButtonW(lsu) + px(14.0f) +
                  ButtonW(lru) + ButtonW(len) + ButtonW(lh) + px(14.0f);
        ImGui::SameLine(ImGui::GetWindowWidth() - px(14.0f) - w);
        {
            float want_s = g_ui.ui_scale;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            ImGui::BeginDisabled(g_ui.ui_scale <= UI_SCALE_MIN + 0.001f);
            if (ImGui::Button(lsd)) want_s = ui_scale_step(g_ui.ui_scale, -1);
            ImGui::EndDisabled();
            bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            ImGui::SameLine();
            ImGui::BeginDisabled(g_ui.ui_scale >= UI_SCALE_MAX - 0.001f);
            if (ImGui::Button(lsu)) want_s = ui_scale_step(g_ui.ui_scale, +1);
            ImGui::EndDisabled();
            hov = hov || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            ImGui::PopStyleVar();
            if (hov) ImGui::SetTooltip(T("status.scale_tip"), (int)std::lround(g_ui.ui_scale * 100.0f));
            if (std::fabs(want_s - g_ui.ui_scale) > 0.001f) {
                g_ui.ui_scale = want_s;
                g_ui.scale_req = true;          // атлас пересоберёт main.cpp перед следующим кадром
                ui_save_cfg();
            }
        }
        ImGui::SameLine(0, px(14.0f));
        Lang cur = i18n_get();
        Lang want = cur;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        if (SegButton(lru, cur == Lang::Ru)) want = Lang::Ru;
        ImGui::SameLine();
        if (SegButton(len, cur == Lang::En)) want = Lang::En;
        ImGui::PopStyleVar();
        if (want != cur) {
            i18n_set(want);
            ui_save_cfg();
        }
        ImGui::SameLine(0, px(14.0f));
        if (ImGui::Button(lh)) g_ui.help_req = true;
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

void DrawShell()
{
    ds::Snapshot s = g_dev.snapshot();
    ds::TopBar tb = ds::top_bar(s, g_ui.mode);
    DrawTopBar(s, tb);
    DrawStatusBar(s);
}
