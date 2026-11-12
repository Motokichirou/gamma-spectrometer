// ui_wizard.cpp — см. ui_wizard.h. Логика шагов (условия успеха, команды прибора, подбор ВВ, обучение
// эталона, оценка разрешения) перенесена из прежнего мастера main.cpp без изменения смысла; процессы
// идут в WizTick (каждый кадр), а не только пока шаг на экране. Одна акцентная кнопка на экран:
// пока шаг не пройден — его действие, после — «Дальше».
#include "ui_wizard.h"
#include "ui_shell.h"
#include "ui_home.h"
#include "device.h"
#include "devstate.h"
#include "fwflash.h"
#include "peakfit.h"
#include "tpl_learn.h"
#include "i18n.h"
#include "theme.h"
#include <algorithm>
#include <deque>
#include "implot.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace theme;

namespace {

// размеры при 100 %; в пикселях — × масштаб интерфейса (theme::px)
float RAIL_W() { return px(270.0f); }   // рейка шагов (макет: .rail 270 px)
// полоса навигации внизу: промежуток, черта, отступ 8, кнопка 34, низ 10 (при 100 %) — из фактического стиля,
// иначе при крупном масштабе кнопки срезаются строкой состояния
float FOOT_H() { return 2.0f * ImGui::GetStyle().ItemSpacing.y + 1.0f + px(8.0f + 34.0f + 10.0f); }
float PAD()    { return px(18.0f); }

struct Wiz {
    // прошивка
    std::atomic<int> fl_state{ 0 };            // 0 — нет, 1 — идёт, 2 — готово, 3 — ошибка
    std::atomic<double> fl_prog{ 0 };
    std::atomic<bool> fl_reconnect{ false };
    std::mutex fl_mtx;
    std::vector<std::string> fl_log;
    std::string fl_err, fl_port;
    int port_sel = -1;
    // проверка электроники: 0 — нет, 1 — жду ответа -diag, 2 — линейность, 3 — готово (итог записан)
    int chk_phase = 0; double chk_t0 = 0; bool chk_ok = false;
    bool diag_ok = false; std::string diag_text;
    float pt_ref = 25.0f;
    // ноль монитора
    bool zero_wait = false; int zero_err0 = 0; std::string zero_reply; bool zero_ok = false;
    // ВВ: какой шаг запустил подбор (Hv или Hv2), N — никакой
    ds::Step hv_run = ds::Step::N; double hv_t0 = 0;
    ds::Step hv_last = ds::Step::N;          // шаг, чей подбор закончился последним (показать итог или ошибку)
    float start_v = 450.0f;
    double hv1_v = 0, hv1_peak = 0, hv2_v = 0, hv2_peak = 0;
    // эталон: 0 — нет, 1 — сбор, 2 — расчёт, 3 — загрузка, 4 — готово, 5 — ошибка.
    // Сбор идёт до tpl_target одиночных импульсов в рабочем окне амплитуд, не дольше tpl_timeout с;
    // одиночные считаются на лету по новым порциям (tpl::count_singles)
    int tpl_phase = 0, tpl_err0 = 0;
    std::atomic<int> tpl_state{ 0 };           // расчёт в потоке: 1 — считаю, 2 — готово
    tpl::Result tpl_res;
    int tpl_target = 10000; float tpl_timeout = 900.0f;
    double tpl_t0 = 0, tpl_el = 0;
    size_t tpl_counted = 0; int tpl_n = 0;     // порций просчитано, одиночных в них
    bool tpl_fin_req = false;                  // сбор останавливается (набрано или вышел предел)
    bool tpl_partial = false, tpl_few = false; // набрано меньше заданного / меньше минимума build (640)
    // разрешение: 0 — нет, 1 — набор, 3 — оценено. Набор до fw_count импульсов в окне фотопика
    // (±1.5·FWHM по живой подгонке), не дольше fw_timeout с; «Хватит, посчитать» — с 10 000 в пике
    int fw_phase = 0; double fw_t0 = 0;
    int fw_count = 100000; float fw_timeout = 1800.0f;
    uint64_t fw_n = 0; double fw_el = 0, fw_eval_t = 0; bool fw_timeout_hit = false;
    bool fw_user_stop = false;                 // нажата «Хватит, посчитать»
    peakfit::Accept fw_acc = peakfit::Accept::Reject;   // итог: принят полностью / с пометкой / нет
    peakfit::PeakWin fw_win;                   // окно и подгонка: живые в ходе набора, итоговые после
    peakfit::Result fwr;
    // график: лог/лин, запрос вида (приблизить к пику / весь спектр) — по счётчику на каждый экземпляр
    bool fw_log = false; int fw_view_req = 0; bool fw_view_full = false; int fw_view_done[2] = { 0, 0 };
    // шкала: 0 — нет, 1 — пишу, 2 — записана, 3 — ошибка
    int cal_phase = 0, cal_err0 = 0; double cal_k = 0; std::string cal_err;
    // ноль монитора снят в этой сессии (для отчёта: иначе дата из записи прибора)
    bool zero_run = false;
    // итог
    bool saved = false; std::string report_path;
    bool autohv = true; int hvboot_v = 0;      // автозапуск ВВ при подаче питания (-hvboot)
    // запись итога шага в прибор (-setup …) через очередь
    bool rec_wait = false; int rec_err0 = 0; std::string rec_err;
};
Wiz g_wiz;
const int TPL_MAX_BLOCKS = 6000;              // страховка по памяти: 6000 порций × 16 КБ ≈ 98 МБ (~30 мин сбора)

// вид импульсов при сборе сырых порций (мастер и вкладка «Эталон»): разбирается только новейшая
// порция, не чаще VIEW_DT; кольцо последних 30 нормированных одиночных
const double TPL_VIEW_DT = 0.3;
struct TplView {
    tpl::PulseRing ring{ 30 };
    std::vector<uint16_t> blk;                 // последняя разобранная порция
    std::vector<float> ok_x, ok_y, bad_x, bad_y;   // метки на осциллограмме
    double base = 0;                           // база порции, коды
    size_t seen = 0;                           // порций в g_dev.raw при последнем разборе
    double t = -1e9;
};
TplView g_tv;

// бегущая лента, конвейер из трёх порций: одна играет, следующая ждёт в буфере (уже разобрана),
// самая свежая разбирается (g_tv). Порции идут подряд, стык помечен чертой. Порция — 2.4 мс сигнала,
// между порциями прибор сигнал не пишет; лента замедлена ≈1000× и показывает часть порций.
constexpr double TAPE_WIN = 1500.0;           // ширина окна, отсчётов
constexpr double TAPE_SPEED = 300.0;          // отсчётов в секунду экрана: окно проходит за 5 с
// сегмент ленты: порция без тишины — от каждого импульса кусок −TAPE_PRE…+TAPE_POST, между кусками
// TAPE_GAP отсчётов базы; метки пересчитаны в новые координаты. Время на ленте не в масштабе.
constexpr int TAPE_PRE = 15, TAPE_POST = 45, TAPE_GAP = 8;
struct TapeSeg {
    std::vector<float> y, ok_x, ok_y, bad_x, bad_y;
    size_t seen = 0;
    double base = 0;
    bool have = false;
    double len() const { return (double)y.size(); }
    void take(const TplView& v) {
        y.clear(); ok_x.clear(); ok_y.clear(); bad_x.clear(); bad_y.clear();
        seen = v.seen; base = v.base; have = true;
        // куски вокруг всех отмеченных импульсов (принятых и отбракованных), слитые при перекрытии
        std::vector<std::pair<int, int>> cuts;
        std::vector<float> c(v.ok_x); c.insert(c.end(), v.bad_x.begin(), v.bad_x.end());
        std::sort(c.begin(), c.end());
        const int n = (int)v.blk.size();
        for (float f : c) {
            int lo = (std::max)(0, (int)f - TAPE_PRE), hi = (std::min)(n, (int)f + TAPE_POST + 1);
            if (!cuts.empty() && lo <= cuts.back().second + TAPE_GAP) cuts.back().second = (std::max)(cuts.back().second, hi);
            else cuts.push_back({ lo, hi });
        }
        auto remap = [&](const std::vector<float>& xs, const std::vector<float>& ys, std::vector<float>& ox, std::vector<float>& oy, int lo, int hi, int at) {
            for (size_t i = 0; i < xs.size(); i++)
                if (xs[i] >= lo && xs[i] < hi) { ox.push_back((float)(at + (xs[i] - lo))); oy.push_back(ys[i]); }
        };
        for (auto& ct : cuts) {
            for (int i = 0; i < TAPE_GAP; i++) y.push_back((float)base);
            int at = (int)y.size();
            for (int i = ct.first; i < ct.second; i++) y.push_back((float)v.blk[i]);
            remap(v.ok_x, v.ok_y, ok_x, ok_y, ct.first, ct.second, at);
            remap(v.bad_x, v.bad_y, bad_x, bad_y, ct.first, ct.second, at);
        }
        if (y.empty()) for (int i = 0; i < 200; i++) y.push_back((float)base);   // порция без импульсов
    }
};
struct TplTape {
    // цепочка порций подряд: [0] играет, дальше — в буфере; новая разобранная (g_tv) встаёт в хвост
    std::deque<std::pair<double, TapeSeg>> segs;   // (начало порции в координатах ленты, порция)
    double pos = 0;                           // левый край окна, координаты ленты
    double t = -1;                            // время прошлого кадра
    double ytop = 0;                          // верх оси Y: быстро растёт, медленно опускается
    size_t last_seen = 0;                     // последняя взятая порция g_tv
    double end() const { return segs.empty() ? 0 : segs.back().first + segs.back().second.len(); }
};
TplTape g_tape;

void TplViewTick(double now)
{
    TplView& v = g_tv;
    const auto& B = g_dev.raw.blocks;
    if (B.size() < v.seen) {                   // начат новый сбор (raw_start обнулил порции)
        v.ring.clear(); v.blk.clear();
        v.ok_x.clear(); v.ok_y.clear(); v.bad_x.clear(); v.bad_y.clear();
        v.seen = 0;
    }
    if (B.size() <= v.seen || now - v.t < TPL_VIEW_DT) return;
    v.seen = B.size(); v.t = now;
    const std::vector<uint16_t>& b = B.back();
    if (b.size() != (size_t)tpl::BLK) return;
    tpl::BlockView bv = tpl::view_block(b);
    v.blk = b; v.base = bv.base;
    for (const auto& p : bv.pulses) v.ring.push(p);
    v.ok_x.clear(); v.ok_y.clear(); v.bad_x.clear(); v.bad_y.clear();
    for (int i : bv.ok)  { v.ok_x.push_back((float)i);  v.ok_y.push_back((float)b[i]); }
    for (int i : bv.bad) { v.bad_x.push_back((float)i); v.bad_y.push_back((float)b[i]); }
}

// результат «Разрешения» принят (полностью или с пометкой) и пик найден
bool ResAccepted()
{
    const Wiz& w = g_wiz;
    return w.fw_phase == 3 && w.fwr.ok && w.fw_acc != peakfit::Accept::Reject;
}

// ---------- данные ----------

ds::Snapshot Snap() { return g_dev.snapshot(); }

bool Busy() { return g_dev.lin.running || g_dev.hvp.running || g_dev.raw.running || g_dev.tc.running; }

const char* StepKey(ds::Step s, const char* what)
{
    static const char* names[] = { "flash", "check", "zero", "hv", "tpl", "hv2", "res", "scale", "save" };
    static char buf[4][64];
    static int k = 0;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof buf[k], "wiz.step.%s.%s", names[(int)s], what);
    return T(buf[k]);
}

