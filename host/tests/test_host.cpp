// test_host.cpp — консольные тесты чистой логики пульта (devstate и др.), без COM-порта и UI.
// Макросы — общие с тестами прошивки: firmware/gamma_fw/tests/t.h.
#include "t.h"
#include "devstate.h"
#include "theme_tokens.h"
#include "i18n.h"
#include "appcfg.h"
#include "peakfit.h"
#include "tpl_learn.h"
#include "adc_dnl.h"
#include <random>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>

using namespace ds;

static Snapshot connected_snap()
{
    Snapshot s;
    s.connected = true;
    s.fw_ok = true;
    return s;
}

static void test_parse_setup()
{
    SetupRec r;
    const char* line = "-ok setup tpl=1 cal=0 chk=1762700000/1 zero=1762700100 hv=1762700200/565.0/1180.5 "
                       "tplu=1762700300 res=1762800000/6.99/1190.0 done=1";
    CHECK(parse_setup(line, r));
    CHECK(r.tpl);
    CHECK(!r.cal);
    CHECK(r.chk_unix == 1762700000u);
    CHECK(r.chk_ok);
    CHECK(r.zero_unix == 1762700100u);
    CHECK(r.hv_unix == 1762700200u);
    CHECK_NEAR(r.hv_v, 565.0, 1e-9);
    CHECK_NEAR(r.hv_peak_ch, 1180.5, 1e-9);
    CHECK(r.tpl_unix == 1762700300u);           // tplu= не путается с tpl=
    CHECK(r.res_unix == 1762800000u);
    CHECK_NEAR(r.res_fwhm, 6.99, 1e-9);
    CHECK_NEAR(r.res_peak_ch, 1190.0, 1e-9);
    CHECK(r.done_unix == 1u);

    // формат прибора 3a6a41b, проверенный 2026-11-12
    SetupRec z;
    CHECK(parse_setup("-ok setup tpl=1 cal=1 chk=0/0 zero=0 hv=0/0.0/0.0 tplu=0 res=0/0.00/0.0 done=1\r\n", z));
    CHECK(z.tpl && z.cal && z.done_unix == 1u && z.tpl_unix == 0u && z.res_unix == 0u && !z.chk_ok);

    // tplu раньше tpl — разбор по целому ключу
    SetupRec o;
    CHECK(parse_setup("-ok setup tplu=7 tpl=0 cal=1 chk=0/0 zero=0 hv=0/0.0/0.0 res=0/0.00/0.0 done=0", o));
    CHECK(!o.tpl && o.tpl_unix == 7u && o.cal);

    SetupRec g;
    CHECK(!parse_setup("мусор", g));
    CHECK(!parse_setup("-err unknown", g));
    CHECK(!parse_setup("-ok setup", g));               // голый ответ на запись — не запись
    CHECK(!parse_setup("-ok setup tpl=1 cal=x", g));   // неполный/испорченный
    CHECK(!parse_setup("", g));
}

static void test_old_firmware()
{
    Snapshot s = connected_snap();
    s.known = SetupKnown::OldFirmware;
    CHECK(home_view(s) == Home::NotConfigured);
    CHECK(first_open_step(s) == Step::Flash);
    CHECK(!step_done(s, Step::Flash));
}

static void test_migrated_ready()
{
    Snapshot s = connected_snap();
    s.known = SetupKnown::Yes;
    s.s.tpl = true; s.s.cal = true; s.s.done_unix = 1;
    CHECK(home_view(s) == Home::Ready);

    Snapshot n;                                    // без связи
    CHECK(home_view(n) == Home::NotConnected);
    Snapshot u = connected_snap();                 // связь есть, ответа -setup ещё нет
    CHECK(home_view(u) == Home::NotConfigured);
    Snapshot d = s; d.s.done_unix = 0;
    CHECK(home_view(d) == Home::NotConfigured);
}

static void test_steps()
{
    Snapshot s = connected_snap();
    s.known = SetupKnown::Yes;                     // пустая запись
    CHECK(step_done(s, Step::Flash));
    CHECK(first_open_step(s) == Step::Check);

    s.s.chk_unix = 3; s.s.chk_ok = false;
    CHECK(!step_done(s, Step::Check));             // неудачная проверка — не сделано
    s.s.chk_ok = true;
    CHECK(step_done(s, Step::Check));
    CHECK(first_open_step(s) == Step::Zero);

    s.s.hv_unix = 5; s.s.tpl_unix = 9; s.s.tpl = true;
    CHECK(step_done(s, Step::Hv));
    CHECK(step_done(s, Step::Tpl));
    CHECK(!step_done(s, Step::Hv2));
    s.s.hv_unix = 10;
    CHECK(step_done(s, Step::Hv2));

    s.s.done_unix = 11; s.saved = 0;
    CHECK(!step_done(s, Step::Save));
    s.saved = 1;
    CHECK(step_done(s, Step::Save));

    Snapshot all = s;
    all.s.zero_unix = 4; all.s.res_unix = 12; all.s.cal = true;
    CHECK(first_open_step(all) == Step::Save);     // всё сделано → последний шаг
}

static void test_prereq()
{
    Snapshot s = connected_snap();
    s.known = SetupKnown::Yes;
    CHECK(step_prereq(s, Step::Scale) == (int)Step::Res);
    s.s.res_unix = 100;
    CHECK(step_prereq(s, Step::Scale) == -1);
    CHECK(step_prereq(s, Step::Hv2) == (int)Step::Tpl);
    CHECK(step_prereq(s, Step::Tpl) == (int)Step::Hv);
    CHECK(step_prereq(s, Step::Res) == (int)Step::Hv);
    s.s.hv_unix = 50;
    CHECK(step_prereq(s, Step::Tpl) == -1);
    CHECK(step_prereq(s, Step::Res) == -1);
    s.s.tpl = true;
    CHECK(step_prereq(s, Step::Hv2) == -1);
    CHECK(step_prereq(s, Step::Check) == -1);

    // перенесённый с v1 прибор: done=1, дат подбора ВВ нет — «Разрешение» и «Форма импульса» доступны сразу
    Snapshot m = connected_snap();
    m.known = SetupKnown::Yes;
    m.s.tpl = m.s.cal = true;
    m.s.done_unix = 1;
    CHECK(step_prereq(m, Step::Res) == -1);
    CHECK(step_prereq(m, Step::Tpl) == -1);
}

static void test_top_bar()
{
    Snapshot n;
    TopBar b = top_bar(n, Mode::Home);
    CHECK(!b.modes_enabled);
    CHECK(!b.kill_visible && !b.hv_chip_on);

    Snapshot s = connected_snap();
    s.hv_on = true; s.hv_v = 565;
    const Mode modes[] = { Mode::Home, Mode::Wizard, Mode::Advanced };
    for (Mode m : modes) {
        TopBar t = top_bar(s, m);
        CHECK(t.kill_visible);
        CHECK(t.hv_chip_on);
        CHECK(t.modes_enabled);
    }
    TopBar w = top_bar(s, Mode::Wizard);
    CHECK(w.wiz_on && !w.adv_on);
    TopBar a = top_bar(s, Mode::Advanced);
    CHECK(!a.wiz_on && a.adv_on);
    TopBar h = top_bar(s, Mode::Home);
    CHECK(!h.wiz_on && !h.adv_on);

    s.hv_on = false;
    CHECK(!top_bar(s, Mode::Wizard).kill_visible);
}

static void test_connect_hint()
{
    CHECK(std::strcmp(connect_hint_key(5), "conn.port_busy") == 0);   // ERROR_ACCESS_DENIED
    CHECK(std::strcmp(connect_hint_key(2), "conn.port_gone") == 0);   // ERROR_FILE_NOT_FOUND
    CHECK(std::strcmp(connect_hint_key(31), "conn.failed") == 0);
    CHECK(std::strcmp(connect_hint_key(0), "conn.failed") == 0);
}

static void test_display_serial()
{
    CHECK(display_serial("04740001") == "GS474-0001");
    CHECK(display_serial("0474abCD") == "GS474-abCD");
    CHECK(display_serial("ABC") == "ABC");
    CHECK(display_serial("04750001") == "04750001");
    CHECK(display_serial("0474000G") == "0474000G");
    CHECK(display_serial("047400011") == "047400011");
    CHECK(display_serial("") == "");
}

