// ui_home.cpp — см. ui_home.h. Раскладка по макету: .connect (экран 1), .state + .steps + .kv (экран 2),
// .state + .tasks + .hvbox + .kv (экран 4). Одна главная (акцентная) кнопка на вид.
#include "ui_home.h"
#include "ui_shell.h"
#include "device.h"
#include "devstate.h"
#include "fwflash.h"
#include "i18n.h"
#include "theme.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace theme;

namespace {

const float GAP = 18.0f;          // промежуток между колонками и карточками
const float RIGHT_W = 300.0f;     // правая колонка показаний (макет: 300 px)
const float BIG_H = 44.0f;        // высота главной кнопки (.btn.big)

// ---------- данные ----------

// число из -gst; false — ключа нет, «-», nan
bool GNum(const char* key, double& v)
{
    std::string s = g_dev.gstr(key, "");
    if (s.empty() || s == "-") return false;
    char* e = nullptr;
    v = strtod(s.c_str(), &e);
    return e != s.c_str() && !*e && std::isfinite(v);
}

// дата записи о настройке; 0 и 1 (настроен до учёта) — «дата неизвестна»
std::string FmtDate(uint32_t unix_t)
{
    if (unix_t < 1000000000u) return T("home.date_unknown");
    time_t t = (time_t)unix_t;
    struct tm lt;
    if (localtime_s(&lt, &t) != 0) return T("home.date_unknown");
    char b[32];
    strftime(b, sizeof b, "%d.%m.%Y", &lt);
    return b;
}

// «GammaSpec v1 RISE …» → «GammaSpec v1»
std::string FwName()
{
    const std::string& f = g_dev.fw_info;
    size_t p = f.find(" RISE");
    return p == std::string::npos ? f : f.substr(0, p);
}

const char* StepKey(ds::Step s, const char* what)
{
    static const char* names[] = { "flash", "check", "zero", "hv", "tpl", "hv2", "res", "scale", "save" };
    static char buf[64];
    snprintf(buf, sizeof buf, "wiz.step.%s.%s", names[(int)s], what);
    return T(buf);
}

// ---------- виджеты ----------

bool PrimaryBig(const char* label, float w)
{
    ImGui::PushStyleColor(ImGuiCol_Button,        c_accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c_accent_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  c_accent_active);
    ImGui::PushStyleColor(ImGuiCol_Text,          c_on_accent);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(22, 0));
    bool r = ImGui::Button(label, ImVec2(w, BIG_H));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return r;
}

// второстепенное действие текстом-ссылкой (подчёркивается при наведении)
bool LinkButton(const char* label)
{
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text,          c_accent);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    bool r = ImGui::Button(label);
    if (ImGui::IsItemHovered()) {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y - 1), U32(c_accent));
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return r;
}

void Dim(const char* s)
{
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}

void SmallHeader(const char* s)
{
    ImGui::PushFont(font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Title(const char* s)
{
    ImGui::PushFont(font_title);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_strong);
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// галочка (как .ic в макете)
void CheckIcon(ImDrawList* dl, ImVec2 p, float s, ImU32 col, float th = 2.0f)
{
    ImVec2 pts[3] = { ImVec2(p.x + s * 0.12f, p.y + s * 0.53f), ImVec2(p.x + s * 0.38f, p.y + s * 0.78f),
                      ImVec2(p.x + s * 0.88f, p.y + s * 0.22f) };
    dl->AddPolyline(pts, 3, col, 0, th);
}

// знак вида 44×44: треугольник с «!» (внимание) или круг с галочкой (готово)
void Mark(bool ok)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float S = 44.0f;
    if (ok) {
        ImU32 c = U32(c_ok);
        dl->AddCircle(ImVec2(p.x + 22, p.y + 22), 19, c, 0, 3.0f);
        ImVec2 pts[3] = { ImVec2(p.x + 12, p.y + 23), ImVec2(p.x + 19, p.y + 30), ImVec2(p.x + 32, p.y + 15) };
        dl->AddPolyline(pts, 3, c, 0, 3.5f);
    } else {
        ImU32 c = U32(c_warn);
        dl->AddTriangle(ImVec2(p.x + 22, p.y + 4), ImVec2(p.x + 41, p.y + 38), ImVec2(p.x + 3, p.y + 38), c, 3.0f);
        dl->AddRectFilled(ImVec2(p.x + 20.5f, p.y + 15), ImVec2(p.x + 23.5f, p.y + 27), c);
        dl->AddRectFilled(ImVec2(p.x + 20.5f, p.y + 30), ImVec2(p.x + 23.5f, p.y + 33), c);
    }
    ImGui::Dummy(ImVec2(S, S));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(ok ? "home.mark.ok" : "home.mark.warn"));
}

