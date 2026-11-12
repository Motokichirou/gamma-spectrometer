// ui_shell.h — оболочка пульта: верхняя полоса (имя, прибор, ВВ, режимы) и строка состояния
// (связь, ВВ, сохранено ли, язык, справка). Содержимое режимов рисует main.cpp в области между ними.
#pragma once
#include <string>
#include "devstate.h"

class Device;
extern Device g_dev;                 // сессия связи с прибором (определена в main.cpp)

// страницы расширенного режима (порядок навигации макета; Task 10 рисует их, пока g_ui.adv_page только запоминается)
enum AdvPage { Overview, Spectrum, Hv, Template, Scale, Thermo, Detector, Sensors, Selftest, Logger, Firmware, Console, AdvN };

struct Ui {
    ds::Mode mode = ds::Mode::Home;
    ds::Step wiz_step = ds::Step::Flash;
    int adv_page = Overview;         // AdvPage
    std::string flash_port;          // порт для шага «Прошивка»: главный экран кладёт его перед ui_go(Wizard, Flash)
    bool help_req = false;           // строка состояния просит открыть справку (окно справки рисует main.cpp)
};
extern Ui g_ui;

// Единственный способ сменить режим. Wizard без шага: при входе в мастер — первый несделанный шаг.
// Уход в Home запрашивает у прибора запись о настройке (-setup).
void ui_go(ds::Mode m, ds::Step s = ds::Step::N);

void  DrawShell();                   // верхняя полоса + строка состояния
float TopBarHeight();
float StatusBarHeight();