// WCAG-контраст токенов темы (спека: основной ≥ 7:1, вторичный ≥ 4.5:1)
static void test_contrast()
{
    using theme::contrast; const theme::Tok& k = theme::tok;
    double bw = contrast(0xffffff, 0x000000);
    CHECK(bw > 20.99 && bw < 21.01);
    CHECK(contrast(0x000000, 0xffffff) == bw);                      // порядок аргументов не важен
    CHECK(contrast(k.text, k.bg) >= 7.0);
    CHECK(contrast(k.text, k.panel) >= 7.0);
    CHECK(contrast(k.text, k.panel2) >= 7.0);
    CHECK(contrast(k.text, k.sel_bg) >= 7.0);
    for (unsigned b : { k.bg, k.panel, k.panel2, k.sel_bg }) CHECK(contrast(k.text2, b) >= 4.5);
    CHECK(contrast(k.on_accent, k.accent) >= 4.5);
    for (unsigned c : { k.ok, k.warn, k.danger, k.hv }) CHECK(contrast(c, k.panel) >= 4.5);
    CHECK(contrast(k.hv, k.hv_bg) >= 4.5);                          // чип «ВВ включено»
    CHECK(contrast(k.warn, k.warn_bg) >= 4.5);
}

// строки интерфейса: язык, пропуски, вычитка таблицы
static void test_i18n()
{
    i18n_set(Lang::Ru);
    CHECK(i18n_get() == Lang::Ru);
    std::string ru = T("home.ready.title");
    CHECK(ru != "home.ready.title");
    i18n_set(Lang::En);
    CHECK(i18n_get() == Lang::En);
    std::string en = T("home.ready.title");
    CHECK(en != "home.ready.title");
    CHECK(en != ru);
    i18n_set(Lang::Ru);
    CHECK(T("home.ready.title") == ru);                             // переключение без перезапуска

    // нет ключа: возвращается сам ключ и запоминается один раз
    size_t m0 = i18n_missing().size();
    CHECK(std::strcmp(T("нет.такого"), "нет.такого") == 0);
    CHECK(std::strcmp(T("нет.такого"), "нет.такого") == 0);
    CHECK(i18n_missing().size() == m0 + 1);
    CHECK(i18n_missing().back() == "нет.такого");

    // ключи подсказок подключения из devstate есть в таблице на обоих языках
    for (Lang l : { Lang::Ru, Lang::En }) {
        i18n_set(l);
        for (unsigned long e : { 5ul, 2ul, 31ul }) {
            const char* k = connect_hint_key(e);
            CHECK(std::strcmp(T(k), k) != 0);
        }
    }
    i18n_set(Lang::Ru);
    CHECK(i18n_missing().size() == m0 + 1);

    std::string report;
    bool ok = i18n_check(report);
    if (!ok) printf("%s\n", report.c_str());
    CHECK(ok);
    CHECK(i18n_count() > 50);

    // сама проверка ловит нарушения
    const I18nEntry bad[] = {
        { "a.dup",  "раз",            "one" },
        { "a.dup",  "два",            "two" },
        { "a.empty", "",              "x" },
        { "a.dash", "Прибор \u2014 ок", "ok" },
        { "a.ndash", "слово\u2013слово", "ok" },
        { "a.range", "3\u20135 минут",   "3\u20135 min" },          // диапазон цифра-цифра разрешён
    };
    std::string r2;
    CHECK(!i18n_check_entries(bad, sizeof bad / sizeof bad[0], r2));
    CHECK(r2.find("a.dup") != std::string::npos);
    CHECK(r2.find("a.empty") != std::string::npos);
    CHECK(r2.find("a.dash") != std::string::npos);
    CHECK(r2.find("a.ndash") != std::string::npos);
    CHECK(r2.find("a.range") == std::string::npos);
    std::string r3;
    CHECK(i18n_check_entries(bad + 5, 1, r3));
}

static std::string tmp_ini(const char* name)
{
    const char* t = std::getenv("TEMP");
    return std::string(t ? t : ".") + "\\" + name;
}

// настройки пульта: язык в gammapult.ini
static void test_appcfg()
{
    std::string p = tmp_ini("gammapult_test_cfg.ini");
    std::remove(p.c_str());
    AppCfg c; c.lang = Lang::En;
    CHECK(appcfg_save(c, p));
    AppCfg d;
    CHECK(d.lang == Lang::Ru);
    CHECK(appcfg_load(d, p));
    CHECK(d.lang == Lang::En);
    c.lang = Lang::Ru;
    CHECK(appcfg_save(c, p));
    d.lang = Lang::En;
    CHECK(appcfg_load(d, p));
    CHECK(d.lang == Lang::Ru);

    // битый файл: язык по умолчанию, загрузка не падает
    FILE* f = std::fopen(p.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) { std::fputs("\xff\xfe garbage\nlang=klingon\n=\n", f); std::fclose(f); }
    AppCfg e; e.lang = Lang::En;
    CHECK(appcfg_load(e, p));
    CHECK(e.lang == Lang::Ru);
    std::remove(p.c_str());

    // файла нет: тоже Ru (первый запуск)
    AppCfg g; g.lang = Lang::En;
    appcfg_load(g, p);
    CHECK(g.lang == Lang::Ru);
}