bool LinOk()
{
    const Device::LinRun& L = g_dev.lin;
    if (!L.fitted || L.running) return false;
    for (auto& p : L.pts) if (!p.ok || p.n < (uint64_t)(0.99 * L.n)) return false;
    return L.r2 > 0.9999 && L.dev < 0.2;
}

// записать итог шага в прибор; после ответа WizTick перечитает -setup
void Record(ds::Step s, double a = 0, double b = 0)
{
    std::string c = ds::setup_cmd(s, (uint32_t)time(nullptr), a, b);
    if (c.empty()) return;
    Wiz& w = g_wiz;
    if (!w.rec_wait) w.rec_err0 = g_dev.queue_errors;
    w.rec_err.clear();
    g_dev.queue({ c });
    w.rec_wait = true;
}

std::string Fmt(const char* fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return b;
}

std::string MinSec(double s)
{
    if (s < 0) s = 0;
    int t = (int)s;
    char b[16];
    snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

// «≈ 3 мин» / «≈ 40 с»; пусто — оценки нет
std::string Eta(double s)
{
    if (s < 0) return "";
    if (s < 90) return Fmt(T("wiz.eta.sec"), (int)ceil(s));
    return Fmt(T("wiz.eta.min"), (int)ceil(s / 60.0));
}

// ---------- виджеты ----------

bool AccentButton(const char* label, const ImVec2& sz)
{
    ImGui::PushStyleColor(ImGuiCol_Button,        c_accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c_accent_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  c_accent_active);
    ImGui::PushStyleColor(ImGuiCol_Text,          c_on_accent);
    bool r = ImGui::Button(label, sz);
    ImGui::PopStyleColor(4);
    return r;
}

// действие шага: акцентное, пока шаг не пройден (одна главная кнопка на экран)
bool ActionButton(const char* label, bool primary)
{
    ImVec2 sz(ImGui::CalcTextSize(label).x + px(44.0f), px(36.0f));
    return primary ? AccentButton(label, sz) : ImGui::Button(label, sz);
}

void Para(ImVec4 col, const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void Small(ImVec4 col, const char* text)
{
    ImGui::PushFont(font_small);
    Para(col, text);
    ImGui::PopFont();
}

void CheckIcon(ImDrawList* dl, ImVec2 p, float s, ImU32 col)
{
    ImVec2 pts[3] = { ImVec2(p.x + s * 0.12f, p.y + s * 0.53f), ImVec2(p.x + s * 0.38f, p.y + s * 0.78f),
                      ImVec2(p.x + s * 0.88f, p.y + s * 0.22f) };
    dl->AddPolyline(pts, 3, col, 0, px(2.0f));
}

// строка итога с признаком: галочка — норма, «!» — нет (не только цвет). Галочка рисуется:
// в шрифте интерфейса нет «✓».
void PassLine(bool ok, const std::string& text)
{
    float lh = ImGui::GetTextLineHeight();
    if (ok) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        CheckIcon(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + (lh - px(13)) * 0.5f), px(13), U32(c_ok));
        ImGui::Dummy(ImVec2(px(14), lh));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, c_warn);
        ImGui::TextUnformatted("!");
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::TextWrapped("%s", text.c_str());
}

// блок с рамкой; edge_warn — блок «От вас» (.you: рамка и фон warn)
bool BeginBox(const char* id, bool you)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, you ? c_warn_bg : c_child);
    ImGui::PushStyleColor(ImGuiCol_Border,  you ? c_warn : c_border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16), px(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 2.0f);
    bool r = ImGui::BeginChild(id, ImVec2(-1, 0),
                               ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    return r;
}
void EndBox() { ImGui::EndChild(); }

// три крупных показания (.big3)
void Big3(const char* l1, const std::string& v1, const char* l2, const std::string& v2, const char* l3, const std::string& v3)
{
    if (!ImGui::BeginTable("##big3", 3, ImGuiTableFlags_SizingStretchSame)) return;
    const char* ls[3] = { l1, l2, l3 };
    const std::string* vs[3] = { &v1, &v2, &v3 };
    ImGui::TableNextRow();
    for (int i = 0; i < 3; i++) {
        ImGui::TableNextColumn();
        ImGui::PushFont(font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
        ImGui::TextUnformatted(ls[i]);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PushFont(font_title);
        ImGui::PushStyleColor(ImGuiCol_Text, c_text_strong);
        ImGui::TextUnformatted(vs[i]->c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::EndTable();
}

// ---------- отчёт ----------

// строка отчёта о шаге, который в этой сессии не проводился: дата из записи прибора
std::string NotRun(uint32_t last_unix)
{
    bool known = g_dev.setup_known == ds::SetupKnown::Yes;
    return ds::report_not_run(known ? last_unix : 0);
}

// UTF-8 → UTF-16 и обратно: имя отчёта с кириллицей. Узкий fopen берёт кодовую страницу ANSI
// и записывал байты UTF-8 как cp1251 («gammapult_РІРІРѕРґ_…» прямо в имени файла).
std::wstring Wide(const std::string& u)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, &w[0], n);
    return w;
}

std::string Utf8(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &u[0], n, nullptr, nullptr);
    return u;
}