// карточка (.card): рамка, фон панели, внутренний отступ; высота по содержимому, если h == 0
bool BeginCard(const char* id, float w, float h = 0.0f, ImVec2 pad = ImVec2(18, 16))
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c_child);
    ImGui::PushStyleColor(ImGuiCol_Border,  c_border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 2.0f);
    ImGuiChildFlags f = ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (h <= 0.0f) f |= ImGuiChildFlags_AutoResizeY;
    bool r = ImGui::BeginChild(id, ImVec2(w, h), f, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    return r;
}

// конец карточки; edge — цвет черты 4 px слева (.state.warnc/.okc), nullptr — без неё
void EndCard(const ImVec4* edge = nullptr)
{
    ImGui::EndChild();
    if (edge) {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + 4, b.y), U32(*edge));
    }
}

// промежуток между карточками колонки
void CardGap() { ImGui::Dummy(ImVec2(0, 8)); }

// колонка без рамки и фона
void BeginColumn(const char* id, float w, float h)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleColor();
}

// ---------- таблица «подпись — значение» (.kv) ----------

struct KvVal { std::string text; ImVec4 col; bool mono; };

bool BeginKv(const char* id)
{
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, c_border);
    bool r = ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp);
    if (r) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch, 1.3f);
    }
    return r;
}

void EndKv()
{
    ImGui::EndTable();
    ImGui::PopStyleColor();
}

void KvRow(const char* k, const KvVal& v)
{
    ImGui::TableNextRow(0, 30.0f);
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
    ImGui::TextWrapped("%s", k);
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    if (v.mono) ImGui::PushFont(font_mono);
    float tw = ImGui::CalcTextSize(v.text.c_str()).x;
    float cw = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Text, v.col);
    if (tw <= cw) {                                  // вправо, как .kv td:last-child
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cw - tw);
        ImGui::TextUnformatted(v.text.c_str());
    } else {
        ImGui::TextWrapped("%s", v.text.c_str());    // не влезает — переносом, а не обрезкой
    }
    ImGui::PopStyleColor();
    if (v.mono) ImGui::PopFont();
}

std::string Fmt(const char* key, double a)
{
    char b[128];
    const char* f = T(key);
    // все форматы home.val.* с одной подстановкой: %d или %.Nf
    if (strstr(f, "%d")) snprintf(b, sizeof b, f, (int)std::lround(a));
    else                 snprintf(b, sizeof b, f, a);
    return b;
}

// строки показаний; which — что показывать (вид «не настроен» и «готово» показывают разное)
enum { KV_HV = 1, KV_TEMP = 2, KV_RATE = 4, KV_VREF = 8, KV_LOG = 16, KV_RES = 32, KV_SCALE = 64 };

void KvRows(unsigned which)
{
    double v;
    if (which & KV_HV) {
        if (g_dev.gst_age() < 5.0) {
            if (g_dev.hv_on()) KvRow(T("home.kv.hv"), { Fmt("home.val.hv_on_v", std::fabs(g_dev.gnum("hv_out"))), c_hv, false });
            else               KvRow(T("home.kv.hv"), { T("home.val.off"), c_text, false });
        }
    }
    if ((which & KV_TEMP) && g_dev.gstr("pt", "-") == "ok" && GNum("t", v))
        KvRow(T("home.kv.temp"), { Fmt("home.val.temp", v), c_text, true });
    if ((which & KV_RATE) && GNum("cps_in", v))
        KvRow(T("home.kv.rate"), { Fmt("home.val.cps", v), c_text, true });
    if (which & KV_VREF) {
        std::string ok = g_dev.gstr("vref_ok", "-");
        if (ok == "1") KvRow(T("home.kv.vref"), { std::string("\xE2\x9C\x93 ") + T("home.val.ok"), c_ok, false });   // ✓
        else if (ok == "0") KvRow(T("home.kv.vref"), { std::string("! ") + T("home.val.bad"), c_warn, false });
    }
    if ((which & KV_RES) && g_dev.setup_known == ds::SetupKnown::Yes && g_dev.setup.res_unix && g_dev.setup.res_fwhm > 0) {
        char b[64]; snprintf(b, sizeof b, "%.2f %%", g_dev.setup.res_fwhm);
        KvRow(T("home.kv.res"), { b, c_text, true });
    }
    if ((which & KV_SCALE) && g_dev.becq_ok && g_dev.becq_c[1] > 0)
        KvRow(T("home.kv.scale"), { Fmt("home.val.kev_ch", g_dev.becq_c[1]), c_text, true });
    if (which & KV_LOG) {
        ds::LogState ls;
        if (ds::parse_log_status(g_dev.log_status, ls)) {
            if (ls.active)       KvRow(T("home.kv.logger"), { Fmt("home.val.logger_on_n", (double)ls.records), c_ok, false });
            else if (ls.enabled) KvRow(T("home.kv.logger"), { Fmt("home.val.logger_armed_n", (double)ls.records), c_text, false });
            else if (ls.records) KvRow(T("home.kv.logger"), { Fmt("home.val.logger_off_n", (double)ls.records), c_text_dim, false });
            else                 KvRow(T("home.kv.logger"), { T("home.val.logger_off"), c_text_dim, false });
        }
    }
}

