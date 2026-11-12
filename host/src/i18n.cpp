// i18n.cpp — таблица строк интерфейса пульта (русский на «вы», английский по смыслу) и T().
// Правила строк: кнопки глаголом; без «—» и «–», кроме диапазонов «3–5»; формат printf (%d, %s)
// в ru и en одинаковый. Строки шагов мастера здесь короткие, полные тексты добавляет мастер.
#include "i18n.h"
#include <cstring>
#include <set>
#include <unordered_map>

static const I18nEntry k_table[] = {
    // ---- верхняя полоса ----
    { "top.no_link",        "Нет связи",                      "Not connected" },
    { "top.hv_off",         "ВВ выключено",                   "HV off" },
    { "top.hv_on",          "ВВ включено · %d В",             "HV on · %d V" },
    { "top.hv_unknown",     "ВВ: ?",                          "HV: ?" },
    { "top.kill_hv",        "Выключить ВВ",                   "Turn off HV" },
    { "top.wizard",         "Мастер настройки",               "Setup wizard" },
    { "top.advanced",       "Расширенный режим",              "Advanced mode" },
    { "top.home",           "Главная",                        "Home" },
    { "top.disconnect",     "Отключить",                      "Disconnect" },
    { "top.lang",           "Язык",                           "Language" },
    { "top.lang_ru",        "Русский",                        "Русский" },
    { "top.lang_en",        "English",                        "English" },

    // ---- строка состояния ----
    { "status.no_link",     "Связи нет",                      "No connection" },
    { "status.link",        "Связь: %s",                      "Connected: %s" },
    { "status.hv_unknown",  "ВВ: неизвестно",                 "HV: unknown" },
    { "status.hv_off",      "ВВ: выключено",                  "HV: off" },
    { "status.hv_on",       "ВВ: включено, %d В",             "HV: on, %d V" },
    { "status.ports",       "Найдено портов: %d",             "Ports found: %d" },
    { "status.saved",       "Всё сохранено",                  "All saved" },
    { "status.unsaved",     "! Настройки не сохранены в прибор", "! Settings not saved to the device" },
    { "status.help",        "Справка",                        "Help" },

    // ---- экран подключения ----
    { "conn.title",         "Подключите прибор",              "Connect the device" },
    { "conn.hint",          "Вставьте кабель USB и выберите порт.", "Plug in the USB cable and choose the port." },
    { "conn.port",          "Порт",                           "Port" },
    { "conn.no_ports",      "Портов не найдено",              "No ports found" },
    { "conn.refresh",       "Обновить список",                "Refresh list" },
    { "conn.connect",       "Подключить",                     "Connect" },
    { "conn.connecting",    "Подключение…",                   "Connecting…" },
    { "conn.one_program",   "Порт может держать только одна программа. Если открыт BecqMoni, сначала отключитесь в нём.",
                            "Only one program can use the port at a time. If BecqMoni is open, disconnect it there first." },
    { "conn.port_busy",     "Порт занят другой программой. Отключитесь в BecqMoni или закройте его и попробуйте снова.",
                            "The port is in use by another program. Disconnect or close BecqMoni, then try again." },
    { "conn.port_gone",     "Порт не найден. Проверьте кабель USB и обновите список.",
                            "Port not found. Check the USB cable and refresh the list." },
    { "conn.failed",        "Не удалось подключиться. Переподключите кабель и попробуйте снова.",
                            "Couldn't connect. Replug the cable and try again." },
    { "conn.no_answer",     "Прибор не отвечает. Проверьте, что выбран нужный порт.",
                            "The device isn't responding. Make sure you chose the right port." },

    // ---- главный экран ----
    { "home.mark.warn",     "Внимание",                       "Attention" },
    { "home.mark.ok",       "Готово",                         "Ready" },
    { "home.notset.title",  "Прибор не настроен",             "The device isn't set up yet" },
    { "home.notset.progress", "Пройдено шагов: %d из %d.",    "Steps done: %d of %d." },
    { "home.notset.next",   "Следующий шаг: %s.",             "Next step: %s." },
    { "home.notset.need_cs", "Понадобится источник Cs-137 и минут пять.", "You'll need a Cs-137 source and about five minutes." },
    { "home.notset.start",  "Начать настройку",               "Start setup" },
    { "home.notset.continue", "Продолжить настройку",         "Continue setup" },
    { "home.steps_title",   "Шаги настройки",                 "Setup steps" },
    { "home.step.next",     "следующий",                      "next" },
    { "home.step.needs_cs", "нужен Cs-137",                   "needs Cs-137" },
    { "home.now",           "Сейчас",                         "Now" },
    { "home.kv.hv",         "Высокое напряжение",             "High voltage" },
    { "home.kv.temp",       "Температура кристалла",          "Crystal temperature" },
    { "home.kv.rate",       "Счёт",                           "Count rate" },
    { "home.kv.vref",       "Опорное напряжение",             "Reference voltage" },
    { "home.kv.logger",     "Запись без компьютера",          "Standalone logging" },
    { "home.kv.res",        "Разрешение по Cs-137",           "Cs-137 resolution" },
    { "home.kv.scale",      "Шкала",                          "Energy scale" },
    { "home.val.on",        "включено",                       "on" },
    { "home.val.off",       "выключено",                      "off" },
    { "home.val.ok",        "норма",                          "OK" },
    { "home.val.bad",       "не в норме",                     "out of range" },
    { "home.val.logger_off", "выключена",                     "off" },
    { "home.val.logger_off_n", "выключена, записей: %d",      "off, %d records" },
    { "home.val.logger_on_n", "идёт, записей: %d",            "running, %d records" },
    { "home.val.cps",       "%d имп/с",                       "%d cps" },
    { "home.val.kev_ch",    "%.3f кэВ/канал",                 "%.3f keV/channel" },
    { "home.more_in_adv",   "Все числа и ручное управление собраны в расширенном режиме.",
                            "All readings and manual controls are in Advanced mode." },
    { "home.ready.title",   "Всё готово, можно мерить",       "Ready to measure" },
    { "home.ready.date",    "Настроен %s.",                   "Set up on %s." },
    { "home.ready.date_unknown", "Дата настройки неизвестна.", "Setup date unknown." },
    { "home.ready.becq",    "Спектры набирает BecqMoni. Отключитесь здесь и откройте его на %s.",
                            "Spectra are collected in BecqMoni. Disconnect here and open it on %s." },
    { "home.ready.give_port", "Отдать порт BecqMoni",         "Hand the port to BecqMoni" },
    { "home.more",          "Что ещё можно сделать",          "Other tasks" },
    { "home.task.selftest.title", "Проверить прибор",         "Check the device" },
    { "home.task.selftest.desc",  "Самотест на минуту, источник не нужен.", "One-minute self-test, no source needed." },
    { "home.task.logger.title",   "Настроить запись без компьютера", "Set up standalone logging" },
    { "home.task.logger.desc",    "Прибор сам пишет спектры в свою память. Здесь можно настроить запись и забрать записи.",
                                  "The device saves spectra to its own memory. Set it up and download the records here." },
    { "home.task.fw.title",       "Обновить прошивку",         "Update firmware" },
    { "home.task.fw.desc",        "В приборе %s.",             "The device runs %s." },
    { "home.task.fw.app",         "Пульт запишет прошивку %s. Настройки прибора сохранятся.",
                                  "The app writes firmware %s. Device settings are kept." },
    { "home.task.fw.fresh",       "свежая",                    "up to date" },
    { "home.task.fw.newer",       "есть новее: %s",            "newer available: %s" },
    { "home.task.scale.title",    "Поправить шкалу",           "Adjust the energy scale" },
    { "home.task.scale.desc",     "Пик уехал или вы сменили источник.", "The peak drifted or you changed the source." },
    { "home.task.wizard.title",   "Пройти настройку заново",   "Run setup again" },
    { "home.task.wizard.desc",    "Если вы поменяли кристалл или ФЭУ, пройдите все 9 шагов заново.",
                                  "Replaced the crystal or PMT? Go through all 9 steps again." },
    { "home.task.thermo.title",   "Настроить термокомпенсацию", "Set up temperature compensation" },
    { "home.task.thermo.desc",    "Нужен прогон на холоде с записью.", "Needs a logged run in the cold." },
    { "home.task.thermo.not_set", "! не настроена",            "! not set up" },
    { "home.task.thermo.set",     "настроена",                 "set up" },
    { "home.hv.title",      "Высокое напряжение",             "High voltage" },
    { "home.hv.autostart",  "Включается само при подаче питания", "Turns on by itself at power-up" },
    { "home.hv.manual",     "Включается только командой",     "Turns on only by command" },
    { "home.loading",       "Читаю состояние прибора…",       "Reading the device state…" },
    { "home.flash_link",    "Прибор не отвечает? Прошить",    "Device not responding? Flash firmware" },
    { "home.oldfw.title",   "Прошивка в приборе старая",      "The device firmware is outdated" },
    { "home.oldfw.text",    "Обновите прошивку, и пульт увидит, что в приборе уже настроено. Настройки прибора сохранятся.",
                            "Update the firmware so the app can see what is already set up. Device settings are kept." },
    { "home.notset.need_none", "Источник не нужен, это займёт около минуты.", "No source needed, takes about a minute." },
    { "home.date_unknown",  "дата неизвестна",                "date unknown" },
    { "home.res.hv",        "%.0f В, пик на канале %.0f",     "%.0f V, peak at channel %.0f" },
    { "home.res.fwhm",      "FWHM %.2f %%",                   "FWHM %.2f %%" },
    { "home.res.in_device", "есть в приборе",                 "present in the device" },
    { "home.res.failed",    "! не прошла",                    "! failed" },
    { "home.val.logger_armed_n", "включена, записей: %d",     "enabled, %d records" },
    { "home.val.hv_on_v",   "включено, %d В",                 "on, %d V" },
    { "home.val.temp",      "%.1f °C",                        "%.1f °C" },
    { "home.val.volts",     "%d В",                           "%d V" },
    { "home.val.mv",        "%.1f мВ",                        "%.1f mV" },

    // ---- мастер настройки: общее ----
    { "wiz.title",          "Настройка",                      "Setup" },
    { "wiz.step_n",         "Шаг %d из %d",                   "Step %d of %d" },
    { "wiz.you",            "Что нужно от вас:",              "Your part:" },
    { "wiz.exit",           "Выйти из мастера",               "Exit the wizard" },
    { "wiz.back",           "← Назад",                        "← Back" },
    { "wiz.next",           "Дальше →",                       "Next →" },
    { "wiz.start",          "Начать",                         "Start" },
    { "wiz.stop",           "Остановить",                     "Stop" },
    { "wiz.retry",          "Повторить",                      "Try again" },
    { "wiz.elapsed",        "прошло %s",                      "%s elapsed" },
    { "wiz.pass",           "Подбор, проход %d из %d",        "Tuning, pass %d of %d" },
    { "wiz.prereq.missing", "Этот шаг идёт после шага «%s». Сначала пройдите его.",
                            "This step needs “%s” done first." },
    { "wiz.prereq.go",      "Перейти к шагу «%s»",            "Go to “%s”" },

    // ---- шаги мастера: заголовок, короткое имя (список слева), описание ----
    { "wiz.step.flash.title", "Прошивка",                     "Firmware" },
    { "wiz.step.flash.short", "Прошивка",                     "Firmware" },
    { "wiz.step.flash.desc",  "Пульт записывает в прибор свою прошивку. Настройки прибора сохраняются.",
                              "The app writes its bundled firmware to the device. Device settings are kept." },
    { "wiz.step.check.title", "Проверка электроники",         "Electronics check" },
    { "wiz.step.check.short", "Проверка электроники",         "Electronics check" },
    { "wiz.step.check.desc",  "Пульт проверяет опорное напряжение, датчик температуры и линейность тракта. Источник не нужен.",
                              "The app checks the voltage reference, temperature sensor and signal chain linearity. No source needed." },
    { "wiz.step.check.ok",    "опора, датчик температуры и линейность в норме",
                              "reference, temperature sensor and linearity OK" },
    { "wiz.step.zero.title",  "Ноль монитора ВВ",             "HV monitor zero" },
    { "wiz.step.zero.short",  "Ноль монитора ВВ",             "HV monitor zero" },
    { "wiz.step.zero.desc",   "При выключенном ВВ пульт измеряет ноль монитора напряжения.",
                              "With HV off, the app measures the zero point of the voltage monitor." },
    { "wiz.step.hv.title",    "Подбор высокого напряжения",   "High voltage tuning" },
    { "wiz.step.hv.short",    "Подбор ВВ",                    "HV tuning" },
    { "wiz.step.hv.desc",     "Пульт сам поднимает напряжение, пока пик цезия 662 кэВ не встанет на канал %d.",
                              "The app raises the voltage until the Cs-137 662 keV peak sits on channel %d." },
    { "wiz.step.hv.you",      "положите Cs-137 вплотную к торцу детектора и не двигайте его, пока идёт подбор.",
                              "place the Cs-137 source against the detector end and keep it still during tuning." },
    { "wiz.step.hv.hint",     "Обычно 3–5 минут. Если пик не находится, проверьте, что источник на месте и в блок не попадает свет.",
                              "Usually 3–5 minutes. No peak? Check that the source is in place and no light gets into the unit." },
    { "wiz.step.hv.voltage",  "Напряжение",                   "Voltage" },
    { "wiz.step.hv.peak",     "Пик на канале",                "Peak channel" },
    { "wiz.step.hv.target",   "Цель",                         "Target" },
    { "wiz.step.tpl.title",   "Форма импульса",               "Pulse shape" },
    { "wiz.step.tpl.short",   "Форма импульса",               "Pulse shape" },
    { "wiz.step.tpl.desc",    "Пульт набирает импульсы от Cs-137 и записывает в прибор их среднюю форму.",
                              "The app collects Cs-137 pulses and stores their average shape in the device." },
    { "wiz.step.hv2.title",   "Подбор ВВ ещё раз",            "HV tuning, second pass" },
    { "wiz.step.hv2.short",   "Подбор ВВ ещё раз",            "HV tuning again" },
    { "wiz.step.hv2.desc",    "С новой формой импульса пик немного сдвигается. Пульт снова ставит его на канал %d.",
                              "The new pulse shape moves the peak slightly. The app puts it back on channel %d." },
    { "wiz.step.res.title",   "Разрешение",                   "Resolution" },
    { "wiz.step.res.short",   "Разрешение",                   "Resolution" },
    { "wiz.step.res.desc",    "Пульт набирает спектр Cs-137 и измеряет ширину пика.",
                              "The app collects a Cs-137 spectrum and measures the peak width." },
    { "wiz.step.scale.title", "Шкала энергий",                "Energy scale" },
    { "wiz.step.scale.short", "Шкала энергий",                "Energy scale" },
    { "wiz.step.scale.desc",  "По пику Cs-137 пульт строит шкалу энергий и записывает её в прибор.",
                              "From the Cs-137 peak the app builds the energy scale and saves it to the device." },
    { "wiz.step.save.title",  "Сохранение и отчёт",           "Save and report" },
    { "wiz.step.save.short",  "Сохранение и отчёт",           "Save and report" },
    { "wiz.step.save.desc",   "Пульт сохраняет настройки в прибор и пишет отчёт о настройке.",
                              "The app saves the settings to the device and writes a setup report." },

    // ---- расширенный режим: навигация ----
    { "adv.nav.group.device",  "Прибор",                      "Device" },
    { "adv.nav.group.setup",   "Настройка",                   "Setup" },
    { "adv.nav.group.service", "Обслуживание",                "Maintenance" },
    { "adv.nav.overview",      "Обзор",                       "Overview" },
    { "adv.nav.spectrum",      "Спектр",                      "Spectrum" },
    { "adv.nav.hv",            "Высокое напряжение",          "High voltage" },
    { "adv.nav.tpl",           "Форма импульса",              "Pulse shape" },
    { "adv.nav.scale",         "Шкала энергий",               "Energy scale" },
    { "adv.nav.thermo",        "Термокомпенсация",            "Temperature compensation" },
    { "adv.nav.detector",      "Порог и детектор",            "Threshold and detector" },
    { "adv.nav.sensors",       "Датчики: монитор ВВ и Pt1000", "Sensors: HV monitor and Pt1000" },
    { "adv.nav.selftest",      "Самотест",                    "Self-test" },
    { "adv.nav.logger",        "Регистратор",                 "Logger" },
    { "adv.nav.firmware",      "Прошивка",                    "Firmware" },
    { "adv.nav.console",       "Консоль команд",              "Command console" },

    // ---- расширенный режим: страница ВВ, телеметрия, журнал ----
    { "adv.hv.chart",       "ВВ за последние 5 минут",        "HV, last 5 minutes" },
    { "adv.hv.monitor",     "монитор",                        "monitor" },
    { "adv.hv.setpoint_line", "уставка %d В",                 "setpoint %d V" },
    { "adv.hv.chart_on",    "включено",                       "on" },
    { "adv.hv.chart_now",   "сейчас",                         "now" },
    { "adv.hv.chart_min",   "\xE2\x88\x92%d мин",             "\xE2\x88\x92%d min" },   // U+2212 «минус»
    { "adv.hv.setpoint",    "Уставка",                        "Setpoint" },
    { "adv.hv.set",         "Установить",                     "Apply" },
    { "adv.hv.off",         "Выключить",                      "Turn off" },
    { "adv.hv.autostart",   "Включать ВВ само при подаче питания", "Turn HV on at power-up" },
    { "adv.save",           "Сохранить в прибор",             "Save to device" },
    { "adv.unsaved",        "! не сохранено",                 "! not saved" },
    { "adv.hv.tune",        "Подбор по пику",                 "Tune by peak" },
    { "adv.hv.tune_target", "Пик Cs-137 поставить на канал",  "Put the Cs-137 peak on channel" },
    { "adv.hv.tune_btn",    "Подобрать",                      "Tune" },
    { "adv.hv.tune_hint",   "нужен источник, 3–5 минут",      "needs a source, 3–5 min" },
    { "adv.hv.zero",        "Измерить ноль монитора",         "Measure monitor zero" },
    { "adv.hv.zero_hint",   "только при выключенном ВВ",      "only with HV off" },
    { "adv.tele",           "Телеметрия",                     "Telemetry" },
    { "adv.tele.hv_mon",    "ВВ по монитору",                 "HV (monitor)" },
    { "adv.tele.setpoint",  "Уставка",                        "Setpoint" },
    { "adv.tele.vref",      "Опора",                          "Reference" },
    { "adv.tele.pt1000",    "Pt1000",                         "Pt1000" },
    { "adv.tele.rate",      "Счёт",                           "Count rate" },
    { "adv.tele.noise",     "Шум базы σ",                     "Baseline noise σ" },
    { "adv.tele.cpu",       "Загрузка МК",                    "MCU load" },
    { "adv.tele.errors",    "Ошибки обмена",                  "Link errors" },
    { "adv.log",            "Журнал команд",                  "Command log" },
    { "adv.log.missing_key", "Нет строки для ключа: %s",      "No string for key: %s" },
};