// Отчёт пишется по-русски (документ, не интерфейс). Шаги, пройденные в этой сессии, идут с числами
// сессии; остальные — «не проводилось в этот раз (последний раз: …)» по записи прибора (-setup).
void WriteReport()
{
    Wiz& w = g_wiz;
    const ds::SetupRec& R = g_dev.setup;
    bool rec = g_dev.setup_known == ds::SetupKnown::Yes;
    time_t t = time(nullptr); struct tm lt; localtime_s(&lt, &t);
    char ts[64], d8[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", &lt);
    strftime(d8, sizeof d8, "%Y%m%d_%H%M", &lt);
    std::string sn = g_dev.serial.empty() ? std::string("noserial") : ds::display_serial(g_dev.serial);
    std::string fn = "gammapult_ввод_" + sn + "_" + d8 + ".txt";
    std::wstring wfn = Wide(fn);
    FILE* f = _wfopen(wfn.c_str(), L"w");
    if (!f) { w.report_path.clear(); return; }
    const Device::LinRun& L = g_dev.lin;
    fprintf(f, "Ввод в строй гамма-спектрометра — %s\n", ts);
    fprintf(f, "Серийник: %s\nПрошивка: %s\n\n", sn.c_str(), g_dev.fw_info.c_str());

    // проверка электроники
    if (w.chk_phase == 3) {
        fprintf(f, "Проверка электроники: %s; -diag %s\n", w.chk_ok ? "прошла" : "НЕ ПРОШЛА", w.diag_ok ? "ok" : "НЕ ОК");
        if (L.fitted) fprintf(f, "  Самотест ЦАП: канал = %.4f·код %+.2f, R² %.6f, отклонение ≤ %.3f %%\n", L.k, L.b, L.r2, L.dev);
        else fprintf(f, "  Самотест ЦАП: не выполнен%s%s\n", L.msg.empty() ? "" : ": ", L.msg.c_str());
    } else {
        std::string nr = NotRun(R.chk_unix);
        if (rec && R.chk_unix) nr += R.chk_ok ? ", прошла" : ", НЕ ПРОШЛА";
        fprintf(f, "Проверка электроники: %s\n", nr.c_str());
    }
    fprintf(f, "  Сейчас: опора %.1f мВ; шум базы σ %.2f кода; Pt1000 %s %.2f °C\n",
            g_dev.gnum("vref"), g_dev.gnum("sigma"), g_dev.gstr("pt").c_str(), g_dev.gnum("t"));

    // ноль монитора
    if (w.zero_run) fprintf(f, "Ноль монитора ВВ: снят сейчас\n");
    else fprintf(f, "Ноль монитора ВВ: %s\n", NotRun(R.zero_unix).c_str());
    fprintf(f, "  Монитор ВВ: ноль %.1f мВ, k %.4f мВ/В\n", g_dev.gnum("hv_zero"), g_dev.gnum("hv_k"));

    // подбор ВВ: в записи прибора один (последний) подбор, общий для шагов 4 и 6
    auto hv_line = [&](const char* name, double v, double pk) {
        if (v > 0) { fprintf(f, "%s: %.0f В, пик %.1f\n", name, v, pk); return; }
        std::string nr = NotRun(R.hv_unix);
        if (rec && R.hv_unix && R.hv_v > 0) {
            char b[96]; snprintf(b, sizeof b, "; в приборе: %.0f В, пик %.1f", R.hv_v, R.hv_peak_ch); nr += b;
        }
        fprintf(f, "%s: %s\n", name, nr.c_str());
    };
    hv_line("ВВ по пику (до эталона)", w.hv1_v, w.hv1_peak);

    // эталон формы
    if (w.tpl_phase == 4) {
        fprintf(f, "Эталон формы: записан сейчас; %s", w.tpl_res.msg.c_str());
        if (w.tpl_partial) fprintf(f, " (одиночных %d из заданных %d: вышел предел времени)", w.tpl_n, w.tpl_target);
        fprintf(f, "\n");
    }
    else {
        std::string nr = NotRun(R.tpl_unix);
        if (rec && R.tpl) nr += "; эталон в приборе есть";
        fprintf(f, "Эталон формы: %s\n", nr.c_str());
    }
    hv_line("ВВ по пику (рабочее)", w.hv2_v, w.hv2_peak);

    // разрешение
    if (ResAccepted()) {
        fprintf(f, "Разрешение Cs-137 (%llu в пике, %.0f с): пик %.2f, FWHM %.2f ± %.2f %%",
                (unsigned long long)w.fw_n, w.fw_el, w.fwr.peak, w.fwr.fwhm_pct, w.fwr.fwhm_err);
        if (w.fw_acc == peakfit::Accept::Partial)
            fprintf(f, " (набрано меньше заданных %d: %s)", w.fw_count, w.fw_user_stop ? "остановлено вручную" : "вышел предел времени");
        fprintf(f, "\n");
    } else {
        std::string nr = NotRun(R.res_unix);
        if (rec && R.res_unix && R.res_fwhm > 0) {
            char b[96]; snprintf(b, sizeof b, "; в приборе: FWHM %.2f %%, пик %.1f", R.res_fwhm, R.res_peak_ch); nr += b;
        }
        if (w.fw_phase == 3 && !ResAccepted()) {
            char b[160]; snprintf(b, sizeof b, "; набор сейчас прерван: в пике %llu (меньше 10 000) за %.0f с",
                                  (unsigned long long)w.fw_n, w.fw_el); nr += b;
        }
        fprintf(f, "Разрешение Cs-137: %s\n", nr.c_str());
    }

    // шкала
    if (w.cal_phase == 2) fprintf(f, "Энергокалибровка (базовая, по Cs-137): E = %.6f · канал, записана сейчас\n", w.cal_k);
    else if (g_dev.becq_ok) fprintf(f, "Энергокалибровка: не записывалась в этот раз; в приборе E = %.6f · канал %+.3f\n", g_dev.becq_c[1], g_dev.becq_c[0]);
    else fprintf(f, "Энергокалибровка: не проводилось в этот раз (в приборе шкалы нет)\n");

    if (w.hvboot_v > 0) fprintf(f, "Автозапуск ВВ при подаче питания: %d В\n", w.hvboot_v);
    else fprintf(f, "Автозапуск ВВ: выключен (ВВ только командой)\n");
    fprintf(f, "Порог: %s; термокомпенсация: %s\n", g_dev.thr_codes == 0 ? "авто 5σ" : std::to_string(g_dev.thr_codes).c_str(),
            g_dev.tc_reply.empty() ? "не настроена" : g_dev.tc_reply.c_str());
    fclose(f);
    wchar_t full[MAX_PATH];
    w.report_path = _wfullpath(full, wfn.c_str(), MAX_PATH) ? Utf8(full) : fn;
}

// ---------- действия ----------

void FlashStart(const std::string& port)
{
    Wiz& w = g_wiz;
    std::vector<uint8_t> img; std::string tag;
    if (!fwflash::embedded(img, tag)) { w.fl_err = T("wiz.flash.no_image"); w.fl_state = 3; return; }
    g_dev.disconnect();
    w.fl_state = 1; w.fl_prog = 0; w.fl_err.clear(); w.fl_port = port;
    { std::lock_guard<std::mutex> lk(w.fl_mtx); w.fl_log.clear(); w.fl_log.push_back(Fmt(T("wiz.flash.image"), tag.c_str(), (int)img.size())); }
    std::thread([img, port]() {
        Wiz& w = g_wiz;
        std::string err;
        bool ok = fwflash::flash(port, img, true,
            [](const std::string& m) { std::lock_guard<std::mutex> lk(g_wiz.fl_mtx); g_wiz.fl_log.push_back(m); },
            [](double f) { g_wiz.fl_prog = f; }, err);
        { std::lock_guard<std::mutex> lk(w.fl_mtx); w.fl_err = err; }
        w.fl_state = ok ? 2 : 3;
        w.fl_reconnect = true;
    }).detach();
}

void CheckStart()
{
    Wiz& w = g_wiz;
    w.chk_phase = 1; w.chk_t0 = ImGui::GetTime(); w.chk_ok = false;
    w.diag_ok = false; w.diag_text.clear();
    g_dev.last_reply.clear();
    g_dev.send_cmd("-diag");
}

void HvStart(ds::Step s)
{
    Wiz& w = g_wiz;
    bool second = s == ds::Step::Hv2;
    double v0 = second ? std::fabs(g_dev.gnum("hv_set")) : (double)w.start_v;
    if (second && v0 < 300) v0 = w.hv1_v > 0 ? w.hv1_v : w.start_v;
    w.hv_run = s; w.hv_t0 = ImGui::GetTime();
    g_dev.hvp_start(v0);
}

// процесс шага идёт (для «Остановить»)
bool StepRunning(ds::Step s)
{
    const Wiz& w = g_wiz;
    switch (s) {
    case ds::Step::Check: return w.chk_phase == 1 || w.chk_phase == 2;
    case ds::Step::Hv: case ds::Step::Hv2: return g_dev.hvp.running && w.hv_run == s;
    case ds::Step::Tpl:   return w.tpl_phase == 1;
    case ds::Step::Res:   return w.fw_phase == 1;
    default:              return false;
    }
}

void StepStop(ds::Step s)
{
    Wiz& w = g_wiz;
    switch (s) {
    case ds::Step::Check: g_dev.lin_abort(); w.chk_phase = 0; break;   // прервано — итог не пишется
    case ds::Step::Hv: case ds::Step::Hv2: g_dev.hvp_abort(); w.hv_run = ds::Step::N; break;
    case ds::Step::Tpl:   g_dev.raw_abort(); w.tpl_phase = 0; break;
    case ds::Step::Res:   g_dev.stop(); w.fw_phase = 0; w.fw_win = peakfit::PeakWin(); break;
    default: break;
    }
}

// ---------- тела шагов ----------

void BodyFlash(bool primary)
{
    Wiz& w = g_wiz;
    bool conn = g_dev.connected();
    const std::vector<std::string>& ports = g_dev.ports;
    if (!g_ui.flash_port.empty()) {             // R1: порт выбран на главном экране («Прибор не отвечает? Прошить»)
        for (int i = 0; i < (int)ports.size(); i++) if (ports[i] == g_ui.flash_port) w.port_sel = i;
        g_ui.flash_port.clear();
    }
    if (conn) for (int i = 0; i < (int)ports.size(); i++) if (ports[i] == g_dev.port_name) w.port_sel = i;
    if (w.port_sel < 0 || w.port_sel >= (int)ports.size()) w.port_sel = ports.empty() ? -1 : (int)ports.size() - 1;

    std::vector<uint8_t> img; std::string tag;
    bool emb = fwflash::embedded(img, tag);
    if (conn && g_dev.fw_info.rfind("GammaSpec", 0) == 0) {
        PassLine(true, Fmt(T("wiz.flash.answers"), g_dev.fw_info.c_str()));
        Small(c_text_dim, Fmt(T("wiz.flash.update"), emb ? tag.c_str() : T("wiz.flash.no_image")).c_str());
        ImGui::Spacing();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("wiz.flash.port"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(160));
    ImGui::BeginDisabled(conn || w.fl_state == 1);
    if (ImGui::BeginCombo("##wport", w.port_sel < 0 ? T("conn.no_ports") : ports[w.port_sel].c_str())) {
        for (int i = 0; i < (int)ports.size(); i++)
            if (ImGui::Selectable(ports[i].c_str(), i == w.port_sel)) w.port_sel = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("conn.refresh"))) { g_dev.refresh_ports(); w.port_sel = -1; }
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::BeginDisabled(w.fl_state == 1 || !emb || w.port_sel < 0 || Busy());
    std::string b = Fmt(T("wiz.flash.btn"), emb ? tag.c_str() : T("wiz.flash.no_image"));
    if (ActionButton(b.c_str(), primary)) FlashStart(ports[w.port_sel]);
    ImGui::EndDisabled();
    if (!conn && w.fl_state != 1 && w.port_sel >= 0) {
        ImGui::SameLine();
        if (ImGui::Button(T("wiz.flash.connect"), ImVec2(0, px(36)))) g_dev.connect(ports[w.port_sel]);
    }
    if (w.fl_state == 1) ImGui::ProgressBar((float)w.fl_prog, ImVec2(-1, 0));
    {
        std::lock_guard<std::mutex> lk(w.fl_mtx);
        ImGui::PushFont(font_small);
        for (auto& m : w.fl_log) Para(c_text_dim, m.c_str());
        ImGui::PopFont();
    }
    if (w.fl_state == 2) {
        PassLine(true, T("wiz.flash.ok"));
        Para(c_warn, T("wiz.flash.boot0"));
    }
    if (w.fl_state == 3) {
        std::lock_guard<std::mutex> lk(w.fl_mtx);
        Para(c_danger, Fmt(T("wiz.flash.err"), w.fl_err.c_str()).c_str());
    }
}

void BodyCheck(bool primary)
{
    Wiz& w = g_wiz;
    bool conn = g_dev.connected();
    ImGui::BeginDisabled(!conn || Busy() || w.chk_phase == 1 || w.chk_phase == 2);
    if (ActionButton(T("wiz.check.run"), primary)) CheckStart();
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (w.chk_phase == 1) Small(c_text_dim, T("wiz.check.wait_diag"));
    if (!w.diag_text.empty() || w.chk_phase >= 2) PassLine(w.diag_ok, T("wiz.check.diag"));
    PassLine(g_dev.gnum("vref_ok", 0) > 0.5, Fmt(T("wiz.check.vref"), g_dev.gnum("vref")));
    double sg = g_dev.gnum("sigma");
    PassLine(sg > 0.5 && sg < 3.5, Fmt(T("wiz.check.noise"), sg));
    bool pok = g_dev.gstr("pt", "-") == "ok";
    PassLine(pok, pok ? Fmt(T("wiz.check.pt"), g_dev.gnum("t")) : std::string(T("wiz.check.pt_bad")));
    const Device::LinRun& L = g_dev.lin;
    if (w.chk_phase >= 2) {
        if (L.running) {
            int n = (int)L.codes.size(), i = (int)L.pts.size() + 1;
            ImGui::TextUnformatted(Fmt(T("wiz.check.lin_step"), i > n ? n : i, n).c_str());
            ImGui::ProgressBar(n ? (float)L.pts.size() / n : 0.0f, ImVec2(-1, 0), "");
        } else if (L.fitted) {
            PassLine(LinOk(), Fmt(T("wiz.check.lin"), L.r2, L.dev));
        } else if (!L.msg.empty()) {
            PassLine(false, L.msg);
        }
    }
    if (w.chk_phase == 3 && !w.chk_ok) Para(c_warn, T("wiz.check.failed"));
}

void BodyZero(bool primary)
{
    Wiz& w = g_wiz;
    bool conn = g_dev.connected();
    ImGui::TextUnformatted(Fmt(T("wiz.zero.now"), g_dev.gnum("hv_zero"), g_dev.gnum("mon_mv")).c_str());
    if (g_dev.hv_on()) Para(c_warn, T("wiz.zero.hv_on"));
    ImGui::Spacing();
    ImGui::BeginDisabled(!conn || g_dev.hv_on() || w.zero_wait || g_dev.queue_busy());
    if (ActionButton(T("wiz.zero.btn"), primary)) {
        w.zero_err0 = g_dev.queue_errors; w.zero_reply.clear(); w.zero_ok = false;
        g_dev.queue({ "-hvzero" });
        w.zero_wait = true;
    }
    ImGui::EndDisabled();
    if (!w.zero_reply.empty()) {
        if (w.zero_ok) PassLine(true, Fmt(T("wiz.reply"), w.zero_reply.c_str()));
        else Para(c_warn, Fmt(T("wiz.reply"), w.zero_reply.c_str()).c_str());
    }
}

void BodyHv(ds::Step s, bool primary)
{
    Wiz& w = g_wiz;
    bool conn = g_dev.connected();
    bool second = s == ds::Step::Hv2;
    Device::HvPeak& h = g_dev.hvp;
    bool mine = w.hv_run == s;
    bool last = w.hv_last == s;              // подбор этого шага уже закончился — итог или ошибка
    if (!second) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T("wiz.hv.start_v"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(px(110));
        ImGui::BeginDisabled(h.running);
        ImGui::InputFloat("##wstartv", &w.start_v, 10, 50, "%.0f");
        ImGui::EndDisabled();
        if (w.start_v < 300) w.start_v = 300;
        if (w.start_v > 1100) w.start_v = 1100;
    }
    ImGui::BeginDisabled(!conn || Busy());
    if (ActionButton(T(second ? "wiz.hv2.btn" : "wiz.hv.btn"), primary)) HvStart(s);
    ImGui::EndDisabled();

    double res_v = second ? w.hv2_v : w.hv1_v, res_pk = second ? w.hv2_peak : w.hv1_peak;
    if (mine && h.running) {
        ImGui::Spacing();
        if (BeginBox("##hvlive", false)) {
            std::string pass = Fmt(T("wiz.pass"), h.iter + 1, h.max_iter);
            std::string el = Fmt(T("wiz.elapsed"), MinSec(ImGui::GetTime() - w.hv_t0).c_str());
            ImGui::TextUnformatted(pass.c_str());
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(el.c_str()).x);
            ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
            ImGui::TextUnformatted(el.c_str());
            ImGui::PopStyleColor();
            // прогресс прохода: импульсов в фотопике из N
            char ov[96] = "";
            if (h.phase == 2) {
                if (h.win_ok) snprintf(ov, sizeof ov, T("wiz.in_peak"), (unsigned long long)h.n_in, h.count);
                else snprintf(ov, sizeof ov, "%s", T("wiz.res.seeking"));
            }
            ImGui::ProgressBar(h.phase == 2 && h.count > 0 ? (float)(h.n_in / h.count) : 0.0f, ImVec2(-1, 0), ov);
            if (h.phase == 2) Small(c_text_dim, Fmt(T("wiz.hv.live"), (unsigned)g_dev.cps, MinSec(h.pass_s).c_str()).c_str());
            ImGui::Spacing();
            double pk = h.hist.empty() ? 0 : h.hist.back().second;
            Big3(T("wiz.step.hv.voltage"), Fmt(T("wiz.hv.volts"), std::fabs(g_dev.gnum("hv_out"))),
                 T("wiz.step.hv.peak"), pk > 0 ? Fmt("%.0f", pk) : std::string("…"),
                 T("wiz.step.hv.target"), Fmt("%.0f", h.target));
            ImGui::PushFont(font_small);
            Para(c_text_dim, h.msg.c_str());
            ImGui::PopFont();
        }
        EndBox();
    } else if (mine || last || res_v > 0) {
        if (last && !h.done && !h.msg.empty()) Para(c_warn, ("! " + h.msg).c_str());
        else if (res_v > 0) PassLine(true, Fmt(T("wiz.hv.result"), res_v, res_pk));
        else if (!h.msg.empty()) Para(c_warn, ("! " + h.msg).c_str());
    }
    if (mine || last || res_v > 0) {
        ImGui::PushFont(font_small);
        for (auto& p : h.hist) Para(c_text_dim, Fmt(T("wiz.hv.history"), p.first, p.second).c_str());
        ImGui::PopFont();
    }
}

