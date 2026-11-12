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

// ================= мастер ввода в строй =================
// Из правильно собранного прибора (стек + делитель + ФЭУ + кристалл) — в готовый к работе: прошивка,
// проверка тракта, монитор ВВ, ВВ по пику Cs-137, эталон формы, разрешение, базовая энергокалибровка,
// сохранение и отчёт. Каждое действие — кнопкой; «Далее» открывается, когда шаг выполнен.

struct Wiz {
    int  step = 0;                             // 0 — «Подготовка», 1… — ds::Step + 1
    bool chk[7] = {};
    // прошивка
    std::atomic<int> fl_state{ 0 };            // 0 — нет, 1 — идёт, 2 — готово, 3 — ошибка
    std::atomic<double> fl_prog{ 0 };
    std::mutex fl_mtx;
    std::vector<std::string> fl_log;
    std::string fl_err, fl_port;
    bool fl_reconnect = false;
    // тракт
    bool diag_sent = false, diag_ok = false;
    std::string diag_text;
    bool lin_started = false;
    float pt_ref = 25.0f;
    // монитор
    bool hvzero_sent = false;
    // ВВ
    float start_v = 600.0f;
    bool hv1_started = false, hv2_started = false;
    double hv1_v = 0, hv1_peak = 0, hv2_v = 0, hv2_peak = 0;
    // эталон: 0 — нет, 1 — сбор, 2 — расчёт, 3 — загрузка, 4 — готово, 5 — ошибка
    int tpl_phase = 0; int tpl_err0 = 0;
    // разрешение
    int fw_phase = 0; double fw_t0 = 0; float fw_secs = 120.0f;
    peakfit::Result fwr;
    // энергокалибровка
    bool cal_sent = false; int cal_err0 = 0; double cal_k = 0;
    // итог
    bool saved = false; std::string report_path;
    bool autohv = true; int hvboot_v = 0;      // автозапуск ВВ при подаче питания (-hvboot)
};
static Wiz g_wiz;

static const char* WIZ_STEPS[] = {
    "Подготовка", "Прошивка", "Проверка тракта", "Монитор ВВ", "ВВ по пику",
    "Эталон формы", "Повторная подстройка", "Разрешение", "Энергокалибровка", "Сохранение и отчёт"
};
static const int WIZ_N = 10;

static bool WizLinOk()
{
    const Device::LinRun& L = g_dev.lin;
    if (!L.fitted || L.running) return false;
    for (auto& p : L.pts) if (!p.ok || p.n < (uint64_t)(0.99 * L.n)) return false;
    return L.r2 > 0.9999 && L.dev < 0.2;
}

static bool WizDone(int s)
{
    Wiz& w = g_wiz;
    switch (s) {
    case 0: { for (bool c : w.chk) if (!c) return false; return true; }
    case 1: return g_dev.connected() && g_dev.fw_info.rfind("GammaSpec", 0) == 0;
    case 2: return w.diag_ok && g_dev.gnum("vref_ok", 0) > 0.5 && g_dev.gstr("pt", "-") == "ok" && WizLinOk();
    case 3: return w.hvzero_sent;
    case 4: return w.hv1_started && g_dev.hvp.done && !g_dev.hvp.running && w.hv1_peak > 0;
    case 5: return w.tpl_phase == 4;
    case 6: return w.hv2_started && g_dev.hvp.done && !g_dev.hvp.running && w.hv2_peak > 0;
    case 7: return w.fw_phase == 3 && w.fwr.ok;
    case 8: return w.cal_sent && !g_dev.queue_busy() && g_dev.queue_errors == w.cal_err0;
    case 9: return w.saved;
    }
    return false;
}

static void PassLine(bool ok, const char* text)
{
    using namespace theme;
    ImGui::PushStyleColor(ImGuiCol_Text, ok ? c_ok : c_warn);
    ImGui::TextUnformatted(ok ? "√" : "•");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", text);
}

