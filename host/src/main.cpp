// gamma_host — диагностический пульт гамма-спектрометра (v1: связь + диагностика)
// Win32 + DirectX 11 + Dear ImGui + ImPlot. Единый .exe без зависимостей.
// Раскладка — фиксированная (дизайн-спека §4): лево 300 | центр | право 340.

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "implot.h"

#include <d3d11.h>
#include <tchar.h>
#include <cstdio>
#include <cstring>

#include "device.h"
#include "theme.h"
#include "host_app.h"

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
static Device g_dev;
static bool   g_quit = false;
static int    g_port_sel = 0;
static bool   g_log_scale = true;
static char   g_cmd_buf[128] = "";
static bool   g_open_help = false;
static float  gLeftW = 300.0f, gRightW = 340.0f, gConnH = 276.0f, gConsoleH = 190.0f;
static float  g_spec_x[Device::CHANNELS];
static float  g_spec_y[Device::CHANNELS];

// ================= помощники UI =================

static bool PrimaryButton(const char* label, const ImVec2& sz = ImVec2(0, 0))
{
    using namespace theme;
    ImGui::PushStyleColor(ImGuiCol_Button,        c_accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c_accent_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  c_accent_active);
    ImGui::PushStyleColor(ImGuiCol_Text,          c_white);
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

// Раздвижной разделитель: тонкое окно поверх стыка панелей. Возвращает дельту
// перетаскивания (по X для вертикального, по Y для горизонтального).
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
    const char* prev = g_dev.ports.empty() ? "(нет портов)"
                                           : g_dev.ports[g_port_sel].c_str();
    if (ImGui::BeginCombo("##port", prev)) {
        for (int i = 0; i < (int)g_dev.ports.size(); i++)
            if (ImGui::Selectable(g_dev.ports[i].c_str(), i == g_port_sel))
                g_port_sel = i;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    Caption("Скорость");
    ImGui::SameLine();
    ImGui::PushFont(font_mono); ImGui::TextUnformatted("600000"); ImGui::PopFont();
    ImGui::SameLine(); Caption("бод · 8N1");
    ImGui::Spacing();

    if (g_dev.state == Device::State::Disconnected || g_dev.state == Device::State::Error) {
        if (PrimaryButton("Подключить", ImVec2(-1, 26)) && !g_dev.ports.empty())
            g_dev.connect(g_dev.ports[g_port_sel]);
        if (ImGui::Button("Обновить порты", ImVec2(-1, 0)))
            g_dev.refresh_ports();
    } else {
        if (!g_dev.acquiring) {
            if (PrimaryButton("Старт набора (-sta)", ImVec2(-1, 26)))
                g_dev.start();
        } else {
            if (DangerButton("Стоп набора (-sto)", ImVec2(-1, 26)))
                g_dev.stop();
        }
        if (ImGui::Button("Сброс спектра (-rst)", ImVec2(-1, 0)))
            g_dev.reset_spectrum();
        if (DangerButton("Отключить", ImVec2(-1, 0)))
            g_dev.disconnect();
    }

    ImGui::Separator();
    StatusDot(StateColor());
    ImGui::TextUnformatted(g_dev.status_text.c_str());
    ImGui::Separator();

    Caption("Серийник");
    ImGui::PushFont(font_mono);
    ImGui::PushStyleColor(ImGuiCol_Text, c_accent);
    ImGui::TextUnformatted(g_dev.serial.empty() ? "—" : g_dev.serial.c_str());
    ImGui::PopStyleColor(); ImGui::PopFont();

    Caption("Прошивка");
    ImGui::PushFont(font_mono);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_strong);
    ImGui::TextWrapped("%s", g_dev.fw_info.empty() ? "—" : g_dev.fw_info.c_str());
    ImGui::PopStyleColor(); ImGui::PopFont();

    ImGui::End();
}