static Lang g_lang = Lang::Ru;
static std::vector<std::string> g_missing;

static const std::unordered_map<std::string, const I18nEntry*>& index_()
{
    static std::unordered_map<std::string, const I18nEntry*> m = [] {
        std::unordered_map<std::string, const I18nEntry*> r;
        for (const I18nEntry& e : k_table) r.emplace(e.key, &e);   // при дубле берётся первый, дубль ловит i18n_check
        return r;
    }();
    return m;
}

void i18n_set(Lang l) { g_lang = l; }
Lang i18n_get() { return g_lang; }
size_t i18n_count() { return sizeof k_table / sizeof k_table[0]; }
const std::vector<std::string>& i18n_missing() { return g_missing; }

const char* T(const char* key)
{
    if (!key) return "";
    const auto& m = index_();
    auto it = m.find(key);
    if (it == m.end()) {
        bool seen = false;
        for (const std::string& s : g_missing) if (s == key) { seen = true; break; }
        if (!seen) g_missing.emplace_back(key);
        return key;
    }
    return g_lang == Lang::En ? it->second->en : it->second->ru;
}

static bool is_digit_(unsigned char c) { return c >= '0' && c <= '9'; }

// «—» везде запрещено; «–» разрешено только между цифрами («3–5»)
static const char* dash_problem_(const char* s)
{
    size_t n = std::strlen(s);
    for (size_t i = 0; i + 3 <= n; ++i) {
        const unsigned char* p = (const unsigned char*)s + i;
        if (p[0] != 0xE2 || p[1] != 0x80) continue;
        if (p[2] == 0x94) return "U+2014";
        if (p[2] == 0x93) {
            bool ok = i > 0 && is_digit_((unsigned char)s[i - 1]) && i + 3 < n && is_digit_((unsigned char)s[i + 3]);
            if (!ok) return "U+2013";
        }
    }
    return nullptr;
}