void BodyTpl(bool primary)
{
    Wiz& w = g_wiz;
    bool conn = g_dev.connected();
    Device::RawRun& R = g_dev.raw;
    ImGui::BeginDisabled(!conn || Busy() || w.tpl_phase == 2 || w.tpl_phase == 3);
    if (ActionButton(T("wiz.tpl.btn"), primary)) {
        w.tpl_phase = 1; w.tpl_state = 0;
        w.tpl_t0 = ImGui::GetTime(); w.tpl_el = 0;
        w.tpl_counted = 0; w.tpl_n = 0;
        w.tpl_fin_req = w.tpl_partial = w.tpl_few = false;
        g_dev.raw_start(TPL_MAX_BLOCKS);
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (w.tpl_phase == 1) {
        float fr = w.tpl_target > 0 ? (float)w.tpl_n / w.tpl_target : 0.0f;
        ImGui::ProgressBar(fr > 1 ? 1.0f : fr, ImVec2(-1, 0), Fmt(T("wiz.tpl.collect"), w.tpl_n, w.tpl_target).c_str());
        Small(c_text_dim, Fmt(T("wiz.tpl.live"), (int)R.blocks.size(), MinSec(w.tpl_el).c_str(), MinSec(w.tpl_timeout).c_str()).c_str());
        if (w.tpl_el >= 10.0 && w.tpl_n > 0 && !w.tpl_fin_req) {
            double rate = w.tpl_n / w.tpl_el;
            double eta = peakfit::eta_s(rate, (uint64_t)w.tpl_n, (uint64_t)w.tpl_target);
            Small(c_text_dim, Fmt(T("wiz.tpl.eta"), w.tpl_target, Eta(eta).c_str()).c_str());
            double at_limit = rate * w.tpl_timeout;
            if (at_limit < tpl::NPH * 10)
                Small(c_warn, Fmt(T("wiz.tpl.eta_few"), MinSec(w.tpl_timeout).c_str(), at_limit).c_str());
            else if (eta > w.tpl_timeout - w.tpl_el)
                Small(c_text_dim, Fmt(T("wiz.tpl.eta_over"), MinSec(w.tpl_timeout).c_str(), at_limit).c_str());
        }
    }
    if (w.tpl_phase == 2) ImGui::TextUnformatted(T("wiz.tpl.calc"));
    if (w.tpl_phase == 3) ImGui::TextUnformatted(T("wiz.tpl.load"));
    if (w.tpl_phase == 4) {
        PassLine(true, T("wiz.tpl.ok"));
        if (w.tpl_partial) Small(c_warn, Fmt(T("wiz.tpl.partial"), w.tpl_n, w.tpl_target).c_str());
        PassLine(w.tpl_res.resid_med < 1.0 && w.tpl_res.spread_after < 1.0,
                 Fmt(T("wiz.tpl.quality"), w.tpl_res.resid_med, w.tpl_res.spread_after));
    }
    if (w.tpl_phase >= 3 && w.tpl_res.ok) Small(c_text_dim, w.tpl_res.msg.c_str());
    if (w.tpl_phase == 5) {
        if (w.tpl_few) Para(c_danger, Fmt(T("wiz.tpl.few"), w.tpl_n, MinSec(w.tpl_el).c_str()).c_str());
        else Para(c_danger, Fmt(T("wiz.tpl.failed"), (R.msg + " " + w.tpl_res.msg).c_str()).c_str());
    }
    ImGui::Spacing();
    TplPulsePlots(w.tpl_phase >= 3 && w.tpl_res.ok ? &w.tpl_res : nullptr);
}

// экземпляр тела шага: false — мастер, true — расширенный режим (у графика свой запрос вида)
bool g_body_embedded = false;

// живой спектр шага «Разрешение»: полоса окна пика, поверх подогнанный колокол с линейным фоном
void ResPlot()
{
    Wiz& w = g_wiz;
    const peakfit::PeakWin& pw = w.fw_win;
    const peakfit::Result& fr = (w.fw_phase == 3 && w.fwr.ok) ? w.fwr : pw.fit;
    bool have_fit = fr.ok && (pw.ok || w.fw_phase == 3);
    double pk = have_fit ? fr.peak : 0;

    // переключатели над графиком
    auto seg = [&](const char* lbl, bool active) {
        if (active) { ImGui::PushStyleColor(ImGuiCol_Button, c_accent); ImGui::PushStyleColor(ImGuiCol_Text, c_on_accent); }
        bool r = ImGui::Button(lbl, ImVec2(px(48), 0));
        if (active) ImGui::PopStyleColor(2);
        return r;
    };
    if (seg(T("wiz.plot.lin"), !w.fw_log)) w.fw_log = false;
    ImGui::SameLine(0, px(2));
    if (seg(T("wiz.plot.log"), w.fw_log)) w.fw_log = true;
    ImGui::SameLine(0, px(16));
    ImGui::BeginDisabled(pk <= 0);
    if (ImGui::Button(T("wiz.plot.to_peak"))) { w.fw_view_full = false; w.fw_view_req++; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("wiz.plot.full"))) { w.fw_view_full = true; w.fw_view_req++; }

    int inst = g_body_embedded ? 1 : 0;
    bool apply = w.fw_view_done[inst] != w.fw_view_req;
    w.fw_view_done[inst] = w.fw_view_req;

    ImPlot::GetInputMap().Pan = ImGuiMouseButton_Left;
    if (!ImPlot::BeginPlot("##resplot", ImVec2(-1, px(280)), ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) return;
    ImPlot::SetupAxis(ImAxis_X1, T("wiz.plot.channel"));
    ImPlot::SetupAxis(ImAxis_Y1, T("wiz.plot.counts"), ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit);
    if (w.fw_log) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
    ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, (double)Device::CHANNELS);
    ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, w.fw_log ? 0.5 : 0.0, 1.0e12);
    if (apply) {
        if (w.fw_view_full || pk <= 0) ImPlot::SetupAxisLimits(ImAxis_X1, 0, Device::CHANNELS, ImPlotCond_Always);
        else ImPlot::SetupAxisLimits(ImAxis_X1, 0.5 * pk, 1.6 * pk, ImPlotCond_Always);
    } else if (pk > 0) ImPlot::SetupAxisLimits(ImAxis_X1, 0.5 * pk, 1.6 * pk, ImPlotCond_Once);
    else ImPlot::SetupAxisLimits(ImAxis_X1, 0, Device::CHANNELS, ImPlotCond_Once);
    ImPlot::SetupLegend(ImPlotLocation_NorthEast);

    // полоса окна пика на всю высоту области графика
    if (pw.ok && pw.hi > pw.lo) {
        ImPlotRect lim = ImPlot::GetPlotLimits();
        ImVec2 a = ImPlot::PlotToPixels(pw.lo, lim.Y.Max), b = ImPlot::PlotToPixels(pw.hi, lim.Y.Min);
        ImVec4 band = c_accent; band.w = 0.16f;
        ImPlot::PushPlotClipRect();
        ImPlot::GetPlotDrawList()->AddRectFilled(a, b, U32(band));
        ImPlot::PopPlotClipRect();
        double xs[2] = { (double)pw.lo, (double)pw.hi };
        ImPlot::PushStyleColor(ImPlotCol_Line, c_accent_line);
        ImPlot::PlotInfLines(T("wiz.plot.window"), xs, 2);
        ImPlot::PopStyleColor();
    }
    ImPlot::PushStyleColor(ImPlotCol_Line, c_trace);
    ImPlot::PlotStairs(T("wiz.plot.spectrum"), g_dev.spectrum, Device::CHANNELS);
    ImPlot::PopStyleColor();
    if (have_fit && fr.sigma > 0) {
        // колокол с линейным фоном на ±4σ вокруг пика
        static double gx[241], gy[241];
        double x0 = fr.peak - 4 * fr.sigma, dx = 8 * fr.sigma / 240;
        for (int i = 0; i <= 240; i++) {
            gx[i] = x0 + i * dx;
            double y = fr.model(gx[i]);
            gy[i] = (w.fw_log && y < 0.5) ? 0.5 : y;
        }
        ImPlot::SetNextLineStyle(c_ok, 2.0f);
        ImPlot::PlotLine(T("wiz.plot.fit"), gx, gy, 241);

        // разметка FWHM: отрезок на половине высоты колокола (над линейным фоном) с засечками,
        // пунктир от вершины вниз до фона, подпись над отрезком
        double hw = fr.sigma * 2.3548200 / 2.0;
        double xl = fr.peak - hw, xr = fr.peak + hw;
        double bgp = fr.bg0 + fr.bg1 * (fr.peak - fr.x0);
        double yl = fr.model(xl), yr = fr.model(xr), yt = fr.model(fr.peak);
        if (!w.fw_log || (yl > 0.5 && yr > 0.5 && bgp > 0.5)) {
            ImPlot::PushPlotClipRect();
            ImDrawList* dl = ImPlot::GetPlotDrawList();
            ImU32 col = U32(c_warn);
            float th = px(2.0f), tick = px(6.0f);
            ImVec2 pl = ImPlot::PlotToPixels(xl, yl), pr = ImPlot::PlotToPixels(xr, yr);
            dl->AddLine(pl, pr, col, th);
            dl->AddLine(ImVec2(pl.x, pl.y - tick), ImVec2(pl.x, pl.y + tick), col, th);
            dl->AddLine(ImVec2(pr.x, pr.y - tick), ImVec2(pr.x, pr.y + tick), col, th);
            ImVec2 pt = ImPlot::PlotToPixels(fr.peak, yt), pb = ImPlot::PlotToPixels(fr.peak, bgp);
            for (float y = pt.y; y < pb.y; y += px(8.0f))       // пунктир по центру пика
                dl->AddLine(ImVec2(pt.x, y), ImVec2(pt.x, (std::min)(y + px(4.0f), pb.y)), col, px(1.0f));
            ImPlot::PopPlotClipRect();
            ImPlot::PushStyleColor(ImPlotCol_InlayText, c_warn);
            ImPlot::PlotText(Fmt(T("wiz.plot.fwhm"), fr.fwhm_pct).c_str(), fr.peak, 0.5 * (yl + yr),
                             ImVec2(0, -px(14.0f)));
            ImPlot::PopStyleColor();
        }
    }
    ImPlot::EndPlot();
}

