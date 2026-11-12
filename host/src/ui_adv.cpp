// ui_adv.cpp — расширенный режим пульта (макет host/design/mockup.html, экран 5).
// Слева навигация по задачам (группы «Прибор», «Настройка», «Обслуживание»), посередине страница,
// справа колонка телеметрии (PanelTelemetry) и последние строки журнала обмена.
// Страницы собраны из прежних панелей и вкладок (ui_panels.h) и тел шагов мастера (ui_wizard.h):
// ни одна функция прежнего пульта не потеряна.
#include "ui_adv.h"
#include "ui_panels.h"
#include "ui_shell.h"
#include "ui_wizard.h"
#include "device.h"
#include "theme.h"
#include "i18n.h"

namespace {

using namespace theme;

const float NAV_W   = 250.0f;   // навигация
const float TELE_W  = 330.0f;   // колонка телеметрии
const float TITLE_H = 52.0f;    // полоса заголовка страницы
const float GAP     = 5.0f;
const int   LOG_N   = 12;       // строк журнала в колонке

// ключи заголовков страниц, в порядке AdvPage
const char* const k_page_key[AdvN] = {
    "adv.nav.overview", "adv.nav.spectrum", "adv.nav.hv",
    "adv.nav.tpl", "adv.nav.scale", "adv.nav.thermo", "adv.nav.detector", "adv.nav.sensors",
    "adv.nav.selftest", "adv.nav.logger", "adv.nav.firmware", "adv.nav.console",
};

struct NavGroup { const char* key; int first, last; };   // страницы first…last включительно
const NavGroup k_groups[] = {
    { "adv.nav.group.device",  Overview, Hv },
    { "adv.nav.group.setup",   Template, Sensors },
    { "adv.nav.group.service", Selftest, Console },
};

const ImGuiWindowFlags k_fixed = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;

bool BeginFixed(const char* id, ImVec2 pos, ImVec2 size, ImGuiWindowFlags extra = 0)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    return ImGui::Begin(id, nullptr, k_fixed | extra);
}

void SmallHeader(const char* s)
{
    ImGui::PushFont(font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// навигация: группы и пункты; выбранный — подложка и полоса акцента слева
void Nav(ImVec2 pos, ImVec2 size)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 12));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, c_child);
    BeginFixed("##advnav", pos, size);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const NavGroup& g : k_groups) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::SetCursorPosX(16);
        SmallHeader(T(g.key));
        ImGui::Dummy(ImVec2(0, 2));
        for (int p = g.first; p <= g.last; p++) {
            bool sel = g_ui.adv_page == p;
            ImGui::PushID(p);
            ImGui::PushStyleColor(ImGuiCol_Header, c_accent_sel);
            if (ImGui::Selectable("##nav", sel, 0, ImVec2(0, 30))) g_ui.adv_page = p;
            ImGui::PopStyleColor();
            ImGui::PopID();
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            if (sel) dl->AddRectFilled(a, ImVec2(a.x + 3, b.y), U32(c_accent));
            float ty = a.y + (b.y - a.y - ImGui::GetTextLineHeight()) * 0.5f;
            dl->AddText(ImVec2(a.x + 18, ty), U32(sel ? c_text_strong : c_text), T(k_page_key[p]));
        }
    }
    ImGui::End();
}

// заголовок страницы
void Title(ImVec2 pos, ImVec2 size, int page)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    BeginFixed("##advtitle", pos, size, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(2);
    ImGui::PushFont(font_title);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_strong);
    ImGui::TextUnformatted(T(k_page_key[page]));
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::End();
}

// страница из вкладки: окно с прокруткой, внутри — функция-вкладка
template <class F>
void TabPage(const char* id, ImVec2 pos, ImVec2 size, F body)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    BeginFixed(id, pos, size);
    ImGui::PopStyleVar();
    body();
    ImGui::End();
}