// подстановки printf по порядку (без %%), чтобы ru и en принимали одни и те же аргументы
static std::string fmt_sig_(const char* s)
{
    std::string r;
    for (const char* p = s; *p; ++p) {
        if (*p != '%') continue;
        ++p;
        if (*p == '%') continue;
        while (*p && std::strchr("0123456789.-+ #", *p)) ++p;
        if (*p) r += *p; else break;
    }
    return r;
}

bool i18n_check_entries(const I18nEntry* e, size_t n, std::string& report)
{
    bool ok = true;
    std::set<std::string> keys;
    auto bad = [&](const char* key, const std::string& what) {
        ok = false;
        report += key ? key : "(null)";
        report += ": ";
        report += what;
        report += "\n";
    };
    for (size_t i = 0; i < n; ++i) {
        const I18nEntry& x = e[i];
        if (!x.key || !*x.key) { bad(x.key, "пустой ключ"); continue; }
        if (!keys.insert(x.key).second) bad(x.key, "ключ повторяется");
        if (!x.ru || !*x.ru) bad(x.key, "пустая ru");
        if (!x.en || !*x.en) bad(x.key, "пустая en");
        if (x.ru) if (const char* d = dash_problem_(x.ru)) bad(x.key, std::string("ru: тире ") + d);
        if (x.en) if (const char* d = dash_problem_(x.en)) bad(x.key, std::string("en: тире ") + d);
        if (x.ru && x.en && fmt_sig_(x.ru) != fmt_sig_(x.en)) bad(x.key, "подстановки %… в ru и en разные");
    }
    return ok;
}

bool i18n_check(std::string& report)
{
    return i18n_check_entries(k_table, i18n_count(), report);
}
