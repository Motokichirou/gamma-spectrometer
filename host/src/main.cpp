// main.cpp — GUI-режим gammapult: сервисный пульт гамма-спектрометра (прошивка firmware/gamma_fw).
// Win32 + DirectX 11 + Dear ImGui + ImPlot. Единый .exe без зависимостей.
// Раздвижные панели: подключение, телеметрия (-gst), высокое напряжение, живой спектр, консоль,
// сервис (самотест ЦАП, калибровки монитора ВВ и Pt1000, настройки). Энергокалибровка — в BecqMoni.

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "implot.h"

#include <d3d11.h>
#include <tchar.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#include "device.h"
#include "tpl_learn.h"
#include "fwflash.h"
#include "peakfit.h"
#include "logdump.h"
#include <filesystem>
#include <ctime>
#include <cstdlib>
#include "theme.h"
#include "host_app.h"
#include "appcfg.h"
#include "ui_shell.h"
#include "ui_home.h"
#include "ui_wizard.h"
#include "i18n.h"

// --- DirectX 11 ---
static ID3D11Device*           g_pd3dDevice = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static bool CreateDeviceD3D(HWND hWnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
static LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// --- состояние приложения ---
Device g_dev;                                  // общий с оболочкой (ui_shell.cpp)
static int    g_port_sel = 0;
static bool   g_log_scale = true;
static char   g_cmd_buf[128] = "";
static float  gLeftW = 330.0f, gRightW = 380.0f, gConnH = 245.0f, gConsoleH = 200.0f;
static float  gTeleH = 285.0f;
static float  g_spec_x[Device::CHANNELS];
static float  g_spec_y[Device::CHANNELS];

// ВВ
static int    g_hv_set = 750;
static bool   g_hv_confirm = false;            // открыть подтверждение включения ВВ
// самотест ЦАП
static char   g_lin_codes[128] = "50 100 200 400 600 800 1000 1200 1350";
static int    g_lin_n = 10000, g_lin_period = 100;
// калибровки
static float  g_mon_meas_v = 0.0f;             // ВВ по мультиметру, В
static float  g_mon_rec_mv = 0.0f;             // запомненное показание монитора, мВ
static float  g_mon_rec_set = 0.0f;
static float  g_mon_k_new = 0.0f;
static float  g_pt_ref = 25.0f;
// настройки
static int    g_thr = 0;
static int    g_pga = 4;
// мастер эталона
static int    g_tpl_blocks = 2000;
static std::atomic<int> g_tpl_state{ 0 };      // 0 — нет, 1 — считаю, 2 — готово
static tpl::Result g_tpl_res;
static std::string g_tpl_info;
static bool   g_tpl_loaded = false;
static int    g_tpl_load_err0 = 0;
// термокомпенсация
static float  g_tc_t0 = 25.0f;
static int    g_tc_order = 1;

// ================= помощники UI =================

static bool PrimaryButton(const char* label, const ImVec2& sz = ImVec2(0, 0))
{
    using namespace theme;
    ImGui::PushStyleColor(ImGuiCol_Button,        c_accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c_accent_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  c_accent_active);
    ImGui::PushStyleColor(ImGuiCol_Text,          c_on_accent);
    bool r = ImGui::Button(label, sz);
    ImGui::PopStyleColor(4);
    return r;
}

static bool DangerButton(const char* label, const ImVec2& sz = ImVec2(0, 0))
{
    using namespace theme;
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c_danger_bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  c_danger_bg);
    ImGui::PushStyleColor(ImGuiCol_Text,          c_danger);
    ImGui::PushStyleColor(ImGuiCol_Border,        c_danger);
    bool r = ImGui::Button(label, sz);
    ImGui::PopStyleColor(5);
    return r;
}

static void StatusDot(const ImVec4& col)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  h = ImGui::GetTextLineHeight();
    float  r = 4.5f;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r + 1, p.y + h * 0.5f),
                                                r, theme::U32(col));
    ImGui::Dummy(ImVec2(r * 2 + 7, h));
    ImGui::SameLine();
}

static void Caption(const char* s)
{
    ImGui::PushStyleColor(ImGuiCol_Text, theme::c_text_dim);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}

static void PanelHeader(const char* title)
{
    ImGui::PushFont(theme::font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::c_text_dim);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
}

static bool BeginPanel(const char* id, ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings;
    return ImGui::Begin(id, nullptr, f);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static float Splitter(const char* id, ImVec2 pos, ImVec2 size, bool vertical)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin(id, nullptr, f);
    ImGui::InvisibleButton("s", size);
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();
    if (active || hovered) {
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        ImU32 col = theme::U32(active ? theme::c_accent : theme::c_accent_line);
        ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), col);
    }
    float d = active ? (vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y) : 0.0f;
    ImGui::End();
    ImGui::PopStyleVar(2);
    return d;
}

static ImVec4 StateColor()
{
    using namespace theme;
    switch (g_dev.state) {
    case Device::State::Disconnected: return c_text_dim;
    case Device::State::Connecting:   return c_accent;
    case Device::State::Connected:    return c_ok;
    case Device::State::Acquiring:    return c_ok;
    case Device::State::Error:        return c_danger;
    }
    return c_text_dim;
}

// строка «подпись — значение» моноширинным
static void KV(const char* k, const char* v, const ImVec4* col = nullptr)
{
    Caption(k);
    ImGui::SameLine(130);
    ImGui::PushFont(theme::font_mono);
    if (col) ImGui::PushStyleColor(ImGuiCol_Text, *col);
    ImGui::TextUnformatted(v);
    if (col) ImGui::PopStyleColor();
    ImGui::PopFont();
}

static bool Busy() { return g_dev.lin.running || g_dev.hvp.running || g_dev.raw.running || g_dev.tc.running; }

// ================= панели =================