void BodyRes(bool primary)
{
    Wiz& w = g_wiz;
    bool conn = g_dev.connected();
    bool run = w.fw_phase == 1;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("wiz.res.count"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(140));
    ImGui::BeginDisabled(run);
    ImGui::InputInt("##wcount", &w.fw_count, 1000, 5000);
    ImGui::EndDisabled();
    if (w.fw_count < 1000) w.fw_count = 1000;
    if (w.fw_count > 10000000) w.fw_count = 10000000;
    ImGui::BeginDisabled(!conn || Busy() || run);
    if (ActionButton(T("wiz.res.btn"), primary)) {
        g_dev.stop(); g_dev.reset_spectrum(); g_dev.start();
        w.fw_phase = 1; w.fw_t0 = ImGui::GetTime(); w.fw_eval_t = w.fw_t0;
        w.fw_n = 0; w.fw_el = 0; w.fw_timeout_hit = false; w.fw_user_stop = false;
        w.fw_acc = peakfit::Accept::Reject;
        w.fw_win = peakfit::PeakWin(); w.fwr = peakfit::Result();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (run) {
        char ov[96];
        if (w.fw_win.ok) snprintf(ov, sizeof ov, T("wiz.in_peak"), (unsigned long long)w.fw_n, (double)w.fw_count);
        else snprintf(ov, sizeof ov, "%s", T("wiz.res.seeking"));
        float fr = w.fw_count > 0 ? (float)((double)w.fw_n / w.fw_count) : 0.0f;
        ImGui::ProgressBar(fr > 1 ? 1.0f : fr, ImVec2(-1, 0), ov);
        Small(c_text_dim, Fmt(T("wiz.res.live"), (unsigned)g_dev.cps, MinSec(w.fw_el).c_str(), MinSec(w.fw_timeout).c_str()).c_str());
        // скорость в пике и сколько ждать: когда пик найден и прошло 10 с
        if (w.fw_win.ok && w.fw_el >= 10.0 && w.fw_n > 0) {
            double rate = w.fw_n / w.fw_el;
            double eta = peakfit::eta_s(rate, w.fw_n, (uint64_t)w.fw_count);
            Small(c_text_dim, Fmt(T("wiz.res.eta"), w.fw_count, Eta(eta).c_str()).c_str());
            double at_limit = rate * w.fw_timeout;
            if (at_limit < (double)peakfit::RES_MIN)
                Small(c_warn, Fmt(T("wiz.res.eta_few"), MinSec(w.fw_timeout).c_str(), at_limit).c_str());
            else if (eta > w.fw_timeout - w.fw_el)
                Small(c_text_dim, Fmt(T("wiz.res.eta_over"), MinSec(w.fw_timeout).c_str(), at_limit).c_str());
        }
        // второстепенная: остановить набор и принять то, что есть (не меньше 10 000 в пике)
        ImGui::Spacing();
        bool can = w.fw_win.ok && w.fw_n >= peakfit::RES_MIN;
        ImGui::BeginDisabled(!can);
        if (ImGui::Button(T("wiz.res.enough"))) w.fw_user_stop = true;
        ImGui::EndDisabled();
        if (!can) {
            ImGui::SameLine();
            ImGui::PushFont(font_small); ImGui::AlignTextToFramePadding();
            ImGui::TextColored(c_text_dim, "%s", T("wiz.res.enough_hint"));
            ImGui::PopFont();
        }
    }
    if (w.fw_phase == 3) {
        if (w.fw_acc == peakfit::Accept::Reject && (w.fw_timeout_hit || w.fw_user_stop))
            Para(c_warn, Fmt(T("wiz.res.timeout"), (unsigned long long)w.fw_n, MinSec(w.fw_el).c_str()).c_str());
        if (w.fwr.ok) {
            bool acc = w.fw_acc != peakfit::Accept::Reject;
            PassLine(acc && w.fwr.fwhm_pct <= 9.0, Fmt(T("wiz.res.result"), w.fwr.fwhm_pct, w.fwr.fwhm_err, w.fwr.peak));
            if (acc) {
                if (w.fw_acc == peakfit::Accept::Partial)
                    Small(c_warn, Fmt(T("wiz.res.partial"), (unsigned long long)w.fw_n, w.fw_count, w.fwr.fwhm_err).c_str());
                Small(c_text_dim, Fmt(T("wiz.res.typical")).c_str());   // «%%» в строке
                if (w.fwr.fwhm_pct > 9.0) Small(c_warn, T("wiz.res.wide"));
            }
            Small(c_text_dim, Fmt(T("wiz.in_peak"), (unsigned long long)w.fw_n, (double)w.fw_count).c_str());
        } else {
            const char* np = T("wiz.res.nopeak");
            PassLine(false, std::string(np).rfind("! ", 0) == 0 ? np + 2 : np);   // «!» рисует PassLine
        }
    }
    if (w.fw_phase == 1 || w.fw_phase == 3) {
        ImGui::Spacing();
        ResPlot();
    }
}

// пик Cs-137 для шкалы: из набора в этой сессии, иначе из записи прибора (шаг «Разрешение»)
double ScalePeak()
{
    const Wiz& w = g_wiz;
    if (ResAccepted()) return w.fwr.peak;
    if (g_dev.setup_known == ds::SetupKnown::Yes && g_dev.setup.res_unix && g_dev.setup.res_peak_ch > 0) return g_dev.setup.res_peak_ch;
    return 0;
}

void BodyScale(bool primary)
{
    Wiz& w = g_wiz;
    bool conn = g_dev.connected();
    double pk = ScalePeak();
    if (pk > 0) {
        w.cal_k = 661.66 / pk;
        ImGui::TextUnformatted(Fmt(T("wiz.scale.calc"), pk, w.cal_k).c_str());
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(!conn || pk <= 0 || g_dev.queue_busy() || g_dev.acquiring);
    if (ActionButton(T("wiz.scale.btn"), primary)) {
        double c[5] = { 0, w.cal_k, 0, 0, 0 };
        char cmds[11][40]; peakfit::becq_cal_commands(c, cmds);
        std::vector<std::string> v; for (auto& x : cmds) v.push_back(x);
        w.cal_err0 = g_dev.queue_errors; g_dev.queue(v); w.cal_phase = 1;
    }
    ImGui::EndDisabled();
    if (w.cal_phase == 1) ImGui::TextUnformatted(T("wiz.scale.writing"));
    if (w.cal_phase == 2) PassLine(true, T("wiz.scale.ok"));
    if (w.cal_phase == 3) Para(c_warn, Fmt(T("wiz.scale.err"), w.cal_err.c_str()).c_str());
}

void BodySave(bool primary)
{
    Wiz& w = g_wiz;
    bool conn = g_dev.connected();
    ds::Snapshot snap = Snap();
    for (int i = 0; i < (int)ds::Step::Save; i++) {
        bool d = ds::step_done(snap, (ds::Step)i);
        PassLine(d, StepKey((ds::Step)i, "title"));
    }
    ImGui::Spacing();
    // рабочее ВВ: итог подбора в этой сессии → текущая уставка → уже записанный автозапуск
    // (ВВ выключено ⇒ hv_set = 0; без этого мастер молча снял бы автозапуск закрытого блока)
    double hv_w = w.hv2_v > 0 ? w.hv2_v : g_dev.gnum("hv_set");
    if (hv_w <= 0) hv_w = g_dev.gnum("hv_boot");
    int hv_work = (int)(hv_w + 0.5);
    std::string lb = Fmt(T("wiz.save.autohv"), hv_work);
    ImGui::Checkbox(lb.c_str(), &w.autohv);
    ImGui::Spacing();
    ImGui::BeginDisabled(!conn || Busy() || g_dev.acquiring || g_dev.queue_busy());
    if (ActionButton(T("wiz.save.btn"), primary)) {
        char c[32];
        if (w.autohv && hv_work > 0) snprintf(c, sizeof c, "-hvboot %d", hv_work); else snprintf(c, sizeof c, "-hvboot off");
        w.hvboot_v = (w.autohv && hv_work > 0) ? hv_work : 0;
        // запись «настроен» — до -save, чтобы попала во флеш; "-ok saved" сам перечитывает -setup
        std::string done = ds::setup_cmd(ds::Step::Save, (uint32_t)time(nullptr), 0, 0);
        if (!w.rec_wait) w.rec_err0 = g_dev.queue_errors;
        w.rec_err.clear();
        g_dev.queue({ c, done, "-save" });
        w.rec_wait = true;
        WriteReport(); w.saved = true;
    }
    ImGui::EndDisabled();
    if (w.saved) {
        if (w.report_path.empty()) Para(c_warn, ("! " + Fmt(T("wiz.save.report"), T("wiz.save.report_fail"))).c_str());
        else PassLine(true, Fmt(T("wiz.save.report"), w.report_path.c_str()));
    }
}

// ---------- шаг целиком ----------

void Body(ds::Step s, bool embedded)
{
    Wiz& w = g_wiz;
    ds::Snapshot snap = Snap();
    bool conn = snap.connected;
    int target = (int)g_dev.hvp.target;
    g_body_embedded = embedded;

    if (!embedded) {
        ImGui::PushFont(font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
        ImGui::Text(T("wiz.step_n"), (int)s + 1, (int)ds::Step::N);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PushFont(font_title);
        Para(c_text_strong, StepKey(s, "title"));
        ImGui::PopFont();
    }
    Para(c_text, Fmt(StepKey(s, "desc"), target).c_str());   // у hv/hv2 в описании канал %d
    ImGui::Dummy(ImVec2(0, px(6)));

    if (!conn && s != ds::Step::Flash) {                       // без связи — только прошивка
        Para(c_warn, (std::string("! ") + T("wiz.no_link")).c_str());
        ImGui::Spacing();
        std::string go = Fmt(T("wiz.prereq.go"), StepKey(ds::Step::Flash, "title"));
        if (AccentButton(go.c_str(), ImVec2(0, px(36)))) ui_go(ds::Mode::Wizard, ds::Step::Flash);
        return;
    }
    int pre = ds::step_prereq(snap, s);
    if (pre >= 0) {                                            // не хватает предыдущего шага: команд не шлём
        ds::Step ps = (ds::Step)pre;
        Para(c_warn, (std::string("! ") + Fmt(T("wiz.prereq.missing"), StepKey(ps, "title"))).c_str());
        ImGui::Spacing();
        std::string go = Fmt(T("wiz.prereq.go"), StepKey(ps, "title"));
        if (AccentButton(go.c_str(), ImVec2(0, px(36)))) ui_go(ds::Mode::Wizard, ps);
        return;
    }

    // «От вас»
    if (BeginBox("##you", true)) {
        ImGui::PushStyleColor(ImGuiCol_Text, c_warn);
        ImGui::TextUnformatted(T("wiz.you"));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", StepKey(s, "you"));
    }
    EndBox();
    ImGui::Dummy(ImVec2(0, px(2)));
    Small(c_text_dim, StepKey(s, "need"));
    ImGui::Dummy(ImVec2(0, px(6)));

    bool done = ds::step_done(snap, s);
    if (done) {
        std::string r = HomeStepResult(snap, s);
        PassLine(true, r.empty() ? std::string(T("wiz.step_done_plain")) : Fmt(T("wiz.step_done"), r.c_str()));
        ImGui::Spacing();
    }

    // ход работы и живые числа
    if (BeginBox("##work", false)) {
        bool primary = !done;
        switch (s) {
        case ds::Step::Flash: BodyFlash(primary); break;
        case ds::Step::Check: BodyCheck(primary); break;
        case ds::Step::Zero:  BodyZero(primary);  break;
        case ds::Step::Hv:
        case ds::Step::Hv2:   BodyHv(s, primary); break;
        case ds::Step::Tpl:   BodyTpl(primary);   break;
        case ds::Step::Res:   BodyRes(primary);   break;
        case ds::Step::Scale: BodyScale(primary); break;
        case ds::Step::Save:  BodySave(primary);  break;
        default: break;
        }
        if (embedded && StepRunning(s)) {
            ImGui::Spacing();
            if (ImGui::Button(T("wiz.stop"))) StepStop(s);
        }
        if (w.rec_wait) Small(c_text_dim, T("wiz.rec.saving"));
        else if (!w.rec_err.empty()) Para(c_warn, Fmt(T("wiz.rec.failed"), w.rec_err.c_str()).c_str());
    }
    EndBox();

    // если не получается
    ImGui::Dummy(ImVec2(0, px(4)));
    Small(c_text_dim, StepKey(s, "hint"));

    // технические подробности
    ImGui::Dummy(ImVec2(0, px(4)));
    if (ImGui::CollapsingHeader((std::string(T("wiz.more")) + "###wmore").c_str())) {   // ID не зависит от языка
        ImGui::PushFont(font_small);
        Para(c_text_dim, Fmt(StepKey(s, "more"), target).c_str());
        ImGui::PopFont();
        if (s == ds::Step::Check) {
            if (!w.diag_text.empty()) Small(c_text_dim, w.diag_text.c_str());
            ImGui::SetNextItemWidth(px(80)); ImGui::InputFloat(T("wiz.check.ptref"), &w.pt_ref, 0, 0, "%.2f");
            ImGui::SameLine();
            ImGui::BeginDisabled(!conn || g_dev.gstr("pt", "-") != "ok");
            if (ImGui::Button(T("wiz.check.ptcal"))) { char c[48]; snprintf(c, sizeof c, "-ptcal %.2f", w.pt_ref); g_dev.send_cmd(c); }
            ImGui::EndDisabled();
        }
        if (s == ds::Step::Hv || s == ds::Step::Hv2) {
            Device::HvPeak& h = g_dev.hvp;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T("wiz.hv.window"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(px(70)); float lo = (float)h.lo; if (ImGui::InputFloat("##wlo", &lo, 0, 0, "%.0f")) h.lo = lo;
            ImGui::SameLine(); ImGui::TextUnformatted("…"); ImGui::SameLine();
            ImGui::SetNextItemWidth(px(70)); float hi = (float)h.hi; if (ImGui::InputFloat("##whi", &hi, 0, 0, "%.0f")) h.hi = hi;
            ImGui::BeginDisabled(h.running);
            ImGui::SetNextItemWidth(px(90)); float cn = (float)h.count;
            if (ImGui::InputFloat(T("wiz.hv.count"), &cn, 0, 0, "%.0f")) h.count = cn < 100 ? 100 : cn;
            ImGui::SetNextItemWidth(px(90)); float tm = (float)h.timeout;
            if (ImGui::InputFloat(T("wiz.hv.timeout"), &tm, 0, 0, "%.0f")) h.timeout = tm < 10 ? 10 : tm;
            ImGui::EndDisabled();
        }
        if (s == ds::Step::Tpl) {
            ImGui::BeginDisabled(w.tpl_phase == 1);
            ImGui::SetNextItemWidth(px(90));
            if (ImGui::InputInt(T("wiz.tpl.target_lbl"), &w.tpl_target, 0, 0)) {
                if (w.tpl_target < tpl::NPH * 10) w.tpl_target = tpl::NPH * 10;
                if (w.tpl_target > 100000) w.tpl_target = 100000;
            }
            ImGui::SetNextItemWidth(px(90));
            if (ImGui::InputFloat((std::string(T("wiz.res.timeout_lbl")) + "##tpltm").c_str(), &w.tpl_timeout, 0, 0, "%.0f") && w.tpl_timeout < 30)
                w.tpl_timeout = 30;
            ImGui::EndDisabled();
        }
        if (s == ds::Step::Res) {
            ImGui::BeginDisabled(w.fw_phase == 1);
            ImGui::SetNextItemWidth(px(90));
            if (ImGui::InputFloat(T("wiz.res.timeout_lbl"), &w.fw_timeout, 0, 0, "%.0f") && w.fw_timeout < 30) w.fw_timeout = 30;
            ImGui::EndDisabled();
        }
    }
}

// рейка слева: номер, короткое имя, галочка и короткий результат; клик — переход к шагу
void Rail(const ds::Snapshot& snap)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c_child);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, px(14)));
    ImGui::BeginChild("##rail", ImVec2(RAIL_W(), 0), ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SetCursorPosX(px(16));
    ImGui::PushFont(font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, c_text_dim);
    ImGui::TextUnformatted(T("wiz.title"));
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Spacing();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < (int)ds::Step::N; i++) {
        ds::Step st = (ds::Step)i;
        bool d = ds::step_done(snap, st), cur = st == g_ui.wiz_step;
        std::string res = d ? HomeStepResult(snap, st) : std::string();
        float h = px(res.empty() ? 34.0f : 50.0f);
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Header, c_accent_sel);
        if (ImGui::Selectable("##st", cur, 0, ImVec2(0, h))) g_ui.wiz_step = st;
        ImGui::PopStyleColor();
        ImGui::PopID();
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (cur) dl->AddRectFilled(a, ImVec2(a.x + px(3), b.y), U32(c_accent));
        float ty = a.y + px(8);
        char n[8]; snprintf(n, sizeof n, "%d", i + 1);
        dl->AddText(ImVec2(a.x + px(16), ty), U32(cur ? c_accent : c_text_dim), n);
        dl->AddText(ImVec2(a.x + px(40), ty), U32(cur || d ? c_text : c_text_dim), StepKey(st, "short"));
        if (d) CheckIcon(dl, ImVec2(b.x - px(30), ty + px(1)), px(15), U32(c_ok));
        if (!res.empty())
            dl->AddText(font_small, font_small->FontSize, ImVec2(a.x + px(40), ty + ImGui::GetTextLineHeight() + px(2)), U32(c_text_dim), res.c_str());
    }
    ImGui::EndChild();
}

} // namespace