// ================= вид 1: нет связи =================

int  s_sel = -1;                  // выбранный порт
bool s_failed = false;            // последняя попытка «Подключить» не удалась (показывать до следующей)
unsigned long s_err = 0;

void ViewNotConnected(ImVec2 size)
{
    const std::vector<std::string>& ports = g_dev.ports;
    if (s_sel < 0 || s_sel >= (int)ports.size()) s_sel = ports.empty() ? -1 : (int)ports.size() - 1;   // FT231X обычно последний

    const float W = 480.0f;
    float x = (size.x - W) * 0.5f;
    if (x < GAP) x = GAP;
    ImGui::SetCursorPos(ImVec2(x, size.y * 0.14f));
    BeginCard("##connect", W, 0.0f, ImVec2(32, 28));
    Title(T("conn.title"));
    ImGui::PushFont(font_small);
    Dim(T("conn.hint"));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 10));

    ImGui::PushFont(font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
    ImGui::TextUnformatted(T("conn.port"));
    ImGui::PopStyleColor();
    ImGui::PopFont();
    const char* refresh = T("conn.refresh");
    float rw = ImGui::CalcTextSize(refresh).x + 2 * ImGui::GetStyle().FramePadding.x;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - rw - ImGui::GetStyle().ItemSpacing.x);
    const char* prev = s_sel < 0 ? T("conn.no_ports") : ports[s_sel].c_str();
    if (ImGui::BeginCombo("##port", prev)) {
        for (int i = 0; i < (int)ports.size(); i++)
            if (ImGui::Selectable(ports[i].c_str(), i == s_sel)) s_sel = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(refresh)) {
        g_dev.refresh_ports();
        s_sel = g_dev.ports.empty() ? -1 : (int)g_dev.ports.size() - 1;
    }
    ImGui::Dummy(ImVec2(0, 6));

    ImGui::BeginDisabled(s_sel < 0);
    if (PrimaryBig(T("conn.connect"), -1.0f)) {
        s_failed = false;
        if (!g_dev.connect(ports[s_sel])) { s_failed = true; s_err = g_dev.last_open_error; }
    }
    ImGui::EndDisabled();

    if (s_failed) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, c_warn);
        ImGui::TextWrapped("! %s", T(ds::connect_hint_key(s_err)));
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushFont(font_small);
    Dim(T("conn.one_program"));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::BeginDisabled(s_sel < 0);
    if (LinkButton(T("home.flash_link"))) {          // R1: мастер на шаге «Прошивка» работает без связи
        g_ui.flash_port = ports[s_sel];
        ui_go(ds::Mode::Wizard, ds::Step::Flash);
    }
    ImGui::EndDisabled();
    EndCard();
}

// ================= вид 2: не настроен =================