static void PanelTelemetry(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    BeginPanel("##tele", pos, size);
    PanelHeader("СТАТУС / ТЕЛЕМЕТРИЯ");

    Caption("Время набора");
    char tb[16];
    uint32_t s = g_dev.elapsed_s;
    snprintf(tb, sizeof(tb), "%02u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    ImGui::PushFont(font_mono_time); ImGui::TextUnformatted(tb); ImGui::PopFont();
    ImGui::Spacing();

    Caption("Скорость счёта, cps");
    char cb[24]; snprintf(cb, sizeof(cb), "%u", g_dev.cps);
    ImGui::PushFont(font_mono_big);
    ImGui::PushStyleColor(ImGuiCol_Text, g_dev.acquiring ? c_accent : c_text_strong);
    ImGui::TextUnformatted(cb);
    ImGui::PopStyleColor(); ImGui::PopFont();
    ImGui::Spacing();

    Caption("Отбраковано"); ImGui::SameLine();
    char ib[24]; snprintf(ib, sizeof(ib), "%u", g_dev.invalid);
    ImGui::PushFont(font_mono);
    ImGui::PushStyleColor(ImGuiCol_Text, c_warn); ImGui::TextUnformatted(ib);
    ImGui::PopStyleColor(); ImGui::PopFont();

    Caption("Суммарный счёт"); ImGui::SameLine();
    char sb[32]; snprintf(sb, sizeof(sb), "%llu", (unsigned long long)g_dev.total());
    ImGui::PushFont(font_mono); ImGui::TextUnformatted(sb); ImGui::PopFont();

    ImGui::Spacing(); ImGui::Separator();
    if (g_dev.acquiring) {
        StatusDot(c_accent);
        ImGui::PushStyleColor(ImGuiCol_Text, c_accent);
        ImGui::TextUnformatted("ИДЁТ НАБОР");
        ImGui::PopStyleColor();
    } else {
        StatusDot(c_text_dim); Caption("Набор остановлен");
    }
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

    // сегмент Лин/Лог справа
    float rx = ImGui::GetWindowWidth() - 8 - 88;
    ImGui::SameLine(rx);
    auto seg = [&](const char* lbl, bool active) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, c_accent);
            ImGui::PushStyleColor(ImGuiCol_Text, c_white);
        }
        bool r = ImGui::Button(lbl, ImVec2(42, 0));
        if (active) ImGui::PopStyleColor(2);
        return r;
    };
    if (seg("Лин", !g_log_scale)) g_log_scale = false;
    ImGui::SameLine(0, 2);
    if (seg("Лог", g_log_scale)) g_log_scale = true;
    ImGui::Separator();

    char st[128];
    snprintf(st, sizeof(st), "Σ %llu    пик %d",
             (unsigned long long)g_dev.total(), g_dev.peak_channel());
    ImGui::PushFont(font_mono);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
    ImGui::TextUnformatted(st);
    ImGui::PopStyleColor(); ImGui::PopFont();

    for (int i = 0; i < Device::CHANNELS; i++) {
        g_spec_x[i] = (float)i;
        g_spec_y[i] = (float)g_dev.spectrum[i];
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ImPlot::BeginPlot("##plot", avail,
                          ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
        // X — навигация мышью (колесо/перетаскивание). Y — авто-подгон под данные,
        // поэтому спектр виден и в линейном, и в лог-масштабе.
        // Shift: колесо масштабирует Y (X на это время фиксируется).
        bool shift = ImGui::GetIO().KeyShift;
        ImPlot::SetupAxis(ImAxis_X1, "канал", shift ? ImPlotAxisFlags_Lock : ImPlotAxisFlags_None);
        ImPlot::SetupAxis(ImAxis_Y1, "счёт",  shift ? ImPlotAxisFlags_None : ImPlotAxisFlags_AutoFit);
        if (g_log_scale) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
        // нельзя уехать за пределы данных: X в [0..8192], Y не ниже нуля
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, (double)Device::CHANNELS);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, g_log_scale ? 0.1 : 0.0, 1.0e12);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, Device::CHANNELS, ImPlotCond_Once);
        ImPlot::PushStyleColor(ImPlotCol_Line, c_trace);
        ImPlot::PlotLine("спектр", g_spec_x, g_spec_y, Device::CHANNELS);
        ImPlot::PopStyleColor();
        ImPlot::EndPlot();
    }
    ImGui::End();
}