// ================= снаружи =================

void TplPulsePlots(const tpl::Result* res)
{
    const TplView& v = g_tv;
    bool have_tpl = res && res->ok && !res->shape_x.empty();
    if (v.ring.size() == 0 && v.blk.empty() && !have_tpl) { Small(c_text_dim, T("wiz.tpl.v_empty")); return; }

    // наложение одиночных импульсов
    Small(c_text_dim, Fmt(T("wiz.tpl.v_over"), (int)v.ring.size()).c_str());
    static float xs[tpl::VW];
    for (int i = 0; i < tpl::VW; i++) xs[i] = (float)(i - tpl::VPRE);
    if (ImPlot::BeginPlot("##tplover", ImVec2(-1, px(220)), ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxis(ImAxis_X1, T("wiz.tpl.v_ax_x"));
        ImPlot::SetupAxis(ImAxis_Y1, T("wiz.tpl.v_ax_y"));
        ImPlot::SetupAxisLimits(ImAxis_X1, -tpl::VPRE, tpl::VPOST, ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -0.15, 1.15, ImPlotCond_Once);
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        ImVec4 tc = c_trace; tc.w = 0.25f;
        for (size_t i = 0; i < v.ring.size(); i++) {
            ImPlot::SetNextLineStyle(tc, 1.0f);
            ImPlot::PlotLine(T("wiz.tpl.v_pulses"), xs, v.ring.at(i).data(), tpl::VW);   // одна строка легенды на все
        }
        if (have_tpl) {
            ImPlot::SetNextLineStyle(c_ok, 2.5f);
            ImPlot::PlotLine(T("wiz.tpl.v_tpl"), res->shape_x.data(), res->shape_y.data(), (int)res->shape_x.size());
        }
        ImPlot::EndPlot();
    }
    if (have_tpl)
        Small(c_text_dim, Fmt(T("wiz.tpl.v_tpl_note"), res->resid_med, res->spread_before, res->spread_after).c_str());

    // осциллограмма: бегущая лента
    if (v.blk.empty()) { g_tape = TplTape{}; return; }
    TplTape& tp = g_tape;
    double now = ImGui::GetTime();
    if (v.seen < tp.last_seen) tp = TplTape{};                   // новый сбор
    if (v.seen != tp.last_seen) {
        if (tp.segs.empty()) { tp.segs.push_back({ 0.0, TapeSeg{} }); tp.segs.back().second.take(v); tp.pos = 0; tp.t = now; }
        else if (tp.segs.size() > 1 && tp.segs.back().first > tp.pos + TAPE_WIN) tp.segs.back().second.take(v);   // ещё не видна — свежее
        else if (tp.end() - tp.pos < 1.5 * TAPE_WIN) { double at = tp.end(); tp.segs.push_back({ at, TapeSeg{} }); tp.segs.back().second.take(v); }
        else goto keep;                       // впереди и так хватает — порцию не берём (её место займёт свежая)
        tp.last_seen = v.seen;
    }
keep:
    double dt = tp.t < 0 ? 0 : now - tp.t;
    tp.t = now;
    if (dt > 0.1) dt = 0.1;                                      // окно свёрнуто/подвисло — без скачка
    tp.pos += dt * TAPE_SPEED;
    if (tp.pos > tp.end() - TAPE_WIN) tp.pos = (std::max)(0.0, tp.end() - TAPE_WIN);   // ждём следующую порцию
    while (tp.segs.size() > 1 && tp.segs[1].first <= tp.pos - 1.0) tp.segs.pop_front();   // ушли за левый край
    const TapeSeg& cur = tp.segs.front().second;

    ImGui::Spacing();
    Small(c_text_dim, Fmt(T("wiz.tpl.v_raw"), (int)cur.ok_x.size(), (int)cur.bad_x.size()).c_str());
    Small(c_text_dim, T("wiz.tpl.v_tape"));
    if (ImPlot::BeginPlot("##tplraw", ImVec2(-1, px(170)), ImPlotFlags_NoMenus | ImPlotFlags_NoTitle | ImPlotFlags_NoInputs)) {
        ImPlot::SetupAxis(ImAxis_X1, T("wiz.tpl.v_ax_n"));
        ImPlot::SetupAxis(ImAxis_Y1, T("wiz.tpl.v_ax_code"));
        ImPlot::SetupAxisLimits(ImAxis_X1, tp.pos, tp.pos + TAPE_WIN, ImPlotCond_Always);
        // ось Y без скачков: низ — база −60, верх — не ниже база+1300 (рабочее окно 100…1200),
        // растёт сразу до пришедшего максимума, опускается медленно (≈ за 20 с)
        float ymax = 0;
        for (const auto& sp : tp.segs) for (float y : sp.second.y) if (y > ymax) ymax = y;
        double floor_top = cur.base + 1300.0;
        double want = (std::max)((double)ymax + 40.0, floor_top);
        if (want > tp.ytop) tp.ytop = want;
        else tp.ytop = (std::max)(want, tp.ytop - (tp.ytop - want) * (std::min)(1.0, dt / 20.0) - 0.5);
        ImPlot::SetupAxisLimits(ImAxis_Y1, cur.base - 60.0, tp.ytop, ImPlotCond_Always);
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        static std::vector<float> ox, oy, bx, by;
        ox.clear(); oy.clear(); bx.clear(); by.clear();
        auto seg = [&](const TapeSeg& sg, double off) {
            if (!sg.have) return;
            int a = (int)(std::max)(0.0, tp.pos - off), b = (int)(std::min)((double)sg.y.size(), tp.pos + TAPE_WIN + 1 - off);
            if (b > a) {
                ImPlot::SetNextLineStyle(c_trace, 1.0f);
                ImPlot::PlotLine("##raw", sg.y.data() + a, b - a, 1.0, (double)a + off);
            }
            for (size_t i = 0; i < sg.ok_x.size(); i++)  { ox.push_back(sg.ok_x[i] + (float)off);  oy.push_back(sg.ok_y[i]); }
            for (size_t i = 0; i < sg.bad_x.size(); i++) { bx.push_back(sg.bad_x[i] + (float)off); by.push_back(sg.bad_y[i]); }
        };
        static std::vector<double> joins;
        joins.clear();
        for (size_t i = 0; i < tp.segs.size(); i++) {
            seg(tp.segs[i].second, tp.segs[i].first);
            if (i > 0) joins.push_back(tp.segs[i].first);       // стык порций
        }
        if (!joins.empty()) {
            ImPlot::SetNextLineStyle(c_text_dim, 1.0f);
            ImPlot::PlotInfLines("##join", joins.data(), (int)joins.size());
        }
        if (!ox.empty()) {
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.5f, c_ok, 1.0f, c_ok);
            ImPlot::PlotScatter(T("wiz.tpl.v_ok"), ox.data(), oy.data(), (int)ox.size());
        }
        if (!bx.empty()) {
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Square, 3.5f, c_danger, 1.0f, c_danger);
            ImPlot::PlotScatter(T("wiz.tpl.v_bad"), bx.data(), by.data(), (int)bx.size());
        }
        ImPlot::EndPlot();
    }
}