std::string StepResult(const ds::Snapshot& s, ds::Step st)
{
    const ds::SetupRec& r = s.s;
    char b[128];
    switch (st) {
    case ds::Step::Flash: return FwName();
    case ds::Step::Check: return FmtDate(r.chk_unix);
    case ds::Step::Zero: {
        double mv;
        if (GNum("hv_zero", mv)) { snprintf(b, sizeof b, T("home.val.mv"), mv); return FmtDate(r.zero_unix) + ", " + b; }
        return FmtDate(r.zero_unix);
    }
    case ds::Step::Hv:
    case ds::Step::Hv2:
        snprintf(b, sizeof b, T("home.res.hv"), r.hv_v, r.hv_peak_ch);
        return b;
    case ds::Step::Tpl:   return r.tpl_unix ? FmtDate(r.tpl_unix) : T("home.res.in_device");
    case ds::Step::Res:
        snprintf(b, sizeof b, T("home.res.fwhm"), r.res_fwhm);
        return b;
    case ds::Step::Scale:
        if (g_dev.becq_ok && g_dev.becq_c[1] > 0) return Fmt("home.val.kev_ch", g_dev.becq_c[1]);
        return T("home.res.in_device");
    case ds::Step::Save:  return FmtDate(r.done_unix);
    default:              return "";
    }
}

void ViewNotConfigured(ImVec2 avail, const ds::Snapshot& snap)
{
    bool old = snap.known == ds::SetupKnown::OldFirmware;
    int done = ds::steps_done_count(snap);
    ds::Step next = ds::first_open_step(snap);
    float leftW = avail.x - RIGHT_W - GAP;
    if (leftW < 360.0f) leftW = 360.0f;

    BeginColumn("##left", leftW, avail.y);
    // ---- карточка состояния ----
    BeginCard("##state", -1.0f, 0.0f, ImVec2(22, 20));
    {
        const char* btn = T(done == 0 ? "home.notset.start" : "home.notset.continue");
        float bw = ImGui::CalcTextSize(btn).x + 44.0f;
        float y0 = ImGui::GetCursorPosY();
        Mark(false);
        ImGui::SameLine(0, 16);
        float tw = ImGui::GetContentRegionAvail().x - bw - 16;
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
        Title(T(old ? "home.oldfw.title" : "home.notset.title"));
        char b[256];
        if (old) {
            ImGui::TextWrapped("%s", T("home.oldfw.text"));
        } else {
            snprintf(b, sizeof b, T("home.notset.progress"), done, (int)ds::Step::N);
            ImGui::TextWrapped("%s", b);
        }
        snprintf(b, sizeof b, T("home.notset.next"), StepKey(next, "title"));
        ImGui::TextWrapped("%s", b);
        ImGui::PushFont(font_small);
        Dim(T(ds::step_needs_cs(next) ? "home.notset.need_cs" : "home.notset.need_none"));
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        float gh = ImGui::GetItemRectSize().y;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bw);
        if (gh > BIG_H) ImGui::SetCursorPosY(y0 + (gh - BIG_H) * 0.5f);
        if (PrimaryBig(btn, bw)) ui_go(ds::Mode::Wizard, next);
    }
    EndCard(&c_warn);
    CardGap();

    // ---- список шагов ----
    BeginCard("##steps", -1.0f, 0.0f, ImVec2(18, 12));
    SmallHeader(T("home.steps_title"));
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, c_border);
    if (ImGui::BeginTable("##st", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthFixed, 26.0f);
        ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("r", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        for (int i = 0; i < (int)ds::Step::N; i++) {
            ds::Step st = (ds::Step)i;
            bool d = ds::step_done(snap, st), cur = st == next;
            ImGui::TableNextRow(0, 30.0f);
            if (cur) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, U32(c_accent_sel));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, cur ? c_accent : c_text_dim);
            ImGui::Text("%d", i + 1);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, d || cur ? c_text : c_text_dim);
            ImGui::TextUnformatted(StepKey(st, "title"));
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (d) {
                ImVec2 p = ImGui::GetCursorScreenPos();
                float h = ImGui::GetFrameHeight();
                CheckIcon(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + (h - 14) * 0.5f), 14, U32(c_ok));
                ImGui::Dummy(ImVec2(18, 1));
                ImGui::SameLine(0, 4);
                std::string r = StepResult(snap, st);
                ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
                ImGui::TextUnformatted(r.c_str());
                ImGui::PopStyleColor();
            } else if (cur) {
                std::string r = T("home.step.next");
                if (ds::step_needs_cs(st)) { r += " \xC2\xB7 "; r += T("home.step.needs_cs"); }   // ·
                ImGui::PushStyleColor(ImGuiCol_Text, c_accent);
                ImGui::TextUnformatted(r.c_str());
                ImGui::PopStyleColor();
            } else if (st == ds::Step::Check && snap.s.chk_unix && !snap.s.chk_ok) {
                ImGui::PushStyleColor(ImGuiCol_Text, c_warn);
                ImGui::TextUnformatted(T("home.res.failed"));
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor();
    EndCard();
    ImGui::EndChild();

    // ---- справа: показания ----
    ImGui::SameLine(0, GAP);
    BeginColumn("##right", RIGHT_W, avail.y);
    BeginCard("##now", -1.0f);
    SmallHeader(T("home.now"));
    if (BeginKv("##kv")) { KvRows(KV_HV | KV_TEMP | KV_RATE | KV_VREF | KV_LOG); EndKv(); }
    EndCard();
    CardGap();
    ImGui::PushFont(font_small);
    Dim(T("home.more_in_adv"));
    ImGui::PopFont();
    ImGui::EndChild();
}