// короткий журнал команд: последние LOG_N строк обмена
void CmdLog(ImVec2 pos, ImVec2 size)
{
    BeginFixed("##advlog", pos, size, ImGuiWindowFlags_NoScrollbar);
    SmallHeader(T("adv.log"));
    ImGui::Separator();
    ImGui::BeginChild("##advlogb", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGui::PushFont(font_mono);
    const auto& log = g_dev.log;
    size_t from = log.size() > (size_t)LOG_N ? log.size() - LOG_N : 0;
    for (size_t i = from; i < log.size(); i++) {
        const auto& l = log[i];
        ImVec4 col; const char* pre;
        switch (l.dir) {
        case 1:  col = c_accent;   pre = "> ";  break;
        case 2:  col = c_ok;       pre = "";    break;
        case 3:  col = c_danger;   pre = "! ";  break;
        default: col = c_text_dim; pre = "";    break;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextWrapped("%s%s", pre, l.text.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::End();
}

} // namespace

void DrawAdvanced(ImVec2 pos, ImVec2 size)
{
    int page = g_ui.adv_page;
    if (page < 0 || page >= AdvN) page = g_ui.adv_page = Overview;

    bool tele = page != Console;                     // консоль — во всю ширину
    float x0 = pos.x + NAV_W;
    float pw = size.x - NAV_W - (tele ? TELE_W : 0.0f);

    Nav(pos, ImVec2(NAV_W, size.y));
    Title(ImVec2(x0, pos.y), ImVec2(pw, TITLE_H), page);

    ImVec2 cp(x0 + GAP, pos.y + TITLE_H);            // область страницы
    ImVec2 cs(pw - 2 * GAP, size.y - TITLE_H - GAP);
    float half = (cs.x - GAP) * 0.5f;

    switch (page) {
    case Overview:
        PanelConnection(cp, ImVec2(cs.x < 460 ? cs.x : 460, cs.y));
        break;
    case Spectrum:
        PanelSpectrum(cp, cs);
        break;
    case Hv:
        PanelHV(cp, ImVec2(half, cs.y));
        TabPage("##advhvboot", ImVec2(cp.x + half + GAP, cp.y), ImVec2(half, cs.y), [] { TabHvBoot(); });
        break;
    case Template:
        TabPage("##advtpl", cp, cs, [] { TabTemplate(); });
        break;
    case Scale:
        TabPage("##advscale", cp, cs, [] {
            ImGui::PushID("res");   ImGui::PushFont(font_small); ImGui::TextUnformatted(T("wiz.step.res.short")); ImGui::PopFont();
            WizStepBodyEmbedded(ds::Step::Res);   ImGui::PopID();
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::PushID("scale"); ImGui::PushFont(font_small); ImGui::TextUnformatted(T("wiz.step.scale.short")); ImGui::PopFont();
            WizStepBodyEmbedded(ds::Step::Scale); ImGui::PopID();
        });
        break;
    case Thermo:
        TabPage("##advtc", cp, cs, [] { TabThermo(); });
        break;
    case Detector:
        TabPage("##advdet", cp, cs, [] { TabDetector(); });
        break;
    case Sensors:
        TabPage("##advsens", cp, cs, [] { TabCalib(); });
        break;
    case Selftest:
        TabPage("##advself", cp, cs, [] { TabDiag(); ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); TabSelftest(); });
        break;
    case Logger:
        TabPage("##advlogger", cp, cs, [] { TabLogger(); });
        break;
    case Firmware:
        TabPage("##advfw", cp, cs, [] { WizStepBodyEmbedded(ds::Step::Flash); });
        break;
    case Console:
        PanelConsole(cp, cs);
        break;
    default: break;
    }

    if (tele) {
        float rx = pos.x + size.x - TELE_W;
        float th = size.y * 0.55f;
        if (th < 330) th = 330;
        PanelTelemetry(ImVec2(rx, pos.y + GAP), ImVec2(TELE_W - GAP, th));
        CmdLog(ImVec2(rx, pos.y + GAP + th + GAP), ImVec2(TELE_W - GAP, size.y - th - 3 * GAP));
    }
}
