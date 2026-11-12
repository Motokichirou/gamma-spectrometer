// test_host.cpp — консольные тесты чистой логики пульта (devstate и др.), без COM-порта и UI.
// Макросы — общие с тестами прошивки: firmware/gamma_fw/tests/t.h.
#include "t.h"
#include "devstate.h"
#include "theme_tokens.h"
#include "i18n.h"
#include "appcfg.h"
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

int main()
{
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
    return T_DONE();
}