// ================= вид 3: всё готово =================

// карточка задачи (.task): вся карточка — кнопка; status — метка снизу (nullptr — нет)
bool TaskCard(const char* id, float w, float h, const char* title, const std::string& desc,
              const char* status, const ImVec4& status_col)
{
    BeginCard(id, w, h, ImVec2(16, 14));
    ImVec2 p0 = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ImVec2 cp = ImGui::GetCursorPos();               // начало содержимого = внутренний отступ карточки
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::SetNextItemAllowOverlap();
    bool clicked = ImGui::InvisibleButton("##hit", sz);
    bool hov = ImGui::IsItemHovered();
    if (hov) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
        dl->PushClipRect(p0, p1, false);             // внутренний клип карточки срезает поля — рисуем по всей
        dl->AddRectFilled(p0, p1, U32(c_head));
        dl->AddRect(p0, p1, U32(c_accent));
        dl->PopClipRect();
    }
    ImGui::SetCursorPos(cp);
    ImGui::PushTextWrapPos(sz.x - cp.x);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_strong);
    ImGui::TextWrapped("%s", title);
    ImGui::PopStyleColor();
    ImGui::PushFont(font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
    ImGui::TextWrapped("%s", desc.c_str());
    ImGui::PopStyleColor();
    if (status) {
        ImGui::PushStyleColor(ImGuiCol_Text, status_col);
        ImGui::TextWrapped("%s", status);
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    EndCard();
    return clicked;
}

void GoAdv(AdvPage p)
{
    g_ui.adv_page = p;
    ui_go(ds::Mode::Advanced);
}

void ViewReady(ImVec2 avail, const ds::Snapshot& snap)
{
    float leftW = avail.x - RIGHT_W - GAP;
    if (leftW < 420.0f) leftW = 420.0f;

    BeginColumn("##left", leftW, avail.y);
    BeginCard("##state", -1.0f, 0.0f, ImVec2(22, 20));
    {
        const char* btn = T("home.ready.give_port");
        float bw = ImGui::CalcTextSize(btn).x + 44.0f;
        float y0 = ImGui::GetCursorPosY();
        Mark(true);
        ImGui::SameLine(0, 16);
        float tw = ImGui::GetContentRegionAvail().x - bw - 16;
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
        Title(T("home.ready.title"));
        char b[256];
        ImGui::PushFont(font_small);
        if (snap.s.done_unix < 1000000000u) Dim(T("home.ready.date_unknown"));
        else { snprintf(b, sizeof b, T("home.ready.date"), FmtDate(snap.s.done_unix).c_str()); Dim(b); }
        ImGui::PopFont();
        snprintf(b, sizeof b, T("home.ready.becq"), g_dev.port_name.c_str());
        ImGui::TextWrapped("%s", b);
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        float gh = ImGui::GetItemRectSize().y;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bw);
        if (gh > BIG_H) ImGui::SetCursorPosY(y0 + (gh - BIG_H) * 0.5f);
        if (PrimaryBig(btn, bw)) g_dev.disconnect();          // порт свободен для BecqMoni
    }
    EndCard(&c_ok);
    CardGap();

    // ---- задачи ----
    SmallHeader(T("home.more"));
    {
        const float g = 10.0f, h = 118.0f;
        float w = (ImGui::GetContentRegionAvail().x - 2 * g) / 3.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(g, g));

        std::vector<uint8_t> img; std::string tag;
        bool emb = fwflash::embedded(img, tag);
        char fwd[256];
        if (emb) snprintf(fwd, sizeof fwd, T("home.task.fw.app"), tag.c_str());
        else     snprintf(fwd, sizeof fwd, T("home.task.fw.desc"), FwName().c_str());

        // термокомпенсация: метка только если -tc ответил
        const char* tc_tag = nullptr; ImVec4 tc_col = c_warn;
        double t0, k1, k2;
        if (ds::parse_tc(g_dev.tc_reply, t0, k1, k2) && k1 == 0 && k2 == 0) tc_tag = T("home.task.thermo.not_set");

        if (TaskCard("##t_self", w, h, T("home.task.selftest.title"), T("home.task.selftest.desc"), nullptr, c_text))
            GoAdv(Selftest);
        ImGui::SameLine();
        if (TaskCard("##t_log", w, h, T("home.task.logger.title"), T("home.task.logger.desc"), nullptr, c_text))
            GoAdv(Logger);
        ImGui::SameLine();
        if (TaskCard("##t_fw", w, h, T("home.task.fw.title"), fwd, nullptr, c_text))
            ui_go(ds::Mode::Wizard, ds::Step::Flash);
        if (TaskCard("##t_scale", w, h, T("home.task.scale.title"), T("home.task.scale.desc"), nullptr, c_text))
            ui_go(ds::Mode::Wizard, ds::Step::Res);
        ImGui::SameLine();
        if (TaskCard("##t_wiz", w, h, T("home.task.wizard.title"), T("home.task.wizard.desc"), nullptr, c_text))
            ui_go(ds::Mode::Wizard, ds::Step::Flash);
        ImGui::SameLine();
        if (TaskCard("##t_tc", w, h, T("home.task.thermo.title"), T("home.task.thermo.desc"), tc_tag, tc_col))
            GoAdv(Thermo);
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();

    // ---- справа: ВВ крупно и показания ----
    ImGui::SameLine(0, GAP);
    BeginColumn("##right", RIGHT_W, avail.y);
    BeginCard("##hvbox", -1.0f, 0.0f, ImVec2(18, 14));
    {
        ImGui::PushFont(font_small);
        Dim(T("home.hv.title"));
        ImGui::PopFont();
        bool fresh = g_dev.gst_age() < 5.0;
        if (!fresh) {
            ImGui::PushFont(font_title);
            Dim(T("status.hv_unknown"));
            ImGui::PopFont();
        } else if (g_dev.hv_on()) {
            ImGui::PushFont(font_mono_big);
            ImGui::PushStyleColor(ImGuiCol_Text, c_hv);
            ImGui::TextUnformatted(Fmt("home.val.volts", std::fabs(g_dev.gnum("hv_out"))).c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        } else {
            ImGui::PushFont(font_title);
            ImGui::TextUnformatted(T("home.val.off"));
            ImGui::PopFont();
        }
        double boot;
        if (GNum("hv_boot", boot)) {
            ImGui::PushFont(font_small);
            Dim(T(boot > 0 ? "home.hv.autostart" : "home.hv.manual"));
            ImGui::PopFont();
        }
    }
    bool hv_on_now = g_dev.gst_age() < 5.0 && g_dev.hv_on();
    EndCard(hv_on_now ? &c_hv : nullptr);
    CardGap();
    BeginCard("##now", -1.0f);
    if (BeginKv("##kv")) { KvRows(KV_TEMP | KV_RATE | KV_RES | KV_SCALE | KV_LOG); EndKv(); }
    EndCard();
    ImGui::EndChild();
}

void ViewLoading(ImVec2 size)
{
    const char* s = T("home.loading");
    ImVec2 ts = ImGui::CalcTextSize(s);
    ImGui::SetCursorPos(ImVec2((size.x - ts.x) * 0.5f, size.y * 0.3f));
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}

} // namespace

void DrawHome(ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(GAP, GAP));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings;
    bool vis = ImGui::Begin("##home", nullptr, f);
    ImGui::PopStyleVar(2);
    if (vis) {
        ds::Snapshot snap = g_dev.snapshot();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (snap.connected) s_failed = false;
        if (snap.connected && snap.known == ds::SetupKnown::Unknown && g_dev.setup_pending()) {
            ViewLoading(ImGui::GetWindowSize());
        } else {
            switch (ds::home_view(snap)) {
            case ds::Home::NotConnected:  ViewNotConnected(ImGui::GetWindowSize()); break;
            case ds::Home::NotConfigured: ViewNotConfigured(avail, snap); break;
            case ds::Home::Ready:         ViewReady(avail, snap); break;
            }
        }
    }
    ImGui::End();
}
