// test_host.cpp — консольные тесты чистой логики пульта (devstate и др.), без COM-порта и UI.
// Макросы — общие с тестами прошивки: firmware/gamma_fw/tests/t.h.
#include "t.h"
#include "devstate.h"
#include "theme_tokens.h"
#include "i18n.h"
#include "appcfg.h"
#include "peakfit.h"
#include <vector>
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

// отчёт: шаг не проводился в этот раз — дата из прибора или «нет данных»
static void test_report_not_run()
{
    CHECK(report_not_run(0) == "не проводилось в этот раз (последний раз: нет данных)");
    CHECK(report_not_run(1) == "не проводилось в этот раз (последний раз: дата неизвестна)");
    std::string r = report_not_run(1762800000u);     // 10.11.2025 по UTC; в местном поясе может быть 11.11
    CHECK(r.find("не проводилось в этот раз (последний раз: 1") == 0);
    CHECK(r.find(".11.2025)") != std::string::npos);
}

int main()
{
    test_peak_count();
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
    return T_DONE();
}