static void PanelConnection(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    BeginPanel("##conn", pos, size);
    PanelHeader("ПОДКЛЮЧЕНИЕ");

    bool busy = (g_dev.state != Device::State::Disconnected &&
                 g_dev.state != Device::State::Error);

    Caption("COM-порт");
    if (g_port_sel >= (int)g_dev.ports.size()) g_port_sel = 0;
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(-1);
    const char* prev = g_dev.ports.empty() ? "(нет портов)" : g_dev.ports[g_port_sel].c_str();
    if (ImGui::BeginCombo("##port", prev)) {
        for (int i = 0; i < (int)g_dev.ports.size(); i++)
            if (ImGui::Selectable(g_dev.ports[i].c_str(), i == g_port_sel))
                g_port_sel = i;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();

    if (!busy) {
        if (PrimaryButton("Подключить", ImVec2(-1, 26)) && !g_dev.ports.empty())
            g_dev.connect(g_dev.ports[g_port_sel]);
        if (ImGui::Button("Обновить порты", ImVec2(-1, 0))) {
            g_dev.refresh_ports();
            if (!g_dev.ports.empty()) g_port_sel = (int)g_dev.ports.size() - 1;
        }
    } else {
        ImGui::BeginDisabled(Busy());
        if (!g_dev.acquiring) {
            if (PrimaryButton("Старт набора (-sta)", ImVec2(-1, 26))) g_dev.start();
        } else {
            if (DangerButton("Стоп набора (-sto)", ImVec2(-1, 26))) g_dev.stop();
        }
        if (ImGui::Button("Сброс спектра (-rst)", ImVec2(-1, 0))) g_dev.reset_spectrum();
        ImGui::EndDisabled();
        if (DangerButton("Отключить", ImVec2(-1, 0))) g_dev.disconnect();
    }

    ImGui::Separator();
    StatusDot(StateColor());
    ImGui::TextWrapped("%s", g_dev.status_text.c_str());
    ImGui::Separator();

    KV("Серийник", g_dev.serial.empty() ? "—" : g_dev.serial.c_str(), &c_accent);
    Caption("Прошивка");
    ImGui::PushFont(font_mono);
    ImGui::TextWrapped("%s", g_dev.fw_info.empty() ? "—" : g_dev.fw_info.c_str());
    ImGui::PopFont();
    ImGui::End();
}

static void PanelTelemetry(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    BeginPanel("##tele", pos, size);
    PanelHeader("ТЕЛЕМЕТРИЯ (-gst)");

    char tb[16];
    uint32_t s = g_dev.elapsed_s;
    snprintf(tb, sizeof(tb), "%02u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    ImGui::PushFont(font_mono_time); ImGui::TextUnformatted(tb); ImGui::PopFont();
    ImGui::SameLine();
    char cb[24]; snprintf(cb, sizeof(cb), "%u", g_dev.cps);
    ImGui::PushFont(font_mono_big);
    ImGui::PushStyleColor(ImGuiCol_Text, g_dev.acquiring ? c_accent : c_text_strong);
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(cb).x - 50);
    ImGui::TextUnformatted(cb);
    ImGui::PopStyleColor(); ImGui::PopFont();
    ImGui::SameLine(); Caption("cps");
    ImGui::Separator();

    if (g_dev.gst.empty()) {
        ImGui::TextDisabled(g_dev.connected() ? "жду -gst…" : "нет связи");
        ImGui::End();
        return;
    }
    char b[96];
    bool hv = g_dev.hv_on();
    snprintf(b, sizeof b, "%s  %.0f В (уставка %.0f)", hv ? "ВКЛ" : "выкл",
             fabs(g_dev.gnum("hv_out")), g_dev.gnum("hv_set"));
    KV("ВВ", b, hv ? &c_danger : nullptr);
    std::string trip = g_dev.gstr("hv_trip", "-");
    if (trip != "-") KV("Авария ВВ", trip.c_str(), &c_danger);

    std::string pt = g_dev.gstr("pt", "-");
    if (pt == "ok") snprintf(b, sizeof b, "%.2f °C", g_dev.gnum("t"));
    else            snprintf(b, sizeof b, "датчик: %s", pt.c_str());
    KV("Температура", b, pt == "ok" ? nullptr : &c_warn);

    snprintf(b, sizeof b, "%.1f кодов, σ %.2f", g_dev.gnum("base"), g_dev.gnum("sigma"));
    KV("База / шум", b);
    snprintf(b, sizeof b, "вход %.0f  принято %.0f  брак %.0f", g_dev.gnum("cps_in"), g_dev.gnum("cps_acc"), g_dev.gnum("rej"));
    KV("Импульсы/с", b);
    snprintf(b, sizeof b, "%.0f кодов%s", g_dev.gnum("thr"), g_dev.thr_codes == 0 ? " (авто 5σ)" : "");
    KV("Порог", b);
    snprintf(b, sizeof b, "%.1f / %.1f с", g_dev.gnum("live_s"), g_dev.gnum("real_s"));
    KV("Живое / реальное", b);
    snprintf(b, sizeof b, "%.1f %%  потери %.0f  переполн. %.0f", g_dev.gnum("cpu"), g_dev.gnum("lost"), g_dev.gnum("overrun"));
    KV("ЦП", b);
    bool vok = g_dev.gnum("vref_ok", 0) > 0.5;
    snprintf(b, sizeof b, "%.1f мВ %s", g_dev.gnum("vref"), vok ? "норма" : "ВНЕ НОРМЫ");
    KV("Опора", b, vok ? nullptr : &c_danger);
    std::string fl = g_dev.gstr("flash");
    KV("Флеш", fl.c_str(), fl == "ok" ? nullptr : &c_warn);
    if (g_dev.gst_age() > 3) { ImGui::PushStyleColor(ImGuiCol_Text, c_warn); ImGui::TextUnformatted("телеметрия устарела"); ImGui::PopStyleColor(); }
    ImGui::End();
}

static void PanelHV(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    BeginPanel("##hv", pos, size);
    PanelHeader("ВЫСОКОЕ НАПРЯЖЕНИЕ");
    bool conn = g_dev.connected();
    bool hv = g_dev.hv_on();

    if (hv) {
        StatusDot(c_danger);
        ImGui::PushStyleColor(ImGuiCol_Text, c_danger);
        ImGui::Text("ВВ ВКЛЮЧЕНО: %.0f В", fabs(g_dev.gnum("hv_out")));
        ImGui::PopStyleColor();
    } else {
        StatusDot(c_ok); ImGui::TextUnformatted("ВВ выключено");
    }
    ImGui::BeginDisabled(!conn);
    if (DangerButton("ВЫКЛЮЧИТЬ ВВ", ImVec2(-1, 28))) { g_dev.hvp_abort(); g_dev.send_cmd("-hv off"); }
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("В##hvset", &g_hv_set, 10, 50);
    if (g_hv_set < 0) g_hv_set = 0;
    if (g_hv_set > 1200) g_hv_set = 1200;
    ImGui::SameLine();
    ImGui::BeginDisabled(Busy());
    if (PrimaryButton(hv ? "Установить" : "Включить…")) {
        if (hv) { char c[32]; snprintf(c, sizeof c, "-hv %d", g_hv_set); g_dev.send_cmd(c); }
        else g_hv_confirm = true;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (g_hv_confirm) { ImGui::OpenPopup("Включить ВВ?"); g_hv_confirm = false; }
    if (ImGui::BeginPopupModal("Включить ВВ?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Подать %d В на ФЭУ?", g_hv_set);
        ImGui::TextDisabled("Стек должен быть собран, ФЭУ в темноте, щупы убраны.");
        if (PrimaryButton("Включить", ImVec2(120, 0))) {
            char c[32]; snprintf(c, sizeof c, "-hv %d", g_hv_set); g_dev.send_cmd(c);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::Separator();
    Caption("Подстройка ВВ по пику (источник на месте)");
    Device::HvPeak& h = g_dev.hvp;
    float f;
    ImGui::BeginDisabled(h.running);
    ImGui::SetNextItemWidth(80); f = (float)h.target; if (ImGui::InputFloat("цель, канал", &f, 0, 0, "%.0f")) h.target = f;
    ImGui::SetNextItemWidth(60); f = (float)h.lo; if (ImGui::InputFloat("##lo", &f, 0, 0, "%.0f")) h.lo = f;
    ImGui::SameLine(); ImGui::TextUnformatted("–"); ImGui::SameLine();
    ImGui::SetNextItemWidth(60); f = (float)h.hi; if (ImGui::InputFloat("окно пика", &f, 0, 0, "%.0f")) h.hi = f;
    ImGui::SetNextItemWidth(60); f = (float)h.secs; if (ImGui::InputFloat("набор, с", &f, 0, 0, "%.0f")) h.secs = f < 5 ? 5 : f;
    ImGui::SetNextItemWidth(60); f = (float)h.gain_exp; if (ImGui::InputFloat("показатель усиления", &f, 0, 0, "%.1f")) h.gain_exp = f < 2 ? 2 : f;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!conn || g_dev.lin.running);
    if (!h.running) {
        if (PrimaryButton("Подстроить", ImVec2(-1, 0))) {
            double v0 = hv ? fabs(g_dev.gnum("hv_set")) : g_hv_set;
            g_dev.hvp_start(v0);
        }
    } else if (ImGui::Button("Прервать подстройку", ImVec2(-1, 0))) g_dev.hvp_abort();
    ImGui::EndDisabled();
    if (!h.msg.empty()) ImGui::TextWrapped("%s", h.msg.c_str());
    for (auto& p : h.hist) ImGui::TextDisabled("%.0f В → пик %.1f", p.first, p.second);
    ImGui::End();
}

static void PanelSpectrum(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    BeginPanel("##spec", pos, size);

    ImGui::PushFont(font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
    ImGui::TextUnformatted("СПЕКТР");
    ImGui::PopStyleColor(); ImGui::PopFont();

    float rx = ImGui::GetWindowWidth() - 8 - 88;
    ImGui::SameLine(rx);
    auto seg = [&](const char* lbl, bool active) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, c_accent);
            ImGui::PushStyleColor(ImGuiCol_Text, c_on_accent);
        }
        bool r = ImGui::Button(lbl, ImVec2(42, 0));
        if (active) ImGui::PopStyleColor(2);
        return r;
    };
    if (seg("Лин", !g_log_scale)) g_log_scale = false;
    ImGui::SameLine(0, 2);
    if (seg("Лог", g_log_scale)) g_log_scale = true;
    ImGui::Separator();

    char st[160];
    double m = 0, sd = 0; uint64_t n = 0;
    int pk = g_dev.peak_channel(30);
    if (g_dev.peak_stats(g_dev.spectrum, 30, Device::CHANNELS, m, sd, n) && m > 0)
        snprintf(st, sizeof(st), "Σ %llu    пик %.1f   FWHM %.2f %% (по окну ±4σ, грубо)",
                 (unsigned long long)g_dev.total(), m, 235.48 * sd / m);
    else
        snprintf(st, sizeof(st), "Σ %llu    пик %d", (unsigned long long)g_dev.total(), pk);
    ImGui::PushFont(font_mono);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
    ImGui::TextUnformatted(st);
    ImGui::PopStyleColor(); ImGui::PopFont();

    for (int i = 0; i < Device::CHANNELS; i++) {
        g_spec_x[i] = (float)i;
        g_spec_y[i] = (float)g_dev.spectrum[i];
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImPlot::GetInputMap().Pan = ImGuiMouseButton_Left;
    if (ImPlot::BeginPlot("##plot", avail,
                          ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
        bool shift = ImGui::GetIO().KeyShift;
        ImPlot::SetupAxis(ImAxis_X1, "канал", shift ? ImPlotAxisFlags_Lock : ImPlotAxisFlags_None);
        ImPlot::SetupAxis(ImAxis_Y1, "счёт",  shift ? ImPlotAxisFlags_None : ImPlotAxisFlags_AutoFit);
        if (g_log_scale) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, (double)Device::CHANNELS);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, g_log_scale ? 0.1 : 0.0, 1.0e12);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, Device::CHANNELS, ImPlotCond_Once);
        ImPlot::PushStyleColor(ImPlotCol_Line, c_trace);
        ImPlot::PlotLine("спектр", g_spec_x, g_spec_y, Device::CHANNELS);
        ImPlot::PopStyleColor();
        const Device::HvPeak& h = g_dev.hvp;
        if (h.running || h.done) {           // окно и цель подстройки ВВ
            double xs[2] = { h.lo, h.hi };
            ImPlot::PushStyleColor(ImPlotCol_Line, c_accent_line);
            ImPlot::PlotInfLines("окно", xs, 2);
            ImPlot::PopStyleColor();
            double tg[1] = { h.target };
            ImPlot::PushStyleColor(ImPlotCol_Line, c_cursor);
            ImPlot::PlotInfLines("цель", tg, 1);
            ImPlot::PopStyleColor();
        }
        ImPlot::EndPlot();
    }
    ImGui::End();
}