static void WizFlashStart(const std::string& port)
{
    Wiz& w = g_wiz;
    std::vector<uint8_t> img; std::string tag;
    if (!fwflash::embedded(img, tag)) { w.fl_err = "встроенной прошивки нет"; w.fl_state = 3; return; }
    g_dev.disconnect();
    w.fl_state = 1; w.fl_prog = 0; w.fl_err.clear(); w.fl_port = port;
    { std::lock_guard<std::mutex> lk(w.fl_mtx); w.fl_log.clear(); w.fl_log.push_back("образ " + tag + ", " + std::to_string(img.size()) + " байт"); }
    std::thread([img, port]() {
        Wiz& w = g_wiz;
        std::string err;
        bool ok = fwflash::flash(port, img, true,
            [](const std::string& m) { std::lock_guard<std::mutex> lk(g_wiz.fl_mtx); g_wiz.fl_log.push_back(m); },
            [](double f) { g_wiz.fl_prog = f; }, err);
        w.fl_err = err;
        w.fl_state = ok ? 2 : 3;
        w.fl_reconnect = true;
    }).detach();
}

static void WizWriteReport()
{
    Wiz& w = g_wiz;
    time_t t = time(nullptr); struct tm lt; localtime_s(&lt, &t);
    char fn[160], ts[64];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", &lt);
    std::string sn = g_dev.serial.empty() ? "noserial" : g_dev.serial;
    char d8[32]; strftime(d8, sizeof d8, "%Y%m%d_%H%M", &lt);
    snprintf(fn, sizeof fn, "gammapult_ввод_%s_%s.txt", sn.c_str(), d8);
    FILE* f = fopen(fn, "w");
    if (!f) { w.report_path = "(не удалось записать отчёт)"; return; }
    const Device::LinRun& L = g_dev.lin;
    fprintf(f, "Ввод в строй гамма-спектрометра — %s\n", ts);
    fprintf(f, "Серийник: %s\nПрошивка: %s\n\n", sn.c_str(), g_dev.fw_info.c_str());
    fprintf(f, "Тракт: -diag %s; опора %.1f мВ; шум базы без ВВ σ %.2f кода; Pt1000 %s %.2f °C\n",
            w.diag_ok ? "ok" : "НЕ ОК", g_dev.gnum("vref"), g_dev.gnum("sigma"), g_dev.gstr("pt").c_str(), g_dev.gnum("t"));
    fprintf(f, "Самотест ЦАП: канал = %.4f·код %+.2f, R² %.6f, отклонение ≤ %.3f %%\n", L.k, L.b, L.r2, L.dev);
    fprintf(f, "Монитор ВВ: ноль %.1f мВ, k %.4f мВ/В\n", g_dev.gnum("hv_zero"), g_dev.gnum("hv_k"));
    fprintf(f, "ВВ по пику (до эталона): %.0f В, пик %.1f\n", w.hv1_v, w.hv1_peak);
    fprintf(f, "Эталон формы: %s\n", g_tpl_res.msg.c_str());
    fprintf(f, "ВВ по пику (рабочее): %.0f В, пик %.1f\n", w.hv2_v, w.hv2_peak);
    fprintf(f, "Разрешение Cs-137 (%.0f с): пик %.2f, FWHM %.2f ± %.2f %%\n", w.fw_secs, w.fwr.peak, w.fwr.fwhm_pct, w.fwr.fwhm_err);
    if (w.cal_sent) fprintf(f, "Энергокалибровка (базовая, по Cs-137): E = %.6f · канал\n", w.cal_k);
    if (w.hvboot_v > 0) fprintf(f, "Автозапуск ВВ при подаче питания: %d В\n", w.hvboot_v);
    else fprintf(f, "Автозапуск ВВ: выключен (ВВ только командой)\n");
    fprintf(f, "Порог: %s; термокомпенсация: %s\n", g_dev.thr_codes == 0 ? "авто 5σ" : std::to_string(g_dev.thr_codes).c_str(),
            g_dev.tc_reply.empty() ? "не настроена" : g_dev.tc_reply.c_str());
    fclose(f);
    char full[MAX_PATH];
    w.report_path = _fullpath(full, fn, MAX_PATH) ? full : fn;
}