void WizStepBodyEmbedded(ds::Step s)
{
    if (s < ds::Step::Flash || s >= ds::Step::N) return;
    ImGui::PushID("wizemb");
    Body(s, true);
    ImGui::PopID();
}

void DrawWizard(ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
    bool vis = ImGui::Begin("##wizard", nullptr, f);
    ImGui::PopStyleVar(2);
    if (!vis) { ImGui::End(); return; }

    ds::Snapshot snap = Snap();
    ds::Step s = g_ui.wiz_step;
    if (s >= ds::Step::N) s = g_ui.wiz_step = ds::Step::Save;

    Rail(snap);
    ImGui::SameLine(0, 0);

    ImGui::BeginChild("##wmain", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    // тело шага с прокруткой
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PAD() + px(8), PAD()));
    ImGui::BeginChild("##wbody", ImVec2(0, -FOOT_H()), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + px(820.0f));   // строки не шире 820 px при 100 % (макет: max-width 760)
    Body(s, false);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    // навигация: «Выйти из мастера», «Назад», «Остановить» (пока идёт процесс) … «Дальше»
    ImGui::Separator();
    ImGui::SetCursorPos(ImVec2(PAD(), ImGui::GetCursorPosY() + px(8)));
    const float BH = px(34.0f);
    bool leave = ImGui::Button(T("wiz.exit"), ImVec2(0, BH));
    ImGui::SameLine();
    ImGui::BeginDisabled(s == ds::Step::Flash);
    if (ImGui::Button(T("wiz.back"), ImVec2(0, BH))) g_ui.wiz_step = (ds::Step)((int)s - 1);
    ImGui::EndDisabled();
    if (StepRunning(s)) {
        ImGui::SameLine();
        if (ImGui::Button(T("wiz.stop"), ImVec2(0, BH))) StepStop(s);
    }
    bool last = s == ds::Step::Save;
    const char* nx = T(last ? "wiz.finish" : "wiz.next");
    float nw = ImGui::CalcTextSize(nx).x + px(48.0f);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - nw - PAD());
    bool done = ds::step_done(snap, s);
    ImGui::BeginDisabled(!done);
    bool next = done ? AccentButton(nx, ImVec2(nw, BH)) : ImGui::Button(nx, ImVec2(nw, BH));
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::End();

    if (next) {
        if (last) ui_go(ds::Mode::Home);
        else g_ui.wiz_step = (ds::Step)((int)s + 1);
    }
    if (leave) ui_go(ds::Mode::Home);
}