static void PanelConsole(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    BeginPanel("##con", pos, size);
    PanelHeader("КОНСОЛЬ · shproto");

    const char* chips[] = { "-sta", "-sto", "-rst", "-inf", "-gst", "-diag", "-hv", "-thr", "-opa", "-noise" };
    ImGui::PushFont(font_small);
    for (int i = 0; i < (int)(sizeof(chips) / sizeof(chips[0])); i++) {
        if (i) ImGui::SameLine();
        if (ImGui::SmallButton(chips[i])) g_dev.send_cmd(chips[i]);
    }
    ImGui::PopFont();

    ImGui::BeginChild("log", ImVec2(0, -30), true);
    ImGui::PushFont(font_mono);
    for (const auto& l : g_dev.log) {
        ImVec4 col; const char* pre;
        switch (l.dir) {
        case 1:  col = c_accent;   pre = "-> "; break;
        case 2:  col = c_ok;       pre = "<- "; break;
        case 3:  col = c_danger;   pre = "!! "; break;
        default: col = c_text_dim; pre = ".. "; break;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextWrapped("%s%s", pre, l.text.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    ImGui::EndChild();

    ImGui::SetNextItemWidth(-78);
    bool enter = ImGui::InputText("##cmd", g_cmd_buf, sizeof(g_cmd_buf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((PrimaryButton("Отправить") || enter) && g_cmd_buf[0]) {
        g_dev.send_cmd(g_cmd_buf);
        g_cmd_buf[0] = 0;
    }
    ImGui::End();
}

static void TabSelftest()
{
    using namespace theme;
    Device::LinRun& L = g_dev.lin;
    bool conn = g_dev.connected();
    Caption("Ступеньки ЦАП, коды (1…1350):");
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled(L.running);
    ImGui::InputText("##codes", g_lin_codes, sizeof g_lin_codes);
    ImGui::SetNextItemWidth(80); ImGui::InputInt("импульсов", &g_lin_n, 0, 0);
    ImGui::SetNextItemWidth(80); ImGui::InputInt("период, мкс", &g_lin_period, 0, 0);
    ImGui::EndDisabled();
    if (g_lin_n < 100) g_lin_n = 100;
    if (g_lin_period < 45) g_lin_period = 45;
    ImGui::BeginDisabled(!conn || g_dev.hvp.running);
    if (!L.running) {
        if (PrimaryButton("Прогон линейности", ImVec2(-1, 26))) {
            std::vector<int> codes;
            const char* p = g_lin_codes;
            while (*p) {
                char* e; long v = strtol(p, &e, 10);
                if (e == p) { p++; continue; }
                if (v >= 1 && v <= 1350) codes.push_back((int)v);
                p = e;
            }
            g_dev.lin_start(codes, g_lin_n, g_lin_period);
        }
    } else if (DangerButton("Прервать", ImVec2(-1, 26))) g_dev.lin_abort();
    ImGui::EndDisabled();
    ImGui::TextDisabled("Без ВВ. Набор спектра на время прогона занят.");
    if (!L.msg.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, L.fitted ? c_ok : c_text);
        ImGui::TextWrapped("%s", L.msg.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::BeginTable("lin", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::PushFont(font_small);
        ImGui::TableSetupColumn("код"); ImGui::TableSetupColumn("канал");
        ImGui::TableSetupColumn("σ");   ImGui::TableSetupColumn("в пике");
        ImGui::TableHeadersRow();
        ImGui::PopFont();
        ImGui::PushFont(font_mono);
        for (auto& p : L.pts) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", p.code);
            ImGui::TableSetColumnIndex(1); if (p.ok) ImGui::Text("%.2f", p.ch); else ImGui::TextDisabled("нет пика");
            ImGui::TableSetColumnIndex(2); if (p.ok) ImGui::Text("%.2f", p.sd);
            ImGui::TableSetColumnIndex(3); if (p.ok) ImGui::Text("%llu", (unsigned long long)p.n);
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
    if (L.fitted) {
        static std::vector<float> ax, ay, ar;
        ax.clear(); ay.clear(); ar.clear();
        for (auto& p : L.pts) if (p.ok) {
            ax.push_back((float)p.code); ay.push_back((float)p.ch);
            ar.push_back((float)((p.ch - (L.k * p.code + L.b)) / p.ch * 100.0));
        }
        Caption("Отклонение от прямой, %");
        if (ImPlot::BeginPlot("##linres", ImVec2(-1, 140), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
            ImPlot::SetupAxes("код", "%", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.5f, c_trace2, 1.0f, c_trace2);
            ImPlot::PlotLine("откл", ax.data(), ar.data(), (int)ax.size());
            ImPlot::EndPlot();
        }
    }
}

static void TabCalib()
{
    using namespace theme;
    bool conn = g_dev.connected();
    bool hv = g_dev.hv_on();
    char b[96];

    Caption("Монитор ВВ");
    snprintf(b, sizeof b, "ноль %.1f мВ, k %.4f мВ/В", g_dev.gnum("hv_zero"), g_dev.gnum("hv_k"));
    ImGui::PushFont(font_mono); ImGui::TextUnformatted(b); ImGui::PopFont();
    ImGui::TextDisabled("Ноль: ВВ выкл. ≥ 2 мин (умножитель разрядился).");
    ImGui::BeginDisabled(!conn || hv);
    if (ImGui::Button("Записать ноль (-hvzero)", ImVec2(-1, 0))) g_dev.send_cmd("-hvzero");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Коэффициент: при ВВ ВКЛ измерь выход мультиметром");
    ImGui::TextDisabled("(делитель ≥1 ГОм), введи и запомни показание;");
    ImGui::TextDisabled("затем выключи ВВ и запиши k.");
    ImGui::SetNextItemWidth(90); ImGui::InputFloat("В по мультиметру", &g_mon_meas_v, 0, 0, "%.1f");
    ImGui::BeginDisabled(!conn || !hv || g_mon_meas_v < 100);
    if (ImGui::Button("Запомнить показание монитора", ImVec2(-1, 0))) {
        g_mon_rec_mv = (float)g_dev.gnum("mon_mv");
        g_mon_rec_set = (float)g_dev.gnum("hv_set");
        double zero = g_dev.gnum("hv_zero");
        g_mon_k_new = (float)((zero - g_mon_rec_mv) / g_mon_meas_v);
    }
    ImGui::EndDisabled();
    if (g_mon_k_new > 0) {
        snprintf(b, sizeof b, "монитор %.1f мВ при %.0f В ⇒ k = %.4f", g_mon_rec_mv, g_mon_meas_v, g_mon_k_new);
        ImGui::TextUnformatted(b);
        ImGui::BeginDisabled(!conn || hv || g_mon_k_new < 0.5f || g_mon_k_new > 5.0f);
        snprintf(b, sizeof b, "Записать k = %.4f (-hvk)", g_mon_k_new);
        if (PrimaryButton(b, ImVec2(-1, 0))) {
            char c[48]; snprintf(c, sizeof c, "-hvk %.4f", g_mon_k_new); g_dev.send_cmd(c);
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing(); ImGui::Separator();
    Caption("Pt1000");
    snprintf(b, sizeof b, "%s, R_pu %.1f Ом, raw %.1f", g_dev.gstr("pt").c_str(), g_dev.gnum("pt_rpu"), g_dev.gnum("pt_raw"));
    ImGui::PushFont(font_mono); ImGui::TextUnformatted(b); ImGui::PopFont();
    ImGui::SetNextItemWidth(80); ImGui::InputFloat("°C эталон", &g_pt_ref, 0, 0, "%.2f");
    ImGui::BeginDisabled(!conn || g_pt_ref < 0 || g_pt_ref > 60);
    if (ImGui::Button("Калибровать по эталону (-ptcal)", ImVec2(-1, 0))) {
        char c[48]; snprintf(c, sizeof c, "-ptcal %.2f", g_pt_ref); g_dev.send_cmd(c);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Эталон — термометр на корпусе кристалла после выдержки.");
    ImGui::TextDisabled("Калибровки действуют сразу; во флеш — «Сохранить».");
}

static void TabSettings()
{
    using namespace theme;
    bool conn = g_dev.connected();
    Caption("Порог детектора, коды АЦП (0 — авто 5σ)");
    ImGui::SetNextItemWidth(80); ImGui::InputInt("##thr", &g_thr, 0, 0);
    if (g_thr < 0) g_thr = 0;
    if (g_thr > 1000) g_thr = 1000;
    ImGui::SameLine();
    ImGui::BeginDisabled(!conn);
    if (ImGui::Button("Применить##thr")) { char c[24]; snprintf(c, sizeof c, "-thr %d", g_thr); g_dev.send_cmd(c); g_dev.send_cmd("-thr"); }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Сейчас: %s", g_dev.thr_codes < 0 ? "—" : g_dev.thr_codes == 0 ? "авто" : std::to_string(g_dev.thr_codes).c_str());

    ImGui::Spacing();
    Caption("Усиление PGA (только вне набора, во флеш не пишется)");
    const char* pg[] = { "×2", "×4 (штатно)", "×8", "×16" };
    const int pv[] = { 2, 4, 8, 16 };
    int sel = g_pga == 2 ? 0 : g_pga == 8 ? 2 : g_pga == 16 ? 3 : 1;
    ImGui::SetNextItemWidth(140);
    if (ImGui::Combo("##pga", &sel, pg, 4)) g_pga = pv[sel];
    ImGui::SameLine();
    ImGui::BeginDisabled(!conn || g_dev.acquiring);
    if (ImGui::Button("Применить##pga")) { char c[24]; snprintf(c, sizeof c, "-opa pga %d", g_pga); g_dev.send_cmd(c); }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Эталон формы снят на ×4 — для спектрометрии оставлять ×4.");

    ImGui::Spacing(); ImGui::Separator();
    Caption("Автозапуск ВВ при подаче питания (собранный блок)");
    {
        double hb = g_dev.gnum("hv_boot");
        char b[96];
        if (hb > 0) snprintf(b, sizeof b, "сейчас: %.0f В через ~5 с после старта", hb);
        else snprintf(b, sizeof b, "сейчас: выключен — ВВ включается только командой");
        ImGui::TextUnformatted(b);
        static int s_hvboot = 0;
        if (s_hvboot == 0 && g_dev.gnum("hv_set") > 0) s_hvboot = (int)(g_dev.gnum("hv_set") + 0.5);
        ImGui::BeginDisabled(!conn);
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("В##hvboot", &s_hvboot, 0, 0);
        if (s_hvboot < 0) s_hvboot = 0;
        if (s_hvboot > 1200) s_hvboot = 1200;
        ImGui::SameLine();
        if (ImGui::Button("Включить автозапуск") && s_hvboot > 0) {
            char c[32]; snprintf(c, sizeof c, "-hvboot %d", s_hvboot); g_dev.queue({ c, "-save" });
        }
        ImGui::SameLine();
        if (ImGui::Button("Выключить автозапуск")) g_dev.queue({ "-hvboot off", "-save" });
        ImGui::EndDisabled();
        ImGui::TextDisabled("Сразу пишется во флеш. Перед вскрытием блока — выключить автозапуск и ВВ.");
    }

    ImGui::Spacing(); ImGui::Separator();
    ImGui::BeginDisabled(!conn);
    if (PrimaryButton("Сохранить во флеш (-save)", ImVec2(-1, 26))) g_dev.send_cmd("-save");
    if (ImGui::Button("Проверка тракта (-diag)", ImVec2(-1, 0))) g_dev.send_cmd("-diag");
    if (ImGui::Button("Шум базы (-noise)", ImVec2(-1, 0))) g_dev.send_cmd("-noise");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!conn || g_dev.hv_on());
    if (DangerButton("Умолчания (-defaults, до -save — в ОЗУ)", ImVec2(-1, 0))) g_dev.send_cmd("-defaults");
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextDisabled("Последний ответ:");
    ImGui::PushFont(font_mono); ImGui::TextWrapped("%s", g_dev.last_reply.c_str()); ImGui::PopFont();
}

static void TabTemplate()
{
    using namespace theme;
    bool conn = g_dev.connected();
    Device::RawRun& R = g_dev.raw;
    ImGui::TextWrapped("Эталон формы импульса для детектора. Нужны ВВ на рабочей точке и источник "
                       "(Cs-137): импульсы 100…1200 кодов. Сбор ≈ 0.3 с на порцию.");
    ImGui::SetNextItemWidth(90);
    ImGui::BeginDisabled(R.running);
    ImGui::InputInt("порций", &g_tpl_blocks, 100, 500);
    ImGui::EndDisabled();
    if (g_tpl_blocks < 200) g_tpl_blocks = 200;
    if (g_tpl_blocks > 6000) g_tpl_blocks = 6000;
    ImGui::BeginDisabled(!conn || g_dev.lin.running || g_dev.hvp.running || g_dev.tc.running || g_tpl_state == 1);
    if (!R.running) {
        if (PrimaryButton("1. Собрать порции", ImVec2(-1, 26))) { g_tpl_state = 0; g_tpl_loaded = false; g_dev.raw_start(g_tpl_blocks); }
    } else if (DangerButton("Прервать сбор", ImVec2(-1, 26))) g_dev.raw_abort();
    ImGui::EndDisabled();
    if (R.running || !R.blocks.empty()) {
        float fr = R.target ? (float)R.blocks.size() / R.target : 0.0f;
        char b[64]; snprintf(b, sizeof b, "%zu / %d (сбоев %d)", R.blocks.size(), R.target, R.bad);
        ImGui::ProgressBar(fr, ImVec2(-1, 0), b);
    }
    if (!R.msg.empty()) ImGui::TextDisabled("%s", R.msg.c_str());

    ImGui::BeginDisabled(R.running || R.blocks.empty() || g_tpl_state == 1);
    if (PrimaryButton("2. Рассчитать эталон", ImVec2(-1, 0))) {
        g_tpl_state = 1;
        std::thread([blocks = R.blocks]() {
            tpl::Singles s = tpl::find_singles(blocks);
            tpl::Result r = tpl::build(s);
            char b[200];
            snprintf(b, sizeof b, "порций %d, σ %.2f, одиночных %d, в 100…1200 кодов %d", s.blocks, s.sigma, s.single, s.amp);
            g_tpl_info = b;
            g_tpl_res = r;
            g_tpl_state = 2;
        }).detach();
    }
    ImGui::EndDisabled();
    if (g_tpl_state == 1) ImGui::TextUnformatted("считаю…");
    if (g_tpl_state == 2) {
        ImGui::TextWrapped("%s", g_tpl_info.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, g_tpl_res.ok ? c_ok : c_danger);
        ImGui::TextWrapped("%s", g_tpl_res.msg.c_str());
        ImGui::PopStyleColor();
        if (g_tpl_res.ok) {
            static float xs[tpl::NS * tpl::NPH], ys[tpl::NS * tpl::NPH];
            int k = 0;                          // строки на общей сетке: отсчёт j строки p — время (j − 1) − p/64
            for (int p = 0; p < tpl::NPH; p++) for (int j = 0; j < tpl::NS; j++) {
                xs[k] = (float)(j - tpl::IPK) - (float)p / tpl::NPH; ys[k] = g_tpl_res.q[p][j] / (float)tpl::QMAX; k++;
            }
            if (ImPlot::BeginPlot("##tplshape", ImVec2(-1, 130), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
                ImPlot::SetupAxes("отсчёт от пика", "", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 1.5f, c_trace, 1.0f, c_trace);
                ImPlot::PlotScatter("форма", xs, ys, k);
                ImPlot::EndPlot();
            }
            ImGui::TextDisabled("Норма: невязка < 1 %, размах по фазам < 1 %.");
            ImGui::BeginDisabled(!conn || g_dev.queue_busy() || g_dev.acquiring);
            if (PrimaryButton("3. Загрузить в прибор (-tplw ×64, -tplok)", ImVec2(-1, 0))) {
                g_tpl_load_err0 = g_dev.queue_errors;
                g_dev.queue(tpl::commands(g_tpl_res));
                g_dev.queue({ "-tplok" });
                g_tpl_loaded = true;
            }
            if (ImGui::Button("4. Сохранить во флеш (-save)", ImVec2(-1, 0))) g_dev.send_cmd("-save");
            ImGui::EndDisabled();
            if (g_tpl_loaded) {
                if (g_dev.queue_busy()) ImGui::TextUnformatted("загружаю…");
                else if (g_dev.queue_errors > g_tpl_load_err0) { ImGui::PushStyleColor(ImGuiCol_Text, c_danger); ImGui::TextWrapped("ошибка загрузки: %s", g_dev.queue_last_err.c_str()); ImGui::PopStyleColor(); }
                else { ImGui::PushStyleColor(ImGuiCol_Text, c_ok); ImGui::TextUnformatted("загружен, действует сразу (до -save — в ОЗУ)"); ImGui::PopStyleColor(); }
            }
        }
    }
    ImGui::Spacing(); ImGui::Separator();
    ImGui::BeginDisabled(!conn);
    if (ImGui::Button("Вернуть вшитый эталон (-tplclr)", ImVec2(-1, 0))) g_dev.send_cmd("-tplclr");
    ImGui::EndDisabled();
}

// ---- Регистратор: автономные записи во флешку прибора (SPEC_logger.md) ----
static int   g_lg_period = 600, g_lg_delay = 120;
static char  g_lg_dir[260] = "";
static bool  g_lg_erase_ok = false;
static int   g_lg_sess_idx = 0;
static float g_lg_lo = 300, g_lg_hi = 6000;
static std::string g_lg_msg;
static double g_lg_poll_t = 0;

static std::string LogDefaultDir()
{
    std::filesystem::path base;
    if (const wchar_t* up = _wgetenv(L"USERPROFILE")) base = std::filesystem::path(up) / L"Documents";
    else base = std::filesystem::current_path();
    time_t t = time(nullptr); struct tm lt; localtime_s(&lt, &t);
    char n[48]; strftime(n, sizeof n, "gammapult_log_%Y%m%d_%H%M", &lt);
    std::string u8 = (base / n).u8string();
    return u8;
}

static void TabLogger()
{
    using namespace theme;
    bool conn = g_dev.connected();
    Device::LogDump& D = g_dev.logd;
    ImGui::TextWrapped("Прибор сам набирает спектр и каждые N минут пишет во внутреннюю флешку запись: накопленный "
                       "спектр, температуру Pt1000, ВВ, загрузку. Питание — от пауэрбанка, компьютер не нужен. "
                       "Потом выгрузка сюда: термокомпенсация по прогреву/остыванию, сложение долгих наборов.");
    // состояние — раз в 3 с, пока вкладка открыта
    double now = ImGui::GetTime();
    if (conn && !D.running && now - g_lg_poll_t > 3.0) { g_dev.send_cmd("-log", true); g_lg_poll_t = now; }
    ImGui::PushFont(font_mono);
    ImGui::TextWrapped("%s", g_dev.log_status.empty() ? "—" : g_dev.log_status.c_str());
    ImGui::PopFont();

    Caption("Автозапуск после подачи питания");
    ImGui::SetNextItemWidth(90); ImGui::InputInt("период, с", &g_lg_period, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90); ImGui::InputInt("выдержка, с", &g_lg_delay, 0, 0);
    if (g_lg_period < 60) g_lg_period = 60;
    if (g_lg_period > 86400) g_lg_period = 86400;
    if (g_lg_delay < 0) g_lg_delay = 0;
    if (g_lg_delay > 7200) g_lg_delay = 7200;
    ImGui::BeginDisabled(!conn || D.running);
    if (ImGui::Button("Включить автозапуск")) { char c[48]; snprintf(c, sizeof c, "-log on %d %d", g_lg_period, g_lg_delay); g_dev.send_cmd(c); }
    ImGui::SameLine();
    if (ImGui::Button("Выключить")) g_dev.send_cmd("-log off");
    ImGui::SameLine();
    if (ImGui::Button("Синхронизировать время")) { char c[48]; snprintf(c, sizeof c, "-log time %lld", (long long)time(nullptr)); g_dev.send_cmd(c); }
    if (ImGui::Button("Начать сессию сейчас")) g_dev.send_cmd("-log start");
    ImGui::SameLine();
    if (ImGui::Button("Остановить сессию")) g_dev.send_cmd("-log stop");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Выдержка — после включения: ВВ встаёт ~1.5 мин, ФЭУ устанавливается 30–60 мин.");

    Caption("Выгрузка");
    if (!g_lg_dir[0]) { std::string d = LogDefaultDir(); strncpy(g_lg_dir, d.c_str(), sizeof g_lg_dir - 1); }
    ImGui::SetNextItemWidth(-1); ImGui::InputText("##lgdir", g_lg_dir, sizeof g_lg_dir);
    ImGui::BeginDisabled(!conn || D.running);
    if (PrimaryButton("Выгрузить журнал", ImVec2(-1, 26))) { g_lg_msg.clear(); g_dev.logd_start(0); }
    ImGui::EndDisabled();
    if (D.running) {
        float fr = D.items.empty() ? 0.0f : (float)D.idx / (float)D.items.size();
        ImGui::ProgressBar(fr, ImVec2(-1, 0), D.msg.c_str());
        if (ImGui::Button("Прервать")) g_dev.logd_abort();
    } else if (!D.msg.empty()) ImGui::TextDisabled("%s", D.msg.c_str());
    static bool written = false;
    if (D.running) written = false;
    if (D.done && !written) {
        written = true;
        std::string err;
        if (logdump::write_dir(g_lg_dir, D.recs, (int)g_lg_lo, (int)g_lg_hi, err))
            g_lg_msg = std::string("сохранено: ") + g_lg_dir + (err.empty() ? "" : " — " + err);
        else g_lg_msg = "ошибка: " + err;
        g_lg_sess_idx = 0;
    }
    if (!g_lg_msg.empty()) ImGui::TextWrapped("%s", g_lg_msg.c_str());

    // разбор выгруженного
    auto sess = logdump::sessions(D.recs);
    if (!sess.empty()) {
        Caption("Сессия");
        if (g_lg_sess_idx >= (int)sess.size()) g_lg_sess_idx = (int)sess.size() - 1;
        std::string lbl = std::to_string(sess[g_lg_sess_idx]);
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("##lgsess", lbl.c_str())) {
            for (int i = 0; i < (int)sess.size(); i++) {
                auto ss = logdump::session(D.recs, sess[i]);
                char b[96];
                snprintf(b, sizeof b, "%u — %zu записей, %.1f ч", sess[i], ss.size(), ss.empty() ? 0.0 : ss.back()->h.t_end_ms / 3.6e6);
                if (ImGui::Selectable(b, i == g_lg_sess_idx)) g_lg_sess_idx = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60); ImGui::InputFloat("##lglo", &g_lg_lo, 0, 0, "%.0f");
        ImGui::SameLine(); ImGui::TextUnformatted("–"); ImGui::SameLine();
        ImGui::SetNextItemWidth(60); ImGui::InputFloat("окно пика", &g_lg_hi, 0, 0, "%.0f");
        auto s = logdump::session(D.recs, sess[g_lg_sess_idx]);
        static std::vector<logdump::PeakPt> pk;
        static uint32_t pk_sess = 0xFFFFFFFFu; static float pk_lo = -1, pk_hi = -1; static size_t pk_n = 0;
        if (pk_sess != sess[g_lg_sess_idx] || pk_lo != g_lg_lo || pk_hi != g_lg_hi || pk_n != D.recs.size()) {
            pk = logdump::peaks(s, (int)g_lg_lo, (int)g_lg_hi);
            pk_sess = sess[g_lg_sess_idx]; pk_lo = g_lg_lo; pk_hi = g_lg_hi; pk_n = D.recs.size();
        }
        static std::vector<double> tx, py, ty;
        tx.clear(); py.clear(); ty.clear();
        for (auto& p : pk) if (p.ok) { tx.push_back(p.t_mid_s / 3600.0); py.push_back(p.peak); ty.push_back(std::isfinite(p.temp) ? p.temp : NAN); }
        if (!tx.empty() && ImPlot::BeginPlot("##lgplot", ImVec2(-1, 170), ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
            ImPlot::SetupAxes("ч", "пик, канал", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxis(ImAxis_Y2, "°C", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2.5f, c_trace, 1.0f, c_trace);
            ImPlot::PlotLine("пик", tx.data(), py.data(), (int)tx.size());
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            ImPlot::SetNextLineStyle(c_warn);
            ImPlot::PlotLine("T", tx.data(), ty.data(), (int)tx.size());
            ImPlot::EndPlot();
        }
        ImGui::BeginDisabled(pk.empty());
        if (ImGui::Button("В термокомпенсацию")) {
            g_dev.tc.pts.clear();
            int skip = 0;
            for (auto& p : pk) {
                bool bad = !p.ok || !std::isfinite(p.temp) || (p.flags & (LOGF_HOST_RST | LOGF_HOST_STO | LOGF_HV_TRIP | LOGF_TC_ON))
                         || (std::isfinite(p.temp_span) && p.temp_span > 1.0);
                if (bad) { skip++; continue; }
                g_dev.tc.pts.push_back({ p.temp, p.peak, p.sigma, p.n });
            }
            g_lg_msg = "во вкладку «Термо» передано точек: " + std::to_string(g_dev.tc.pts.size()) +
                       " (пропущено " + std::to_string(skip) + ": события хоста, ВВ, поправка прибора включена или T менялась > 1 °C за интервал)";
        }
        ImGui::SameLine();
        if (ImGui::Button("Сложить с выравниванием (CSV)")) {
            std::vector<double> sum; std::string msg;
            if (logdump::aligned_sum(s, (int)g_lg_lo, (int)g_lg_hi, sum, msg)) {
                std::filesystem::path pth = std::filesystem::u8path(g_lg_dir);
                std::error_code ec; std::filesystem::create_directories(pth, ec);
                char n[48]; snprintf(n, sizeof n, "sum_aligned_s%u.csv", sess[g_lg_sess_idx]);
                FILE* f = _wfopen((pth / n).wstring().c_str(), L"w");
                if (f) { fprintf(f, "channel,count\n"); for (size_t i = 0; i < sum.size(); i++) fprintf(f, "%zu,%.0f\n", i, sum[i]); fclose(f); g_lg_msg = msg + " → " + n; }
                else g_lg_msg = "не открыть файл в папке выгрузки";
            } else g_lg_msg = msg;
        }
        ImGui::EndDisabled();
    }

    Caption("Стирание");
    ImGui::Checkbox("да, стереть весь журнал", &g_lg_erase_ok);
    ImGui::SameLine();
    ImGui::BeginDisabled(!conn || D.running || !g_lg_erase_ok);
    if (DangerButton("Стереть журнал")) { g_dev.send_cmd("-log erase"); g_lg_erase_ok = false; }
    ImGui::EndDisabled();
}

static void TabThermo()
{
    using namespace theme;
    bool conn = g_dev.connected();
    Device::TcRun& T = g_dev.tc;
    ImGui::TextWrapped("Термокомпенсация усиления: запись пика источника при меняющейся температуре "
                       "(прогрев/остывание, часы). ВВ включено, источник на месте, Pt1000 на кристалле. "
                       "На время записи поправка в приборе выключена.");
    ImGui::TextDisabled("Сейчас в приборе: %s", g_dev.tc_reply.empty() ? "— (нажми «Запросить»)" : g_dev.tc_reply.c_str() + 7);
    ImGui::BeginDisabled(!conn || g_dev.queue_busy());
    if (ImGui::SmallButton("Запросить -tc")) g_dev.queue({ "-tc" });
    ImGui::EndDisabled();
    float f;
    ImGui::BeginDisabled(T.running);
    ImGui::SetNextItemWidth(70); f = (float)T.secs; if (ImGui::InputFloat("с на точку", &f, 0, 0, "%.0f")) T.secs = f < 20 ? 20 : f;
    ImGui::SetNextItemWidth(60); f = (float)T.lo; if (ImGui::InputFloat("##tlo", &f, 0, 0, "%.0f")) T.lo = f;
    ImGui::SameLine(); ImGui::TextUnformatted("–"); ImGui::SameLine();
    ImGui::SetNextItemWidth(60); f = (float)T.hi; if (ImGui::InputFloat("окно пика", &f, 0, 0, "%.0f")) T.hi = f;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!conn || g_dev.lin.running || g_dev.hvp.running || g_dev.raw.running);
    if (!T.running) {
        if (PrimaryButton("Начать запись", ImVec2(-1, 26))) g_dev.tc_start();
    } else {
        if (ImGui::Button("Остановить (вернуть прежние k)", ImVec2(-1, 0))) g_dev.tc_stop(true);
    }
    ImGui::EndDisabled();
    if (!T.msg.empty()) ImGui::TextDisabled("%s", T.msg.c_str());

    if (!T.pts.empty()) {
        static std::vector<float> tx, ty;
        tx.clear(); ty.clear();
        double tmin = 1e9, tmax = -1e9;
        for (auto& p : T.pts) { tx.push_back((float)p.t); ty.push_back((float)p.peak); if (p.t < tmin) tmin = p.t; if (p.t > tmax) tmax = p.t; }
        ImGui::Text("точек %zu, температура %.1f…%.1f °C", T.pts.size(), tmin, tmax);
        if (ImPlot::BeginPlot("##tcplot", ImVec2(-1, 140), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
            ImPlot::SetupAxes("°C", "пик", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, c_trace2, 1.0f, c_trace2);
            ImPlot::PlotScatter("пик", tx.data(), ty.data(), (int)tx.size());
            ImPlot::EndPlot();
        }
        ImGui::SetNextItemWidth(70); ImGui::InputFloat("t0, °C", &g_tc_t0, 0, 0, "%.1f");
        ImGui::SameLine();
        if (ImGui::SmallButton("= средняя")) { double s = 0; for (auto& p : T.pts) s += p.t; g_tc_t0 = (float)(s / T.pts.size()); }
        ImGui::RadioButton("линейная", &g_tc_order, 1); ImGui::SameLine(); ImGui::RadioButton("квадратичная", &g_tc_order, 2);
        double k1 = 0, k2 = 0, rms = 0;
        bool ok = g_dev.tc_fit(g_tc_t0, g_tc_order, k1, k2, rms);
        if (ok) {
            ImGui::PushFont(font_mono);
            ImGui::Text("k1 = %.4e /°C  (%.3f %%/°C)", k1, k1 * 100.0);
            ImGui::Text("k2 = %.4e /°C²", k2);
            ImGui::Text("остаток %.3f %%", rms);
            ImGui::PopFont();
            if (tmax - tmin < 5) ImGui::TextColored(c_warn, "размах температур < 5 °C — коэффициенты ненадёжны");
            ImGui::BeginDisabled(!conn || g_dev.queue_busy());
            if (PrimaryButton("Записать в прибор (-tc) и сохранить", ImVec2(-1, 0))) {
                if (T.running) g_dev.tc_stop(false);
                char c[96]; snprintf(c, sizeof c, "-tc %.3f %.6e %.6e", g_tc_t0, k1, k2);
                g_dev.queue({ c, "-save" });
            }
            ImGui::EndDisabled();
        } else ImGui::TextDisabled("для расчёта нужно ≥ %d точек", g_tc_order == 2 ? 4 : 3);
    }
}

static void PanelService(ImVec2 pos, ImVec2 size)
{
    BeginPanel("##svc", pos, size);
    PanelHeader("СЕРВИС");
    if (ImGui::BeginTabBar("##svctabs")) {
        if (ImGui::BeginTabItem("Самотест ЦАП")) { TabSelftest(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Калибровки"))   { TabCalib();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Эталон"))       { TabTemplate(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Термо"))        { TabThermo();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Регистратор"))  { TabLogger();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Настройки"))    { TabSettings(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

static void HelpSection(const char* h)
{
    ImGui::Spacing();
    ImGui::PushFont(theme::font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::c_accent);
    ImGui::TextUnformatted(h);
    ImGui::PopStyleColor(); ImGui::PopFont();
    ImGui::Separator();
}

static void DrawHelp()
{
    if (g_ui.help_req) { ImGui::OpenPopup("###help"); g_ui.help_req = false; }

    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720, 600), ImGuiCond_Appearing);
    char title[64]; snprintf(title, sizeof title, "%s###help", T("status.help"));
    if (!ImGui::BeginPopupModal(title, nullptr)) return;
    ImGui::BeginChild("helpbody", ImVec2(0, -36), false);

    HelpSection("Назначение");
    ImGui::TextWrapped(
        "Сервисный пульт для настройки прибора с прошивкой gamma_fw: высокое напряжение, калибровки "
        "монитора ВВ и Pt1000, порог, самотест тракта. Настроил — сохранил во флеш — закрыл пульт и "
        "работаешь в BecqMoni. Энергокалибровка делается в BecqMoni (кнопка «Записать в устройство»). "
        "Один COM-порт держит одна программа: при работе пульта закрой BecqMoni, и наоборот.");
    HelpSection("Высокое напряжение");
    ImGui::TextWrapped(
        "Включение — с подтверждением. Красная кнопка выключает ВВ сразу. Перед пайкой и щупами — "
        "ВВ выкл и монитор < 10 В. «Подстроить» сам ведёт ВВ так, чтобы пик (например, Cs-137) встал "
        "на заданный канал: набор → центроид в окне → новое ВВ по закону усиления V^k (k ≈ 5.3), шаг "
        "не больше 15 %, до попадания в допуск 1 %. Окно пика ставь так, чтобы в нём был только фотопик.");
    HelpSection("Калибровки");
    ImGui::TextWrapped(
        "Ноль монитора — при выключенном ВВ после разряда. Коэффициент — по мультиметру с высокоомным "
        "делителем: при включённом ВВ ввести показание и «Запомнить», затем выключить ВВ и записать k "
        "(прошивка меняет k только при выключенном ВВ). Pt1000 — по эталонному термометру. После "
        "калибровок — «Сохранить во флеш».");
    HelpSection("Самотест ЦАП");
    ImGui::TextWrapped(
        "Серия импульсов встроенного ЦАП на вход шейпера (без ВВ и источника). Для каждой ступеньки — "
        "центроид пика; прямая канал = k·код + b, R² и отклонение. Норма: R² > 0.9999, отклонение "
        "< 0.2 %, в пике > 99 % импульсов, k ≈ 4.93 канала на код.");
    HelpSection("Эталон формы");
    ImGui::TextWrapped(
        "Вкладка «Эталон»: при ВВ на рабочей точке и с источником собрать порции (≈2000, ~11 мин), "
        "рассчитать (тот же алгоритм, что tools/tpl_pmt.py, результат совпадает бит в бит), загрузить "
        "и сохранить. Нужен при смене детектора или шейпера. Вернуть вшитый — -tplclr.");
    HelpSection("Термокомпенсация");
    ImGui::TextWrapped(
        "Вкладка «Термо»: запись пика источника и температуры Pt1000 точка за точкой, пока прибор "
        "прогревается или остывает (размах ≥ 5…10 °C). По точкам — k1 (и k2) усиления; запись -tc и -save. "
        "Прибор делит канал на G(T) = 1 + k1·(T − t0) + k2·(T − t0)².");
    HelpSection("Командная строка");
    ImGui::PushFont(theme::font_mono);
    ImGui::TextWrapped(
        "gammapult list\n"
        "gammapult info    --port COM4\n"
        "gammapult gst     --port COM4\n"
        "gammapult acquire --port COM4 --seconds 60 --out spec.csv\n"
        "gammapult lin     --port COM4 [--codes \"100 600 1300\"]\n"
        "gammapult cmd     --port COM4 --send \"-diag\"");
    ImGui::PopFont();

    ImGui::EndChild();
    ImGui::Separator();
    if (ImGui::Button("Закрыть", ImVec2(110, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// прежняя раскладка панелей: пока расширенный режим (заменит Task 10)
static void DrawPanels(ImVec2 area, ImVec2 asz)
{
    float p = 5.0f, g = 5.0f;
    float innerX = area.x + p, innerY = area.y + p;
    float innerW = asz.x - 2 * p, innerH = asz.y - 2 * p;

    gLeftW    = clampf(gLeftW, 220.0f, innerW - gRightW - 2 * g - 260.0f);
    gRightW   = clampf(gRightW, 260.0f, innerW - gLeftW - 2 * g - 260.0f);
    gConnH    = clampf(gConnH, 150.0f, innerH - 320.0f);
    gTeleH    = clampf(gTeleH, 120.0f, innerH - gConnH - 180.0f);
    gConsoleH = clampf(gConsoleH, 110.0f, innerH - 220.0f);

    float leftW = gLeftW, rightW = gRightW;
    float centerW = innerW - leftW - rightW - 2 * g;
    float connH = gConnH, teleH = gTeleH;
    float hvH = innerH - connH - teleH - 2 * g;
    float consoleH = gConsoleH, spectrumH = innerH - consoleH - g;
    float lx = innerX, cx = innerX + leftW + g, rx = cx + centerW + g;

    PanelConnection(ImVec2(lx, innerY),                         ImVec2(leftW, connH));
    PanelTelemetry (ImVec2(lx, innerY + connH + g),             ImVec2(leftW, teleH));
    PanelHV        (ImVec2(lx, innerY + connH + g + teleH + g), ImVec2(leftW, hvH));
    PanelSpectrum  (ImVec2(cx, innerY),                         ImVec2(centerW, spectrumH));
    PanelConsole   (ImVec2(cx, innerY + spectrumH + g),         ImVec2(centerW, consoleH));
    PanelService   (ImVec2(rx, innerY),                         ImVec2(rightW, innerH));

    gLeftW    += Splitter("##sp_lc", ImVec2(lx + leftW + g * 0.5f - 3, innerY),             ImVec2(6, innerH), true);
    gRightW   -= Splitter("##sp_cr", ImVec2(rx - g * 0.5f - 3, innerY),                     ImVec2(6, innerH), true);
    gConsoleH -= Splitter("##sp_sc", ImVec2(cx, innerY + spectrumH + g * 0.5f - 3),         ImVec2(centerW, 6), false);
    gConnH    += Splitter("##sp_ct", ImVec2(lx, innerY + connH + g * 0.5f - 3),             ImVec2(leftW, 6), false);
    gTeleH    += Splitter("##sp_tc", ImVec2(lx, innerY + connH + g + teleH + g * 0.5f - 3), ImVec2(leftW, 6), false);
}

static void DrawApp()
{
    DrawShell();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    float topH = TopBarHeight(), statusH = StatusBarHeight();
    ImVec2 area(vp->Pos.x, vp->Pos.y + topH);
    ImVec2 asz(vp->Size.x, vp->Size.y - topH - statusH);

    ds::Mode m = g_ui.mode;                  // режим на этот кадр (ui_go внутри кадра сработает со следующего)
    if (m == ds::Mode::Wizard)    DrawWizard(area, asz);
    else if (m == ds::Mode::Home) DrawHome(area, asz);
    else                          DrawPanels(area, asz);

    WizTick();
    DrawHelp();
}

// ================= main =================

int run_gui()
{
    {   // язык интерфейса из %APPDATA%\gammapult\gammapult.ini (нет файла: русский)
        AppCfg cfg;
        appcfg_load(cfg);
        i18n_set(cfg.lang);
    }
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       GetModuleHandle(nullptr), nullptr, nullptr, nullptr,
                       nullptr, L"gammapult", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"GammaPult - service console",
                                WS_OVERLAPPEDWINDOW, 60, 40, 1440, 920,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    theme::LoadFonts(io);
    theme::ApplyStyle();
    theme::ApplyPlotStyle();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    g_dev.refresh_ports();
    if (!g_dev.ports.empty()) g_port_sel = (int)g_dev.ports.size() - 1;   // мост FT231X обычно последний

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        g_dev.poll();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawApp();

        ImGui::Render();
        const float clear[4] = { 0.043f, 0.051f, 0.067f, 1.0f };   // bg-app
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    g_dev.disconnect();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// ================= DirectX 11 (стандартный пример Dear ImGui) =================

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fla[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fla, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fla, 2, D3D11_SDK_VERSION,
            &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (res != S_OK) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

static void CreateRenderTarget()
{
    ID3D11Texture2D* bb = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_pd3dDevice->CreateRenderTargetView(bb, nullptr, &g_mainRenderTargetView);
    bb->Release();
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                                        DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