static void WizStepBody(int s)
{
    using namespace theme;
    Wiz& w = g_wiz;
    bool conn = g_dev.connected();
    char b[256];
    switch (s) {
    case 0: {
        ImGui::TextWrapped("Проверь сборку перед первым включением. Мастер ведёт от «собранного железа» до прибора, "
                           "готового к работе в BecqMoni. Нужны: источник Cs-137, ~40 минут, по желанию — мультиметр "
                           "с высоковольтным делителем и эталонный термометр.");
        ImGui::Spacing();
        const char* items[7] = {
            "Стек собран: USB — ВВ — МК — делитель; цанговые пины и коаксиал пропаяны",
            "Между платами ВВ и МК стоит экран на GND (без него помеха 257 кГц)",
            "На плате МК: 100 нФ на NRST, провод с BOOT0 (вывод 45) через 10 кОм",
            "Pt1000: перемычка между пятаками TP_TSENS301, второй вывод датчика на GND; датчик на корпусе кристалла",
            "Распиновка ФЭУ сверена с посадкой делителя (P, G, K, Dy1…Dy8); ВВ-провод изолирован",
            "Кристалл и ФЭУ в светонепроницаемом корпусе; ФЭУ не видел яркого света последние 30 минут",
            "Плата ВВ проверена и залита силиконом; рядом нет щупов и голых рук",
        };
        for (int i = 0; i < 7; i++) ImGui::Checkbox(items[i], &w.chk[i]);
        break;
    }
    case 1: {
        if (!g_ui.flash_port.empty()) {             // R1: порт выбран на главном экране («Прибор не отвечает? Прошить»)
            for (int i = 0; i < (int)g_dev.ports.size(); i++)
                if (g_dev.ports[i] == g_ui.flash_port) g_port_sel = i;
            g_ui.flash_port.clear();
        }
        std::vector<uint8_t> img; std::string tag;
        bool emb = fwflash::embedded(img, tag);
        if (conn && g_dev.fw_info.rfind("GammaSpec", 0) == 0) {
            PassLine(true, ("прибор отвечает: " + g_dev.fw_info).c_str());
            ImGui::TextDisabled("Серийник %s. Можно обновить прошивку до встроенной (%s) — настройки сохранятся.",
                                g_dev.serial.c_str(), emb ? tag.c_str() : "нет");
        } else {
            ImGui::TextWrapped("Если прибор ещё не прошит (голый МК): соедини провод BOOT0 (вывод 45) через 10 кОм с +3.3 В, "
                               "затем подключи USB — МК стартует в системный загрузчик. Если прибор уже с прошивкой — просто "
                               "подключи его в панели «Подключение» (или нажми «Прошить» — прошивка сама уйдёт в загрузчик).");
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Порт: %s", g_dev.ports.empty() ? "—" : g_dev.ports[g_port_sel].c_str());
        ImGui::BeginDisabled(w.fl_state == 1 || !emb || g_dev.ports.empty() || Busy());
        snprintf(b, sizeof b, "Прошить встроенную прошивку (%s)", emb ? tag.c_str() : "нет образа");
        if (PrimaryButton(b, ImVec2(-1, 26))) WizFlashStart(g_dev.ports[g_port_sel]);
        ImGui::EndDisabled();
        if (w.fl_state == 1) ImGui::ProgressBar((float)w.fl_prog, ImVec2(-1, 0));
        {
            std::lock_guard<std::mutex> lk(w.fl_mtx);
            for (auto& m : w.fl_log) ImGui::TextDisabled("%s", m.c_str());
        }
        if (w.fl_state == 2) {
            PassLine(true, "прошито и проверено");
            ImGui::TextColored(c_warn, "Если BOOT0 был на +3.3 В — переставь его на GND, иначе после переподключения МК снова уйдёт в загрузчик.");
        }
        if (w.fl_state == 3) { ImGui::PushStyleColor(ImGuiCol_Text, c_danger); ImGui::TextWrapped("ошибка: %s", w.fl_err.c_str()); ImGui::PopStyleColor(); }
        if (!conn && w.fl_state != 1 && !g_dev.ports.empty() && ImGui::Button("Подключиться к прибору", ImVec2(-1, 0)))
            g_dev.connect(g_dev.ports[g_port_sel]);
        break;
    }
    case 2: {
        ImGui::TextWrapped("ВВ выключено. Проверка тракта: напряжения узлов, опора, шум базы, Pt1000, линейность по встроенному ЦАП.");
        ImGui::BeginDisabled(!conn || Busy());
        if (ImGui::Button("Проверка тракта (-diag)")) { w.diag_sent = true; g_dev.last_reply.clear(); g_dev.send_cmd("-diag"); }
        ImGui::EndDisabled();
        if (w.diag_sent && g_dev.last_reply.rfind("-ok diag", 0) == 0) { w.diag_text = g_dev.last_reply; w.diag_ok = w.diag_text.find("tract=ok") != std::string::npos; }
        if (!w.diag_text.empty()) PassLine(w.diag_ok, w.diag_text.c_str());
        bool vok = g_dev.gnum("vref_ok", 0) > 0.5;
        snprintf(b, sizeof b, "опора %.1f мВ (норма 2500 ± 15)", g_dev.gnum("vref")); PassLine(vok, b);
        double sg = g_dev.gnum("sigma");
        snprintf(b, sizeof b, "шум базы σ %.2f кода (без ВВ ожидается ≈ 2…2.5)", sg); PassLine(sg > 0.5 && sg < 3.5, b);
        bool pok = g_dev.gstr("pt", "-") == "ok";
        snprintf(b, sizeof b, "Pt1000: %s, %.2f °C", g_dev.gstr("pt").c_str(), g_dev.gnum("t")); PassLine(pok, b);
        ImGui::SetNextItemWidth(70); ImGui::InputFloat("°C эталон##wpt", &w.pt_ref, 0, 0, "%.2f");
        ImGui::SameLine();
        ImGui::BeginDisabled(!conn || !pok);
        if (ImGui::SmallButton("Калибровать Pt1000 (по желанию)")) { char c[48]; snprintf(c, sizeof c, "-ptcal %.2f", w.pt_ref); g_dev.send_cmd(c); }
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::BeginDisabled(!conn || Busy());
        if (PrimaryButton("Самотест линейности по ЦАП (~1 мин)", ImVec2(-1, 0))) {
            w.lin_started = true;
            g_dev.lin_start({ 50, 100, 200, 400, 600, 800, 1000, 1200, 1350 }, 10000, 100);
        }
        ImGui::EndDisabled();
        if (w.lin_started) {
            const Device::LinRun& L = g_dev.lin;
            if (L.running) { snprintf(b, sizeof b, "ступенька %zu / %zu…", L.pts.size() + 1, L.codes.size()); ImGui::TextUnformatted(b); }
            else if (L.fitted) {
                snprintf(b, sizeof b, "канал = %.4f·код %+.2f; R² %.6f; отклонение %.3f %% (норма: R² > 0.9999, < 0.2 %%, в пике > 99 %%)", L.k, L.b, L.r2, L.dev);
                PassLine(WizLinOk(), b);
            } else if (!L.msg.empty()) PassLine(false, L.msg.c_str());
        }
        break;
    }
    case 3: {
        ImGui::TextWrapped("Ноль монитора ВВ — при выключенном ВВ, через ≥ 2 мин после выключения (у только что "
                           "включённого прибора — сразу). Коэффициент по умолчанию 1.633 мВ/В (прибор №1); точнее — "
                           "мультиметром с высоковольтным делителем на вкладке «Сервис → Калибровки».");
        snprintf(b, sizeof b, "сейчас: ноль %.1f мВ, k %.4f мВ/В, монитор %.1f мВ", g_dev.gnum("hv_zero"), g_dev.gnum("hv_k"), g_dev.gnum("mon_mv"));
        ImGui::TextUnformatted(b);
        ImGui::BeginDisabled(!conn || g_dev.hv_on());
        if (PrimaryButton("Записать ноль монитора (-hvzero)", ImVec2(-1, 0))) { w.hvzero_sent = true; g_dev.send_cmd("-hvzero"); }
        ImGui::EndDisabled();
        if (w.hvzero_sent) ImGui::TextDisabled("ответ: %s", g_dev.last_reply.c_str());
        break;
    }
    case 4: case 6: {
        bool second = (s == 6);
        Device::HvPeak& h = g_dev.hvp;
        if (!second) {
            ImGui::TextWrapped("Положи источник Cs-137 рядом с кристаллом. Мастер включит ВВ, найдёт фотопик 662 кэВ и "
                               "подстроит ВВ так, чтобы он встал на канал ~1180 (запас шкалы до 3.5 МэВ). Начинай с "
                               "заниженного ВВ: для R1306/R1307 с R417 2.7 кОм рабочая точка ~700–800 В.");
            ImGui::SetNextItemWidth(80); ImGui::InputFloat("стартовое ВВ, В", &w.start_v, 10, 50, "%.0f");
            if (w.start_v < 300) w.start_v = 300;
            if (w.start_v > 1100) w.start_v = 1100;
        } else {
            ImGui::TextWrapped("Новый эталон немного меняет амплитуду — пик подстраивается ещё раз от текущего ВВ.");
        }
        ImGui::SetNextItemWidth(60); float lo = (float)h.lo; if (ImGui::InputFloat("##wlo", &lo, 0, 0, "%.0f")) h.lo = lo;
        ImGui::SameLine(); ImGui::TextUnformatted("–"); ImGui::SameLine();
        ImGui::SetNextItemWidth(60); float hi = (float)h.hi; if (ImGui::InputFloat("окно пика##whi", &hi, 0, 0, "%.0f")) h.hi = hi;
        ImGui::BeginDisabled(!conn || Busy());
        if (PrimaryButton(second ? "Подстроить ВВ ещё раз" : "Включить ВВ и подстроить по пику", ImVec2(-1, 26))) {
            double v0 = second ? fabs(g_dev.gnum("hv_set")) : (double)w.start_v;
            if (second && v0 < 300) v0 = w.hv1_v > 0 ? w.hv1_v : w.start_v;
            if (second) w.hv2_started = true; else w.hv1_started = true;
            g_dev.hvp_start(v0);
        }
        ImGui::EndDisabled();
        if (h.running && ImGui::Button("Прервать", ImVec2(-1, 0))) g_dev.hvp_abort();
        if (!h.msg.empty()) ImGui::TextWrapped("%s", h.msg.c_str());
        for (auto& p : h.hist) ImGui::TextDisabled("%.0f В → пик %.1f", p.first, p.second);
        bool started = second ? w.hv2_started : w.hv1_started;
        if (started && h.done && !h.running) {
            if (second) { w.hv2_v = h.v; w.hv2_peak = h.peak; } else { w.hv1_v = h.v; w.hv1_peak = h.peak; }
        }
        break;
    }
    case 5: {
        ImGui::TextWrapped("Эталон формы импульса под этот детектор: сбор 2000 сырых порций (~11 мин, ВВ включено, "
                           "источник на месте), расчёт, загрузка в прибор.");
        Device::RawRun& R = g_dev.raw;
        ImGui::BeginDisabled(!conn || Busy() || w.tpl_phase == 2 || w.tpl_phase == 3);
        if (PrimaryButton("Обучить эталон", ImVec2(-1, 26))) { w.tpl_phase = 1; g_tpl_state = 0; g_dev.raw_start(2000); }
        ImGui::EndDisabled();
        if (w.tpl_phase == 1) {
            float fr = R.target ? (float)R.blocks.size() / R.target : 0.0f;
            snprintf(b, sizeof b, "сбор %zu / %d", R.blocks.size(), R.target);
            ImGui::ProgressBar(fr, ImVec2(-1, 0), b);
            if (!R.running) {
                if ((int)R.blocks.size() >= R.target) {
                    w.tpl_phase = 2; g_tpl_state = 1;
                    std::thread([blocks = R.blocks]() {
                        tpl::Singles s = tpl::find_singles(blocks);
                        g_tpl_res = tpl::build(s);
                        g_tpl_state = 2;
                    }).detach();
                } else w.tpl_phase = 5;
            }
        }
        if (w.tpl_phase == 2) {
            ImGui::TextUnformatted("расчёт…");
            if (g_tpl_state == 2) {
                if (g_tpl_res.ok) { w.tpl_err0 = g_dev.queue_errors; g_dev.queue(tpl::commands(g_tpl_res)); g_dev.queue({ "-tplok" }); w.tpl_phase = 3; }
                else w.tpl_phase = 5;
            }
        }
        if (w.tpl_phase == 3 && !g_dev.queue_busy()) w.tpl_phase = g_dev.queue_errors == w.tpl_err0 ? 4 : 5;
        if (w.tpl_phase >= 3 && g_tpl_res.ok) PassLine(w.tpl_phase != 5, g_tpl_res.msg.c_str());
        if (w.tpl_phase == 4) PassLine(g_tpl_res.resid_med < 1.0 && g_tpl_res.spread_after < 1.0, "загружен (норма: невязка < 1 %, размах по фазам < 1 %)");
        if (w.tpl_phase == 5) { ImGui::PushStyleColor(ImGuiCol_Text, c_danger); ImGui::TextWrapped("не получилось: %s %s", R.msg.c_str(), g_tpl_res.msg.c_str()); ImGui::PopStyleColor(); }
        break;
    }
    case 7: {
        ImGui::TextWrapped("Контрольный набор спектра Cs-137 и подгонка фотопика гауссом с линейным фоном.");
        ImGui::SetNextItemWidth(70); ImGui::InputFloat("с набора", &w.fw_secs, 0, 0, "%.0f");
        if (w.fw_secs < 30) w.fw_secs = 30;
        ImGui::BeginDisabled(!conn || Busy() || w.fw_phase == 1);
        if (PrimaryButton("Набрать и оценить", ImVec2(-1, 26))) { g_dev.stop(); g_dev.reset_spectrum(); g_dev.start(); w.fw_phase = 1; w.fw_t0 = ImGui::GetTime(); }
        ImGui::EndDisabled();
        if (w.fw_phase == 1) {
            double el = ImGui::GetTime() - w.fw_t0;
            ImGui::ProgressBar((float)(el / (w.fw_secs + 2)), ImVec2(-1, 0));
            if (el >= w.fw_secs + 2) {
                g_dev.stop();
                w.fwr = peakfit::fit(g_dev.spectrum, Device::CHANNELS, 300, 7000);
                w.fw_phase = 3;
            }
        }
        if (w.fw_phase == 3) {
            if (w.fwr.ok) {
                snprintf(b, sizeof b, "пик %.1f, FWHM %.2f ± %.2f %% (NaI 40×40…63×63: хорошо < 7.5 %%, норма < 8.5 %%)", w.fwr.peak, w.fwr.fwhm_pct, w.fwr.fwhm_err);
                PassLine(w.fwr.fwhm_pct < 8.5, b);
            } else PassLine(false, "фотопик не найден");
        }
        break;
    }
    case 8: {
        ImGui::TextWrapped("Базовая энергокалибровка по одному пику: E = k·канал, k = 661.66 кэВ / пик. Прибор сразу "
                           "показывает кэВ в BecqMoni; точнее — калибровка по нескольким линиям в BecqMoni "
                           "(«Записать в устройство»). Шаг можно пропустить, если калибровка уже есть.");
        double pk = w.fwr.ok ? w.fwr.peak : 0;
        if (pk > 0) {
            w.cal_k = 661.66 / pk;
            snprintf(b, sizeof b, "пик Cs-137 %.2f ⇒ k = %.6f кэВ/канал", pk, w.cal_k);
            ImGui::TextUnformatted(b);
        } else ImGui::TextDisabled("нужен результат шага «Разрешение»");
        ImGui::BeginDisabled(!conn || pk <= 0 || g_dev.queue_busy() || g_dev.acquiring);
        if (PrimaryButton("Записать калибровку в прибор", ImVec2(-1, 0))) {
            double c[5] = { 0, w.cal_k, 0, 0, 0 };
            char cmds[11][40]; peakfit::becq_cal_commands(c, cmds);
            std::vector<std::string> v; for (auto& x : cmds) v.push_back(x);
            w.cal_err0 = g_dev.queue_errors; g_dev.queue(v); w.cal_sent = true;
        }
        ImGui::EndDisabled();
        if (w.cal_sent) {
            if (g_dev.queue_busy()) ImGui::TextUnformatted("записываю…");
            else PassLine(g_dev.queue_errors == w.cal_err0, g_dev.queue_errors == w.cal_err0 ? "записана" : ("ошибка: " + g_dev.queue_last_err).c_str());
        }
        break;
    }
    case 9: {
        ImGui::TextWrapped("Сохранение всех настроек во флеш и отчёт о вводе в строй. С автозапуском прибор сам "
                           "включает рабочее ВВ через ~5 с после подачи питания (установление ~1.5 мин) — можно закрыть "
                           "пульт и работать в BecqMoni. Без автозапуска ВВ включается только командой из пульта.");
        for (int i = 1; i < 9; i++) PassLine(WizDone(i), WIZ_STEPS[i]);
        int hv_work = (int)((w.hv2_v > 0 ? w.hv2_v : g_dev.gnum("hv_set")) + 0.5);
        {
            char lb[96]; snprintf(lb, sizeof lb, "Автозапуск ВВ %d В при подаче питания (блок закрыт)", hv_work);
            ImGui::Checkbox(lb, &w.autohv);
        }
        ImGui::BeginDisabled(!conn || Busy() || g_dev.acquiring);
        if (PrimaryButton("Сохранить во флеш и записать отчёт", ImVec2(-1, 26))) {
            char c[32];
            if (w.autohv && hv_work > 0) snprintf(c, sizeof c, "-hvboot %d", hv_work); else snprintf(c, sizeof c, "-hvboot off");
            w.hvboot_v = (w.autohv && hv_work > 0) ? hv_work : 0;
            g_dev.queue({ c, "-save" }); WizWriteReport(); w.saved = true;
        }
        ImGui::EndDisabled();
        if (w.saved) PassLine(true, ("отчёт: " + w.report_path).c_str());
        ImGui::BeginDisabled(!conn || !g_dev.hv_on());
        if (DangerButton("Выключить ВВ", ImVec2(-1, 0))) g_dev.send_cmd("-hv off");
        ImGui::EndDisabled();
        break;
    }
    }
}

// каждый кадр, в любом режиме: после прошивки — снова на связь
static void WizTick()
{
    Wiz& w = g_wiz;
    if (w.fl_reconnect && w.fl_state != 1) {
        w.fl_reconnect = false;
        if (w.fl_state == 2) g_dev.connect(w.fl_port);
    }
}

// Мастер в области под верхней полосой (режим ds::Mode::Wizard). Шаг согласуется с g_ui.wiz_step:
// вход в мастер и ui_go(Wizard, шаг) задают шаг снаружи, выбор шага внутри пишется обратно.
// Прежний шаг 0 «Подготовка» в ds::Step не входит (по спеке убран; пока остаётся в списке).
static void DrawWizard(ImVec2 pos, ImVec2 size, bool entered)
{
    using namespace theme;
    Wiz& w = g_wiz;
    static int s_applied = -1;                           // шаг из g_ui.wiz_step, уже применённый к w.step
    int want = (int)g_ui.wiz_step + 1;
    if (entered || want != s_applied) { w.step = want; s_applied = want; }
    if (!BeginPanel("##wizard", pos, size)) { ImGui::End(); return; }
    ImGui::BeginChild("##wsteps", ImVec2(210, 0), true);
    for (int i = 0; i < WIZ_N; i++) {
        bool d = WizDone(i);
        ImGui::PushStyleColor(ImGuiCol_Text, d ? c_ok : (i == w.step ? c_accent : c_text_dim));
        char lb[96]; snprintf(lb, sizeof lb, "%s %d. %s", d ? "√" : " ", i, WIZ_STEPS[i]);
        if (ImGui::Selectable(lb, i == w.step)) w.step = i;
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##wbody", ImVec2(0, 0), false);
    ImGui::PushFont(font_small);
    ImGui::TextColored(c_accent, "Шаг %d из %d", w.step, WIZ_N - 1);
    ImGui::PopFont();
    ImGui::Text("%s", WIZ_STEPS[w.step]);
    ImGui::Separator();
    ImGui::BeginChild("##wcontent", ImVec2(0, -34), false);
    WizStepBody(w.step);
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::BeginDisabled(w.step == 0);
    if (ImGui::Button("← Назад", ImVec2(110, 0))) w.step--;
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool optional = (w.step == 8);
    if (optional && w.step < WIZ_N - 1) { if (ImGui::Button("Пропустить", ImVec2(110, 0))) w.step++; ImGui::SameLine(); }
    ImGui::BeginDisabled(w.step >= WIZ_N - 1 || !WizDone(w.step));
    if (PrimaryButton("Далее →", ImVec2(110, 0))) w.step++;
    ImGui::EndDisabled();
    if (g_dev.hv_on()) { ImGui::SameLine(); ImGui::TextColored(c_danger, "  ВВ %.0f В", fabs(g_dev.gnum("hv_out"))); }
    const char* ex = T("wiz.exit");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(ex).x - 2 * ImGui::GetStyle().FramePadding.x - 4);
    bool leave = ImGui::Button(ex);
    ImGui::EndChild();
    ImGui::End();
    if (w.step >= 1 && w.step != s_applied) {           // шаг выбран внутри мастера
        g_ui.wiz_step = (ds::Step)(w.step - 1);
        s_applied = w.step;
    }
    if (leave) ui_go(ds::Mode::Home);
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
    static ds::Mode s_prev = ds::Mode::Home;
    DrawShell();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    float topH = TopBarHeight(), statusH = StatusBarHeight();
    ImVec2 area(vp->Pos.x, vp->Pos.y + topH);
    ImVec2 asz(vp->Size.x, vp->Size.y - topH - statusH);

    ds::Mode m = g_ui.mode;                  // режим на этот кадр (ui_go внутри кадра сработает со следующего)
    if (m == ds::Mode::Wizard)    DrawWizard(area, asz, s_prev != ds::Mode::Wizard);
    else if (m == ds::Mode::Home) DrawHome(area, asz);
    else                          DrawPanels(area, asz);
    s_prev = m;

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