void WizTick()
{
    Wiz& w = g_wiz;
    double now = ImGui::GetTime();
    TplViewTick(now);                          // вид импульсов: только читает g_dev.raw, работает и без связи

    // после прошивки — снова на связь
    if (w.fl_reconnect && w.fl_state != 1) {
        w.fl_reconnect = false;
        if (w.fl_state == 2) g_dev.connect(w.fl_port);
    }
    if (!g_dev.connected()) {
        // связь пропала: процессы прибора остановлены в Device::disconnect
        if (w.chk_phase == 1 || w.chk_phase == 2) w.chk_phase = 0;
        if (w.tpl_phase == 1 || w.tpl_phase == 3) w.tpl_phase = 0;
        if (w.fw_phase == 1) w.fw_phase = 0;
        w.hv_run = ds::Step::N; w.zero_wait = false; w.rec_wait = false;
        if (w.cal_phase == 1) w.cal_phase = 0;
        return;
    }

    // проверка электроники: -diag, затем самотест линейности; итог — в прибор
    if (w.chk_phase == 1) {
        if (g_dev.last_reply.rfind("-ok diag", 0) == 0) {
            w.diag_text = g_dev.last_reply;
            w.diag_ok = w.diag_text.find("tract=ok") != std::string::npos;
        }
        if (!w.diag_text.empty() || now - w.chk_t0 > 3.0) {
            g_dev.lin_start({ 50, 100, 200, 400, 600, 800, 1000, 1200, 1350 }, 10000, 100);
            w.chk_phase = 2;
        }
    } else if (w.chk_phase == 2 && !g_dev.lin.running) {
        w.chk_ok = w.diag_ok && g_dev.gnum("vref_ok", 0) > 0.5 && g_dev.gstr("pt", "-") == "ok" && LinOk();
        Record(ds::Step::Check, w.chk_ok ? 1 : 0);
        w.chk_phase = 3;
    }

    // ноль монитора: итог пишется, только если прибор ответил -ok
    if (w.zero_wait && !g_dev.queue_busy()) {
        w.zero_wait = false;
        w.zero_ok = g_dev.queue_errors == w.zero_err0;
        w.zero_reply = w.zero_ok ? g_dev.last_reply : g_dev.queue_last_err;
        if (w.zero_ok) { Record(ds::Step::Zero); w.zero_run = true; }
    }

    // подбор ВВ (шаги Hv и Hv2)
    if (w.hv_run != ds::Step::N && !g_dev.hvp.running) {
        const Device::HvPeak& h = g_dev.hvp;
        if (h.done && h.peak > 0) {
            if (w.hv_run == ds::Step::Hv2) { w.hv2_v = h.v; w.hv2_peak = h.peak; }
            else                           { w.hv1_v = h.v; w.hv1_peak = h.peak; }
            Record(w.hv_run, h.v, h.peak);
        }
        w.hv_last = w.hv_run;
        w.hv_run = ds::Step::N;
    }

    // форма импульса
    Device::RawRun& R = g_dev.raw;
    if (w.tpl_phase == 1) {
        w.tpl_el = now - w.tpl_t0;
        // живой счёт одиночных по новым порциям (во время сбора не больше 16 порций за кадр)
        size_t have = R.blocks.size();
        if (w.tpl_counted < have) {
            size_t to = R.running ? (std::min)(have, w.tpl_counted + 16) : have;
            w.tpl_n += tpl::count_singles(R.blocks, w.tpl_counted, to);
            w.tpl_counted = to;
        }
        if (R.running) {
            if (!w.tpl_fin_req &&
                peakfit::count_stop((uint64_t)w.tpl_n, (uint64_t)w.tpl_target, w.tpl_el, w.tpl_timeout) != peakfit::Stop::Run) {
                g_dev.raw_finish();                // закончить на собранных порциях
                w.tpl_fin_req = true;
            }
        } else if (!w.tpl_fin_req && (int)R.blocks.size() < R.target) {
            w.tpl_phase = 5;                       // сбор оборвался (сбои чтения порций)
        } else {
            peakfit::Accept a = peakfit::accept((uint64_t)w.tpl_n, (uint64_t)w.tpl_target, tpl::NPH * 10);
            if (a == peakfit::Accept::Reject) { w.tpl_few = true; w.tpl_phase = 5; }
            else {
                w.tpl_partial = a == peakfit::Accept::Partial;
                w.tpl_phase = 2; w.tpl_state = 1;
                std::thread([blocks = R.blocks]() {
                    tpl::Singles s = tpl::find_singles(blocks);
                    g_wiz.tpl_res = tpl::build(s);
                    g_wiz.tpl_state = 2;
                }).detach();
            }
        }
    }
    if (w.tpl_phase == 2 && w.tpl_state == 2) {
        if (w.tpl_res.ok) { w.tpl_err0 = g_dev.queue_errors; g_dev.queue(tpl::commands(w.tpl_res)); g_dev.queue({ "-tplok" }); w.tpl_phase = 3; }
        else w.tpl_phase = 5;
    }
    if (w.tpl_phase == 3 && !g_dev.queue_busy()) {
        w.tpl_phase = g_dev.queue_errors == w.tpl_err0 ? 4 : 5;
        if (w.tpl_phase == 4) Record(ds::Step::Tpl);
    }

    // разрешение: набор до fw_count импульсов в окне фотопика; пик и окно раз в секунду по живому спектру
    if (w.fw_phase == 1) {
        int lo = (int)g_dev.hvp.lo, hi = (int)g_dev.hvp.hi;        // окно поиска пика (300–6000)
        w.fw_el = now - w.fw_t0;
        if (w.fw_el >= 2.0 && now - w.fw_eval_t >= 1.0) {          // первые 2 с: могли прийти куски до -rst
            w.fw_eval_t = now;
            bool had = w.fw_win.ok;
            w.fw_win = peakfit::peak_window(g_dev.spectrum, Device::CHANNELS, lo, hi);
            w.fw_n = w.fw_win.ok ? peakfit::window_sum(g_dev.spectrum, Device::CHANNELS, w.fw_win.lo, w.fw_win.hi) : 0;
            if (w.fw_win.ok && !had) { w.fw_view_full = false; w.fw_view_req++; }   // пик нашёлся: приблизить
        }
        peakfit::Stop st = peakfit::count_stop(w.fw_n, (uint64_t)w.fw_count, w.fw_el, w.fw_timeout);
        if (st != peakfit::Stop::Run || w.fw_user_stop) {
            g_dev.stop();
            // итог — только уверенный пик (та же проверка, что для счёта); иначе «Пик не найден»
            peakfit::PeakWin fin = peakfit::peak_window(g_dev.spectrum, Device::CHANNELS, lo, hi);
            w.fwr = fin.ok ? fin.fit : peakfit::Result();
            w.fw_win = fin;
            w.fw_n = fin.ok ? peakfit::window_sum(g_dev.spectrum, Device::CHANNELS, fin.lo, fin.hi) : 0;
            w.fw_timeout_hit = st == peakfit::Stop::Timeout;
            // N набрано: принят; меньше N, но не меньше 10 000 (предел или «Хватит»): с пометкой; иначе не пишем
            w.fw_acc = peakfit::accept(w.fw_n, (uint64_t)w.fw_count, peakfit::RES_MIN);
            w.fw_phase = 3;
            w.fw_view_full = false; w.fw_view_req++;
            if (ResAccepted()) Record(ds::Step::Res, w.fwr.fwhm_pct, w.fwr.peak);
        }
    }


    // шкала энергий: после записи перечитать шкалу (-cal) и запись о настройке
    if (w.cal_phase == 1 && !g_dev.queue_busy()) {
        if (g_dev.queue_errors == w.cal_err0) {
            w.cal_phase = 2;
            g_dev.send_cmd("-cal");
            g_dev.setup_refresh();
        } else { w.cal_phase = 3; w.cal_err = g_dev.queue_last_err; }
    }

    // ответ на запись -setup → перечитать запись (рейка и «Дальше» по данным прибора)
    if (w.rec_wait && !g_dev.queue_busy()) {
        w.rec_wait = false;
        if (g_dev.queue_errors != w.rec_err0) w.rec_err = g_dev.queue_last_err;
        g_dev.setup_refresh();
    }
}