// масштаб интерфейса: scale= в gammapult.ini, пределы, итоговый масштаб ui × dpi/96
static bool near_(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void test_appcfg_scale()
{
    std::string p = tmp_ini("gammapult_test_scale.ini");
    std::remove(p.c_str());

    AppCfg d;
    CHECK(near_(d.scale, 1.30f));          // по умолчанию 130 %
    appcfg_load(d, p);                     // файла нет
    CHECK(near_(d.scale, 1.30f));

    AppCfg c; c.lang = Lang::En; c.scale = 1.6f;
    CHECK(appcfg_save(c, p));
    AppCfg e;
    CHECK(appcfg_load(e, p));
    CHECK(near_(e.scale, 1.6f));
    CHECK(e.lang == Lang::En);             // язык не теряется рядом с масштабом

    // запись: строка scale=1.60
    {
        FILE* f = std::fopen(p.c_str(), "rb");
        CHECK(f != nullptr);
        char buf[256] = {0};
        if (f) { size_t n = std::fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; std::fclose(f); }
        CHECK(std::strstr(buf, "scale=1.60") != nullptr);
        CHECK(std::strstr(buf, "lang=en") != nullptr);
    }

    auto load_text = [&](const char* text) {
        FILE* f = std::fopen(p.c_str(), "wb");
        if (f) { std::fputs(text, f); std::fclose(f); }
        AppCfg r; r.scale = 0.5f;
        appcfg_load(r, p);
        return r.scale;
    };
    CHECK(near_(load_text("scale=abc\n"), 1.30f));      // битое
    CHECK(near_(load_text("scale=\n"), 1.30f));
    CHECK(near_(load_text("scale=1.4x\n"), 1.30f));     // хвост — битое
    CHECK(near_(load_text("scale=nan\n"), 1.30f));
    CHECK(near_(load_text("scale=5\n"), 1.80f));        // выше предела — обрезка
    CHECK(near_(load_text("scale=0.2\n"), 0.90f));      // ниже — обрезка
    CHECK(near_(load_text("lang=en\r\nscale = 1.10 \r\n"), 1.10f));
    std::remove(p.c_str());

    // пределы
    CHECK(near_(appcfg_clamp_scale(1.0f), 1.0f));
    CHECK(near_(appcfg_clamp_scale(0.0f), 0.9f));
    CHECK(near_(appcfg_clamp_scale(9.0f), 1.8f));
    CHECK(near_(appcfg_clamp_scale(std::nanf("")), 1.30f));

    // итоговый масштаб
    CHECK(near_(ui_total_scale(1.30f, 96), 1.30f));
    CHECK(near_(ui_total_scale(1.30f, 144), 1.95f));
    CHECK(near_(ui_total_scale(1.0f, 120), 1.25f));
    CHECK(near_(ui_total_scale(3.0f, 96), 1.80f));      // ui обрезается до предела
    CHECK(near_(ui_total_scale(1.0f, 0), 1.0f));        // dpi неизвестен — как 96
    CHECK(near_(ui_total_scale(1.0f, 10), 1.0f));       // dpi нелепо мал — не ниже 96
    CHECK(near_(ui_total_scale(1.0f, 10000), 5.0f));    // и не выше 480

    // шаг кнопок A−/A+
    CHECK(near_(ui_scale_step(1.30f, +1), 1.40f));
    CHECK(near_(ui_scale_step(1.30f, -1), 1.20f));
    CHECK(near_(ui_scale_step(1.80f, +1), 1.80f));
    CHECK(near_(ui_scale_step(0.90f, -1), 0.90f));
    CHECK(near_(ui_scale_step(1.25f, +1), 1.40f));      // к сетке 0.1: 1.25 → 1.3 (округление) → 1.4
}

// ответ -cal как у прошивки: 39 строк hex (10 слов c0…c4, CRC, нули) и серийник последней строкой
static std::string cal_reply(const double c[5])
{
    std::string r;
    char b[16];
    for (int k = 0; k < 5; k++) {
        uint64_t u; std::memcpy(&u, &c[k], 8);
        snprintf(b, sizeof b, "%08X\r\n", (unsigned)(u >> 32)); r += b;
        snprintf(b, sizeof b, "%08X\r\n", (unsigned)u);         r += b;
    }
    r += "1A2B3C\r\n";                                   // CRC (короткий hex, как crc.ToString("X"))
    for (int i = 11; i < 39; i++) r += "00000000\r\n";
    r += "04740001\r\n";
    return r;
}

// чистая логика главного экрана: счёт шагов, источник, -cal, -tc, -log
static void test_home_logic()
{
    Snapshot s = connected_snap();
    s.known = SetupKnown::Yes;
    CHECK(steps_done_count(s) == 1);                 // только «Прошивка»
    s.s.chk_unix = 3; s.s.chk_ok = true; s.s.zero_unix = 4;
    CHECK(steps_done_count(s) == 3);
    Snapshot old = connected_snap();
    old.known = SetupKnown::OldFirmware;
    CHECK(steps_done_count(old) == 0);
    CHECK(first_open_step(old) == Step::Flash);
    CHECK(steps_done_count(Snapshot()) == 0);

    CHECK(!step_needs_cs(Step::Flash) && !step_needs_cs(Step::Check) && !step_needs_cs(Step::Zero));
    CHECK(step_needs_cs(Step::Hv) && step_needs_cs(Step::Res) && step_needs_cs(Step::Scale));
    CHECK(!step_needs_cs(Step::Save));

    double c[5] = { 0, 0.555, 0, 0, 0 }, o[5] = { 9, 9, 9, 9, 9 };
    CHECK(parse_becq_cal(cal_reply(c), o));
    CHECK_NEAR(o[1], 0.555, 1e-12);
    CHECK(o[0] == 0 && o[4] == 0);
    double z[5] = { 0, 0, 0, 0, 0 };
    CHECK(!parse_becq_cal(cal_reply(z), o));          // все нули — шкалы нет
    CHECK(!parse_becq_cal("-ok tc 25 0 0", o));
    CHECK(!parse_becq_cal("", o));

    double t0, k1, k2;
    CHECK(parse_tc("-ok tc 25.000 0.000000e+00 0.000000e+00", t0, k1, k2));
    CHECK(t0 == 25.0 && k1 == 0 && k2 == 0);
    CHECK(parse_tc("-ok tc 24.500 -1.200000e-03 3.000000e-06\r\n", t0, k1, k2));
    CHECK_NEAR(k1, -1.2e-3, 1e-12);
    CHECK(!parse_tc("-ok tc 25", t0, k1, k2));
    CHECK(!parse_tc("-err tc", t0, k1, k2));

    LogState ls;
    CHECK(parse_log_status("-ok log off period 600 delay 120 active 0 session 3 rec 0 records 7 "
                           "used_kb 28 total_kb 8000 wrap 0 id EF4017 err 0 scan done", ls));
    CHECK(!ls.enabled && !ls.active && ls.records == 7);
    CHECK(parse_log_status("-ok log on period 600 delay 120 active 1 session 4 rec 2 records 9 "
                           "used_kb 36 total_kb 8000 wrap 0 id EF4017 err 0 scan done", ls));
    CHECK(ls.enabled && ls.active && ls.records == 9);
    CHECK(!parse_log_status("-ok log on 600 120", ls));      // ответ на "-log on", не статус
    CHECK(!parse_log_status("-err log noflash id 000000", ls));
}

static void test_wizard_records()
{
    // команды записи результатов шагов мастера в прибор (-setup …)
    CHECK(setup_cmd(Step::Res, 1762800000u, 6.99, 1190) == "-setup res 1762800000 6.99 1190.0");
    CHECK(setup_cmd(Step::Check, 1762800000u, 0, 0) == "-setup chk 1762800000 0");
    CHECK(setup_cmd(Step::Check, 1762800000u, 1, 0) == "-setup chk 1762800000 1");
    CHECK(setup_cmd(Step::Save, 1762800000u, 0, 0) == "-setup done 1762800000");
    CHECK(setup_cmd(Step::Zero, 1762800000u, 0, 0) == "-setup zero 1762800000");
    CHECK(setup_cmd(Step::Tpl, 1762800000u, 0, 0) == "-setup tpl 1762800000");
    CHECK(setup_cmd(Step::Hv, 1762800000u, 755.4, 1181.26) == "-setup hv 1762800000 755.4 1181.3");
    CHECK(setup_cmd(Step::Hv2, 1762800000u, 760, 1190) == "-setup hv 1762800000 760.0 1190.0");
    CHECK(setup_cmd(Step::Flash, 1762800000u, 0, 0).empty());    // следа в записи не оставляют
    CHECK(setup_cmd(Step::Scale, 1762800000u, 0, 0).empty());
    // короткий ответ на запись не принимается за запись о настройке
    SetupRec r;
    CHECK(!parse_setup("-ok setup", r));
}

// ---- набор до N импульсов в фотопике (Task 9b) ----

// синтетический спектр: гаусс (площадь area, центр m, σ sg) + линейно спадающий фон + «низ» (комптон/шумы)
static std::vector<uint32_t> synth(double area, double m, double sg, double bg_scale)
{
    std::vector<uint32_t> s(8192, 0);
    for (int i = 0; i < 8192; i++) {
        double g = area / (sg * 2.5066283) * exp(-0.5 * ((i - m) / sg) * ((i - m) / sg));
        double b = bg_scale * ((i < 6000 ? (6000.0 - i) / 6000.0 * 20.0 : 0) + (i < 250 ? 400.0 : 0));
        s[i] = (uint32_t)lround(g + b);
    }
    return s;
}

// детерминированный «шум»: пуассон приближённо, λ + √λ·N(0,1) (N — сумма 12 равномерных − 6)
static void noisify(std::vector<uint32_t>& s, uint32_t seed)
{
    uint32_t x = seed;
    auto u = [&]() { x = x * 1664525u + 1013904223u; return (x >> 8) / 16777216.0; };
    for (auto& v : s) {
        double z = -6; for (int k = 0; k < 12; k++) z += u();
        double y = v + sqrt((double)v) * z;
        v = y > 0 ? (uint32_t)lround(y) : 0;
    }
}

static void test_peak_count()
{
    using namespace peakfit;
    // Cs-137: пик 1180, FWHM 7 % (σ 35.1), 20 000 в пике, фон
    const double m = 1180, sg = 0.07 * 1180 / 2.3548;
    std::vector<uint32_t> s = synth(20000, m, sg, 1.0);
    PeakWin w = peak_window(s.data(), 8192, 300, 6000);
    CHECK(w.ok);
    CHECK_NEAR(w.center, m, 1.0);
    CHECK_NEAR(w.fwhm, 2.3548 * sg, 0.03 * 2.3548 * sg);
    // окно ±1.5·FWHM
    CHECK_NEAR(w.lo, m - 1.5 * 2.3548 * sg, 2.0);
    CHECK_NEAR(w.hi, m + 1.5 * 2.3548 * sg, 2.0);
    // сумма в окне — ровно сумма каналов [lo, hi)
    uint64_t ref = 0; for (int i = w.lo; i < w.hi; i++) ref += s[i];
    CHECK(window_sum(s.data(), 8192, w.lo, w.hi) == ref);
    // ±3.5σ: почти весь пик плюс фон под ним (фон ≈ 20·(6000 − x)/6000 на канал)
    double bgsum = 0; for (int i = w.lo; i < w.hi; i++) bgsum += 20.0 * (6000 - i) / 6000;
    CHECK_NEAR((double)ref, 20000 * 0.9996 + bgsum, 150.0);
    // границы суммы обрезаются по спектру
    CHECK(window_sum(s.data(), 8192, -10, 2) == (uint64_t)s[0] + s[1]);
    CHECK(window_sum(s.data(), 8192, 8190, 9000) == (uint64_t)s[8190] + s[8191]);
    CHECK(window_sum(s.data(), 8192, 100, 100) == 0);
    // подгонка отдаёт колокол с фоном: модель в центре ≈ высота + фон
    CHECK(w.fit.ok);
    CHECK_NEAR(w.fit.model(m), 20000 / (sg * 2.5066283) + 20.0 * (6000 - m) / 6000, 8.0);
    CHECK(w.fit.model(m + 10 * sg) < w.fit.model(m) * 0.2);

    // мало счёта, но с шумом — пик ещё виден (3000 в пике, как проход подбора ВВ)
    std::vector<uint32_t> s2 = synth(3000, 900, 0.075 * 900 / 2.3548, 0.15);
    noisify(s2, 12345);
    PeakWin w2 = peak_window(s2.data(), 8192, 300, 6000);
    CHECK(w2.ok);
    CHECK_NEAR(w2.center, 900, 6.0);

    // неуверенно — счёт не идёт
    std::vector<uint32_t> z(8192, 0);
    CHECK(!peak_window(z.data(), 8192, 300, 6000).ok);           // пусто
    std::vector<uint32_t> flat(8192, 5);
    CHECK(!peak_window(flat.data(), 8192, 300, 6000).ok);        // ровный фон
    std::vector<uint32_t> few(8192, 0);
    few[1000] = 1; few[1003] = 1; few[1300] = 1; few[2000] = 2;
    CHECK(!peak_window(few.data(), 8192, 300, 6000).ok);         // единичные импульсы
    // пик вне окна поиска не берётся
    std::vector<uint32_t> s3 = synth(20000, 7000, 200, 0);
    CHECK(!peak_window(s3.data(), 8192, 300, 6000).ok);

    // условие остановки: по N и по страховочному времени
    CHECK(count_stop(2999, 3000, 10, 120) == Stop::Run);
    CHECK(count_stop(3000, 3000, 10, 120) == Stop::Count);
    CHECK(count_stop(3001, 3000, 200, 120) == Stop::Count);     // набралось — это не отказ
    CHECK(count_stop(100, 3000, 120, 120) == Stop::Timeout);
    CHECK(count_stop(0, 20000, 599.9, 600) == Stop::Run);
    CHECK(count_stop(0, 20000, 600, 600) == Stop::Timeout);

    // набор во времени: 2000/с в пике → N = 20 000 за ~10 с, а не по фиксированному времени
    std::vector<uint32_t> acc(8192, 0);
    std::vector<uint32_t> per_s = synth(2000, m, sg, 0.1);
    int stop_t = -1; Stop why = Stop::Run;
    for (int t = 1; t <= 600 && why == Stop::Run; t++) {
        for (int i = 0; i < 8192; i++) acc[i] += per_s[i];
        PeakWin pw = peak_window(acc.data(), 8192, 300, 6000);
        uint64_t n = pw.ok ? window_sum(acc.data(), 8192, pw.lo, pw.hi) : 0;
        why = count_stop(n, 20000, t, 600);
        stop_t = t;
    }
    CHECK(why == Stop::Count);
    CHECK(stop_t >= 9 && stop_t <= 10);
    // источника нет (только фон) → страховка по времени
    std::vector<uint32_t> bg_only(8192, 0), bg_s = synth(0, m, sg, 0.1);
    why = Stop::Run; stop_t = -1;
    for (int t = 1; t <= 600 && why == Stop::Run; t++) {
        for (int i = 0; i < 8192; i++) bg_only[i] += bg_s[i];
        PeakWin pw = peak_window(bg_only.data(), 8192, 300, 6000);
        uint64_t n = pw.ok ? window_sum(bg_only.data(), 8192, pw.lo, pw.hi) : 0;
        why = count_stop(n, 20000, t, 120);
        stop_t = t;
    }
    CHECK(why == Stop::Timeout);
    CHECK(stop_t == 120);
}

// слабый источник: оценка оставшегося времени и правило «принять / не принять» итог набора
static void test_weak_source()
{
    using namespace peakfit;
    // оставшееся время: (N − n) / скорость
    CHECK_NEAR(eta_s(100.0, 1000, 11000), 100.0, 1e-9);
    CHECK_NEAR(eta_s(250.0, 10000, 100000), 360.0, 1e-9);
    CHECK(eta_s(100.0, 11000, 11000) == 0.0);          // набрано
    CHECK(eta_s(100.0, 12000, 11000) == 0.0);
    CHECK(eta_s(0.0, 10, 11000) < 0);                   // скорость неизвестна
    CHECK(eta_s(-1.0, 10, 11000) < 0);
    // 5 кБк, NaI 25×25: ≈200 имп/с в пике → 100 000 за ≈8 мин, в предел 30 мин укладывается
    CHECK_NEAR(eta_s(200.0, 2000, 100000), 490.0, 1e-9);

    // итог: N набрано — Full; меньше N, но ≥ минимума — Partial (предел времени или «Хватит»); иначе Reject
    CHECK(accept(100000, 100000, RES_MIN) == Accept::Full);
    CHECK(accept(100500, 100000, RES_MIN) == Accept::Full);
    CHECK(accept(99999, 100000, RES_MIN) == Accept::Partial);
    CHECK(accept(10000, 100000, RES_MIN) == Accept::Partial);
    CHECK(accept(9999, 100000, RES_MIN) == Accept::Reject);
    CHECK(accept(0, 100000, RES_MIN) == Accept::Reject);
    CHECK(accept(5000, 5000, RES_MIN) == Accept::Full);  // задано меньше минимума и набрано — принять
    CHECK(accept(4000, 5000, RES_MIN) == Accept::Reject);
    CHECK(RES_MIN == 10000);
    // эталон формы: минимум tpl::build — 640 одиночных
    CHECK(accept(10000, 10000, tpl::NPH * 10) == Accept::Full);
    CHECK(accept(640, 10000, tpl::NPH * 10) == Accept::Partial);
    CHECK(accept(639, 10000, tpl::NPH * 10) == Accept::Reject);

    // подсчёт одиночных по кускам порций = подсчёт по всем сразу (для живого счёта при сборе эталона)
    std::vector<std::vector<uint16_t>> blocks;
    uint32_t rs = 777;
    auto rnd = [&]() { rs = rs * 1664525u + 1013904223u; return (rs >> 8) * (1.0 / 16777216.0); };
    for (int b = 0; b < 7; b++) {
        std::vector<double> x(tpl::BLK);
        for (int i = 0; i < tpl::BLK; i++) x[i] = 400.0 + 2.0 * (rnd() + rnd() + rnd() - 1.5);
        // импульсы: каждые ~400 отсчётов, амплитуда 150…1100; часть парами (не одиночные)
        for (int c = 100 + b * 7; c + 60 < tpl::BLK; c += 400) {
            double A = 150 + 950 * rnd();
            auto put = [&](int c0, double a) {
                for (int k = 0; k < 30 && c0 + k < tpl::BLK; k++) x[c0 + k] += a * (k / 4.0) * exp(1.0 - k / 4.0);
            };
            put(c, A);
            if (((c / 400) % 5) == 0) put(c + 15, 300);     // наложение
        }
        std::vector<uint16_t> blk(tpl::BLK);
        for (int i = 0; i < tpl::BLK; i++) blk[i] = (uint16_t)(x[i] + 0.5);
        blocks.push_back(blk);
    }
    tpl::Singles all = tpl::find_singles(blocks);
    CHECK(all.amp > 50);
    CHECK(all.amp < all.cross);                          // наложения отброшены
    int sum = tpl::count_singles(blocks, 0, 3) + tpl::count_singles(blocks, 3, 4) + tpl::count_singles(blocks, 4, 7);
    CHECK(sum == all.amp);
    CHECK(tpl::count_singles(blocks, 0, 7) == all.amp);
    CHECK(tpl::count_singles(blocks, 5, 5) == 0);
    CHECK(tpl::count_singles(blocks, 6, 99) == tpl::count_singles(blocks, 6, 7));   // конец обрезается
}

// вид одиночных импульсов для шага «Форма импульса»: выделение из порции, нормировка, метки, кольцо
static void test_pulse_view()
{
    // одна порция: база 400 ± шум, импульсы известной формы a·(k/4)·e^(1−k/4), максимум на k = 4
    uint32_t rs = 4242;
    auto rnd = [&]() { rs = rs * 1664525u + 1013904223u; return (rs >> 8) * (1.0 / 16777216.0); };
    std::vector<double> x(tpl::BLK);
    for (int i = 0; i < tpl::BLK; i++) x[i] = 400.0 + 2.0 * (rnd() + rnd() + rnd() - 1.5);
    auto put = [&](int c0, double a) {
        for (int k = 0; k < 40 && c0 + k < tpl::BLK; k++) x[c0 + k] += a * (k / 4.0) * exp(1.0 - k / 4.0);
    };
    std::vector<int> good_at;                              // отсчёт максимума одиночных
    size_t bad_want = 0;
    for (int c = 200; c + 100 < tpl::BLK; c += 400) {
        int n = c / 400;
        if (n % 7 == 3)      { put(c, 600); put(c + 30, 300); bad_want += 2; }   // наложение — оба в брак
        else if (n % 7 == 5) { put(c, 4000); bad_want += 1; }                     // упор в clip — в брак
        else                 { put(c, 200 + 40 * (n % 20)); good_at.push_back(c + 4); }
    }
    std::vector<uint16_t> blk(tpl::BLK);
    for (int i = 0; i < tpl::BLK; i++) blk[i] = (uint16_t)(x[i] > 4095.0 ? 4095.0 : x[i] + 0.5);

    tpl::BlockView v = tpl::view_block(blk);
    std::vector<std::vector<uint16_t>> one{ blk };
    CHECK((int)v.ok.size() == tpl::count_singles(one, 0, 1));   // критерии те же, что у счёта
    CHECK(v.ok.size() == good_at.size());
    for (size_t i = 0; i < v.ok.size() && i < good_at.size(); i++) CHECK(v.ok[i] == good_at[i]);
    CHECK(v.bad.size() == bad_want);                       // наложения и упоры отмечены
    CHECK_NEAR(v.base, 400.0, 1.0);
    CHECK(v.pulses.size() == v.ok.size());
    for (const auto& p : v.pulses) {
        // максимум 1 ровно в отсчёте VPRE
        CHECK_NEAR(p[tpl::VPRE], 1.0, 1e-6);
        int am = 0;
        for (int i = 0; i < tpl::VW; i++) if (p[i] > p[am]) am = i;
        CHECK(am == tpl::VPRE);
        // база вычтена: до фронта ≈ 0
        double pre = 0;
        for (int i = 0; i < tpl::VPRE - 4; i++) pre += p[i];
        CHECK(fabs(pre / (tpl::VPRE - 4)) < 0.02);
        // форма: через 8 отсчётов после максимума (12/4)·e^−2 ≈ 0.406
        CHECK_NEAR(p[tpl::VPRE + 8], 3.0 * exp(-2.0), 0.03);
    }

    // кольцо последних импульсов: хранит cap новейших, at(0) — самый старый
    tpl::PulseRing ring(3);
    CHECK(ring.size() == 0);
    for (int k = 0; k < 5; k++) { tpl::VWin w{}; w[0] = (float)k; ring.push(w); }
    CHECK(ring.size() == 3);
    CHECK(ring.at(0)[0] == 2.0f && ring.at(1)[0] == 3.0f && ring.at(2)[0] == 4.0f);
    ring.clear();
    CHECK(ring.size() == 0);
    tpl::VWin w1{}; w1[0] = 9.0f; ring.push(w1);
    CHECK(ring.size() == 1 && ring.at(0)[0] == 9.0f);

    // эталон отдаёт непрерывную форму для наложения: максимум ≈ 1 около нуля
    std::vector<std::vector<uint16_t>> blocks;
    for (int b = 0; b < 60; b++) {
        std::vector<double> y(tpl::BLK);
        for (int i = 0; i < tpl::BLK; i++) y[i] = 400.0 + 2.0 * (rnd() + rnd() + rnd() - 1.5);
        for (int c = 150; c + 100 < tpl::BLK; c += 300) {
            double fr = rnd(), A = 300 + 700 * rnd();
            for (int k = 0; k < 40 && c + k < tpl::BLK; k++) {
                double u = k - fr;
                if (u > 0) y[c + k] += A * (u / 4.0) * exp(1.0 - u / 4.0);
            }
        }
        std::vector<uint16_t> bb(tpl::BLK);
        for (int i = 0; i < tpl::BLK; i++) bb[i] = (uint16_t)(y[i] + 0.5);
        blocks.push_back(bb);
    }
    tpl::Result r = tpl::build(tpl::find_singles(blocks));
    CHECK(r.ok);
    CHECK(!r.shape_x.empty() && r.shape_x.size() == r.shape_y.size());
    size_t im = 0;
    for (size_t i = 0; i < r.shape_y.size(); i++) if (r.shape_y[i] > r.shape_y[im]) im = i;
    if (!r.shape_y.empty()) {
        CHECK_NEAR(r.shape_y[im], 1.0, 0.03);
        CHECK(fabs(r.shape_x[im]) < 0.6);
    }
}

// ---- таблица хвоста (разбор наложений) ----
// синтетическая форма: (u/4)·e^(1 − u/4) и медленный выброс в минус
static double tail_g(double u)
{
    if (u <= 0) return 0.0;
    double neg = u > 8 ? -0.005 * (1.0 - exp(-(u - 8) / 12.0)) * exp(-u / 100.0) : 0.0;
    return (u / 4.0) * exp(1.0 - u / 4.0) + neg;
}

static std::vector<std::vector<uint16_t>> tail_blocks(int nb, uint32_t seed)
{
    std::vector<std::vector<uint16_t>> blocks;
    uint32_t rs = seed;
    auto rnd = [&]() { rs = rs * 1664525u + 1013904223u; return (rs >> 8) * (1.0 / 16777216.0); };
    for (int b = 0; b < nb; b++) {
        std::vector<double> y(tpl::BLK);
        for (int i = 0; i < tpl::BLK; i++) y[i] = 400.0 + 2.0 * (rnd() + rnd() + rnd() - 1.5);
        for (int c = 150; c + 200 < tpl::BLK; c += 300) {
            double fr = rnd(), A = 600.0 * (0.95 + 0.1 * rnd());     // как фотопик Cs
            for (int k = 0; k < 300 && c + k < tpl::BLK; k++) y[c + k] += A * tail_g(k - fr);
        }
        std::vector<uint16_t> bb(tpl::BLK);
        for (int i = 0; i < tpl::BLK; i++) bb[i] = (uint16_t)(y[i] + 0.5);
        blocks.push_back(bb);
    }
    return blocks;
}

static void test_build_tail_synthetic()
{
    auto blocks = tail_blocks(80, 4242);
    tpl::Singles s = tpl::find_singles(blocks);
    CHECK(s.lwins.size() > 1000);
    tpl::Result r = tpl::build(s);
    CHECK(r.ok);
    tpl::Tail t = tpl::build_tail(s, r);
    CHECK(t.ok);
    if (!t.ok) { printf("  tail: %s\n", tpl::tail_text(t).c_str()); return; }
    // форма: u50 — 50 % фронта, gmax — максимум
    double gmax = 0, u50 = 0;
    for (int i = 0; i <= 20000; i++) gmax = std::max(gmax, tail_g(i * 0.001));
    for (int i = 0; i <= 20000; i++) if (tail_g(i * 0.001) >= 0.5 * gmax) { u50 = i * 0.001; break; }
    // Сверка с аналитической формой: отсчёт imax + j строки r должен быть κ_r·(g(j − t_row[r] + u50 + δ_r)/gmax + b0).
    // Явно отделены три величины, которые не являются ошибкой формы и проверяются отдельно:
    //  - κ_r — масштаб строки: A подгонки эталона у её фаз отличается от истинной высоты на доли процента;
    //  - δ_r — время строки: фазы эталона сопоставлены истинному времени импульса со смещением до ~0.05
    //    отсчёта (подгонка 7 отсчётов выбирает фазу по χ², а не по времени). Таблица следует тому, что видит
    //    детектор, а не g: это проверено по данным (max_pct ниже). Разброс δ_r ограничен 0.1 отсчёта;
    //  - b0 — общая постоянная: база детектора — среднее спокойных отсчётов, а в них выброс в минус предыдущих
    //    импульсов (у синтетики ≈ −0.1 % A); база берётся так же, как в приборе, поэтому общая постоянная
    //    допускается, |b0| ≤ 0.2 % A.
    // После этого форма строки на j = 5…64 обязана совпасть с g ≤ 0.1 % A (бриф).
    auto dev_at = [&](int rr, int j, double d, double b0) {     // j = 1…64
        return fabs(t.s[rr][j - 1] / (32768.0 * t.k_row[rr]) - (tail_g(j - t.t_row[rr] + u50 + d) / gmax + b0));
    };
    double efar = 1e9, b0best = 0, dr[tpl::TL_ROWS] = {};
    for (int qb = -20; qb <= 20; qb++) {
        double b0 = qb * 0.0001, e = 0, dd[tpl::TL_ROWS];
        for (int rr = 0; rr < tpl::TL_ROWS && e < efar; rr++) {
            double be = 1e9;
            for (int q = -75; q <= 75; q++) {
                double d = q * 0.002, er = 0;
                for (int j = 5; j <= tpl::TL_LEN && er < be; j++) er = std::max(er, dev_at(rr, j, d, b0));
                if (er < be) { be = er; dd[rr] = d; }
            }
            e = std::max(e, be);
        }
        if (e < efar) { efar = e; b0best = b0; for (int rr = 0; rr < tpl::TL_ROWS; rr++) dr[rr] = dd[rr]; }
    }
    double dmin = 1e9, dmax = -1e9, enear = 0;
    for (int rr = 0; rr < tpl::TL_ROWS; rr++) {
        dmin = std::min(dmin, dr[rr]); dmax = std::max(dmax, dr[rr]);
        for (int j = 1; j <= 4; j++) enear = std::max(enear, dev_at(rr, j, dr[rr], b0best));
    }
    for (int rr = 0; rr < tpl::TL_ROWS; rr++) {
        CHECK(t.t_row[rr] >= t.off_lo - 1e-9 && t.t_row[rr] <= t.off_hi + 1e-9);
        CHECK(fabs(t.k_row[rr] - 1.0) < 0.01);
    }
    bool neg = false;
    for (int rr = 0; rr < tpl::TL_ROWS; rr++) for (int j = 0; j < tpl::TL_LEN; j++) if (t.s[rr][j] < 0) neg = true;
    printf("  tail synthetic: max |s/κ − g| j 5…64 = %.4f %% A, j 1…4 = %.4f %% A; b0 %+.3f %%, δ_r %+.3f…%+.3f отсч.; "
           "off %.3f…%.3f, rms_pct %.4f, max_pct %.4f\n",
           100 * efar, 100 * enear, 100 * b0best, dmin, dmax, t.off_lo, t.off_hi, t.rms_pct, t.max_pct);
    CHECK(efar <= 0.001);                              // j = 5…64: ≤ 0.1 % A (бриф)
    CHECK(fabs(b0best) <= 0.002);
    CHECK(dmax - dmin <= 0.1 && dmin > -0.15 + 1e-9 && dmax < 0.15 - 1e-9);
    // j = 1…4 (вершина): строка берёт поправку на средний остаток своих импульсов (у реального эталона форма у
    // вершины по фазам расходится сильнее, чем общая S со сдвигом и масштабом: на cs_only до 0.6 % A на j = 1).
    // Поправка следует за подгонкой эталона по окну −1…+5, где смещение A у вершины не сводится к масштабу κ_r.
    // Допуск 0.5 % A: на Δ ≤ 4 второй импульс прибор не принимает (проверка фронта, мин. Δ ≈ 12), эти отсчёты
    // хвоста на амплитуду второго не влияют; по данным поправка проверена через max_pct (≤ 0.1 % на всех j).
    CHECK(enear <= 0.005);
    // главное: хвост по таблице совпадает с хвостами одиночных (A и фаза подгонкой эталона, как в приборе)
    // на всех j = 1…64 и во всех строках ≤ 0.1 % A
    CHECK(t.max_pct <= 0.1);
    CHECK(neg);                                        // выброс в минус в таблице есть
    CHECK(t.off_hi > t.off_lo);
    for (int p = 0; p < tpl::NPH; p++) CHECK(t.row[p] < tpl::TL_ROWS);
    CHECK(t.rms_pct > 0 && t.rms_pct < 0.05);
    // без эталона — не строится
    tpl::Result bad;
    CHECK(!tpl::build_tail(s, bad).ok);
    tpl::Singles few;
    CHECK(!tpl::build_tail(few, r).ok);
}

static void test_tail_commands()
{
    tpl::Tail t;
    for (int r = 0; r < tpl::TL_ROWS; r++) for (int j = 0; j < tpl::TL_LEN; j++) t.s[r][j] = (int16_t)(j % 2 ? -32768 : 32767);
    for (int p = 0; p < tpl::NPH; p++) t.row[p] = (uint8_t)(p % tpl::TL_ROWS);
    t.ok = true;
    auto v = tpl::tail_commands(t);
    CHECK(v.size() == 69u);
    if (v.size() != 69u) return;
    for (int k = 0; k < 64; k++) CHECK(v[k].rfind("-tlw ", 0) == 0);
    for (int k = 64; k < 68; k++) CHECK(v[k].rfind("-tlm ", 0) == 0);
    CHECK(v[68] == "-tlok");
    for (auto& c : v) CHECK(c.size() <= 256);
    CHECK(v[0].rfind("-tlw 0 0 32767 -32768 ", 0) == 0);
    CHECK(v[5] == "-tlw 1 1 32767 -32768 32767 -32768 32767 -32768 32767 -32768 32767 -32768 32767 -32768 32767 -32768 32767 -32768");
    CHECK(v[65] == "-tlm 1 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15");
}

static void test_save_tail_bin()
{
    tpl::Tail t;
    for (int r = 0; r < tpl::TL_ROWS; r++) for (int j = 0; j < tpl::TL_LEN; j++) t.s[r][j] = (int16_t)(r * 100 - j);
    for (int p = 0; p < tpl::NPH; p++) t.row[p] = (uint8_t)(p / 4);
    t.ok = true;
    std::string path = tmp_ini("gp_tail_test.bin");
    CHECK(tpl::save_tail_bin(t, path));
    FILE* f = fopen(path.c_str(), "rb");
    CHECK(f != nullptr);
    if (!f) return;
    unsigned char b[3000];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    remove(path.c_str());
    CHECK(n == 2112u);
    CHECK(b[0] == 0 && b[1] == 0);                                   // s[0][0] = 0
    int16_t v = (int16_t)(b[2] | (b[3] << 8));                       // s[0][1] = −1, little-endian
    CHECK(v == -1);
    int16_t w = (int16_t)(b[2 * 64] | (b[2 * 64 + 1] << 8));         // s[1][0] = 100
    CHECK(w == 100);
    CHECK(b[2048] == 0 && b[2048 + 63] == 15);                       // row
}

// запись эталона и таблицы по шагам: хвост и -save — только после -tplok без ошибок
static void test_tpl_load()
{
    tpl::Result r; r.ok = true;
    tpl::Tail t; t.ok = true;
    tpl::Tail none;                                   // таблица не построена
    {   // всё принято: эталон → таблица → -save
        tpl::TplLoad L;
        auto v = L.start(r, t, true);
        CHECK(v.size() == 65u && v.back() == "-tplok" && v[0].rfind("-tplw 0 ", 0) == 0);
        CHECK(L.busy() && !L.done());
        v = L.next(0, "");
        CHECK(v.size() == 69u && v[0].rfind("-tlw ", 0) == 0 && v.back() == "-tlok");
        CHECK(L.tpl_ok && L.tail_sent);
        v = L.next(0, "");
        CHECK(v.size() == 1u && v[0] == "-save");
        v = L.next(0, "");
        CHECK(v.empty() && L.done() && L.tpl_ok && L.tail_ok && L.save_ok);
    }
    {   // -tplok не прошёл (-err): ни таблицы, ни -save — в приборе старый эталон
        tpl::TplLoad L;
        L.start(r, t, true);
        auto v = L.next(1, "-err tpl rows");
        CHECK(v.empty() && L.done() && !L.tpl_ok && !L.tail_sent && !L.save_sent);
        CHECK(L.tpl_err == "-err tpl rows");
    }
    {   // старая прошивка: 69 × -err unknown на таблицу — эталон записан, -save идёт (эталон верный)
        tpl::TplLoad L;
        L.start(r, t, true);
        L.next(0, "");
        auto v = L.next(69, "-err unknown");
        CHECK(v.size() == 1u && v[0] == "-save");
        CHECK(L.tpl_ok && L.tail_sent && !L.tail_ok && L.tail_err == "-err unknown");
        v = L.next(0, "");
        CHECK(v.empty() && L.done() && L.save_ok);
    }
    {   // таблицы нет, без -save: после эталона — конец
        tpl::TplLoad L;
        L.start(r, none, false);
        auto v = L.next(0, "");
        CHECK(v.empty() && L.done() && L.tpl_ok && !L.tail_sent && !L.save_sent);
        CHECK(L.next(0, "").empty());                 // после конца — ничего
    }
    {   // связь пропала посреди таблицы: конец, ни эталон, ни таблица не считаются принятыми
        tpl::TplLoad L;
        L.start(r, t, true);
        L.next(0, "");
        CHECK(L.busy() && L.tpl_ok && L.tail_sent);
        L.abort("no link");
        CHECK(!L.busy() && L.done() && !L.tpl_ok && !L.tail_ok && !L.save_ok && !L.save_sent);
        CHECK(L.tpl_err == "no link");
        CHECK(L.next(0, "").empty());
        L.abort("x");                                 // после конца — без изменений
        CHECK(L.tpl_err == "no link");
        tpl::TplLoad I;                               // не начата — не трогается
        I.abort("x");
        CHECK(I.stage == tpl::TplLoad::Idle && I.tpl_err.empty());
    }
    {   // -save не прошёл
        tpl::TplLoad L;
        L.start(r, none, true);
        CHECK(L.next(0, "") == std::vector<std::string>{ "-save" });
        L.next(1, "-err save");
        CHECK(L.done() && !L.save_ok && L.save_err == "-err save");
    }
}

// текст итога таблицы хвоста — из таблицы строк, на обоих языках
static void test_tpl_text()
{
    tpl::Result r;
    for (auto e : { tpl::Result::Few, tpl::Result::Shape }) {
        r.err = e; r.n_found = 77;
        i18n_set(Lang::Ru); std::string ru = tpl::result_text(r);
        i18n_set(Lang::En); std::string en = tpl::result_text(r);
        CHECK(!ru.empty() && !en.empty() && ru != en);
        CHECK(ru.find("tpl.") == std::string::npos && en.find("tpl.") == std::string::npos);
    }
    r.ok = true; r.err = tpl::Result::None; r.n = 1234;
    i18n_set(Lang::Ru);
    CHECK(tpl::result_text(r).find("1234") != std::string::npos);
    r.ok = false; r.err = tpl::Result::Few; r.n_found = 77;
    CHECK(tpl::result_text(r).find("77") != std::string::npos && tpl::result_text(r).find("640") != std::string::npos);
}

// очередь команд: фоновая телеметрия -gst не закрывает ожидание ответа, следующий "-err" засчитывается
static void test_queue_reply()
{
    CHECK(ds::queue_reply_kind("-ok gst hv=1000 t=20") == 0);
    CHECK(ds::queue_reply_kind("-ok") == 1 && ds::queue_reply_kind("-ok saved") == 1 && ds::queue_reply_kind("-ok tplw") == 1);
    CHECK(ds::queue_reply_kind("-err dnl") == 2);
    CHECK(ds::queue_reply_kind("GammaSpec") == 0);
    bool wait = true; int errors = 0;
    for (const char* l : { "-ok gst hv=1000", "-err dnl bad", "-ok gst hv=1000" }) {
        int k = ds::queue_reply_kind(l);
        if (wait && k) { wait = false; if (k == 2) errors++; }
    }
    CHECK(errors == 1 && !wait);
}

static void test_tail_text()
{
    tpl::Tail t;
    for (auto e : { tpl::Tail::None, tpl::Tail::NoTpl, tpl::Tail::Few, tpl::Tail::NoTime, tpl::Tail::Cleaned, tpl::Tail::Shape }) {
        t.err = e;
        i18n_set(Lang::Ru); std::string ru = tpl::tail_text(t);
        i18n_set(Lang::En); std::string en = tpl::tail_text(t);
        CHECK(!ru.empty() && !en.empty() && ru != en);
        CHECK(ru.find("tail.") == std::string::npos && en.find("tail.") == std::string::npos);
    }
    i18n_set(Lang::Ru);
    t.err = tpl::Tail::Few; t.n_all = 12;
    CHECK(tpl::tail_text(t).find("12") != std::string::npos);
}

// отчёт: шаг не проводился в этот раз — дата из прибора или «нет данных»
static void test_report_not_run()
{
    CHECK(report_not_run(0) == "не проводилось в этот раз (последний раз: нет данных)");
    CHECK(report_not_run(1) == "не проводилось в этот раз (последний раз: дата неизвестна)");
    std::string r = report_not_run(1762800000u);     // 10.11.2025 по UTC; в местном поясе может быть 11.11
    CHECK(r.find("не проводилось в этот раз (последний раз: 1") == 0);
    CHECK(r.find(".11.2025)") != std::string::npos);
}

// наложение на хвост: второй импульс садится, пока первый ещё выше порога, — нового пересечения
// порога снизу нет, а импульс не одиночный (найдено при приёмке 2026-11-12 на реальных порциях)
static void test_tail_pileup()
{
    uint32_t rs = 777;
    auto rnd = [&]() { rs = rs * 1664525u + 1013904223u; return (rs >> 8) * (1.0 / 16777216.0); };
    std::vector<double> x(tpl::BLK);
    for (int i = 0; i < tpl::BLK; i++) x[i] = 400.0 + 2.0 * (rnd() + rnd() + rnd() - 1.5);
    auto put = [&](int c0, double a) {
        for (int k = 0; k < 40 && c0 + k < tpl::BLK; k++) x[c0 + k] += a * (k / 4.0) * exp(1.0 - k / 4.0);
    };
    put(1000, 600);                    // чистый
    put(2000, 600); put(2012, 150);    // второй на хвосте (+12: хвост первого ≈ 240 кодов над базой)
    put(3000, 800); put(3022, 300);    // второй позже (+22: хвост ≈ 50 кодов, всё ещё выше порога 10σ)
    put(4000, 300); put(4008, 600);    // наложение на фронте: порог пересёк первый, максимум — у второго
    std::vector<uint16_t> blk(tpl::BLK);
    for (int i = 0; i < tpl::BLK; i++) blk[i] = (uint16_t)(x[i] + 0.5);
    tpl::BlockView v = tpl::view_block(blk);
    CHECK(v.ok.size() == 1);
    if (!v.ok.empty()) CHECK(v.ok[0] == 1004);
    std::vector<std::vector<uint16_t>> one{ blk };
    CHECK(tpl::count_singles(one, 0, 1) == 1);
    CHECK(tpl::find_singles(one).wins.size() == 1);
}

// -baud: допустимые скорости и разбор ответа прибора
static void test_baud()
{
    CHECK(ds::BAUD_DEFAULT == 600000u);
    CHECK(ds::baud_allowed(600000) && ds::baud_allowed(1000000) && ds::baud_allowed(1500000) &&
          ds::baud_allowed(2000000) && ds::baud_allowed(3000000));
    CHECK(!ds::baud_allowed(115200) && !ds::baud_allowed(0) && !ds::baud_allowed(2500000) && !ds::baud_allowed(6000000));
    CHECK(ds::parse_baud_reply("-ok baud 3000000") == 3000000);
    CHECK(ds::parse_baud_reply("-ok baud 600000\r\n") == 600000);
    CHECK(ds::parse_baud_reply("-err baud") == -1);
    CHECK(ds::parse_baud_reply("-err baud\r\n") == -1);
    CHECK(ds::parse_baud_reply("-err unknown") == -1);      // старая прошивка
    CHECK(ds::parse_baud_reply("-ok baud 115200") == 0);    // не из списка — не наш ответ
    CHECK(ds::parse_baud_reply("-ok baud 3000000x") == 0);
    CHECK(ds::parse_baud_reply("-ok baud") == 0);
    CHECK(ds::parse_baud_reply("-ok hv 500") == 0);
    CHECK(ds::parse_baud_reply("-err hv") == 0);
}

// сбор порций: дописать в файл по ходу и выбросить из памяти
static void test_append_blocks()
{
    FILE* f = tmpfile();
    CHECK(f != nullptr);
    if (!f) return;
    std::vector<std::vector<uint16_t>> b(2, std::vector<uint16_t>(tpl::BLK));
    for (int i = 0; i < tpl::BLK; i++) { b[0][i] = (uint16_t)i; b[1][i] = (uint16_t)(60000 - i); }
    b.push_back(std::vector<uint16_t>(10, 7));               // не той длины — пропуск
    bool ok = false;
    CHECK(tpl::append_blocks(f, b, ok) == 2);
    CHECK(ok && b.empty());
    b.assign(1, std::vector<uint16_t>(tpl::BLK, 1234));
    CHECK(tpl::append_blocks(f, b, ok) == 1 && ok && b.empty());
    CHECK(ftell(f) == 3L * tpl::BLK * 2);
    rewind(f);
    std::vector<uint16_t> r(tpl::BLK);
    CHECK(fread(r.data(), 2, tpl::BLK, f) == (size_t)tpl::BLK && r[0] == 0 && r[8191] == 8191);
    CHECK(fread(r.data(), 2, tpl::BLK, f) == (size_t)tpl::BLK && r[0] == 60000 && r[100] == 59900);
    CHECK(fread(r.data(), 2, tpl::BLK, f) == (size_t)tpl::BLK && r[5] == 1234);
    fclose(f);
    b.assign(1, std::vector<uint16_t>(tpl::BLK));
    CHECK(tpl::append_blocks(nullptr, b, ok) == 0 && !ok && b.empty());
}

// ---- таблица DNL АЦП (adc_dnl) ----
// Синтетический АЦП: границы кодов edge[c] (код c — отрезок [edge[c], edge[c+1])), спектр — горб базы у 620
// (σ 4) и спад exp(−x/1500) с плавным началом у 700 и концом у 3600 (как у настоящего спектра — без ступенек),
// гистограмма = ожидание по отрезку кода + шум Пуассона.
static std::vector<uint64_t> dnl_hist(const std::vector<double>& edge, double per_code, uint32_t seed)
{
    std::mt19937 rng(seed);
    auto dens = [&](double x) {           // отсчётов на LSB
        double v = 0;
        v += per_code * std::exp(-(x - 2047.0) / 1500.0) / (1.0 + std::exp(-(x - 700.0) / 20.0))
                      / (1.0 + std::exp((x - 3600.0) / 20.0));
        v += 2e6 * std::exp(-0.5 * (x - 620.0) * (x - 620.0) / 16.0);
        return v;
    };
    std::vector<uint64_t> H(adc_dnl::CODES, 0);
    for (int c = 0; c < adc_dnl::CODES; c++) {
        double a = edge[c], b = edge[c + 1];
        if (b <= a) continue;
        double mu = 0;
        const int K = 16;
        for (int k = 0; k < K; k++) mu += dens(a + (b - a) * (k + 0.5) / K) * (b - a) / K;
        if (mu <= 0) continue;
        if (mu > 1e5) H[c] = (uint64_t)std::llround(std::max(0.0, mu + std::sqrt(mu) * std::normal_distribution<double>(0, 1)(rng)));
        else H[c] = std::poisson_distribution<uint64_t>(mu)(rng);
    }
    return H;
}

static std::vector<double> dnl_edges_nominal(double noise, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(-noise, noise);
    std::vector<double> e(adc_dnl::CODES + 1);
    for (int c = 0; c <= adc_dnl::CODES; c++) e[c] = c - 0.5 + ((c > 0 && c < adc_dnl::CODES) ? u(rng) : 0.0);
    return e;
}

static const adc_dnl::Entry* dnl_find(const adc_dnl::Result& r, int code)
{
    for (auto& e : r.table) if (e.code == code) return &e;
    return nullptr;
}

static void test_adc_dnl_gap()
{
    // DNL ±0.1 LSB на всех кодах + пропуск 15 кодов: 2048…2061 не выдаются, всё — в 2047 ([2046.5, 2061.5),
    // центр 2054, сдвиг +7 LSB = 112/16); умеренный DNL у 1791/1792/1793 (ширины 2.4/0.3/0.3)
    std::vector<double> e = dnl_edges_nominal(0.1, 11);
    for (int c = 2048; c <= 2062; c++) e[c] = 2061.5;          // коды 2048…2061 — нулевой ширины
    e[2047] = 2046.5;
    e[1791] = 1790.5; e[1792] = 1792.9; e[1793] = 1793.2; e[1794] = 1793.5;
    auto H = dnl_hist(e, 300.0, 5);
    CHECK(H[2047] > 3000 && H[2050] == 0);
    auto r = adc_dnl::compute(H);
    CHECK(r.ok);
    printf("  adc dnl (пропуск 15 кодов): %s;", r.msg.c_str());
    for (auto& t : r.table) printf(" %d:%d", t.code, t.off16);
    printf("\n");
    const adc_dnl::Entry* g = dnl_find(r, 2047);
    CHECK(g != nullptr);
    if (g) {
        CHECK(std::abs(g->off16 - 112) <= 8);                  // ±0.5 LSB
        CHECK_NEAR(g->width, 15.0, 0.8);
    }
    for (int c = 2048; c <= 2061; c++) CHECK(dnl_find(r, c) == nullptr);   // не встречаются
    const adc_dnl::Entry* a = dnl_find(r, 1791);
    const adc_dnl::Entry* b = dnl_find(r, 1792);
    CHECK(a && std::abs(a->off16 - 11) <= 5);                  // центр 1791.7: +0.7 LSB
    CHECK(b && std::abs(b->off16 - 17) <= 6);                  // центр 1793.05: +1.05 LSB
    int base_hits = 0, other = 0;
    for (auto& t : r.table) {
        if (t.code < 700) base_hits++;                         // горб базы — норма не годится
        else if (t.code != 2047 && t.code != 2062 && (t.code < 1791 || t.code > 1793)) other++;
    }
    CHECK(base_hits == 0);
    CHECK(other == 0);                                         // шум DNL ±0.1 в таблицу не попадает
    CHECK(r.table.size() <= (size_t)adc_dnl::MAX_PAIRS);
    for (size_t i = 1; i < r.table.size(); i++) CHECK(r.table[i - 1].code < r.table[i].code);

    // без дефектов — таблица пуста
    auto r0 = adc_dnl::compute(dnl_hist(dnl_edges_nominal(0.1, 12), 300.0, 6));
    CHECK(r0.ok && r0.table.empty());
    for (auto& t : r0.table) printf("  лишнее: %d:%d (ширина %.2f, норма %.1f)\n", t.code, t.off16, t.width, t.norm);
}

static void test_adc_dnl_top16()
{
    // 20 широких кодов (ширина 1 + d, за ним — пропущенные и узкий код): в таблицу 16 с наибольшим сдвигом
    std::vector<double> e = dnl_edges_nominal(0.0, 1);
    for (int k = 0; k < 20; k++) {
        int c = 900 + 100 * k;
        double d = 1.2 + 0.1 * k;                              // сдвиг центра c: d/2 (0.6…1.55 LSB)
        for (int j = 1; j <= 4; j++) e[c + j] = std::max(e[c + j], c + 0.5 + d);   // следом — пропуск и узкий
    }
    auto r = adc_dnl::compute(dnl_hist(e, 2000.0, 7));
    CHECK(r.ok && r.table.size() == (size_t)adc_dnl::MAX_PAIRS);
    CHECK(r.candidates.size() >= 20);
    int small = 0;
    for (auto& t : r.table) if (t.code < 900 + 100 * 4) small++;   // 4 наименьших (k 0…3) — за бортом
    if (small) { for (auto& t : r.candidates) printf(" %d:%d", t.code, t.off16); printf("\n"); }
    CHECK(small == 0);

    // пропуск в группе с узким кодом ниже 0.1 LSB: такой код не берётся
    std::vector<double> e2 = dnl_edges_nominal(0.0, 1);
    e2[1501] = 1503.45; e2[1502] = 1503.48;                    // 1500 — ширина 2.95, 1501 — 0.03
    auto r2 = adc_dnl::compute(dnl_hist(e2, 2000.0, 8));
    CHECK(dnl_find(r2, 1500) != nullptr);
    CHECK(dnl_find(r2, 1501) == nullptr);
}

static void test_adc_dnl_file()
{
    adc_dnl::Result r;
    r.ok = true; r.msg = "проба";
    adc_dnl::Entry a; a.code = 767; a.off16 = 8; a.off = 0.5; a.width = 2.0; a.count = 46865; a.norm = 23000;
    adc_dnl::Entry b; b.code = 2047; b.off16 = 127; b.off = 7.93; b.width = 16.86; b.count = 2209; b.norm = 131;
    r.table = { a, b };
    std::string txt = adc_dnl::format_table(r, "файлы: mix_1.bin\nвторая строка");
    std::vector<std::pair<int, int>> t;
    std::string err;
    CHECK(adc_dnl::parse_table(txt, t, err));
    CHECK(t.size() == 2 && t[0].first == 767 && t[0].second == 8 && t[1].first == 2047 && t[1].second == 127);
    CHECK(!adc_dnl::parse_table("2047 112 5\n", t, err));
    CHECK(!adc_dnl::parse_table("4096 1\n", t, err));
    CHECK(!adc_dnl::parse_table("100 0\n", t, err));
    CHECK(!adc_dnl::parse_table("100 1025\n", t, err));
    CHECK(!adc_dnl::parse_table("100 5\n100 6\n", t, err));
    CHECK(!adc_dnl::parse_table("abc\n", t, err));
    CHECK(adc_dnl::parse_table("# пусто\n\n", t, err) && t.empty());
    std::string many;
    for (int k = 0; k < 17; k++) many += std::to_string(100 + k) + " 3\n";
    CHECK(!adc_dnl::parse_table(many, t, err));
    auto v = adc_dnl::load_commands({ { 767, 8 }, { 2047, 127 } }, true);
    CHECK(v.size() == 3 && v[0] == "-dnl clr" && v[1] == "-dnl 767 8 2047 127" && v[2] == "-save");
    auto v0 = adc_dnl::load_commands({}, false);
    CHECK(v0.size() == 1 && v0[0] == "-dnl clr");
    std::vector<std::pair<int, int>> full;
    for (int k = 0; k < 16; k++) full.push_back({ 4095 - k, -1024 });
    CHECK(adc_dnl::load_commands(full, false)[1].size() <= 250);   // кадр прибора — 256 байт
    std::vector<uint64_t> h;
    uint16_t smp[4] = { 5, 4095, 4096, 5 };
    uint64_t over = 0;
    adc_dnl::add_samples(h, smp, 4, &over);
    CHECK(h.size() == 4096 && h[5] == 2 && h[4095] == 1 && over == 1);
}

int main()
{
    test_adc_dnl_gap();
    test_adc_dnl_top16();
    test_adc_dnl_file();
    test_baud();
    test_append_blocks();
    test_peak_count();
    test_weak_source();
    test_pulse_view();
    test_tail_pileup();
    test_build_tail_synthetic();
    test_tail_commands();
    test_save_tail_bin();
    test_tpl_load();
    test_tail_text();
    test_tpl_text();
    test_queue_reply();
    test_report_not_run();
    test_wizard_records();
    test_home_logic();
    test_parse_setup();
    test_old_firmware();
    test_migrated_ready();
    test_steps();
    test_prereq();
    test_top_bar();
    test_connect_hint();
    test_display_serial();
    test_contrast();
    test_i18n();
    test_appcfg();
    test_appcfg_scale();
    return T_DONE();
}
