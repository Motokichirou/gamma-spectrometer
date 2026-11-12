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

    // ---- мастер: подача шага (Task 9) ----
    { "wiz.more",           "Подробнее",                      "Details" },
    { "wiz.finish",         "Готово",                         "Done" },
    { "wiz.step_done",      "Шаг пройден: %s",                "Step done: %s" },
    { "wiz.step_done_plain", "Шаг пройден.",                  "Step done." },
    { "wiz.no_link",        "Нет связи с прибором. Без связи можно только прошить прибор.",
                            "No connection to the device. Without it you can only flash the firmware." },
    { "wiz.rec.saving",     "Записываю результат в прибор…",  "Saving the result to the device…" },
    { "wiz.rec.failed",     "! Прибор не принял запись: %s",  "! The device rejected the record: %s" },
    { "wiz.busy_other",     "Пульт занят другой работой. Дождитесь её конца или остановите её.",
                            "The app is busy with another task. Wait for it to finish or stop it." },
    { "wiz.reply",          "Прибор ответил: %s",             "Device reply: %s" },

    { "wiz.step.flash.you",   "подключите прибор кабелем USB. Если прибор ещё ни разу не прошивали, сначала переведите его в режим загрузчика (как, написано в «Подробнее»).",
                              "connect the device by USB. If it has never been flashed, put it into bootloader mode first (see Details)." },
    { "wiz.step.flash.need",  "Понадобится кабель USB и около минуты. Источник не нужен.",
                              "You need a USB cable and about a minute. No source needed." },
    { "wiz.step.flash.hint",  "Не получается? Переподключите кабель, проверьте порт и нажмите «Прошить» ещё раз.",
                              "Not working? Replug the cable, check the port and press Flash again." },
    { "wiz.step.flash.more",  "Новый МК без прошивки стартует в системный загрузчик, если провод BOOT0 (вывод 45) через 10 кОм соединён с +3.3 В. Прибор с прошивкой уходит в загрузчик сам по команде пульта. После прошивки верните BOOT0 на GND, иначе МК снова уйдёт в загрузчик. Эталон, шкала, калибровки и уставка ВВ при прошивке сохраняются.",
                              "A blank MCU starts in the system bootloader when the BOOT0 wire (pin 45) is tied to +3.3 V through 10 kOhm. A device with firmware enters the bootloader on the app's command. After flashing, return BOOT0 to GND, or the MCU will boot into the bootloader again. Template, scale, calibrations and the HV setpoint survive flashing." },
    { "wiz.flash.port",       "Порт",                         "Port" },
    { "wiz.flash.btn",        "Прошить (%s)",                 "Flash (%s)" },
    { "wiz.flash.no_image",   "в пульт не встроена прошивка", "no firmware built into the app" },
    { "wiz.flash.answers",    "Прибор отвечает: %s",          "The device responds: %s" },
    { "wiz.flash.update",     "Можно обновить прошивку до встроенной в пульт (%s). Настройки сохранятся.",
                              "You can update to the firmware built into the app (%s). Settings are kept." },
    { "wiz.flash.ok",         "Прошито и проверено.",         "Flashed and verified." },
    { "wiz.flash.boot0",      "! Если BOOT0 был на +3.3 В, переставьте его на GND, иначе прибор снова уйдёт в загрузчик.",
                              "! If BOOT0 was on +3.3 V, move it to GND, or the device will boot into the bootloader again." },
    { "wiz.flash.err",        "! Ошибка: %s",                 "! Error: %s" },
    { "wiz.flash.connect",    "Подключиться к прибору",       "Connect to the device" },

    { "wiz.step.check.you",   "убедитесь, что ВВ выключено, и нажмите «Проверить».",
                              "make sure HV is off and press Check." },
    { "wiz.step.check.need",  "Понадобится около минуты. Источник не нужен.",
                              "Takes about a minute. No source needed." },
    { "wiz.step.check.hint",  "Не прошло? Выключите ВВ, подождите минуту и проверьте ещё раз. Подробности по каждому пункту есть в расширенном режиме.",
                              "Failed? Turn HV off, wait a minute and check again. Each item is detailed in advanced mode." },
    { "wiz.step.check.more",  "Пульт шлёт -diag: напряжения узлов, опора 2500 ± 15 мВ, шум базы (без ВВ около 2–2.5 кода), Pt1000. Затем самотест линейности: встроенный ЦАП подаёт серию импульсов на вход шейпера. Норма: R² > 0.9999, отклонение < 0.2 %%, в пике > 99 %% импульсов.",
                              "The app sends -diag: node voltages, reference 2500 ± 15 mV, baseline noise (about 2–2.5 codes with HV off), Pt1000. Then the linearity self-test: the internal DAC feeds pulses into the shaper. Pass: R² > 0.9999, deviation < 0.2 %%, > 99 %% of pulses in the peak." },
    { "wiz.check.run",        "Проверить",                    "Check" },
    { "wiz.check.diag",       "Узлы питания и тракт",         "Supply nodes and signal chain" },
    { "wiz.check.vref",       "Опорное напряжение %.1f мВ",   "Reference %.1f mV" },
    { "wiz.check.noise",      "Шум базы %.2f кода",           "Baseline noise %.2f codes" },
    { "wiz.check.pt",         "Датчик температуры: %.2f °C",  "Temperature sensor: %.2f °C" },
    { "wiz.check.pt_bad",     "Датчик температуры не отвечает", "Temperature sensor not responding" },
    { "wiz.check.lin",        "Линейность: R² %.6f, отклонение %.3f %%", "Linearity: R² %.6f, deviation %.3f %%" },
    { "wiz.check.lin_step",   "Проверяю линейность: ступенька %d из %d", "Checking linearity: step %d of %d" },
    { "wiz.check.wait_diag",  "Жду ответа прибора…",          "Waiting for the device…" },
    { "wiz.check.ptref",      "°C по термометру",             "°C by thermometer" },
    { "wiz.check.ptcal",      "Подправить датчик по термометру", "Trim the sensor to the thermometer" },
    { "wiz.check.failed",     "! Проверка не прошла. Пункты с «!» выше.", "! The check failed. See the items marked “!” above." },

    { "wiz.step.zero.you",    "выключите ВВ и подождите не меньше 2 минут. Если прибор только что включили, ждать не нужно.",
                              "turn HV off and wait at least 2 minutes. If the device was just powered on, no need to wait." },
    { "wiz.step.zero.need",   "Понадобится до 2 минут ожидания. Источник не нужен.",
                              "Up to 2 minutes of waiting. No source needed." },
    { "wiz.step.zero.hint",   "Прибор просит подождать? ВВ выключили недавно: подождите и нажмите ещё раз.",
                              "The device asks you to wait? HV was turned off recently: wait and press again." },
    { "wiz.step.zero.more",   "Команда -hvzero запоминает показание монитора при выключенном ВВ как ноль. Коэффициент монитора по умолчанию 1.633 мВ/В. Точнее его задают по мультиметру с высоковольтным делителем в расширенном режиме, страница «Высокое напряжение».",
                              "The -hvzero command stores the monitor reading with HV off as zero. The default monitor factor is 1.633 mV/V. For more accuracy, set it from a multimeter with an HV probe in advanced mode, High voltage page." },
    { "wiz.zero.now",         "Сейчас: ноль %.1f мВ, монитор %.1f мВ", "Now: zero %.1f mV, monitor %.1f mV" },
    { "wiz.zero.btn",         "Измерить ноль",                "Measure zero" },
    { "wiz.zero.hv_on",       "! ВВ включено. Сначала выключите его.", "! HV is on. Turn it off first." },

    { "wiz.step.hv.need",     "Понадобится источник Cs-137 и 3–5 минут.", "You need a Cs-137 source and 3–5 minutes." },
    { "wiz.step.hv.more",     "Начинайте с заниженного напряжения: для R1306/R1307 с R417 2.7 кОм рабочая точка около 700–800 В. Подбор: каждый проход набирает до 3000 импульсов в фотопике (окно ±1.5 FWHM вокруг пика), не дольше 120 с; затем центр пика в окне поиска, новое напряжение по закону усиления V^5.3, шаг не больше 15 %%, пока пик не попадёт в допуск 1 %%. Канал %d оставляет запас шкалы до 3.5 МэВ.",
                              "Start low: for R1306/R1307 with R417 2.7 kOhm the working point is about 700–800 V. Each pass collects up to 3000 counts in the photopeak (window ±1.5 FWHM around the peak), at most 120 s; then the peak centroid in the search window, new voltage from the gain law V^5.3, step at most 15 %%, until the peak is within 1 %%. Channel %d leaves scale headroom up to 3.5 MeV." },
    { "wiz.hv.start_v",       "Начальное напряжение, В",      "Starting voltage, V" },
    { "wiz.hv.window",        "Окно поиска пика, каналы",     "Peak search window, channels" },
    { "wiz.hv.volts",       "%.0f В",                       "%.0f V" },
    { "wiz.hv.btn",           "Подобрать напряжение",         "Tune the voltage" },
    { "wiz.hv2.btn",          "Подобрать ещё раз",            "Tune again" },
    { "wiz.hv.result",        "%.0f В, пик на канале %.1f",   "%.0f V, peak at channel %.1f" },
    { "wiz.hv.history",       "%.0f В: пик на канале %.1f",   "%.0f V: peak at channel %.1f" },

    { "wiz.step.tpl.you",     "оставьте источник Cs-137 у детектора и не двигайте его. ВВ должно быть включено.",
                              "keep the Cs-137 source at the detector and don't move it. HV must be on." },
    { "wiz.step.tpl.need",    "Понадобится источник Cs-137 и около 11 минут.", "You need a Cs-137 source and about 11 minutes." },
    { "wiz.step.tpl.hint",    "Не получилось? Проверьте, что ВВ включено и источник на месте, и начните ещё раз.",
                              "Didn't work? Check that HV is on and the source is in place, then start again." },
    { "wiz.step.tpl.more",    "Пульт собирает 2000 порций сырого сигнала АЦП (-osc raw), выбирает одиночные импульсы, считает эталон 64 фазы × 7 отсчётов и пишет его в прибор (-tplw, -tplok). Норма: невязка < 1 %%, размах по фазам < 1 %%.",
                              "The app collects 2000 raw ADC blocks (-osc raw), picks single pulses, builds a 64 phase × 7 sample template and writes it to the device (-tplw, -tplok). Pass: residual < 1 %%, phase spread < 1 %%." },
    { "wiz.tpl.btn",          "Записать форму импульса",      "Record the pulse shape" },
    { "wiz.tpl.collect",      "Набираю импульсы: %d из %d",   "Collecting pulses: %d of %d" },
    { "wiz.tpl.calc",         "Считаю форму…",                "Computing the shape…" },
    { "wiz.tpl.load",         "Записываю форму в прибор…",    "Writing the shape to the device…" },
    { "wiz.tpl.ok",           "Форма записана в прибор.",     "The shape is stored in the device." },
    { "wiz.tpl.quality",      "Невязка %.2f %%, размах по фазам %.2f %% (норма меньше 1 %%)",
                              "Residual %.2f %%, phase spread %.2f %% (pass: below 1 %%)" },
    { "wiz.tpl.failed",       "! Не получилось: %s",          "! Didn't work: %s" },

    { "wiz.step.hv2.you",     "источник Cs-137 остаётся на месте.", "leave the Cs-137 source in place." },
    { "wiz.step.hv2.need",    "Понадобится источник Cs-137 и 2–4 минуты.", "You need a Cs-137 source and 2–4 minutes." },
    { "wiz.step.hv2.hint",    "Пик ушёл из окна или не находится? Проверьте, что источник на месте, и подберите ещё раз.",
                              "Peak lost or not found? Check that the source is in place and tune again." },
    { "wiz.step.hv2.more",    "Подбор начинается от нынешней уставки ВВ и идёт так же, как в шаге «Подбор ВВ».",
                              "Tuning starts from the current HV setpoint and works as in the HV tuning step." },

    { "wiz.step.res.you",     "оставьте источник на месте. Импульсов в пике можно набрать больше.",
                              "keep the source in place. You can collect more counts in the peak." },
    { "wiz.step.res.need",    "Понадобится источник Cs-137 и 2–3 минуты.", "You need a Cs-137 source and 2–3 minutes." },
    { "wiz.step.res.hint",    "Пик не нашёлся? Проверьте, что источник на месте, и наберите ещё раз.",
                              "No peak found? Check that the source is in place and acquire again." },
    { "wiz.step.res.more",    "Набор идёт, пока в окне ±1.5 FWHM вокруг пика не наберётся заданное число импульсов (по умолчанию 100 000), но не дольше предела времени (600 с). Фотопик подгоняется гауссом с линейным фоном. Для NaI 40×40…63×63 хорошо меньше 7.5 %%, норма меньше 8.5 %%.",
                              "Acquisition runs until the window ±1.5 FWHM around the peak holds the set number of counts (100 000 by default), but no longer than the time limit (600 s). The photopeak is fitted with a Gaussian on a linear background. For NaI 40×40…63×63 below 7.5 %% is good, below 8.5 %% is normal." },
    { "wiz.res.count",        "Импульсов в пике",             "Counts in the peak" },
    { "wiz.res.timeout_lbl",  "Предел времени, с",            "Time limit, s" },
    { "wiz.res.seeking",      "Ищу пик…",                     "Looking for the peak…" },
    { "wiz.res.live",         "%u имп/с · прошло %s · не дольше %s", "%u counts/s · %s elapsed · %s at most" },
    { "wiz.res.timeout",      "! Не набралось %d импульсов в пике за %s: проверьте источник.",
                              "! Fewer than %d counts in the peak after %s: check the source." },
    { "wiz.in_peak",          "В пике: %llu из %.0f",         "In the peak: %llu of %.0f" },
    { "wiz.plot.spectrum",    "Спектр",                       "Spectrum" },
    { "wiz.plot.window",      "Окно пика",                    "Peak window" },
    { "wiz.plot.fit",         "Подгонка",                     "Fit" },
    { "wiz.plot.channel",     "канал",                        "channel" },
    { "wiz.plot.counts",      "счёт",                         "counts" },
    { "wiz.plot.lin",         "Лин",                          "Lin" },
    { "wiz.plot.log",         "Лог",                          "Log" },
    { "wiz.plot.to_peak",     "К пику",                       "Zoom to peak" },
    { "wiz.plot.full",        "Весь спектр",                  "Full spectrum" },
    { "wiz.hv.count",         "Импульсов в пике за проход",   "Counts in the peak per pass" },
    { "wiz.hv.timeout",       "Предел прохода, с",            "Pass time limit, s" },
    { "wiz.hv.live",          "%u имп/с · проход идёт %s",    "%u counts/s · pass running %s" },
    { "adv.hv.count",         "импульсов в пике",             "counts in peak" },
    { "adv.hv.timeout",       "предел, с",                    "limit, s" },
    { "wiz.res.btn",          "Набрать спектр",               "Acquire the spectrum" },
    { "wiz.res.result",       "Разрешение %.2f %% (± %.2f), пик на канале %.1f", "Resolution %.2f %% (± %.2f), peak at channel %.1f" },
    { "wiz.res.good",         "Хорошо: меньше 7.5 %%.",       "Good: below 7.5 %%." },
    { "wiz.res.norm",         "В норме: меньше 8.5 %%.",      "Normal: below 8.5 %%." },
    { "wiz.res.bad",          "! Хуже нормы 8.5 %%.",         "! Worse than the 8.5 %% norm." },
    { "wiz.res.nopeak",       "! Пик не найден.",             "! No peak found." },
    { "wiz.res.fwhm",         "Разрешение",                   "Resolution" },

    { "wiz.step.scale.you",   "ничего делать не нужно. Нажмите «Записать шкалу».",
                              "nothing to do by hand. Press Save the scale." },
    { "wiz.step.scale.need",  "Понадобится результат шага «Разрешение» и несколько секунд.",
                              "Needs the Resolution step result and a few seconds." },
    { "wiz.step.scale.hint",  "Прибор не принял шкалу? Нажмите кнопку ещё раз. Если шкала уже есть, например из BecqMoni, шаг отмечен пройденным и его можно пропустить.",
                              "The device rejected the scale? Press the button again. If a scale is already there, e.g. from BecqMoni, the step is marked done and can be skipped." },
    { "wiz.step.scale.more",  "Шкала по одному пику: E = k · канал, k = 661.66 кэВ / пик. Точнее шкала получается по нескольким линиям в BecqMoni (кнопка «Записать в устройство»).",
                              "One-peak scale: E = k · channel, k = 661.66 keV / peak. A multi-line calibration in BecqMoni (Write to device) is more accurate." },
    { "wiz.scale.calc",       "Пик на канале %.2f, шкала %.6f кэВ на канал", "Peak at channel %.2f, scale %.6f keV per channel" },
    { "wiz.scale.btn",        "Записать шкалу",               "Save the scale" },
    { "wiz.scale.writing",    "Записываю…",                   "Writing…" },
    { "wiz.scale.ok",         "Шкала записана.",              "Scale saved." },
    { "wiz.scale.err",        "! Прибор не принял шкалу: %s", "! The device rejected the scale: %s" },

    { "wiz.step.save.you",    "решите, включать ли ВВ само при подаче питания, и нажмите «Сохранить».",
                              "decide whether HV should turn on by itself at power-up, then press Save." },
    { "wiz.step.save.need",   "Понадобится несколько секунд.", "Takes a few seconds." },
    { "wiz.step.save.hint",   "Отчёт не записался? Проверьте, что в папку пульта можно писать. Настройки в приборе при этом сохранены.",
                              "Report not written? Check that the app folder is writable. The device settings are saved anyway." },
    { "wiz.step.save.more",   "С автозапуском прибор сам включает рабочее ВВ через 5 с после подачи питания, напряжение устанавливается за 1.5 минуты. Можно закрыть пульт и работать в BecqMoni. Без автозапуска ВВ включается только командой из пульта. Отчёт пишется в текстовый файл в папке пульта.",
                              "With autostart the device turns on the working HV 5 s after power-up; it settles in 1.5 minutes. You can close the app and work in BecqMoni. Without autostart HV turns on only by a command from the app. The report is a text file in the app folder." },
    { "wiz.save.autohv",      "Включать ВВ %d В само при подаче питания (блок закрыт)", "Turn on %d V HV by itself at power-up (unit closed)" },
    { "wiz.save.btn",         "Сохранить в прибор и записать отчёт", "Save to the device and write the report" },
    { "wiz.save.report",      "Отчёт: %s",                    "Report: %s" },
    { "wiz.save.report_fail", "не удалось записать отчёт",    "couldn't write the report" },

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
