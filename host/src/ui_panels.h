// ui_panels.h — прежние панели и вкладки сервисного пульта (определены в main.cpp).
// Панели (Panel*) сами открывают окно в заданном прямоугольнике; вкладки (Tab*) рисуют
// в текущее окно. Расширенный режим (ui_adv.cpp) раскладывает их по страницам.
#pragma once
#include "imgui.h"

void PanelConnection(ImVec2 pos, ImVec2 size);   // порт, подключить/отключить, старт/стоп/сброс набора
void PanelTelemetry(ImVec2 pos, ImVec2 size);    // -gst
void PanelHV(ImVec2 pos, ImVec2 size);           // уставка, вкл/выкл ВВ, подбор по пику
void PanelSpectrum(ImVec2 pos, ImVec2 size);     // спектр, Лин/Лог
void PanelConsole(ImVec2 pos, ImVec2 size);      // журнал обмена и строка команды

void TabSelftest();   // самотест ЦАП (прогон линейности)
void TabCalib();      // монитор ВВ (ноль, k) и Pt1000
void TabDetector();   // порог, PGA, сохранить, умолчания
void TabHvBoot();     // автозапуск ВВ при подаче питания
void TabDiag();       // -diag, -noise
void TabTemplate();   // эталон формы импульса
void TabLogger();     // регистратор
void TabThermo();     // термокомпенсация
