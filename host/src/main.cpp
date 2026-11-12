// main.cpp — GUI-режим gammapult: сервисный пульт гамма-спектрометра (прошивка firmware/gamma_fw).
// Win32 + DirectX 11 + Dear ImGui + ImPlot. Единый .exe без зависимостей.
// Здесь панели и вкладки страниц расширенного режима (ui_panels.h), справка и цикл окна.
// Строки интерфейса берутся из таблицы i18n.cpp через T() (ключи adv.*, help.*).

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "implot.h"

#include <d3d11.h>
#include <tchar.h>
#include <cstdarg>
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
#include "ui_adv.h"
#include "ui_panels.h"
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

// строка по формату (формат обычно из T())
static std::string SFmt(const char* fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return b;
}

// подпись виджета на текущем языке с постоянным ID: «<T(key)>##<id>»
static std::string Lbl(const char* key, const char* id)
{
    return std::string(T(key)) + "##" + id;
}

// приглушённый текст с переносом (без разбора формата)
static void DimWrapped(const char* s)
{
    ImGui::PushStyleColor(ImGuiCol_Text, theme::c_text_dim);
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}

// ================= панели =================

void PanelConnection(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    BeginPanel("##conn", pos, size);
    PanelHeader(T("adv.ov.header"));

    bool busy = (g_dev.state != Device::State::Disconnected &&
                 g_dev.state != Device::State::Error);

    Caption(T("adv.ov.port"));
    if (g_port_sel >= (int)g_dev.ports.size()) g_port_sel = 0;
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(-1);
    const char* prev = g_dev.ports.empty() ? T("conn.no_ports") : g_dev.ports[g_port_sel].c_str();
    if (ImGui::BeginCombo("##port", prev)) {
        for (int i = 0; i < (int)g_dev.ports.size(); i++)
            if (ImGui::Selectable(g_dev.ports[i].c_str(), i == g_port_sel))
                g_port_sel = i;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();

    if (!busy) {
        if (PrimaryButton(Lbl("conn.connect", "ovconn").c_str(), ImVec2(-1, 26)) && !g_dev.ports.empty())
            g_dev.connect(g_dev.ports[g_port_sel]);
        if (ImGui::Button(Lbl("conn.refresh", "ovref").c_str(), ImVec2(-1, 0))) {
            g_dev.refresh_ports();
            if (!g_dev.ports.empty()) g_port_sel = (int)g_dev.ports.size() - 1;
        }
    } else {
        ImGui::BeginDisabled(Busy());
        if (!g_dev.acquiring) {
            if (PrimaryButton(Lbl("adv.ov.start", "ovsta").c_str(), ImVec2(-1, 26))) g_dev.start();
        } else {
            if (DangerButton(Lbl("adv.ov.stop", "ovsto").c_str(), ImVec2(-1, 26))) g_dev.stop();
        }
        if (ImGui::Button(Lbl("adv.ov.reset", "ovrst").c_str(), ImVec2(-1, 0))) g_dev.reset_spectrum();
        ImGui::EndDisabled();
        if (DangerButton(Lbl("top.disconnect", "ovdisc").c_str(), ImVec2(-1, 0))) g_dev.disconnect();
    }

    ImGui::Separator();
    StatusDot(StateColor());
    ImGui::TextWrapped("%s", g_dev.status_text.c_str());
    ImGui::Separator();

    KV(T("adv.ov.serial"), g_dev.serial.empty() ? "—" : g_dev.serial.c_str(), &c_accent);
    Caption(T("adv.ov.fw"));
    ImGui::PushFont(font_mono);
    ImGui::TextWrapped("%s", g_dev.fw_info.empty() ? "—" : g_dev.fw_info.c_str());
    ImGui::PopFont();
    ImGui::End();
}

void PanelTelemetry(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    BeginPanel("##tele", pos, size);
    PanelHeader(T("adv.tele.header"));

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
    ImGui::SameLine(); Caption(T("adv.tele.cps"));
    ImGui::Separator();

    if (g_dev.gst.empty()) {
        ImGui::TextDisabled("%s", g_dev.connected() ? T("adv.tele.wait") : T("status.no_link"));
        ImGui::End();
        return;
    }
    char b[128];
    bool hv = g_dev.hv_on();
    snprintf(b, sizeof b, T("adv.tele.hv_fmt"), hv ? T("adv.tele.on") : T("adv.tele.off"),
             fabs(g_dev.gnum("hv_out")), g_dev.gnum("hv_set"));
    KV(T("adv.tele.hv"), b, hv ? &c_danger : nullptr);
    std::string trip = g_dev.gstr("hv_trip", "-");
    if (trip != "-") KV(T("adv.tele.trip"), trip.c_str(), &c_danger);

    std::string pt = g_dev.gstr("pt", "-");
    if (pt == "ok") snprintf(b, sizeof b, "%.2f °C", g_dev.gnum("t"));
    else            snprintf(b, sizeof b, T("adv.tele.sensor"), pt.c_str());
    KV(T("adv.tele.temp"), b, pt == "ok" ? nullptr : &c_warn);

    snprintf(b, sizeof b, T("adv.tele.base_fmt"), g_dev.gnum("base"), g_dev.gnum("sigma"));
    KV(T("adv.tele.base"), b);
    snprintf(b, sizeof b, T("adv.tele.pulses_fmt"), g_dev.gnum("cps_in"), g_dev.gnum("cps_acc"), g_dev.gnum("rej"));
    KV(T("adv.tele.pulses"), b);
    snprintf(b, sizeof b, T("adv.tele.thr_fmt"), g_dev.gnum("thr"), g_dev.thr_codes == 0 ? T("adv.tele.thr_auto") : "");
    KV(T("adv.tele.thr"), b);
    snprintf(b, sizeof b, T("adv.tele.time_fmt"), g_dev.gnum("live_s"), g_dev.gnum("real_s"));
    KV(T("adv.tele.time"), b);
    snprintf(b, sizeof b, T("adv.tele.cpu_fmt"), g_dev.gnum("cpu"), g_dev.gnum("lost"), g_dev.gnum("overrun"));
    KV(T("adv.tele.cpu"), b);
    bool vok = g_dev.gnum("vref_ok", 0) > 0.5;
    snprintf(b, sizeof b, T("adv.tele.vref_fmt"), g_dev.gnum("vref"), vok ? T("home.val.ok") : T("adv.tele.vref_bad"));
    KV(T("adv.tele.vref"), b, vok ? nullptr : &c_danger);
    std::string fl = g_dev.gstr("flash");
    KV(T("adv.tele.flash"), fl.c_str(), fl == "ok" ? nullptr : &c_warn);
    if (g_dev.gst_age() > 3) { ImGui::PushStyleColor(ImGuiCol_Text, c_warn); ImGui::TextUnformatted(T("adv.tele.stale")); ImGui::PopStyleColor(); }
    ImGui::End();
}

void PanelHV(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    BeginPanel("##hv", pos, size);
    PanelHeader(T("adv.hv.header"));
    bool conn = g_dev.connected();
    bool hv = g_dev.hv_on();

    if (hv) {
        StatusDot(c_danger);
        ImGui::PushStyleColor(ImGuiCol_Text, c_danger);
        ImGui::Text(T("adv.hv.is_on"), fabs(g_dev.gnum("hv_out")));
        ImGui::PopStyleColor();
    } else {
        StatusDot(c_ok); ImGui::TextUnformatted(T("top.hv_off"));
    }
    ImGui::BeginDisabled(!conn);
    if (hv) {
        if (DangerButton(Lbl("adv.hv.kill", "hvkill").c_str(), ImVec2(-1, 28))) { g_dev.hvp_abort(); g_dev.send_cmd("-hv off"); }
    } else {
        ImGui::BeginDisabled(true);
        ImGui::Button(Lbl("adv.hv.kill", "hvkill").c_str(), ImVec2(-1, 28));
        ImGui::EndDisabled();
    }
    // поле уставки — из прибора, один раз на подключение (иначе «Установить» при включённом ВВ
    // послало бы прежнее значение поля, например -hv 750 при рабочих 565 В)
    {
        static bool synced = false;
        if (!g_dev.connected()) synced = false;
        else if (!synced && g_dev.gst_age() < 5.0) {
            double v = std::fabs(g_dev.gnum("hv_set"));
            if (v <= 0) v = g_dev.gnum("hv_boot");
            if (v > 0) g_hv_set = (int)(v + 0.5);
            synced = true;
        }
    }
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt(Lbl("adv.hv.unit", "hvset").c_str(), &g_hv_set, 10, 50);
    if (g_hv_set < 0) g_hv_set = 0;
    if (g_hv_set > 1200) g_hv_set = 1200;
    ImGui::SameLine();
    ImGui::BeginDisabled(Busy());
    if (PrimaryButton(hv ? Lbl("adv.hv.set", "hvapply").c_str() : Lbl("adv.hv.on_btn", "hvapply").c_str())) {
        if (hv) { char c[32]; snprintf(c, sizeof c, "-hv %d", g_hv_set); g_dev.send_cmd(c); }
        else g_hv_confirm = true;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (g_hv_confirm) { ImGui::OpenPopup("###hvconfirm"); g_hv_confirm = false; }
    if (ImGui::BeginPopupModal((std::string(T("adv.hv.confirm_title")) + "###hvconfirm").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(T("adv.hv.confirm_q"), g_hv_set);
        ImGui::TextDisabled("%s", T("adv.hv.confirm_note"));
        if (PrimaryButton(Lbl("adv.hv.on", "hvon").c_str(), ImVec2(120, 0))) {
            char c[32]; snprintf(c, sizeof c, "-hv %d", g_hv_set); g_dev.send_cmd(c);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Lbl("adv.cancel", "hvcancel").c_str(), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::Separator();
    Caption(T("adv.hv.tune_caption"));
    Device::HvPeak& h = g_dev.hvp;
    float f;
    ImGui::BeginDisabled(h.running);
    ImGui::SetNextItemWidth(80); f = (float)h.target; if (ImGui::InputFloat(Lbl("adv.hv.target", "hvtg").c_str(), &f, 0, 0, "%.0f")) h.target = f;
    ImGui::SetNextItemWidth(60); f = (float)h.lo; if (ImGui::InputFloat("##lo", &f, 0, 0, "%.0f")) h.lo = f;
    ImGui::SameLine(); ImGui::TextUnformatted("–"); ImGui::SameLine();
    ImGui::SetNextItemWidth(60); f = (float)h.hi; if (ImGui::InputFloat(Lbl("adv.peak_win", "hvhi").c_str(), &f, 0, 0, "%.0f")) h.hi = f;
    ImGui::SetNextItemWidth(60); f = (float)h.count; if (ImGui::InputFloat(Lbl("adv.hv.count", "hvcnt").c_str(), &f, 0, 0, "%.0f")) h.count = f < 100 ? 100 : f;
    ImGui::SetNextItemWidth(60); f = (float)h.timeout; if (ImGui::InputFloat(Lbl("adv.hv.timeout", "hvtmo").c_str(), &f, 0, 0, "%.0f")) h.timeout = f < 10 ? 10 : f;
    ImGui::SetNextItemWidth(60); f = (float)h.gain_exp; if (ImGui::InputFloat(Lbl("adv.hv.gain_exp", "hvexp").c_str(), &f, 0, 0, "%.1f")) h.gain_exp = f < 2 ? 2 : f;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!conn || g_dev.lin.running);
    if (!h.running) {
        if (PrimaryButton(Lbl("adv.hv.tune_btn", "hvtune").c_str(), ImVec2(-1, 0))) {
            double v0 = hv ? fabs(g_dev.gnum("hv_set")) : g_hv_set;
            g_dev.hvp_start(v0);
        }
    } else if (ImGui::Button(Lbl("adv.hv.tune_abort", "hvtune").c_str(), ImVec2(-1, 0))) g_dev.hvp_abort();
    ImGui::EndDisabled();
    if (h.running && h.phase == 2) {          // проход: импульсов в фотопике из N
        char ov[96];
        snprintf(ov, sizeof ov, T("wiz.in_peak"), (unsigned long long)h.n_in, h.count);
        ImGui::ProgressBar(h.count > 0 ? (float)(h.n_in / h.count) : 0.0f, ImVec2(-1, 0), ov);
    }
    if (!h.msg.empty()) ImGui::TextWrapped("%s", h.msg.c_str());
    for (auto& p : h.hist) ImGui::TextDisabled(T("wiz.hv.history"), p.first, p.second);
    ImGui::End();
}

void PanelSpectrum(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    BeginPanel("##spec", pos, size);

    ImGui::PushFont(font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
    ImGui::TextUnformatted(T("adv.spec.header"));
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
    if (seg(Lbl("wiz.plot.lin", "speclin").c_str(), !g_log_scale)) g_log_scale = false;
    ImGui::SameLine(0, 2);
    if (seg(Lbl("wiz.plot.log", "speclog").c_str(), g_log_scale)) g_log_scale = true;
    ImGui::Separator();

    char st[160];
    double m = 0, sd = 0; uint64_t n = 0;
    int pk = g_dev.peak_channel(30);
    if (g_dev.peak_stats(g_dev.spectrum, 30, Device::CHANNELS, m, sd, n) && m > 0)
        snprintf(st, sizeof(st), T("adv.spec.stats_fit"),
                 (unsigned long long)g_dev.total(), m, 235.48 * sd / m);
    else
        snprintf(st, sizeof(st), T("adv.spec.stats"), (unsigned long long)g_dev.total(), pk);
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
        ImPlot::SetupAxis(ImAxis_X1, T("wiz.plot.channel"), shift ? ImPlotAxisFlags_Lock : ImPlotAxisFlags_None);
        ImPlot::SetupAxis(ImAxis_Y1, T("wiz.plot.counts"),  shift ? ImPlotAxisFlags_None : ImPlotAxisFlags_AutoFit);
        if (g_log_scale) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, (double)Device::CHANNELS);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, g_log_scale ? 0.1 : 0.0, 1.0e12);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, Device::CHANNELS, ImPlotCond_Once);
        ImPlot::PushStyleColor(ImPlotCol_Line, c_trace);
        ImPlot::PlotLine("##spectrum", g_spec_x, g_spec_y, Device::CHANNELS);
        ImPlot::PopStyleColor();
        const Device::HvPeak& h = g_dev.hvp;
        if (h.running || h.done) {           // окно и цель подстройки ВВ
            double xs[2] = { h.lo, h.hi };
            ImPlot::PushStyleColor(ImPlotCol_Line, c_accent_line);
            ImPlot::PlotInfLines("##window", xs, 2);
            ImPlot::PopStyleColor();
            double tg[1] = { h.target };
            ImPlot::PushStyleColor(ImPlotCol_Line, c_cursor);
            ImPlot::PlotInfLines("##target", tg, 1);
            ImPlot::PopStyleColor();
        }
        if (h.running && h.phase == 2 && h.win_ok) {   // окно фотопика ±1.5·FWHM, по которому считается проход
            double pw[2] = { (double)h.win_lo, (double)h.win_hi };
            ImPlot::PushStyleColor(ImPlotCol_Line, c_ok);
            ImPlot::PlotInfLines("##pkwin", pw, 2);
            ImPlot::PopStyleColor();
        }
        ImPlot::EndPlot();
    }
    ImGui::End();
}

void PanelConsole(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    BeginPanel("##con", pos, size);
    PanelHeader(T("adv.con.header"));

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
    if ((PrimaryButton(Lbl("adv.con.send", "consend").c_str()) || enter) && g_cmd_buf[0]) {
        g_dev.send_cmd(g_cmd_buf);
        g_cmd_buf[0] = 0;
    }
    ImGui::End();
}

void TabSelftest()
{
    using namespace theme;
    Device::LinRun& L = g_dev.lin;
    bool conn = g_dev.connected();
    Caption(T("adv.lin.codes"));
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled(L.running);
    ImGui::InputText("##codes", g_lin_codes, sizeof g_lin_codes);
    ImGui::SetNextItemWidth(80); ImGui::InputInt(Lbl("adv.lin.n", "linn").c_str(), &g_lin_n, 0, 0);
    ImGui::SetNextItemWidth(80); ImGui::InputInt(Lbl("adv.lin.period", "linper").c_str(), &g_lin_period, 0, 0);
    ImGui::EndDisabled();
    if (g_lin_n < 100) g_lin_n = 100;
    if (g_lin_period < 45) g_lin_period = 45;
    ImGui::BeginDisabled(!conn || g_dev.hvp.running || g_dev.hv_on());   // самотест ЦАП — только без ВВ
    if (!L.running) {
        if (PrimaryButton(Lbl("adv.lin.run", "linrun").c_str(), ImVec2(-1, 26))) {
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
    } else if (DangerButton(Lbl("adv.abort", "linrun").c_str(), ImVec2(-1, 26))) g_dev.lin_abort();
    ImGui::EndDisabled();
    DimWrapped(T("adv.lin.note"));
    if (!L.msg.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, L.fitted ? c_ok : c_text);
        ImGui::TextWrapped("%s", L.msg.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::BeginTable("lin", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::PushFont(font_small);
        ImGui::TableSetupColumn(T("adv.lin.col_code")); ImGui::TableSetupColumn(T("wiz.plot.channel"));
        ImGui::TableSetupColumn("σ");   ImGui::TableSetupColumn(T("adv.lin.col_inpeak"));
        ImGui::TableHeadersRow();
        ImGui::PopFont();
        ImGui::PushFont(font_mono);
        for (auto& p : L.pts) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", p.code);
            ImGui::TableSetColumnIndex(1); if (p.ok) ImGui::Text("%.2f", p.ch); else ImGui::TextDisabled("%s", T("adv.lin.nopeak"));
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
        Caption(T("adv.lin.dev_caption"));
        if (ImPlot::BeginPlot("##linres", ImVec2(-1, 140), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
            ImPlot::SetupAxes(T("adv.lin.col_code"), "%", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.5f, c_trace2, 1.0f, c_trace2);
            ImPlot::PlotLine("##dev", ax.data(), ar.data(), (int)ax.size());
            ImPlot::EndPlot();
        }
    }
}

void TabCalib()
{
    using namespace theme;
    bool conn = g_dev.connected();
    bool hv = g_dev.hv_on();
    char b[160];

    Caption(T("adv.sens.mon"));
    snprintf(b, sizeof b, T("adv.sens.mon_fmt"), g_dev.gnum("hv_zero"), g_dev.gnum("hv_k"));
    ImGui::PushFont(font_mono); ImGui::TextUnformatted(b); ImGui::PopFont();
    DimWrapped(T("adv.sens.zero_note"));
    ImGui::BeginDisabled(!conn || hv);
    if (ImGui::Button(Lbl("adv.sens.zero_btn", "senszero").c_str(), ImVec2(-1, 0))) g_dev.send_cmd("-hvzero");
    ImGui::EndDisabled();
    DimWrapped(T("adv.sens.k_note"));
    ImGui::SetNextItemWidth(90); ImGui::InputFloat(Lbl("adv.sens.meas", "sensmeas").c_str(), &g_mon_meas_v, 0, 0, "%.1f");
    ImGui::BeginDisabled(!conn || !hv || g_mon_meas_v < 100);
    if (ImGui::Button(Lbl("adv.sens.remember", "sensrem").c_str(), ImVec2(-1, 0))) {
        g_mon_rec_mv = (float)g_dev.gnum("mon_mv");
        g_mon_rec_set = (float)g_dev.gnum("hv_set");
        double zero = g_dev.gnum("hv_zero");
        g_mon_k_new = (float)((zero - g_mon_rec_mv) / g_mon_meas_v);
    }
    ImGui::EndDisabled();
    if (g_mon_k_new > 0) {
        snprintf(b, sizeof b, T("adv.sens.k_calc"), g_mon_rec_mv, g_mon_meas_v, g_mon_k_new);
        ImGui::TextUnformatted(b);
        ImGui::BeginDisabled(!conn || hv || g_mon_k_new < 0.5f || g_mon_k_new > 5.0f);
        snprintf(b, sizeof b, T("adv.sens.k_btn"), g_mon_k_new);
        if (PrimaryButton((std::string(b) + "##sensk").c_str(), ImVec2(-1, 0))) {
            char c[48]; snprintf(c, sizeof c, "-hvk %.4f", g_mon_k_new); g_dev.send_cmd(c);
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing(); ImGui::Separator();
    Caption("Pt1000");
    snprintf(b, sizeof b, T("adv.sens.pt_fmt"), g_dev.gstr("pt").c_str(), g_dev.gnum("pt_rpu"), g_dev.gnum("pt_raw"));
    ImGui::PushFont(font_mono); ImGui::TextUnformatted(b); ImGui::PopFont();
    ImGui::SetNextItemWidth(80); ImGui::InputFloat(Lbl("wiz.check.ptref", "sensptref").c_str(), &g_pt_ref, 0, 0, "%.2f");
    ImGui::BeginDisabled(!conn || g_pt_ref < 0 || g_pt_ref > 60);
    if (ImGui::Button(Lbl("adv.sens.pt_btn", "sensptcal").c_str(), ImVec2(-1, 0))) {
        char c[48]; snprintf(c, sizeof c, "-ptcal %.2f", g_pt_ref); g_dev.send_cmd(c);
    }
    ImGui::EndDisabled();
    DimWrapped(T("adv.sens.pt_note"));
    DimWrapped(T("adv.sens.save_note"));
}

// «Порог и детектор»: порог, PGA, сохранить, умолчания (часть прежней вкладки «Настройки»)
void TabDetector()
{
    using namespace theme;
    bool conn = g_dev.connected();
    Caption(T("adv.det.thr"));
    ImGui::SetNextItemWidth(80); ImGui::InputInt("##thr", &g_thr, 0, 0);
    if (g_thr < 0) g_thr = 0;
    if (g_thr > 1000) g_thr = 1000;
    ImGui::SameLine();
    ImGui::BeginDisabled(!conn);
    if (ImGui::Button(Lbl("adv.apply", "thr").c_str())) { char c[24]; snprintf(c, sizeof c, "-thr %d", g_thr); g_dev.send_cmd(c); g_dev.send_cmd("-thr"); }
    ImGui::EndDisabled();
    {
        std::string now = g_dev.thr_codes < 0 ? "—" : g_dev.thr_codes == 0 ? T("adv.det.auto") : std::to_string(g_dev.thr_codes);
        ImGui::TextDisabled(T("adv.det.now"), now.c_str());
    }

    ImGui::Spacing();
    Caption(T("adv.det.pga"));
    const char* pg[] = { "×2", T("adv.det.pga4"), "×8", "×16" };
    const int pv[] = { 2, 4, 8, 16 };
    int sel = g_pga == 2 ? 0 : g_pga == 8 ? 2 : g_pga == 16 ? 3 : 1;
    ImGui::SetNextItemWidth(140);
    if (ImGui::Combo("##pga", &sel, pg, 4)) g_pga = pv[sel];
    ImGui::SameLine();
    ImGui::BeginDisabled(!conn || g_dev.acquiring);
    if (ImGui::Button(Lbl("adv.apply", "pga").c_str())) { char c[24]; snprintf(c, sizeof c, "-opa pga %d", g_pga); g_dev.send_cmd(c); }
    ImGui::EndDisabled();
    DimWrapped(T("adv.det.pga_note"));

    ImGui::Spacing(); ImGui::Separator();
    ImGui::BeginDisabled(!conn);
    if (PrimaryButton(Lbl("adv.det.save", "detsave").c_str(), ImVec2(-1, 26))) g_dev.send_cmd("-save");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!conn || g_dev.hv_on());
    if (DangerButton(Lbl("adv.det.defaults", "detdef").c_str(), ImVec2(-1, 0))) g_dev.send_cmd("-defaults");
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("adv.last_reply"));
    ImGui::PushFont(font_mono); ImGui::TextWrapped("%s", g_dev.last_reply.c_str()); ImGui::PopFont();
}

// «Высокое напряжение»: автозапуск ВВ при подаче питания (часть прежней вкладки «Настройки»)
void TabHvBoot()
{
    bool conn = g_dev.connected();
    Caption(T("adv.hvboot.caption"));
    double hb = g_dev.gnum("hv_boot");
    char b[160];
    if (hb > 0) snprintf(b, sizeof b, T("adv.hvboot.on"), hb);
    else snprintf(b, sizeof b, "%s", T("adv.hvboot.off"));
    ImGui::TextUnformatted(b);
    static int s_hvboot = 0;
    if (s_hvboot == 0 && g_dev.gnum("hv_set") > 0) s_hvboot = (int)(g_dev.gnum("hv_set") + 0.5);
    ImGui::BeginDisabled(!conn);
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt(Lbl("adv.hv.unit", "hvboot").c_str(), &s_hvboot, 0, 0);
    if (s_hvboot < 0) s_hvboot = 0;
    if (s_hvboot > 1200) s_hvboot = 1200;
    ImGui::SameLine();
    if (ImGui::Button(Lbl("adv.autostart.on", "hvbooton").c_str()) && s_hvboot > 0) {
        char c[32]; snprintf(c, sizeof c, "-hvboot %d", s_hvboot); g_dev.queue({ c, "-save" });
    }
    ImGui::SameLine();
    if (ImGui::Button(Lbl("adv.autostart.off", "hvbootoff").c_str())) g_dev.queue({ "-hvboot off", "-save" });
    ImGui::EndDisabled();
    DimWrapped(T("adv.hvboot.note"));
}

// «Самотест»: проверка тракта и шум базы (часть прежней вкладки «Настройки»)
void TabDiag()
{
    using namespace theme;
    bool conn = g_dev.connected();
    ImGui::BeginDisabled(!conn);
    if (ImGui::Button(Lbl("adv.diag.diag", "diag").c_str(), ImVec2(-1, 0))) g_dev.send_cmd("-diag");
    if (ImGui::Button(Lbl("adv.diag.noise", "noise").c_str(), ImVec2(-1, 0))) g_dev.send_cmd("-noise");
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", T("adv.last_reply"));
    ImGui::PushFont(font_mono); ImGui::TextWrapped("%s", g_dev.last_reply.c_str()); ImGui::PopFont();
}

void TabTemplate()
{
    using namespace theme;
    bool conn = g_dev.connected();
    Device::RawRun& R = g_dev.raw;
    ImGui::TextWrapped("%s", T("adv.tpl.intro"));
    ImGui::SetNextItemWidth(90);
    ImGui::BeginDisabled(R.running);
    ImGui::InputInt(Lbl("adv.tpl.blocks", "tplblk").c_str(), &g_tpl_blocks, 100, 500);
    ImGui::EndDisabled();
    if (g_tpl_blocks < 200) g_tpl_blocks = 200;
    if (g_tpl_blocks > 6000) g_tpl_blocks = 6000;
    ImGui::BeginDisabled(!conn || g_dev.lin.running || g_dev.hvp.running || g_dev.tc.running || g_tpl_state == 1);
    if (!R.running) {
        if (PrimaryButton(Lbl("adv.tpl.collect", "tplcol").c_str(), ImVec2(-1, 26))) { g_tpl_state = 0; g_tpl_loaded = false; g_dev.raw_start(g_tpl_blocks); }
    } else if (DangerButton(Lbl("adv.tpl.collect_abort", "tplcol").c_str(), ImVec2(-1, 26))) g_dev.raw_abort();
    ImGui::EndDisabled();
    if (R.running || !R.blocks.empty()) {
        float fr = R.target ? (float)R.blocks.size() / R.target : 0.0f;
        char b[64]; snprintf(b, sizeof b, T("adv.tpl.progress"), R.blocks.size(), R.target, R.bad);
        ImGui::ProgressBar(fr, ImVec2(-1, 0), b);
    }
    if (!R.msg.empty()) ImGui::TextDisabled("%s", R.msg.c_str());

    ImGui::BeginDisabled(R.running || R.blocks.empty() || g_tpl_state == 1);
    if (PrimaryButton(Lbl("adv.tpl.calc", "tplcalc").c_str(), ImVec2(-1, 0))) {
        g_tpl_state = 1;
        const char* info_fmt = T("adv.tpl.info");        // T() — в потоке UI, формат на текущем языке
        std::thread([blocks = R.blocks, info_fmt]() {
            tpl::Singles s = tpl::find_singles(blocks);
            tpl::Result r = tpl::build(s);
            char b[200];
            snprintf(b, sizeof b, info_fmt, s.blocks, s.sigma, s.single, s.amp);
            g_tpl_info = b;
            g_tpl_res = r;
            g_tpl_state = 2;
        }).detach();
    }
    ImGui::EndDisabled();
    if (g_tpl_state == 1) ImGui::TextUnformatted(T("wiz.tpl.calc"));
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
                ImPlot::SetupAxes(T("adv.tpl.axis"), "", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 1.5f, c_trace, 1.0f, c_trace);
                ImPlot::PlotScatter("##shape", xs, ys, k);
                ImPlot::EndPlot();
            }
            ImGui::TextDisabled("%s", T("adv.tpl.norm"));
            ImGui::BeginDisabled(!conn || g_dev.queue_busy() || g_dev.acquiring);
            if (PrimaryButton(Lbl("adv.tpl.load", "tplload").c_str(), ImVec2(-1, 0))) {
                g_tpl_load_err0 = g_dev.queue_errors;
                g_dev.queue(tpl::commands(g_tpl_res));
                g_dev.queue({ "-tplok" });
                g_tpl_loaded = true;
            }
            if (ImGui::Button(Lbl("adv.tpl.save", "tplsave").c_str(), ImVec2(-1, 0))) g_dev.send_cmd("-save");
            ImGui::EndDisabled();
            if (g_tpl_loaded) {
                if (g_dev.queue_busy()) ImGui::TextUnformatted(T("adv.tpl.loading"));
                else if (g_dev.queue_errors > g_tpl_load_err0) { ImGui::PushStyleColor(ImGuiCol_Text, c_danger); ImGui::TextWrapped(T("adv.tpl.load_err"), g_dev.queue_last_err.c_str()); ImGui::PopStyleColor(); }
                else { ImGui::PushStyleColor(ImGuiCol_Text, c_ok); ImGui::TextUnformatted(T("adv.tpl.loaded")); ImGui::PopStyleColor(); }
            }
        }
    }
    ImGui::Spacing(); ImGui::Separator();
    ImGui::BeginDisabled(!conn);
    if (ImGui::Button(Lbl("adv.tpl.clear", "tplclr").c_str(), ImVec2(-1, 0))) g_dev.send_cmd("-tplclr");
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

void TabLogger()
{
    using namespace theme;
    bool conn = g_dev.connected();
    Device::LogDump& D = g_dev.logd;
    ImGui::TextWrapped("%s", T("adv.lg.intro"));
    // состояние — раз в 3 с, пока вкладка открыта
    double now = ImGui::GetTime();
    if (conn && !D.running && now - g_lg_poll_t > 3.0) { g_dev.send_cmd("-log", true); g_lg_poll_t = now; }
    ImGui::PushFont(font_mono);
    ImGui::TextWrapped("%s", g_dev.log_status.empty() ? "—" : g_dev.log_status.c_str());
    ImGui::PopFont();

    Caption(T("adv.lg.auto"));
    ImGui::SetNextItemWidth(90); ImGui::InputInt(Lbl("adv.lg.period", "lgper").c_str(), &g_lg_period, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90); ImGui::InputInt(Lbl("adv.lg.delay", "lgdel").c_str(), &g_lg_delay, 0, 0);
    if (g_lg_period < 60) g_lg_period = 60;
    if (g_lg_period > 86400) g_lg_period = 86400;
    if (g_lg_delay < 0) g_lg_delay = 0;
    if (g_lg_delay > 7200) g_lg_delay = 7200;
    ImGui::BeginDisabled(!conn || D.running);
    if (ImGui::Button(Lbl("adv.autostart.on", "lgon").c_str())) { char c[48]; snprintf(c, sizeof c, "-log on %d %d", g_lg_period, g_lg_delay); g_dev.send_cmd(c); }
    ImGui::SameLine();
    if (ImGui::Button(Lbl("adv.autostart.off", "lgoff").c_str())) g_dev.send_cmd("-log off");
    ImGui::SameLine();
    if (ImGui::Button(Lbl("adv.lg.sync", "lgsync").c_str())) { char c[48]; snprintf(c, sizeof c, "-log time %lld", (long long)time(nullptr)); g_dev.send_cmd(c); }
    if (ImGui::Button(Lbl("adv.lg.start", "lgstart").c_str())) g_dev.send_cmd("-log start");
    ImGui::SameLine();
    if (ImGui::Button(Lbl("adv.lg.stop", "lgstop").c_str())) g_dev.send_cmd("-log stop");
    ImGui::EndDisabled();
    DimWrapped(T("adv.lg.delay_note"));

    Caption(T("adv.lg.dump"));
    if (!g_lg_dir[0]) { std::string d = LogDefaultDir(); strncpy(g_lg_dir, d.c_str(), sizeof g_lg_dir - 1); }
    ImGui::SetNextItemWidth(-1); ImGui::InputText("##lgdir", g_lg_dir, sizeof g_lg_dir);
    ImGui::BeginDisabled(!conn || D.running);
    if (PrimaryButton(Lbl("adv.lg.dump_btn", "lgdump").c_str(), ImVec2(-1, 26))) { g_lg_msg.clear(); g_dev.logd_start(0); }
    ImGui::EndDisabled();
    if (D.running) {
        float fr = D.items.empty() ? 0.0f : (float)D.idx / (float)D.items.size();
        ImGui::ProgressBar(fr, ImVec2(-1, 0), D.msg.c_str());
        if (ImGui::Button(Lbl("adv.abort", "lgabort").c_str())) g_dev.logd_abort();
    } else if (!D.msg.empty()) ImGui::TextDisabled("%s", D.msg.c_str());
    static bool written = false;
    if (D.running) written = false;
    if (D.done && !written) {
        written = true;
        std::string err;
        if (logdump::write_dir(g_lg_dir, D.recs, (int)g_lg_lo, (int)g_lg_hi, err))
            g_lg_msg = err.empty() ? SFmt(T("adv.lg.saved"), g_lg_dir) : SFmt(T("adv.lg.saved_err"), g_lg_dir, err.c_str());
        else g_lg_msg = SFmt(T("adv.error"), err.c_str());
        g_lg_sess_idx = 0;
    }
    if (!g_lg_msg.empty()) ImGui::TextWrapped("%s", g_lg_msg.c_str());

    // разбор выгруженного
    auto sess = logdump::sessions(D.recs);
    if (!sess.empty()) {
        Caption(T("adv.lg.session"));
        if (g_lg_sess_idx >= (int)sess.size()) g_lg_sess_idx = (int)sess.size() - 1;
        std::string lbl = std::to_string(sess[g_lg_sess_idx]);
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("##lgsess", lbl.c_str())) {
            for (int i = 0; i < (int)sess.size(); i++) {
                auto ss = logdump::session(D.recs, sess[i]);
                char b[96];
                snprintf(b, sizeof b, T("adv.lg.sess_item"), sess[i], ss.size(), ss.empty() ? 0.0 : ss.back()->h.t_end_ms / 3.6e6);
                if (ImGui::Selectable(b, i == g_lg_sess_idx)) g_lg_sess_idx = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60); ImGui::InputFloat("##lglo", &g_lg_lo, 0, 0, "%.0f");
        ImGui::SameLine(); ImGui::TextUnformatted("–"); ImGui::SameLine();
        ImGui::SetNextItemWidth(60); ImGui::InputFloat(Lbl("adv.peak_win", "lghi").c_str(), &g_lg_hi, 0, 0, "%.0f");
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
            ImPlot::SetupAxes(T("adv.lg.ax_h"), T("adv.lg.ax_peak"), ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxis(ImAxis_Y2, "°C", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2.5f, c_trace, 1.0f, c_trace);
            ImPlot::PlotLine(Lbl("adv.peak", "lgpk").c_str(), tx.data(), py.data(), (int)tx.size());
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            ImPlot::SetNextLineStyle(c_warn);
            ImPlot::PlotLine("T", tx.data(), ty.data(), (int)tx.size());
            ImPlot::EndPlot();
        }
        ImGui::BeginDisabled(pk.empty());
        if (ImGui::Button(Lbl("adv.lg.to_tc", "lgtotc").c_str())) {
            g_dev.tc.pts.clear();
            int skip = 0;
            for (auto& p : pk) {
                bool bad = !p.ok || !std::isfinite(p.temp) || (p.flags & (LOGF_HOST_RST | LOGF_HOST_STO | LOGF_HV_TRIP | LOGF_TC_ON))
                         || (std::isfinite(p.temp_span) && p.temp_span > 1.0);
                if (bad) { skip++; continue; }
                g_dev.tc.pts.push_back({ p.temp, p.peak, p.sigma, p.n });
            }
            g_lg_msg = SFmt(T("adv.lg.to_tc_msg"), (int)g_dev.tc.pts.size(), skip);
        }
        ImGui::SameLine();
        if (ImGui::Button(Lbl("adv.lg.sum", "lgsum").c_str())) {
            std::vector<double> sum; std::string msg;
            if (logdump::aligned_sum(s, (int)g_lg_lo, (int)g_lg_hi, sum, msg)) {
                std::filesystem::path pth = std::filesystem::u8path(g_lg_dir);
                std::error_code ec; std::filesystem::create_directories(pth, ec);
                char n[48]; snprintf(n, sizeof n, "sum_aligned_s%u.csv", sess[g_lg_sess_idx]);
                FILE* f = _wfopen((pth / n).wstring().c_str(), L"w");
                if (f) { fprintf(f, "channel,count\n"); for (size_t i = 0; i < sum.size(); i++) fprintf(f, "%zu,%.0f\n", i, sum[i]); fclose(f); g_lg_msg = msg + " → " + n; }
                else g_lg_msg = T("adv.lg.file_err");
            } else g_lg_msg = msg;
        }
        ImGui::EndDisabled();
    }

    Caption(T("adv.lg.erase"));
    ImGui::Checkbox(Lbl("adv.lg.erase_ok", "lgeraseok").c_str(), &g_lg_erase_ok);
    ImGui::SameLine();
    ImGui::BeginDisabled(!conn || D.running || !g_lg_erase_ok);
    if (DangerButton(Lbl("adv.lg.erase_btn", "lgerase").c_str())) { g_dev.send_cmd("-log erase"); g_lg_erase_ok = false; }
    ImGui::EndDisabled();
}

void TabThermo()
{
    using namespace theme;
    bool conn = g_dev.connected();
    Device::TcRun& TR = g_dev.tc;
    ImGui::TextWrapped("%s", T("adv.tc.intro"));
    ImGui::TextDisabled(T("adv.tc.now"), g_dev.tc_reply.empty() ? T("adv.tc.unknown") : g_dev.tc_reply.c_str() + 7);
    ImGui::BeginDisabled(!conn || g_dev.queue_busy());
    if (ImGui::SmallButton(Lbl("adv.tc.query", "tcq").c_str())) g_dev.queue({ "-tc" });
    ImGui::EndDisabled();
    float f;
    ImGui::BeginDisabled(TR.running);
    ImGui::SetNextItemWidth(70); f = (float)TR.secs; if (ImGui::InputFloat(Lbl("adv.tc.secs", "tcsecs").c_str(), &f, 0, 0, "%.0f")) TR.secs = f < 20 ? 20 : f;
    ImGui::SetNextItemWidth(60); f = (float)TR.lo; if (ImGui::InputFloat("##tlo", &f, 0, 0, "%.0f")) TR.lo = f;
    ImGui::SameLine(); ImGui::TextUnformatted("–"); ImGui::SameLine();
    ImGui::SetNextItemWidth(60); f = (float)TR.hi; if (ImGui::InputFloat(Lbl("adv.peak_win", "tchi").c_str(), &f, 0, 0, "%.0f")) TR.hi = f;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!conn || g_dev.lin.running || g_dev.hvp.running || g_dev.raw.running);
    if (!TR.running) {
        if (PrimaryButton(Lbl("adv.tc.start", "tcrun").c_str(), ImVec2(-1, 26))) g_dev.tc_start();
    } else {
        if (ImGui::Button(Lbl("adv.tc.stop", "tcrun").c_str(), ImVec2(-1, 0))) g_dev.tc_stop(true);
    }
    ImGui::EndDisabled();
    if (!TR.msg.empty()) ImGui::TextDisabled("%s", TR.msg.c_str());

    if (!TR.pts.empty()) {
        static std::vector<float> tx, ty;
        tx.clear(); ty.clear();
        double tmin = 1e9, tmax = -1e9;
        for (auto& p : TR.pts) { tx.push_back((float)p.t); ty.push_back((float)p.peak); if (p.t < tmin) tmin = p.t; if (p.t > tmax) tmax = p.t; }
        ImGui::Text(T("adv.tc.pts"), TR.pts.size(), tmin, tmax);
        if (ImPlot::BeginPlot("##tcplot", ImVec2(-1, 140), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
            ImPlot::SetupAxes("°C", T("adv.peak"), ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, c_trace2, 1.0f, c_trace2);
            ImPlot::PlotScatter("##pk", tx.data(), ty.data(), (int)tx.size());
            ImPlot::EndPlot();
        }
        ImGui::SetNextItemWidth(70); ImGui::InputFloat("t0, °C", &g_tc_t0, 0, 0, "%.1f");
        ImGui::SameLine();
        if (ImGui::SmallButton(Lbl("adv.tc.mean", "tcmean").c_str())) { double s = 0; for (auto& p : TR.pts) s += p.t; g_tc_t0 = (float)(s / TR.pts.size()); }
        ImGui::RadioButton(Lbl("adv.tc.lin", "tclin").c_str(), &g_tc_order, 1); ImGui::SameLine(); ImGui::RadioButton(Lbl("adv.tc.quad", "tcquad").c_str(), &g_tc_order, 2);
        double k1 = 0, k2 = 0, rms = 0;
        bool ok = g_dev.tc_fit(g_tc_t0, g_tc_order, k1, k2, rms);
        if (ok) {
            ImGui::PushFont(font_mono);
            ImGui::Text("k1 = %.4e /°C  (%.3f %%/°C)", k1, k1 * 100.0);
            ImGui::Text("k2 = %.4e /°C²", k2);
            ImGui::Text(T("adv.tc.rms"), rms);
            ImGui::PopFont();
            if (tmax - tmin < 5) ImGui::TextColored(c_warn, "%s", T("adv.tc.span_warn"));
            ImGui::BeginDisabled(!conn || g_dev.queue_busy());
            if (PrimaryButton(Lbl("adv.tc.write", "tcwrite").c_str(), ImVec2(-1, 0))) {
                if (TR.running) g_dev.tc_stop(false);
                char c[96]; snprintf(c, sizeof c, "-tc %.3f %.6e %.6e", g_tc_t0, k1, k2);
                g_dev.queue({ c, "-save" });
            }
            ImGui::EndDisabled();
        } else ImGui::TextDisabled(T("adv.tc.need"), g_tc_order == 2 ? 4 : 3);
    }
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

    static const char* const sections[][2] = {
        { "help.h.purpose", "help.purpose" },
        { "help.h.home",    "help.home" },
        { "help.h.wizard",  "help.wizard" },
        { "help.h.adv",     "help.adv" },
        { "help.h.hv",      "help.hv" },
        { "help.h.lang",    "help.lang" },
    };
    for (const auto& s : sections) {
        HelpSection(T(s[0]));
        ImGui::TextWrapped("%s", T(s[1]));
    }
    HelpSection(T("help.h.cli"));
    ImGui::TextWrapped("%s", T("help.cli"));
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
    if (ImGui::Button(Lbl("help.close", "helpclose").c_str(), ImVec2(110, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
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
    else                          DrawAdvanced(area, asz);

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