static void PanelConsole(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    BeginPanel("##con", pos, size);
    PanelHeader("КОНСОЛЬ · shproto");

    const char* chips[] = { "-sta", "-sto", "-rst", "-inf", "-cal",
                            "-tst spec", "-tst off", "-tst enc" };
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

static void PanelSelftest(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    BeginPanel("##enc", pos, size);
    PanelHeader("САМОТЕСТ · ENC");

    bool conn = (g_dev.state == Device::State::Connected ||
                 g_dev.state == Device::State::Acquiring);
    ImGui::BeginDisabled(!conn);
    if (PrimaryButton("Прогон ENC", ImVec2(-1, 26))) {
        g_dev.enc = Device::EncResult{};
        g_dev.enc.running = true;
        g_dev.send_cmd("-tst enc");
    }
    ImGui::EndDisabled();

    Caption("Тест-генератор");
    if (ImGui::Button("spec"))  g_dev.send_cmd("-tst spec");
    ImGui::SameLine(); if (ImGui::Button("off"))  g_dev.send_cmd("-tst off");
    ImGui::SameLine(); if (ImGui::Button("stat")) g_dev.send_cmd("-tst stat");
    ImGui::Separator();

    const Device::EncResult& e = g_dev.enc;
    if (e.running) {
        ImGui::PushStyleColor(ImGuiCol_Text, c_accent);
        ImGui::TextUnformatted("идёт прогон ~6 c…");
        ImGui::PopStyleColor();
    }

    if (ImGui::BeginTable("enc", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::PushFont(font_small);
        ImGui::TableSetupColumn("Ампл.");
        ImGui::TableSetupColumn("Центроид");
        ImGui::TableSetupColumn("σ");
        ImGui::TableSetupColumn("FWHM %");
        ImGui::TableHeadersRow();
        ImGui::PopFont();
        ImGui::PushFont(font_mono);
        int rows = e.valid ? e.n : 5;
        for (int i = 0; i < rows; i++) {
            ImGui::TableNextRow();
            if (e.valid) {
                const Device::EncPoint& p = e.pts[i];
                ImGui::TableSetColumnIndex(0); ImGui::Text("%d", p.amp);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f", p.ch);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f", p.sig);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.2f", p.ch > 0 ? 100.0f * p.fwhm / p.ch : 0.0f);
            } else {
                for (int c = 0; c < 4; c++) { ImGui::TableSetColumnIndex(c); ImGui::TextDisabled("—"); }
            }
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }

    ImGui::PushFont(font_mono);
    if (e.valid) {
        ImGui::PushStyleColor(ImGuiCol_Text, c_ok);
        ImGui::Text("k=%.4f ch/code", e.k);
        ImGui::PopStyleColor();
        ImGui::Text("INL=%.3f%%FS   R²=%.4f", e.inl, e.r2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
        ImGui::TextUnformatted("k = —   INL = —   R² = —");
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();

    if (e.valid && e.n >= 2) {
        static float ax[8], cy[8], fy[8], lx[2], ly[2];
        for (int i = 0; i < e.n; i++) {
            ax[i] = (float)e.pts[i].amp;
            cy[i] = e.pts[i].ch;
            fy[i] = e.pts[i].ch > 0 ? 100.0f * e.pts[i].fwhm / e.pts[i].ch : 0.0f;
        }
        lx[0] = ax[0]; lx[1] = ax[e.n - 1];
        ly[0] = e.k * lx[0] + e.b; ly[1] = e.k * lx[1] + e.b;

        ImGui::Spacing();
        Caption("Линейность: центроид vs код");
        if (ImPlot::BeginPlot("##lin", ImVec2(-1, 130),
                              ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
            ImPlot::SetupAxes("код", "канал");
            ImPlot::PushStyleColor(ImPlotCol_Line, c_accent);
            ImPlot::PlotLine("фит", lx, ly, 2);
            ImPlot::PopStyleColor();
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.5f, c_trace2, 1.0f, c_trace2);
            ImPlot::PlotScatter("точки", ax, cy, e.n);
            ImPlot::EndPlot();
        }
        Caption("FWHM, % vs код");
        if (ImPlot::BeginPlot("##fwhm", ImVec2(-1, 130),
                              ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
            ImPlot::SetupAxes("код", "FWHM %");
            ImPlot::PushStyleColor(ImPlotCol_Line, c_warn);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, c_warn, 1.0f, c_warn);
            ImPlot::PlotLine("fwhm", ax, fy, e.n);
            ImPlot::PopStyleColor();
            ImPlot::EndPlot();
        }
    } else if (!e.running) {
        ImGui::Spacing();
        ImGui::TextDisabled("Нажми «Прогон ENC» — таблица и");
        ImGui::TextDisabled("графики заполнятся (~6 c).");
    }

    ImGui::End();
}

static void MenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("Файл")) {
        if (ImGui::MenuItem("Выход")) g_quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Прибор")) {
        if (ImGui::MenuItem("Обновить порты")) g_dev.refresh_ports();
        bool dis = (g_dev.state != Device::State::Disconnected && g_dev.state != Device::State::Error);
        if (ImGui::MenuItem("Подключить", nullptr, false, !dis && !g_dev.ports.empty()))
            g_dev.connect(g_dev.ports[g_port_sel]);
        if (ImGui::MenuItem("Отключить", nullptr, false, dis))
            g_dev.disconnect();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Вид")) {
        ImGui::MenuItem("Лог-шкала спектра", nullptr, &g_log_scale);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Самотест")) {
        if (ImGui::MenuItem("Прогон ENC")) g_dev.send_cmd("-tst enc");
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Справка")) g_open_help = true;
    ImGui::BeginMenu("Калибровка", false);
    ImGui::BeginMenu("Высокое напряжение", false);
    ImGui::BeginMenu("Термокалибровка", false);

    const char* hint = "shproto · 600000 8N1 · v0.1";
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(hint).x - 14);
    ImGui::TextDisabled("%s", hint);
    ImGui::EndMainMenuBar();
}

static void StatusBar(ImVec2 pos, ImVec2 size)
{
    using namespace theme;
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 3));
    ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##status", nullptr, f);
    StatusDot(StateColor());
    ImGui::PushFont(font_mono);
    char buf[256];
    if (g_dev.acquiring)
        snprintf(buf, sizeof(buf), "ИДЁТ НАБОР   t=%us   cps=%u   Σ=%llu   drop=%u",
                 g_dev.elapsed_s, g_dev.cps, (unsigned long long)g_dev.total(), g_dev.invalid);
    else
        snprintf(buf, sizeof(buf), "%s", g_dev.state_str());
    ImGui::TextUnformatted(buf);

    char right[64];
    snprintf(right, sizeof(right), "%s   600000 8N1",
             g_dev.port_name.empty() ? "—" : g_dev.port_name.c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right).x - 12);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
    ImGui::TextUnformatted(right);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar();
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
    if (g_open_help) { ImGui::OpenPopup("Справка по настройке"); g_open_help = false; }

    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(710, 580), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Справка по настройке", nullptr)) return;

    ImGui::BeginChild("helpbody", ImVec2(0, -36), false);

    HelpSection("1. Подключение");
    ImGui::TextWrapped(
        "Подключи прибор по USB — появится виртуальный COM-порт (600000 бод, 8N1). "
        "В панели «Подключение» нажми «Обновить порты», выбери COM и «Подключить». "
        "При подключении пульт сам шлёт -inf и -cal — серийник и версия прошивки "
        "появятся в полях ниже.");

    HelpSection("2. Набор спектра");
    ImGui::TextWrapped(
        "Команды/чипы: -sta старт набора, -sto стоп, -rst сброс. Гистограмма на 8192 "
        "канала рисуется в центре; переключатель Лин/Лог меняет шкалу Y.");

    HelpSection("3. Телеметрия");
    ImGui::TextWrapped(
        "Время набора, скорость счёта (cps), доля отбракованных (наложения/шум) и "
        "суммарный счёт. В наборе cps подсвечивается акцентом.");

    HelpSection("4. Тест-генератор (диагностика без источника)");
    ImGui::TextWrapped(
        "-tst spec — встроенное распределение Cs-137; -tst mono <amp> [период_мкс] — "
        "моно-линия; -tst off — выключить. По умолчанию генератор ВЫКЛЮЧЕН: на боевой "
        "плате вход АЦП — реальный сигнал ФЭУ, и генератор не должен стартовать сам.");

    HelpSection("5. Самотест ENC");
    ImGui::TextWrapped(
        "«Прогон ENC» гоняет 5 амплитуд и считает линейность k (кан/код), INL (%FS) и "
        "FWHM по точкам. Отчёт приходит в консоль.");

    HelpSection("6. Консоль shproto");
    ImGui::TextWrapped(
        "Лог кадров (TX/RX), чипы быстрых команд и поле ввода. Протокол shproto — "
        "контракт с BecqMoni, вручную его править не нужно.");

    HelpSection("7. BecqMoni и порт");
    ImGui::TextWrapped(
        "Для повседневного просмотра спектров прибор подключается в BecqMoni (тот же COM, "
        "shproto). Этот пульт — для наладки/диагностики/калибровки. ВАЖНО: один COM-порт "
        "держит одна программа — при работе пульта закрой BecqMoni, и наоборот.");

    HelpSection("8. Командная строка (автоматизация)");
    ImGui::PushFont(theme::font_mono);
    ImGui::TextWrapped(
        "gammapult list\n"
        "gammapult info    --port COM6\n"
        "gammapult acquire --port COM6 --seconds 30 --gen spec --out spec.csv\n"
        "gammapult enc     --port COM6\n"
        "gammapult cmd     --port COM6 --send \"-inf\"");
    ImGui::PopFont();

    HelpSection("9. Высокое напряжение / Термокалибровка");
    ImGui::TextWrapped(
        "В разработке. ВВ (0…−1500 В) — с явной индикацией опасности; термокалибровка — "
        "запись спектров по температуре и расчёт компенсации усиления.");

    ImGui::EndChild();
    ImGui::Separator();
    if (ImGui::Button("Закрыть", ImVec2(110, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void DrawApp()
{
    MenuBar();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    float menuH = ImGui::GetFrameHeight();
    float statusH = 24.0f;
    float p = 5.0f, g = 5.0f;

    ImVec2 area(vp->Pos.x, vp->Pos.y + menuH);
    ImVec2 asz(vp->Size.x, vp->Size.y - menuH - statusH);

    float innerX = area.x + p, innerY = area.y + p;
    float innerW = asz.x - 2 * p, innerH = asz.y - 2 * p;

    // раздвижные границы — ограничения
    gLeftW    = clampf(gLeftW, 180.0f, innerW - gRightW - 2 * g - 260.0f);
    gRightW   = clampf(gRightW, 200.0f, innerW - gLeftW - 2 * g - 260.0f);
    gConnH    = clampf(gConnH, 150.0f, innerH - 150.0f);
    gConsoleH = clampf(gConsoleH, 110.0f, innerH - 220.0f);

    float leftW = gLeftW, rightW = gRightW;
    float centerW = innerW - leftW - rightW - 2 * g;
    float connH = gConnH, telemetryH = innerH - connH - g;
    float consoleH = gConsoleH, spectrumH = innerH - consoleH - g;
    float lx = innerX, cx = innerX + leftW + g, rx = cx + centerW + g;

    PanelConnection(ImVec2(lx, innerY),                  ImVec2(leftW, connH));
    PanelTelemetry (ImVec2(lx, innerY + connH + g),      ImVec2(leftW, telemetryH));
    PanelSpectrum  (ImVec2(cx, innerY),                  ImVec2(centerW, spectrumH));
    PanelConsole   (ImVec2(cx, innerY + spectrumH + g),  ImVec2(centerW, consoleH));
    PanelSelftest  (ImVec2(rx, innerY),                  ImVec2(rightW, innerH));

    // раздвижные разделители (поверх стыков панелей)
    gLeftW    += Splitter("##sp_lc", ImVec2(lx + leftW + g * 0.5f - 3, innerY),            ImVec2(6, innerH), true);
    gRightW   -= Splitter("##sp_cr", ImVec2(rx - g * 0.5f - 3, innerY),                    ImVec2(6, innerH), true);
    gConsoleH -= Splitter("##sp_sc", ImVec2(cx, innerY + spectrumH + g * 0.5f - 3),        ImVec2(centerW, 6), false);
    gConnH    += Splitter("##sp_ct", ImVec2(lx, innerY + connH + g * 0.5f - 3),            ImVec2(leftW, 6), false);

    StatusBar(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - statusH),
              ImVec2(vp->Size.x, statusH));

    DrawHelp();
}

// ================= main =================

int run_gui()
{
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       GetModuleHandle(nullptr), nullptr, nullptr, nullptr,
                       nullptr, L"gamma_host", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"GammaPult - service console",
                                WS_OVERLAPPEDWINDOW, 80, 60, 1200, 760,
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

    bool done = false;
    while (!done && !g_quit) {
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
